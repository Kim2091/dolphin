// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Remix/RemixTexture.h"

#include <cstring>

#include <xxhash.h>

namespace Remix
{
RemixTexture::RemixTexture(const TextureConfig& tex_config) : AbstractTexture(tex_config)
{
}

void RemixTexture::CopyRectangleFromTexture(const AbstractTexture* src,
                                            const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                            u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                            u32 dst_layer, u32 dst_level)
{
}

void RemixTexture::ResolveFromTexture(const AbstractTexture* src,
                                      const MathUtil::Rectangle<int>& rect, u32 layer, u32 level)
{
}

void RemixTexture::Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer,
                        size_t buffer_size, u32 layer)
{
  // v1 captures only the base level of the base layer, and only uncompressed
  // RGBA8. Compressed formats (custom/hires texture packs) are left without
  // data, which makes their draws classify as "no usable texture" and be
  // skipped - see the RemixVertexManager classifier. Mip chains are out of
  // scope; the uploaded Remix texture declares mipLevels = 1.
  if (level != 0 || layer != 0 || m_config.format != AbstractTextureFormat::RGBA8)
    return;
  if (buffer == nullptr || width == 0 || height == 0)
    return;

  const size_t dst_pitch = static_cast<size_t>(width) * 4;
  const size_t src_pitch = CalculateStrideForFormat(m_config.format, row_length);
  if (src_pitch == 0 || buffer_size < src_pitch * (height - 1) + dst_pitch)
    return;

  // Repack tightly: remixapi_TextureInfo has no pitch field, so the upload has
  // to be width*4 per row.
  m_pixels.resize(dst_pitch * height);
  for (u32 y = 0; y < height; ++y)
    std::memcpy(m_pixels.data() + dst_pitch * y, buffer + src_pitch * y, dst_pitch);

  // Hash of the *decoded* pixels. Dolphin's own texture identity (TextureInfo)
  // hashes the raw GC-format bytes plus TLUT instead; hashing decoded texels is
  // strictly safer for our purposes because two TMEM entries that decode to the
  // same image legitimately share one Remix texture, and the Remix handle is
  // the hash.
  m_content_hash = XXH64(m_pixels.data(), m_pixels.size(), 0);
  // A zero hash is not a usable Remix handle value; nudge it off zero. The odds
  // are astronomical, but keep it deterministic.
  if (m_content_hash == 0)
    m_content_hash = 0xD6E8FEB86659FD93ULL;
  // Cheapest possible answer to "can this texture's alpha mask anything?".
  // A GX format with no alpha - or a palette whose TLUT has none - decodes to
  // alpha 255 everywhere, and any blend or cutout keyed on source alpha is then
  // a no-op. That is invisible from the format alone once the decode has
  // happened, and guessing it wrong cost a playtest round on Wind Waker's eyes.
  m_min_alpha = 255;
  for (size_t i = 3; i < m_pixels.size(); i += 4)
    m_min_alpha = std::min(m_min_alpha, m_pixels[i]);

  m_has_data = true;
}

RemixStagingTexture::RemixStagingTexture(StagingTextureType type, const TextureConfig& config)
    : AbstractStagingTexture(type, config)
{
  m_texture_buf.resize(m_texel_size * config.width * config.height);
  m_map_pointer = reinterpret_cast<char*>(m_texture_buf.data());
  m_map_stride = m_texel_size * config.width;
}

RemixStagingTexture::~RemixStagingTexture() = default;

void RemixStagingTexture::CopyFromTexture(const AbstractTexture* src,
                                          const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                          u32 src_level, const MathUtil::Rectangle<int>& dst_rect)
{
  m_needs_flush = true;
}

void RemixStagingTexture::CopyToTexture(const MathUtil::Rectangle<int>& src_rect,
                                        AbstractTexture* dst,
                                        const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                                        u32 dst_level)
{
  m_needs_flush = true;
}

bool RemixStagingTexture::Map()
{
  return true;
}

void RemixStagingTexture::Unmap()
{
}

void RemixStagingTexture::Flush()
{
  m_needs_flush = false;
}

RemixFramebuffer::RemixFramebuffer(AbstractTexture* color_attachment,
                                   AbstractTexture* depth_attachment,
                                   std::vector<AbstractTexture*> additional_color_attachments,
                                   AbstractTextureFormat color_format,
                                   AbstractTextureFormat depth_format, u32 width, u32 height,
                                   u32 layers, u32 samples)
    : AbstractFramebuffer(color_attachment, depth_attachment,
                          std::move(additional_color_attachments), color_format, depth_format,
                          width, height, layers, samples)
{
}

std::unique_ptr<RemixFramebuffer>
RemixFramebuffer::Create(RemixTexture* color_attachment, RemixTexture* depth_attachment,
                         std::vector<AbstractTexture*> additional_color_attachments)
{
  if (!ValidateConfig(color_attachment, depth_attachment, additional_color_attachments))
    return nullptr;

  const AbstractTextureFormat color_format =
      color_attachment ? color_attachment->GetFormat() : AbstractTextureFormat::Undefined;
  const AbstractTextureFormat depth_format =
      depth_attachment ? depth_attachment->GetFormat() : AbstractTextureFormat::Undefined;
  const RemixTexture* either_attachment = color_attachment ? color_attachment : depth_attachment;
  const u32 width = either_attachment->GetWidth();
  const u32 height = either_attachment->GetHeight();
  const u32 layers = either_attachment->GetLayers();
  const u32 samples = either_attachment->GetSamples();

  return std::make_unique<RemixFramebuffer>(color_attachment, depth_attachment,
                                            std::move(additional_color_attachments), color_format,
                                            depth_format, width, height, layers, samples);
}

}  // namespace Remix
