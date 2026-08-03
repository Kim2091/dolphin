// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

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

  // EFB clears. VideoCommon routes every one of them here
  // (BPStructs.cpp:387 -> BPFunctions::ClearScreen -> FramebufferManager::ClearEFB
  // -> g_gfx->ClearRegion, FramebufferManager.cpp:882), including the clear half
  // of a copy-with-clear. Without an override the AbstractGfx fallback draws a
  // quad through this backend's stub pipelines, i.e. does nothing.
  void ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable, bool alphaEnable,
                   bool zEnable, u32 color, u32 z) override;

private:
  // GFX_REMIX_EFB_EMULATION, read once at construction. Off restores the
  // AbstractGfx fallback, i.e. the pre-change no-op.
  bool m_efb_emulation = true;
};

// Writes one pixel into the shared EFB colour plane, packed for the current
// bpmem.zcontrol.pixel_format exactly the way EfbInterface::SetPixelAlphaColor
// does (SWEfbInterface.cpp:105-145). `store` is the store's own byte order -
// A,B,G,R in memory, i.e. the u32 0xRRGGBBAA.
//
// Exists as a free function rather than living inside the EFB interface because
// two unrelated callers need the identical packing: CPU pokes, and RemixApi's
// fold of the 2D layer into the EFB before an executed copy. Neither may route
// through EfbInterface::SetColor, which applies the CURRENT DRAW's
// blendmode/zmode update masks (SWEfbInterface.cpp:452-472) - state that has
// nothing to do with either of them.
void EfbWriteStoreColor(u16 x, u16 y, u32 store);

// A real CPU-side EFB, sharing the Software backend's store rather than keeping
// a second one: EfbInterface's packed 640x528 colour+depth array
// (SWEfbInterface.cpp:24-38) is what the clear writes, what the copy encoders
// read, and what these peeks and pokes address.
//
// Threading matches the Software backend's accepted model exactly: peeks and
// pokes arrive on the CPU thread through MMU::EFB_Read/EFB_Write
// (MMU.cpp:142-190) while clears and copies run on the GPU thread, and the
// array is touched directly with no locking. The hardware interface marshals
// through AsyncRequests instead (EFBInterface.cpp:38-45); SW::SWEFBInterface
// does not, and it is the accuracy reference.
class RemixEFBInterface final : public EFBInterfaceBase
{
public:
  RemixEFBInterface();

  // Per-frame peek/poke counts, moved into the frame stats line. Exchanged to
  // zero by the reader, so each frame reports its own. Static because the
  // counters are written from the CPU thread by whichever interface instance
  // exists and read from the GPU thread by RemixApi, which has no handle to it.
  static void DrainAccessCounters(u32& peeks, u32& pokes);

private:
  void ReinterpretPixelData(EFBReinterpretType convtype) override;

  void PokeColor(u16 x, u16 y, u32 color) override;
  void PokeDepth(u16 x, u16 y, u32 depth) override;

  u32 PeekColorInternal(u16 x, u16 y) override;
  u32 PeekDepthInternal(u16 x, u16 y) override;

  // GFX_REMIX_EFB_EMULATION, read once at construction. Off makes every method
  // behave exactly as it did before this existed: peeks return 0, pokes do
  // nothing.
  bool m_efb_emulation = true;

  static std::atomic<u32> s_peeks;
  static std::atomic<u32> s_pokes;
};

}  // namespace Remix
