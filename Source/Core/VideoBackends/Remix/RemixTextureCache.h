// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/TextureCacheBase.h"

namespace Remix
{
// Same shape as the Null backend's texture cache: EFB/XFB copies are not
// executed, so a cache entry produced by one never receives pixels through
// AbstractTexture::Load. RemixTexture::HasData() is therefore false for exactly
// those entries, which is how RemixVertexManager recognizes (and skips) draws
// textured by an EFB copy - see the draw classification in
// RemixVertexManager::DrawCurrentBatch.
class TextureCache final : public TextureCacheBase
{
protected:
  // Copies nothing, as before - but RECORDS that the copy happened. This is the
  // hook VideoCommon actually reaches on this backend: bSupportsCopyToVram is
  // false (VideoConfig.h:162, never set by Remix::VideoBackend::InitBackendInfo),
  // so CopyRenderTargetToTexture takes the copy-to-RAM arm and calls this on
  // every EFB copy the game triggers, XFB copies included
  // (TextureCacheBase.cpp:2388-2400). CopyEFBToCacheEntry below is dead here for
  // the same reason.
  //
  // Defined out of line so the recorder can talk to RemixApi and bpmem without
  // dragging either into every translation unit that constructs a texture cache.
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
