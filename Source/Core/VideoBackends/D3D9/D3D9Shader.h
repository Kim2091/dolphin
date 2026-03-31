// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/AbstractShader.h"

namespace DX9Remix
{
// Stub shader for the D3D9 fixed-function backend.
// The shader cache will create these but they hold nothing -
// all rendering goes through D3D9 fixed-function pipeline.
class D3D9Shader final : public AbstractShader
{
public:
  explicit D3D9Shader(ShaderStage stage) : AbstractShader(stage) {}
};
}  // namespace DX9Remix
