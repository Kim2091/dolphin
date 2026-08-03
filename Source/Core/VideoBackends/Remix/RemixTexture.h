// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"

namespace Remix
{
// A Null-style texture that additionally keeps a CPU copy of its level-0
// pixels. Dolphin's texture cache decodes GC/Wii texture formats on the CPU
// (bSupportsGPUTextureDecoding is false for this backend) and hands the result
// to Load(), so this is the only place the backend ever sees texel data.
class RemixTexture final : public AbstractTexture
{
public:
  explicit RemixTexture(const TextureConfig& config);

  void CopyRectangleFromTexture(const AbstractTexture* src,
                                const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                u32 dst_layer, u32 dst_level) override;
  void ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                          u32 layer, u32 level) override;
  void Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer, size_t buffer_size,
            u32 layer) override;

  // False until level 0 of an RGBA8 texture has been uploaded. Entries backed
  // by an EFB/XFB copy never get there, because this backend's texture cache
  // does not implement EFB copies - that is what makes this flag the draw
  // classifier's "is this a render-to-texture surface?" test.
  bool HasData() const { return m_has_data; }
  u64 GetContentHash() const { return m_content_hash; }
  const std::vector<u8>& GetPixels() const { return m_pixels; }

private:
  std::vector<u8> m_pixels;
  u64 m_content_hash = 0;
  bool m_has_data = false;
};

class RemixStagingTexture final : public AbstractStagingTexture
{
public:
  explicit RemixStagingTexture(StagingTextureType type, const TextureConfig& config);
  ~RemixStagingTexture() override;

  void CopyFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& src_rect,
                       u32 src_layer, u32 src_level,
                       const MathUtil::Rectangle<int>& dst_rect) override;
  void CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                     u32 dst_level) override;

  bool Map() override;
  void Unmap() override;
  void Flush() override;

  // Overrides the row stride the encoders and the subsequent ReadTexels use.
  // Mirrors Software/SWTexture.h:56 and exists for the same reason: the copy
  // encoders write tightly packed rows at the COPY's stride, while this texture
  // is 2560 wide (TextureCacheBase.cpp:2812-2823), and ReadTexels must then walk
  // it back out at that same stride.
  void SetMapStride(size_t stride) { m_map_stride = stride; }

  // Total bytes behind m_map_pointer. The discard path bounds its zero-fill by
  // this rather than trusting a stride the caller supplied.
  size_t GetBufferSize() const { return m_texture_buf.size(); }

private:
  std::vector<u8> m_texture_buf;
};

class RemixFramebuffer final : public AbstractFramebuffer
{
public:
  explicit RemixFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                            std::vector<AbstractTexture*> additional_color_attachments,
                            AbstractTextureFormat color_format, AbstractTextureFormat depth_format,
                            u32 width, u32 height, u32 layers, u32 samples);

  static std::unique_ptr<RemixFramebuffer>
  Create(RemixTexture* color_attachment, RemixTexture* depth_attachment,
         std::vector<AbstractTexture*> additional_color_attachments);
};

}  // namespace Remix
