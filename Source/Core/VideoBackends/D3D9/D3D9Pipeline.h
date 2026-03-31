// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/AbstractPipeline.h"

namespace DX9Remix
{
// Stub pipeline for the D3D9 fixed-function backend.
// Stores the render state configuration (depth, blend, rasterization)
// but ignores shader references since we use fixed-function.
class D3D9Pipeline final : public AbstractPipeline
{
public:
  explicit D3D9Pipeline(const AbstractPipelineConfig& config) : AbstractPipeline(config) {}
};
}  // namespace DX9Remix
