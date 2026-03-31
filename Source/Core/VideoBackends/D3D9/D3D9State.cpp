// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D9/D3D9State.h"

#include "VideoBackends/D3D9/D3D9Device.h"
#include "VideoCommon/BPMemory.h"

namespace DX9Remix
{
namespace StateManager
{
static D3DCULL MapCullMode(CullMode mode)
{
  switch (mode)
  {
  case CullMode::None:
    return D3DCULL_NONE;
  case CullMode::Front:
    return D3DCULL_CW;
  case CullMode::Back:
    return D3DCULL_CCW;
  case CullMode::All:
    return D3DCULL_NONE;  // D3D9 doesn't have "cull all", handle via disabling draw
  default:
    return D3DCULL_NONE;
  }
}

static D3DCMPFUNC MapCompareMode(CompareMode mode)
{
  switch (mode)
  {
  case CompareMode::Never:
    return D3DCMP_NEVER;
  case CompareMode::Less:
    return D3DCMP_LESS;
  case CompareMode::Equal:
    return D3DCMP_EQUAL;
  case CompareMode::LEqual:
    return D3DCMP_LESSEQUAL;
  case CompareMode::Greater:
    return D3DCMP_GREATER;
  case CompareMode::NEqual:
    return D3DCMP_NOTEQUAL;
  case CompareMode::GEqual:
    return D3DCMP_GREATEREQUAL;
  case CompareMode::Always:
    return D3DCMP_ALWAYS;
  default:
    return D3DCMP_ALWAYS;
  }
}

static D3DBLEND MapSrcBlendFactor(SrcBlendFactor factor)
{
  switch (factor)
  {
  case SrcBlendFactor::Zero:
    return D3DBLEND_ZERO;
  case SrcBlendFactor::One:
    return D3DBLEND_ONE;
  case SrcBlendFactor::DstClr:
    return D3DBLEND_DESTCOLOR;
  case SrcBlendFactor::InvDstClr:
    return D3DBLEND_INVDESTCOLOR;
  case SrcBlendFactor::SrcAlpha:
    return D3DBLEND_SRCALPHA;
  case SrcBlendFactor::InvSrcAlpha:
    return D3DBLEND_INVSRCALPHA;
  case SrcBlendFactor::DstAlpha:
    return D3DBLEND_DESTALPHA;
  case SrcBlendFactor::InvDstAlpha:
    return D3DBLEND_INVDESTALPHA;
  default:
    return D3DBLEND_ONE;
  }
}

static D3DBLEND MapDstBlendFactor(DstBlendFactor factor)
{
  switch (factor)
  {
  case DstBlendFactor::Zero:
    return D3DBLEND_ZERO;
  case DstBlendFactor::One:
    return D3DBLEND_ONE;
  case DstBlendFactor::SrcClr:
    return D3DBLEND_SRCCOLOR;
  case DstBlendFactor::InvSrcClr:
    return D3DBLEND_INVSRCCOLOR;
  case DstBlendFactor::SrcAlpha:
    return D3DBLEND_SRCALPHA;
  case DstBlendFactor::InvSrcAlpha:
    return D3DBLEND_INVSRCALPHA;
  case DstBlendFactor::DstAlpha:
    return D3DBLEND_DESTALPHA;
  case DstBlendFactor::InvDstAlpha:
    return D3DBLEND_INVDESTALPHA;
  default:
    return D3DBLEND_ZERO;
  }
}

void ApplyRasterizationState(const RasterizationState& state)
{
  if (!D3D9::device)
    return;

  D3D9::device->SetRenderState(D3DRS_CULLMODE, MapCullMode(state.cull_mode));
}

void ApplyDepthState(const DepthState& state)
{
  if (!D3D9::device)
    return;

  D3D9::device->SetRenderState(D3DRS_ZENABLE, state.test_enable ? D3DZB_TRUE : D3DZB_FALSE);
  D3D9::device->SetRenderState(D3DRS_ZWRITEENABLE, state.update_enable.Value());
  D3D9::device->SetRenderState(D3DRS_ZFUNC, MapCompareMode(state.func));
}

void ApplyBlendingState(const BlendingState& state)
{
  if (!D3D9::device)
    return;

  // If logic op is enabled, approximate it with blending
  BlendingState effective_state = state;
  if (state.logic_op_enable && !state.blend_enable)
  {
    effective_state.ApproximateLogicOpWithBlending();
  }

  D3D9::device->SetRenderState(D3DRS_ALPHABLENDENABLE, effective_state.blend_enable.Value());

  if (effective_state.blend_enable)
  {
    D3D9::device->SetRenderState(D3DRS_SRCBLEND, MapSrcBlendFactor(effective_state.src_factor));
    D3D9::device->SetRenderState(D3DRS_DESTBLEND, MapDstBlendFactor(effective_state.dst_factor));
    D3D9::device->SetRenderState(D3DRS_BLENDOP,
                                 effective_state.subtract ? D3DBLENDOP_SUBTRACT : D3DBLENDOP_ADD);

    // Separate alpha blending
    D3D9::device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    D3D9::device->SetRenderState(D3DRS_SRCBLENDALPHA,
                                 MapSrcBlendFactor(effective_state.src_factor_alpha));
    D3D9::device->SetRenderState(D3DRS_DESTBLENDALPHA,
                                 MapDstBlendFactor(effective_state.dst_factor_alpha));
    D3D9::device->SetRenderState(
        D3DRS_BLENDOPALPHA, effective_state.subtract_alpha ? D3DBLENDOP_SUBTRACT : D3DBLENDOP_ADD);
  }

  // Color/alpha write masks
  DWORD write_mask = 0;
  if (effective_state.color_update)
    write_mask |= D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;
  if (effective_state.alpha_update)
    write_mask |= D3DCOLORWRITEENABLE_ALPHA;
  D3D9::device->SetRenderState(D3DRS_COLORWRITEENABLE, write_mask);
}

void ApplySamplerState(u32 index, const SamplerState& state)
{
  if (!D3D9::device || index >= 8)
    return;

  // Min/Mag/Mip filters
  D3D9::device->SetSamplerState(index, D3DSAMP_MINFILTER,
                                state.tm0.min_filter == FilterMode::Linear ? D3DTEXF_LINEAR
                                                                          : D3DTEXF_POINT);
  D3D9::device->SetSamplerState(index, D3DSAMP_MAGFILTER,
                                state.tm0.mag_filter == FilterMode::Linear ? D3DTEXF_LINEAR
                                                                          : D3DTEXF_POINT);
  D3D9::device->SetSamplerState(index, D3DSAMP_MIPFILTER,
                                state.tm0.mipmap_filter == FilterMode::Linear ? D3DTEXF_LINEAR
                                                                             : D3DTEXF_POINT);

  // Wrap modes
  auto MapWrapMode = [](WrapMode mode) -> D3DTEXTUREADDRESS {
    switch (mode)
    {
    case WrapMode::Clamp:
      return D3DTADDRESS_CLAMP;
    case WrapMode::Repeat:
      return D3DTADDRESS_WRAP;
    case WrapMode::Mirror:
      return D3DTADDRESS_MIRROR;
    default:
      return D3DTADDRESS_WRAP;
    }
  };

  D3D9::device->SetSamplerState(index, D3DSAMP_ADDRESSU, MapWrapMode(state.tm0.wrap_u));
  D3D9::device->SetSamplerState(index, D3DSAMP_ADDRESSV, MapWrapMode(state.tm0.wrap_v));
}
}  // namespace StateManager
}  // namespace DX9Remix
