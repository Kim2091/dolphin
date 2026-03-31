// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D9/D3D9TEVMapper.h"

#include "Common/Logging/Log.h"
#include "VideoBackends/D3D9/D3D9Device.h"
#include "VideoCommon/BPMemory.h"

namespace DX9Remix
{
namespace TEVMapper
{
u32 MapTEVToD3D9(const BPMemory& bp, D3D9StageConfig* out_stages, u32 max_stages)
{
  const u32 num_tev_stages = bp.genMode.numtevstages + 1;

  // For now, use a simple heuristic approach:
  // If there are textures, use the first one modulated by vertex color.
  // This covers the most common case and gives RTX Remix enough to work with.

  // Check if any texture is referenced
  bool has_texture = false;
  for (u32 i = 0; i < num_tev_stages && i < 16; i++)
  {
    // Each TwoTevStageOrders holds two stages (even/odd).
    // getEnable(0) = even stage, getEnable(1) = odd stage.
    const auto& order = bp.tevorders[i / 2];
    const int sub_index = i & 1;

    if (order.getEnable(sub_index))
    {
      has_texture = true;
      break;
    }
  }

  u32 num_d3d9_stages = 0;

  if (has_texture)
  {
    // Stage 0: Texture modulated by vertex diffuse color
    out_stages[0].colorOp = D3DTOP_MODULATE;
    out_stages[0].colorArg1 = D3DTA_TEXTURE;
    out_stages[0].colorArg2 = D3DTA_DIFFUSE;
    out_stages[0].alphaOp = D3DTOP_MODULATE;
    out_stages[0].alphaArg1 = D3DTA_TEXTURE;
    out_stages[0].alphaArg2 = D3DTA_DIFFUSE;
    num_d3d9_stages = 1;
  }
  else
  {
    // No texture - just use vertex color
    out_stages[0].colorOp = D3DTOP_SELECTARG1;
    out_stages[0].colorArg1 = D3DTA_DIFFUSE;
    out_stages[0].colorArg2 = D3DTA_DIFFUSE;
    out_stages[0].alphaOp = D3DTOP_SELECTARG1;
    out_stages[0].alphaArg1 = D3DTA_DIFFUSE;
    out_stages[0].alphaArg2 = D3DTA_DIFFUSE;
    num_d3d9_stages = 1;
  }

  // Disable remaining stages
  for (u32 i = num_d3d9_stages; i < max_stages; i++)
  {
    out_stages[i].colorOp = D3DTOP_DISABLE;
    out_stages[i].alphaOp = D3DTOP_DISABLE;
  }

  return num_d3d9_stages;
}

void ApplyStages(const D3D9StageConfig* stages, u32 num_stages)
{
  if (!D3D9::device)
    return;

  for (u32 i = 0; i < 8; i++)
  {
    D3D9::device->SetTextureStageState(i, D3DTSS_COLOROP, stages[i].colorOp);
    if (stages[i].colorOp != D3DTOP_DISABLE)
    {
      D3D9::device->SetTextureStageState(i, D3DTSS_COLORARG1, stages[i].colorArg1);
      D3D9::device->SetTextureStageState(i, D3DTSS_COLORARG2, stages[i].colorArg2);
    }
    D3D9::device->SetTextureStageState(i, D3DTSS_ALPHAOP, stages[i].alphaOp);
    if (stages[i].alphaOp != D3DTOP_DISABLE)
    {
      D3D9::device->SetTextureStageState(i, D3DTSS_ALPHAARG1, stages[i].alphaArg1);
      D3D9::device->SetTextureStageState(i, D3DTSS_ALPHAARG2, stages[i].alphaArg2);
    }
  }
}
}  // namespace TEVMapper
}  // namespace DX9Remix
