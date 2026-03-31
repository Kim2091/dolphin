// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d9.h>
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"

namespace DX9Remix
{
using Microsoft::WRL::ComPtr;

class D3D9Texture final : public AbstractTexture
{
public:
  explicit D3D9Texture(const TextureConfig& config, ComPtr<IDirect3DTexture9> texture);

  void CopyRectangleFromTexture(const AbstractTexture* src,
                                const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                u32 dst_layer, u32 dst_level) override;
  void ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                          u32 layer, u32 level) override;
  void Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer, size_t buffer_size,
            u32 layer) override;

  IDirect3DTexture9* GetD3DTexture() const { return m_texture.Get(); }

  static std::unique_ptr<D3D9Texture> Create(const TextureConfig& config);

private:
  ComPtr<IDirect3DTexture9> m_texture;
};

class D3D9StagingTexture final : public AbstractStagingTexture
{
public:
  D3D9StagingTexture(StagingTextureType type, const TextureConfig& config);
  ~D3D9StagingTexture() override;

  void CopyFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& src_rect,
                       u32 src_layer, u32 src_level,
                       const MathUtil::Rectangle<int>& dst_rect) override;
  void CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                     u32 dst_level) override;

  bool Map() override;
  void Unmap() override;
  void Flush() override;

private:
  std::vector<u8> m_texture_buf;
};

class D3D9Framebuffer final : public AbstractFramebuffer
{
public:
  D3D9Framebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                  std::vector<AbstractTexture*> additional_color_attachments,
                  AbstractTextureFormat color_format, AbstractTextureFormat depth_format, u32 width,
                  u32 height, u32 layers, u32 samples);

  static std::unique_ptr<D3D9Framebuffer>
  Create(D3D9Texture* color_attachment, D3D9Texture* depth_attachment,
         std::vector<AbstractTexture*> additional_color_attachments);
};
}  // namespace DX9Remix
