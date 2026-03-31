// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d9.h>

#include "Common/CommonTypes.h"

struct BPMemory;

namespace DX9Remix
{
// Maps GC/Wii TEV (Texture Environment) stages to D3D9 fixed-function
// texture stage states. Uses pattern matching for common TEV configurations.
//
// D3D9 supports up to 8 texture stages with limited combiners.
// GC/Wii TEV supports up to 16 stages with complex operations.
// We do best-effort mapping and fall back to approximation for unsupported configs.
namespace TEVMapper
{
struct D3D9StageConfig
{
  D3DTEXTUREOP colorOp = D3DTOP_DISABLE;
  DWORD colorArg1 = D3DTA_TEXTURE;
  DWORD colorArg2 = D3DTA_DIFFUSE;
  D3DTEXTUREOP alphaOp = D3DTOP_DISABLE;
  DWORD alphaArg1 = D3DTA_TEXTURE;
  DWORD alphaArg2 = D3DTA_DIFFUSE;
};

// Maps the current TEV configuration to D3D9 texture stage states.
// Returns the number of D3D9 stages used (0-8).
// out_stages must be an array of at least 8 elements.
u32 MapTEVToD3D9(const BPMemory& bpmem, D3D9StageConfig* out_stages, u32 max_stages = 8);

// Apply the mapped stages to the D3D9 device.
void ApplyStages(const D3D9StageConfig* stages, u32 num_stages);
}  // namespace TEVMapper
}  // namespace DX9Remix
