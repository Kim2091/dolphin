// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D9/D3D9Texture.h"

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "VideoBackends/D3D9/D3D9Device.h"
#include "VideoCommon/TextureConfig.h"

namespace DX9Remix
{
static D3DFORMAT GetD3D9Format(AbstractTextureFormat format)
{
  switch (format)
  {
  case AbstractTextureFormat::RGBA8:
    return D3DFMT_A8R8G8B8;
  case AbstractTextureFormat::DXT1:
    return D3DFMT_DXT1;
  case AbstractTextureFormat::DXT3:
    return D3DFMT_DXT3;
  case AbstractTextureFormat::DXT5:
    return D3DFMT_DXT5;
  case AbstractTextureFormat::D24_S8:
    return D3DFMT_D24S8;
  case AbstractTextureFormat::D32F:
    return D3DFMT_D32F_LOCKABLE;
  default:
    return D3DFMT_A8R8G8B8;
  }
}

// D3D9Texture

D3D9Texture::D3D9Texture(const TextureConfig& config, ComPtr<IDirect3DTexture9> texture)
    : AbstractTexture(config), m_texture(std::move(texture))
{
}

std::unique_ptr<D3D9Texture> D3D9Texture::Create(const TextureConfig& config)
{
  if (!D3D9::device)
    return nullptr;

  const D3DFORMAT format = GetD3D9Format(config.format);
  const bool is_depth = AbstractTexture::IsDepthFormat(config.format);

  DWORD usage = 0;
  D3DPOOL pool = D3DPOOL_MANAGED;

  if (config.IsRenderTarget())
  {
    usage = is_depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET;
    pool = D3DPOOL_DEFAULT;
  }

  ComPtr<IDirect3DTexture9> texture;
  HRESULT hr = D3D9::device->CreateTexture(config.width, config.height, config.levels, usage,
                                           format, pool, &texture, nullptr);
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create D3D9 texture ({}x{}, fmt={}): {:#010x}", config.width,
                  config.height, static_cast<u32>(config.format), static_cast<u32>(hr));
    return nullptr;
  }

  return std::make_unique<D3D9Texture>(config, std::move(texture));
}

void D3D9Texture::CopyRectangleFromTexture(const AbstractTexture* src,
                                           const MathUtil::Rectangle<int>& src_rect,
                                           u32 src_layer, u32 src_level,
                                           const MathUtil::Rectangle<int>& dst_rect,
                                           u32 dst_layer, u32 dst_level)
{
  // For D3D9, we'd need to use StretchRect or manual copy.
  // For now, this is a stub since RTX Remix doesn't need EFB copies.
}

void D3D9Texture::ResolveFromTexture(const AbstractTexture* src,
                                     const MathUtil::Rectangle<int>& rect, u32 layer, u32 level)
{
  // MSAA resolve - not needed for D3D9 fixed-function backend
}

void D3D9Texture::Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer,
                       size_t buffer_size, u32 layer)
{
  if (!m_texture)
    return;

  D3DLOCKED_RECT locked_rect;
  HRESULT hr = m_texture->LockRect(level, &locked_rect, nullptr, 0);
  if (FAILED(hr))
    return;

  const u32 texel_size = AbstractTexture::GetTexelSizeForFormat(m_config.format);
  const u32 src_stride = row_length * texel_size;
  const u32 dst_stride = static_cast<u32>(locked_rect.Pitch);
  const u32 copy_width = width * texel_size;

  const u8* src_ptr = buffer;
  u8* dst_ptr = static_cast<u8*>(locked_rect.pBits);

  if (m_config.format == AbstractTextureFormat::RGBA8)
  {
    // Swizzle RGBA -> BGRA for D3D9's D3DFMT_A8R8G8B8
    for (u32 y = 0; y < height; y++)
    {
      for (u32 x = 0; x < width; x++)
      {
        const u32 src_offset = x * 4;
        const u32 dst_offset = x * 4;
        dst_ptr[dst_offset + 0] = src_ptr[src_offset + 2];  // B <- R
        dst_ptr[dst_offset + 1] = src_ptr[src_offset + 1];  // G <- G
        dst_ptr[dst_offset + 2] = src_ptr[src_offset + 0];  // R <- B
        dst_ptr[dst_offset + 3] = src_ptr[src_offset + 3];  // A <- A
      }
      src_ptr += src_stride;
      dst_ptr += dst_stride;
    }
  }
  else
  {
    // Direct copy for compressed formats and others
    for (u32 y = 0; y < height; y++)
    {
      std::memcpy(dst_ptr, src_ptr, copy_width);
      src_ptr += src_stride;
      dst_ptr += dst_stride;
    }
  }

  m_texture->UnlockRect(level);
}

