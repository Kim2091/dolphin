// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D9/D3D9Gfx.h"

#include <cstdio>

#include "VideoBackends/D3D9/D3D9Device.h"
#include "VideoBackends/D3D9/D3D9VertexManager.h"
#include "VideoBackends/D3D9/D3D9NativeVertexFormat.h"
#include "VideoBackends/D3D9/D3D9Pipeline.h"
#include "VideoBackends/D3D9/D3D9Shader.h"
#include "VideoBackends/D3D9/D3D9State.h"
#include "VideoBackends/D3D9/D3D9Texture.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VideoConfig.h"

namespace DX9Remix
{
Gfx::Gfx(float backbuffer_scale) : m_backbuffer_scale(backbuffer_scale)
{
  UpdateActiveConfig();

  // Set initial D3D9 render states for fixed-function
  if (D3D9::device)
  {
    D3D9::device->SetRenderState(D3DRS_LIGHTING, FALSE);
    D3D9::device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    D3D9::device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    D3D9::device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);

    // Set up fixed-function vertex processing
    D3D9::device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    D3D9::device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    D3D9::device->SetRenderState(D3DRS_FOGENABLE, FALSE);

    // Default texture stage: modulate texture with vertex color
    D3D9::device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    D3D9::device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    D3D9::device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    D3D9::device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    D3D9::device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    D3D9::device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    // Disable remaining stages
    for (int i = 1; i < 8; i++)
    {
      D3D9::device->SetTextureStageState(i, D3DTSS_COLOROP, D3DTOP_DISABLE);
      D3D9::device->SetTextureStageState(i, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }

    // Start the first scene immediately so game draws (which happen BEFORE
    // BindBackbuffer in Dolphin's frame lifecycle) are inside a valid
    // BeginScene/EndScene pair.  D3D9 silently rejects draws outside a scene.
    D3D9::device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                        D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    D3D9::device->BeginScene();
  }
}

Gfx::~Gfx()
{
  // End the scene we started in the constructor (or after the last Present)
  if (D3D9::device)
    D3D9::device->EndScene();
  UpdateActiveConfig();
}

bool Gfx::IsHeadless() const
{
  return false;
}

bool Gfx::SupportsUtilityDrawing() const
{
  return false;
}

std::unique_ptr<AbstractTexture> Gfx::CreateTexture(const TextureConfig& config,
                                                    [[maybe_unused]] std::string_view name)
{
  return D3D9Texture::Create(config);
}

std::unique_ptr<AbstractStagingTexture> Gfx::CreateStagingTexture(StagingTextureType type,
                                                                   const TextureConfig& config)
{
  return std::make_unique<D3D9StagingTexture>(type, config);
}

std::unique_ptr<AbstractFramebuffer>
Gfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                       std::vector<AbstractTexture*> additional_color_attachments)
{
  return D3D9Framebuffer::Create(static_cast<D3D9Texture*>(color_attachment),
                                 static_cast<D3D9Texture*>(depth_attachment),
                                 std::move(additional_color_attachments));
}

std::unique_ptr<AbstractShader>
Gfx::CreateShaderFromSource(ShaderStage stage, [[maybe_unused]] std::string_view source,
                            [[maybe_unused]] VideoCommon::ShaderIncluder* shader_includer,
                            [[maybe_unused]] std::string_view name)
{
  // Return a stub shader - D3D9 fixed-function doesn't use shaders.
  // The shader cache needs these to be non-null.
  return std::make_unique<D3D9Shader>(stage);
}

std::unique_ptr<AbstractShader>
Gfx::CreateShaderFromBinary(ShaderStage stage, [[maybe_unused]] const void* data,
                            [[maybe_unused]] size_t length,
                            [[maybe_unused]] std::string_view name)
{
  return std::make_unique<D3D9Shader>(stage);
}

std::unique_ptr<NativeVertexFormat>
Gfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return D3D9VertexFormat::Create(vtx_decl);
}

std::unique_ptr<AbstractPipeline> Gfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                      const void* cache_data,
                                                      size_t cache_data_length)
{
  // Return a stub pipeline that stores the render state config.
  // Shader references are ignored - we use fixed-function.
  return std::make_unique<D3D9Pipeline>(config);
}

// Framebuffer overrides: intentionally keep D3D9 render target as the backbuffer.
// Dolphin's renderer switches to EFB framebuffers for game draws, but for RTX Remix
// we need all draws to go through the D3D9 backbuffer so Remix can capture them.
void Gfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  m_current_framebuffer = framebuffer;
}

void Gfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  m_current_framebuffer = framebuffer;
}

void Gfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                 float depth_value)
{
  m_current_framebuffer = framebuffer;
}

void Gfx::SetPipeline(const AbstractPipeline* pipeline)
{
  m_current_pipeline = pipeline;
  if (!pipeline || !D3D9::device)
    return;

  const auto& config = static_cast<const D3D9Pipeline*>(pipeline)->m_config;
  StateManager::ApplyRasterizationState(config.rasterization_state);
  StateManager::ApplyDepthState(config.depth_state);
  StateManager::ApplyBlendingState(config.blending_state);
}

void Gfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  if (!D3D9::device)
    return;

  if (texture)
  {
    const auto* d3d9_tex = static_cast<const D3D9Texture*>(texture);
    D3D9::device->SetTexture(index, d3d9_tex->GetD3DTexture());
  }
  else
  {
    D3D9::device->SetTexture(index, nullptr);
  }
}

void Gfx::SetSamplerState(u32 index, const SamplerState& state)
{
  if (!D3D9::device)
    return;

  StateManager::ApplySamplerState(index, state);
}

void Gfx::SetViewport(float x, float y, float width, float height, float near_depth,
                      float far_depth)
{
  if (!D3D9::device)
    return;

  D3DVIEWPORT9 vp;
  vp.X = static_cast<DWORD>(x);
  vp.Y = static_cast<DWORD>(y);
  vp.Width = static_cast<DWORD>(width);
  vp.Height = static_cast<DWORD>(height);
  vp.MinZ = near_depth;
  vp.MaxZ = far_depth;
  D3D9::device->SetViewport(&vp);
}

void Gfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  if (!D3D9::device)
    return;

  RECT rect;
  rect.left = rc.left;
  rect.top = rc.top;
  rect.right = rc.right;
  rect.bottom = rc.bottom;
  D3D9::device->SetScissorRect(&rect);
  D3D9::device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
}

void Gfx::Draw(u32 base_vertex, u32 num_vertices)
{
  if (D3D9::device)
    D3D9::device->DrawPrimitive(D3DPT_TRIANGLELIST, base_vertex, num_vertices / 3);
}

void Gfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  // This is called from DrawCurrentBatch via the base VertexManager.
  // The actual D3D9 DrawIndexedPrimitive call happens in D3D9VertexManager::DrawCurrentBatch.
}

bool Gfx::BindBackbuffer(const ClearColor& clear_color)
{
  // In Dolphin's frame lifecycle, game draws happen BEFORE BindBackbuffer.
  // The scene is already active (started in constructor / after last Present).
  // Don't Clear here — that would wipe the game draws we just rendered.
  // Don't BeginScene here — we're already in a scene.
  return D3D9::device != nullptr;
}

void Gfx::PresentBackbuffer()
{
  if (!D3D9::device)
    return;

  // Debug: write frame stats to file
  {
    static u32 s_frame_count = 0;
    s_frame_count++;
    if (s_frame_count <= 10)
    {
      FILE* f = fopen("d3d9_debug.txt", "a");
      if (f)
      {
        fprintf(f, "Frame %u: draws=%u, uploads=%u, indices=%u\n", s_frame_count,
                VertexManager::s_draw_calls_this_frame, VertexManager::s_upload_calls_this_frame,
                VertexManager::s_total_indices_this_frame);
        fclose(f);
      }
    }

    // Reset counters for next frame
    VertexManager::s_draw_calls_this_frame = 0;
    VertexManager::s_upload_calls_this_frame = 0;
    VertexManager::s_total_indices_this_frame = 0;
  }

  // End the current frame's scene and present
  D3D9::device->EndScene();
  D3D9::device->Present(nullptr, nullptr, nullptr, nullptr);

  // Immediately start the next frame: clear and begin a new scene so that
  // game draws (which happen before the next BindBackbuffer) are valid.
  D3D9::device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                      D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
  D3D9::device->BeginScene();
}

void Gfx::ShowImage(const AbstractTexture* /*source_texture*/,
                    const MathUtil::Rectangle<int>& /*source_rc*/)
{
  // Called by the Presenter when SupportsUtilityDrawing() is false.
  // Game draws are already on the D3D9 backbuffer — just present them.
  PresentBackbuffer();
}

void Gfx::Flush()
{
  // D3D9 has no explicit flush mechanism
}

void Gfx::WaitForGPUIdle()
{
  // D3D9 has no explicit GPU idle wait
}

SurfaceInfo Gfx::GetSurfaceInfo() const
{
  SurfaceInfo info;
  info.width = 640;
  info.height = 528;
  info.scale = m_backbuffer_scale;
  info.format = AbstractTextureFormat::RGBA8;
  return info;
}

// D3D9 EFB Interface stubs

void D3D9EFBInterface::ReinterpretPixelData(EFBReinterpretType convtype)
{
}

void D3D9EFBInterface::PokeColor(u16 x, u16 y, u32 color)
{
}

void D3D9EFBInterface::PokeDepth(u16 x, u16 y, u32 depth)
{
}

u32 D3D9EFBInterface::PeekColorInternal(u16 x, u16 y)
{
  return 0;
}

u32 D3D9EFBInterface::PeekDepthInternal(u16 x, u16 y)
{
  return 0;
}
}  // namespace DX9Remix
