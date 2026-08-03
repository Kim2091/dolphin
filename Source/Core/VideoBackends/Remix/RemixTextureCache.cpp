// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Remix/RemixTextureCache.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "VideoBackends/Remix/RemixApi.h"
#include "VideoBackends/Remix/RemixTexture.h"

// The Software backend's copy encoders, reused wholesale - every GC destination
// format for the texture path, plus the YUV encoder for the XFB one. Linked in
// rather than duplicated; see CMakeLists.txt.
#include "VideoBackends/Software/SWEfbInterface.h"
#include "VideoBackends/Software/TextureEncoder.h"

#include "VideoCommon/BPMemory.h"

namespace Remix
{
namespace
{
// Writes zeroes over exactly the region a later ReadTexels will copy out of this
// staging texture, and nothing beyond it.
//
// This is what makes "discard" mean the same thing it meant before EFB emulation
// existed. The staging textures are POOLED (TextureCacheBase.cpp:2582-2598), so
// once any copy encodes, a reused buffer carries the previous encode's image;
// without this a discarded copy would write that image into an unrelated
// destination - a recognisable UI fragment surfacing in the wrong texture.
// Before, the buffers were value-initialized and nothing ever wrote them, so
// every destination got zeroes; this reproduces that byte for byte.
void ZeroStagingRegion(RemixStagingTexture* dst, u32 bytes_per_row, u32 num_blocks_y)
{
  char* const base = dst->GetMappedPointer();
  if (base == nullptr)
    return;

  const size_t stride = dst->GetMappedStride();
  const size_t capacity = dst->GetBufferSize();
  if (stride == 0 || capacity == 0)
    return;

  // ReadTexels copies min(row bytes, stride) per row and steps by stride
  // (AbstractStagingTexture.cpp:57), so that is exactly the span to clear.
  const size_t row_bytes = std::min(static_cast<size_t>(bytes_per_row), stride);
  for (u32 row = 0; row < num_blocks_y; ++row)
  {
    const size_t offset = static_cast<size_t>(row) * stride;
    if (offset >= capacity)
      break;
    std::memset(base + offset, 0, std::min(row_bytes, capacity - offset));
  }
}
}  // namespace

void TextureCache::CopyEFB(AbstractStagingTexture* dst, const EFBCopyParams& params,
                           u32 native_width, u32 bytes_per_row, u32 num_blocks_y,
                           u32 memory_stride, const MathUtil::Rectangle<int>& src_rect,
                           bool scale_by_half, bool linear_filter, float y_scale, float gamma,
                           bool clamp_top, bool clamp_bottom,
                           const std::array<u32, 3>& filter_coefficients)
{
  // Deliberately NOT gated on RecordEfbCopies(). The destination addresses are
  // what the UI draw filter tests against, and the classification below decides
  // whether pixels are written at all, so turning the diagnostic logging off
  // must not quietly turn either of them off with it; NoteEfbCopy decides for
  // itself which parts of the record are trace-only. This runs a handful of
  // times a frame.
  if (g_remix_api == nullptr || !g_remix_api->IsValid())
    return;

  // bpmem is read HERE rather than at frame end because this call is
  // synchronous with the BPMEM_TRIGGER_EFB_COPY write that caused it:
  // TextureCacheBase.cpp:2403 forces the immediate path whenever there is no
  // VRAM copy, which on this backend is always. The clear bit and the
  // destination address are not passed down as parameters, so this is the only
  // place either is knowable per copy. If deferred EFB copies ever become
  // reachable here, this read goes stale and the two fields have to be plumbed
  // through CopyRenderTargetToTexture instead.
  const u32 dst_addr = bpmem.copyTexDest << 5;  // mirrors BPStructs.cpp:246
  const bool clear = bpmem.triggerEFBCopy.clear;
  const bool is_xfb = params.copy_format == EFBCopyFormat::XFB;

  // The EFB clear COLOUR, which this backend implements nowhere. A GC title is
  // free to render its sky as "clear the framebuffer to sky blue and draw only
  // the clouds over it", in which case there is no sky geometry to submit and
  // the background is this constant - so a black background here is not
  // necessarily a dropped draw. Decoded exactly as BPFunctions.cpp:319 does it
  // (ARGB8888 = clearcolorAR << 16 | clearcolorGB), and qualified by the same
  // colour_update bit BPStructs.cpp:380 reads, because a clear that writes no
  // colour says nothing about the background.
  if (clear && g_remix_api->ShouldTraceDraws())
  {
    const u32 argb = (bpmem.clearcolorAR << 16) | bpmem.clearcolorGB;
    INFO_LOG_FMT(VIDEO,
                 "Remix EFB clear: colour [{} {} {}] alpha {} | colour_update {} alpha_update {} "
                 "z_update {} | pixfmt {} | xfb {}",
                 (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF,
                 bpmem.blendmode.color_update ? 1 : 0, bpmem.blendmode.alpha_update ? 1 : 0,
                 bpmem.zmode.update_enable ? 1 : 0,
                 static_cast<u32>(bpmem.zcontrol.pixel_format.Value()),
                 is_xfb ? 1 : 0);
  }

  // Classifies as well as records; see EfbCopyClass. Returns the action, whose
  // fall-through is discard.
  const bool execute =
      g_remix_api->NoteEfbCopy(src_rect, dst_addr, static_cast<u8>(params.copy_format), is_xfb,
                               clear, scale_by_half, params.depth, params.yuv, y_scale);

  // Master knob off: return without touching the staging texture at all, which
  // is the pre-change path in every particular. Not even the zero-fill is owed -
  // with no copy anywhere in the session executing, no pooled buffer can hold a
  // previous encode to smear.
  if (!g_remix_api->EfbEmulationEnabled())
    return;

  if (dst == nullptr)
    return;
  // Every staging texture reaching this backend is one of ours -
  // RemixGfx::CreateStagingTexture is the only source
  // (TextureCacheBase.cpp:2589) - which is what makes this cast safe. It is
  // also precisely why TextureEncoder::Encode cannot be reused as-is: it casts
  // to SW::SWStagingTexture (TextureEncoder.cpp:1487).
  RemixStagingTexture* const staging = static_cast<RemixStagingTexture*>(dst);

  if (!execute)
  {
    // Discard. The zero-fill IS the observable status quo, and it is also the
    // guard against the pooled buffer carrying another copy's image into this
    // destination.
    ZeroStagingRegion(staging, bytes_per_row, num_blocks_y);
    return;
  }

  // --- Execute ---
  const auto encode_start = std::chrono::steady_clock::now();

  // Composite the frame's 2D layer into the EFB first, so the encode picks up
  // the UI the console would have had there rather than bare clear colour. Not
  // for the XFB copy: its encoder reads the same store, but the XFB path is a
  // whole-frame presentation copy that this backend does not present from, and
  // folding for it would pay the fold on the one copy nothing consumes.
  if (!is_xfb)
    g_remix_api->FoldUiIntoEfb();

  // The encoders assume they are writing straight to memory with tightly packed
  // rows at the copy's own stride, while this texture is 2560 texels wide
  // (TextureCacheBase.cpp:2812-2823). Same override TextureEncoder::Encode
  // applies (:1482-1487), and it must be set before the encode because
  // ReadTexels walks the result back out at the same stride.
  if (memory_stride <= staging->GetConfig().width * staging->GetTexelSize())
    staging->SetMapStride(memory_stride);

  // Zeroed first, deliberately. Both encoders have a `default:` arm that writes
  // NOTHING for an unrecognised source pixel format (TextureEncoder.cpp:1450,
  // 1470), and a pooled buffer that keeps its previous contents through such an
  // arm is the staging-pool smear again. Zero-first degrades that case to a
  // discard, and costs nothing otherwise: an encode that runs writes every byte
  // it is responsible for.
  ZeroStagingRegion(staging, bytes_per_row, num_blocks_y);

  u8* const mapped = reinterpret_cast<u8*>(staging->GetMappedPointer());
  if (mapped != nullptr)
  {
    // Mirrors TextureEncoder::Encode's branch (:1489-1498) exactly.
    if (is_xfb)
    {
      EfbInterface::EncodeXFB(mapped, native_width, src_rect, y_scale, gamma);
    }
    else
    {
      TextureEncoder::EncodeEfbCopy(mapped, params, native_width, bytes_per_row, num_blocks_y,
                                    memory_stride, src_rect, scale_by_half);
    }
  }

  g_remix_api->Stats().efb_encode_us +=
      static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - encode_start)
                           .count());
}
}  // namespace Remix
