// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/TextureCacheBase.h"

namespace Remix
{
// EFB copies are CLASSIFIED here, and then either executed against the CPU-side
// EFB or deliberately discarded.
//
// Discard is the status quo and the safe default. Before EFB emulation existed
// this class wrote nothing at all, so every copy destination received the
// staging buffer's zeroes - and for a whole class of copies that is the
// behaviour worth keeping: a game's baked shadow maps, mirrored-camera water
// reflections and bloom chains are screen-space fakes of things the path tracer
// does natively, and discarding them is what lets the traced result show. The
// classifier (RemixApi::NoteEfbCopy, EfbCopyClass) turns that accident into a
// per-copy decision whose fall-through arm is still discard, so an unrecognised
// copy cannot look worse than it did before.
//
// The one class that executes by default is a colour copy taken before any
// perspective draw of the frame: with no world geometry drawn yet, the console's
// EFB held clear colour plus 2D draws plus pokes, which is exactly what the CPU
// EFB holds, so the encode is correct by construction rather than by luck.
//
// Consequences downstream. A discarded copy's destination is zero-filled, so a
// texture decoded over it decodes zeroes - RemixTexture::HasData() is TRUE for
// such an entry (decoding zero bytes succeeds), which is why
// RemixVertexManager's skipped_efb_texture guard does not catch it and why the
// UI path filters on the copy DESTINATION ADDRESS instead. An executed copy's
// destination holds real bytes and decodes real content on the next bind; the
// copy invalidates overlapping cache entries for us (TextureCacheBase.cpp:2431).
class TextureCache final : public TextureCacheBase
{
protected:
  // The hook VideoCommon actually reaches on this backend: bSupportsCopyToVram
  // is false (VideoConfig.h:162, never set by Remix::VideoBackend::
  // InitBackendInfo), so CopyRenderTargetToTexture forces the copy-to-RAM arm
  // (TextureCacheBase.cpp:2193-2197) and calls this on every EFB copy the game
  // triggers, XFB copies included (:2388-2400). There is no separate EFB2Tex
  // path to implement, and CopyEFBToCacheEntry below stays dead for the same
  // reason.
  //
  // Defined out of line so the classifier and encoders can talk to RemixApi,
  // bpmem and the Software backend's EFB core without dragging any of them into
  // every translation unit that constructs a texture cache.
  void CopyEFB(AbstractStagingTexture* dst, const EFBCopyParams& params, u32 native_width,
               u32 bytes_per_row, u32 num_blocks_y, u32 memory_stride,
               const MathUtil::Rectangle<int>& src_rect, bool scale_by_half, bool linear_filter,
               float y_scale, float gamma, bool clamp_top, bool clamp_bottom,
               const std::array<u32, 3>& filter_coefficients) override;

  void CopyEFBToCacheEntry(RcTcacheEntry& entry, bool is_depth_copy,
                           const MathUtil::Rectangle<int>& src_rect, bool scale_by_half,
                           bool linear_filter, EFBCopyFormat dst_format, bool is_intensity,
                           float gamma, bool clamp_top, bool clamp_bottom,
                           const std::array<u32, 3>& filter_coefficients) override
  {
  }
};

}  // namespace Remix
