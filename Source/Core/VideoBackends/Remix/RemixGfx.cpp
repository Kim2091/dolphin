// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Remix/RemixGfx.h"

#include <cstring>

#include "VideoBackends/Remix/RemixApi.h"
#include "VideoBackends/Remix/RemixTexture.h"

// The Software backend's EFB core, linked in rather than duplicated (see
// CMakeLists.txt). EfbCopy::ClearEfb is the clear; EfbInterface owns the store.
#include "VideoBackends/Software/EfbCopy.h"
#include "VideoBackends/Software/SWEfbInterface.h"

#include "Core/Config/GraphicsSettings.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace Remix
{
RemixGfx::RemixGfx()
{
  // Read once here rather than per clear: ClearRegion is on the GPU thread's
  // per-frame path and Config::Get is not free at that rate (RemixApi.h:808-810).
  m_efb_emulation = Config::Get(Config::GFX_REMIX_EFB_EMULATION);
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

void RemixGfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable,
                           bool alphaEnable, bool zEnable, u32 color, u32 z)
{
  if (!m_efb_emulation)
  {
    // Exactly the pre-change path: the base implementation draws a quad through
    // this backend's stub pipelines, which produces nothing.
    AbstractGfx::ClearRegion(target_rc, colorEnable, alphaEnable, zEnable, color, z);
    return;
  }

  // Every argument is deliberately ignored, following SWGfx::ClearRegion
  // (SWGfx.cpp:117-121) term for term. ClearEfb re-derives the rect from
  // bpmem.copyTexSrcXY/WH and the colour/Z from bpmem.clearcolorAR/GB/clearZValue
  // itself (EfbCopy.cpp:16-34), and it writes through EfbInterface::SetColor /
  // SetDepth, which apply the same colour_update / alpha_update / zmode masks
  // BPStructs.cpp:380 qualifies the clear by. `target_rc` is the SCALED rect,
  // which means nothing to a 640x528 CPU store.
  //
  // This also covers the clear half of a copy-with-clear, and it does so
  // independently of what the copy itself did: the clear arrives through
  // BPFunctions::ClearScreen, never through TextureCache::CopyEFB, so a copy
  // this backend DISCARDS still gets its clear - which is the console ordering
  // (copy first, then clear: BPStructs.cpp:309-393).
  EfbCopy::ClearEfb();
}

std::atomic<u32> RemixEFBInterface::s_peeks{0};
std::atomic<u32> RemixEFBInterface::s_pokes{0};

RemixEFBInterface::RemixEFBInterface()
{
  m_efb_emulation = Config::Get(Config::GFX_REMIX_EFB_EMULATION);
  if (!m_efb_emulation)
    return;

  // The store is a file-scope array in the Software backend, shared by whichever
  // backend is up. Switching SW -> Remix inside one session would otherwise leak
  // SW's last frame into this backend's first peeks, so start from a known state.
  // Layout is colour plane then depth plane, three bytes per pixel each
  // (SWEfbInterface.cpp:24-38), hence the 6.
  //
  // That layout is adopted UNCHANGED on purpose, and it is load-bearing well
  // past this memset: TextureEncoder's box filters walk rows by a hard-coded
  // 640-pixel stride (TextureEncoder.cpp:63-67, and every Boxfilter* beside it).
  // Reusing this exact store is what makes those encoders correct here; any
  // future change to the store's shape breaks all of them silently.
  std::memset(EfbInterface::GetPixelPointer(0, 0, false), 0,
              static_cast<size_t>(EFB_WIDTH) * EFB_HEIGHT * 6);
}

void RemixEFBInterface::DrainAccessCounters(u32& peeks, u32& pokes)
{
  peeks = s_peeks.exchange(0, std::memory_order_relaxed);
  pokes = s_pokes.exchange(0, std::memory_order_relaxed);
}

void RemixEFBInterface::ReinterpretPixelData(EFBReinterpretType convtype)
{
  // No-op, matching SW::SWEFBInterface (SWEfbInterface.cpp:740-742). The
  // conversion rewrites every pixel between EFB pixel formats; the accuracy
  // reference declines to and nothing in this backend needs it more than SW does.
}

// Declared in RemixGfx.h; see there for why this is not EfbInterface::SetColor.
void EfbWriteStoreColor(u16 x, u16 y, u32 store)
{
  u8* const pixel = EfbInterface::GetPixelPointer(x, y, false);
  u32 dst = 0;
  std::memcpy(&dst, pixel, sizeof(u32));

  switch (bpmem.zcontrol.pixel_format)
  {
  case PixelFormat::RGB8_Z24:
  case PixelFormat::Z24:
  // RGB565_Z16 is not stored correctly by the reference either; it takes the
  // same arm there and does here.
  case PixelFormat::RGB565_Z16:
    dst = (dst & 0xff000000) | (store >> 8);
    break;
  case PixelFormat::RGBA6_Z24:
    dst &= 0xff000000;
    dst |= (store >> 2) & 0x0000003f;  // alpha
    dst |= (store >> 4) & 0x00000fc0;  // blue
    dst |= (store >> 6) & 0x0003f000;  // green
    dst |= (store >> 8) & 0x00fc0000;  // red
    break;
  default:
    // Unknown format: leave the pixel alone rather than corrupt it. The
    // reference logs an error here; a poke is not worth a per-access log line.
    return;
  }

  std::memcpy(pixel, &dst, sizeof(u32));
}

void RemixEFBInterface::PokeColor(u16 x, u16 y, u32 color)
{
  if (!m_efb_emulation)
    return;
  // Not applied for us, unlike PeekColor/PeekDepth: the base class wraps the
  // peeks and leaves the pokes to the implementation, which is what
  // HardwareEFBInterface::PokeColor does too (EFBInterface.cpp:130-133). It is
  // also the bounds check - MMU hands x and y up to 1023 (MMU.cpp:169-171).
  if (ShouldSkipAccess(x, y))
    return;

  // MMU delivers the CPU's word as ARGB (0xAARRGGBB); the store wants A,B,G,R in
  // memory, i.e. 0xRRGGBBAA. That is a rotate, not the hardware interface's
  // BGRA->RGBA swap, because the destination byte order differs.
  const u32 store = (color << 8) | (color >> 24);
  EfbWriteStoreColor(x, y, store);
  s_pokes.fetch_add(1, std::memory_order_relaxed);
}

void RemixEFBInterface::PokeDepth(u16 x, u16 y, u32 depth)
{
  if (!m_efb_emulation)
    return;
  if (ShouldSkipAccess(x, y))
    return;

  // Mirrors EfbInterface::SetPixelDepth (SWEfbInterface.cpp:174-201): 24 bits of
  // depth into the low three bytes, top byte preserved. Not routed through
  // EfbInterface::SetDepth, which would drop the write whenever the last draw
  // left zmode.update_enable clear.
  u8* const pixel = EfbInterface::GetPixelPointer(x, y, true);
  u32 dst = 0;
  std::memcpy(&dst, pixel, sizeof(u32));
  dst = (dst & 0xff000000) | (depth & 0x00ffffff);
  std::memcpy(pixel, &dst, sizeof(u32));
  s_pokes.fetch_add(1, std::memory_order_relaxed);
}

u32 RemixEFBInterface::PeekColorInternal(u16 x, u16 y)
{
  if (!m_efb_emulation)
    return 0;

  s_peeks.fetch_add(1, std::memory_order_relaxed);

  // Same shuffle SW::SWEFBInterface does (SWEfbInterface.cpp:752-760): GetColor
  // returns 0xRRGGBBAA and the caller wants ARGB. The PE alpha-read mode
  // (GX_PokeAlphaRead) is applied by the base class on top of this
  // (EFBInterface.cpp:62-89), so it must not be applied here as well.
  const u32 color = EfbInterface::GetColor(x, y);
  return (color >> 8) | ((color & 0xff) << 24);
}

u32 RemixEFBInterface::PeekDepthInternal(u16 x, u16 y)
{
  if (!m_efb_emulation)
    return 0;

  s_peeks.fetch_add(1, std::memory_order_relaxed);
  return EfbInterface::GetDepth(x, y);
}

}  // namespace Remix
