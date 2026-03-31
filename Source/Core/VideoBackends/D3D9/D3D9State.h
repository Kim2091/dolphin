// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d9.h>

#include "VideoCommon/RenderState.h"

namespace DX9Remix
{
namespace StateManager
{
void ApplyRasterizationState(const RasterizationState& state);
void ApplyDepthState(const DepthState& state);
void ApplyBlendingState(const BlendingState& state);
void ApplySamplerState(u32 index, const SamplerState& state);
}  // namespace StateManager
}  // namespace DX9Remix
