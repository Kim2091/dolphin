// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Remix/RemixTextureCache.h"

#include "VideoBackends/Remix/RemixApi.h"

#include "VideoCommon/BPMemory.h"

namespace Remix
{
void TextureCache::CopyEFB(AbstractStagingTexture* dst, const EFBCopyParams& params,
                           u32 native_width, u32 bytes_per_row, u32 num_blocks_y,
                           u32 memory_stride, const MathUtil::Rectangle<int>& src_rect,
                           bool scale_by_half, bool linear_filter, float y_scale, float gamma,
                           bool clamp_top, bool clamp_bottom,
                           const std::array<u32, 3>& filter_coefficients)
{
  // Still copies nothing - see the class comment. The record is the point.
  //
  // Deliberately NOT gated on RecordEfbCopies() any more. The destination
  // addresses are what the UI draw filter tests against, so turning the
  // diagnostic logging off must not quietly turn the filter off with it;
  // NoteEfbCopy decides for itself which parts of the record are trace-only.
  // This runs a handful of times a frame.
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
                 params.copy_format == EFBCopyFormat::XFB ? 1 : 0);
  }

  g_remix_api->NoteEfbCopy(src_rect, dst_addr, static_cast<u8>(params.copy_format),
                           params.copy_format == EFBCopyFormat::XFB, clear, scale_by_half);
}
}  // namespace Remix