// D3D9StagingTexture

D3D9StagingTexture::D3D9StagingTexture(StagingTextureType type, const TextureConfig& config)
    : AbstractStagingTexture(type, config)
{
  const u32 texel_size = AbstractTexture::GetTexelSizeForFormat(config.format);
  m_texture_buf.resize(config.width * config.height * texel_size);
  m_map_pointer = reinterpret_cast<char*>(m_texture_buf.data());
  m_map_stride = config.width * texel_size;
}

D3D9StagingTexture::~D3D9StagingTexture() = default;

void D3D9StagingTexture::CopyFromTexture(const AbstractTexture* src,
                                         const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                         u32 src_level,
                                         const MathUtil::Rectangle<int>& dst_rect)
{
  // CPU-side stub - zero fill for now
  std::memset(m_texture_buf.data(), 0, m_texture_buf.size());
}

void D3D9StagingTexture::CopyToTexture(const MathUtil::Rectangle<int>& src_rect,
                                       AbstractTexture* dst,
                                       const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                                       u32 dst_level)
{
  // Stub
}

bool D3D9StagingTexture::Map()
{
  return true;
}

void D3D9StagingTexture::Unmap()
{
}

void D3D9StagingTexture::Flush()
{
}

// D3D9Framebuffer

D3D9Framebuffer::D3D9Framebuffer(AbstractTexture* color_attachment,
                                 AbstractTexture* depth_attachment,
                                 std::vector<AbstractTexture*> additional_color_attachments,
                                 AbstractTextureFormat color_format,
                                 AbstractTextureFormat depth_format, u32 width, u32 height,
                                 u32 layers, u32 samples)
    : AbstractFramebuffer(color_attachment, depth_attachment,
                          std::move(additional_color_attachments), color_format, depth_format, width,
                          height, layers, samples)
{
}

std::unique_ptr<D3D9Framebuffer>
D3D9Framebuffer::Create(D3D9Texture* color_attachment, D3D9Texture* depth_attachment,
                        std::vector<AbstractTexture*> additional_color_attachments)
{
  AbstractTextureFormat color_format = AbstractTextureFormat::Undefined;
  AbstractTextureFormat depth_format = AbstractTextureFormat::Undefined;
  u32 width = 0, height = 0, layers = 1, samples = 1;

  if (color_attachment)
  {
    color_format = color_attachment->GetFormat();
    width = color_attachment->GetWidth();
    height = color_attachment->GetHeight();
    layers = color_attachment->GetLayers();
    samples = color_attachment->GetSamples();
  }
  if (depth_attachment)
  {
    depth_format = depth_attachment->GetFormat();
    if (!color_attachment)
    {
      width = depth_attachment->GetWidth();
      height = depth_attachment->GetHeight();
      layers = depth_attachment->GetLayers();
      samples = depth_attachment->GetSamples();
    }
  }

  return std::make_unique<D3D9Framebuffer>(
      color_attachment, depth_attachment, std::move(additional_color_attachments), color_format,
      depth_format, width, height, layers, samples);
}
}  // namespace DX9Remix
