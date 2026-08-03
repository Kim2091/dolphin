// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "VideoCommon/TextureCacheBase.h"

namespace TextureEncoder
{
void Encode(AbstractStagingTexture* dst, const EFBCopyParams& params, u32 native_width,
            u32 bytes_per_row, u32 num_blocks_y, u32 memory_stride,
            const MathUtil::Rectangle<int>& src_rect, bool scale_by_half, float y_scale,
            float gamma);

// The non-XFB half of Encode, without its SW::SWStagingTexture cast. Exported so
// the Remix backend can reach the encoders with its own staging texture type;
// Encode itself cannot be reused because of that cast (TextureEncoder.cpp:1487).
// Reads the shared EfbInterface store, so the caller is responsible for that
// store holding what it wants encoded.
void EncodeEfbCopy(u8* dst, const EFBCopyParams& params, u32 native_width, u32 bytes_per_row,
                   u32 num_blocks_y, u32 memory_stride, const MathUtil::Rectangle<int>& src_rect,
                   bool scale_by_half);
}
