// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d9.h>
#include <wrl/client.h>

#include "Common/CommonTypes.h"

namespace DX9Remix
{
using Microsoft::WRL::ComPtr;

class SwapChain
{
public:
  SwapChain() = default;
  ~SwapChain() = default;

  // D3D9 swap chain is managed through the device's implicit swap chain.
  // Present() is called via IDirect3DDevice9::Present().
  bool Present();
  bool Resize(u32 width, u32 height);

  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }

private:
  u32 m_width = 640;
  u32 m_height = 528;
};
}  // namespace DX9Remix
