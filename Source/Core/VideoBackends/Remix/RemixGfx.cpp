// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Remix/RemixGfx.h"

#include "VideoBackends/Remix/RemixApi.h"
#include "VideoBackends/Remix/RemixTexture.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VideoConfig.h"

namespace Remix
{
RemixGfx::RemixGfx()
{
  UpdateActiveConfig();
}

RemixGfx::~RemixGfx()
{
  UpdateActiveConfig();
}

bool RemixGfx::IsHeadless() const
{
  return true;
}

bool RemixGfx::SupportsUtilityDrawing() const
{
  return false;
}

void RemixGfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  if (g_remix_api)
    g_remix_api->SetBoundTexture(index, static_cast<const RemixTexture*>(texture));
}

void RemixGfx::UnbindTexture(const AbstractTexture* texture)
{
  if (g_remix_api)
    g_remix_api->UnbindTexture(static_cast<const RemixTexture*>(texture));
}

std::unique_ptr<AbstractTexture> RemixGfx::CreateTexture(const TextureConfig& config,
                                                         std::string_view /* name */)
{
  return std::make_unique<RemixTexture>(config);
}

std::unique_ptr<AbstractStagingTexture> RemixGfx::CreateStagingTexture(StagingTextureType type,
                                                                       const TextureConfig& config)
{
  return std::make_unique<RemixStagingTexture>(type, config);
}

class RemixShader final : public AbstractShader
{
public:
  explicit RemixShader(ShaderStage stage) : AbstractShader(stage) {}
};

std::unique_ptr<AbstractShader>
RemixGfx::CreateShaderFromSource(ShaderStage stage, std::string_view /* source */,
                                 VideoCommon::ShaderIncluder* /* shader_includer */,
                                 std::string_view /* name */)
{
  return std::make_unique<RemixShader>(stage);
}

std::unique_ptr<AbstractShader> RemixGfx::CreateShaderFromBinary(ShaderStage stage,
                                                                 const void* /* data */,
                                                                 size_t /* length */,
                                                                 std::string_view /* name */)
{
  return std::make_unique<RemixShader>(stage);
}

class RemixPipeline final : public AbstractPipeline
{
};

std::unique_ptr<AbstractPipeline> RemixGfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                           const void* cache_data,
                                                           size_t cache_data_length)
{
  return std::make_unique<RemixPipeline>();
}

std::unique_ptr<AbstractFramebuffer>
RemixGfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                            std::vector<AbstractTexture*> additional_color_attachments)
{
  return RemixFramebuffer::Create(static_cast<RemixTexture*>(color_attachment),
                                  static_cast<RemixTexture*>(depth_attachment),
                                  std::move(additional_color_attachments));
}

std::unique_ptr<NativeVertexFormat>
RemixGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<NativeVertexFormat>(vtx_decl);
}

void RemixEFBInterface::ReinterpretPixelData(EFBReinterpretType convtype)
{
}

void RemixEFBInterface::PokeColor(u16 x, u16 y, u32 color)
{
}

void RemixEFBInterface::PokeDepth(u16 x, u16 y, u32 depth)
{
}

u32 RemixEFBInterface::PeekColorInternal(u16 x, u16 y)
{
  return 0;
}

u32 RemixEFBInterface::PeekDepthInternal(u16 x, u16 y)
{
  return 0;
}

}  // namespace Remix
