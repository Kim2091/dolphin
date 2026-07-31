// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/EFBInterface.h"

namespace Remix
{
// Headless like the Null backend: Dolphin never presents anything through this
// class. The path-traced image is produced and presented by the Remix runtime
// (see RemixApi), which owns Dolphin's render surface HWND.
class RemixGfx final : public AbstractGfx
{
public:
  RemixGfx();
  ~RemixGfx() override;

  bool IsHeadless() const override;
  bool SupportsUtilityDrawing() const override;

  // Only override point that observes per-stage texture binding; fed by
  // TextureCacheBase::BindTextures right before every GX draw is flushed.
  void SetTexture(u32 index, const AbstractTexture* texture) override;
  void UnbindTexture(const AbstractTexture* texture) override;

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view name) override;
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) override;
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments) override;

  std::unique_ptr<AbstractShader>
  CreateShaderFromSource(ShaderStage stage, std::string_view source,
                         VideoCommon::ShaderIncluder* shader_includer,
                         std::string_view name) override;
  std::unique_ptr<AbstractShader> CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length,
                                                         std::string_view name) override;
  std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl) override;
  std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0) override;
  SurfaceInfo GetSurfaceInfo() const override { return {}; }
};

class RemixEFBInterface final : public EFBInterfaceBase
{
  void ReinterpretPixelData(EFBReinterpretType convtype) override;

  void PokeColor(u16 x, u16 y, u32 color) override;
  void PokeDepth(u16 x, u16 y, u32 depth) override;

  u32 PeekColorInternal(u16 x, u16 y) override;
  u32 PeekDepthInternal(u16 x, u16 y) override;
};

}  // namespace Remix
