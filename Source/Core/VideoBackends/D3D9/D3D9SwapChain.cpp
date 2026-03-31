// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D9/D3D9SwapChain.h"

#include "Common/Logging/Log.h"
#include "VideoBackends/D3D9/D3D9Device.h"

namespace DX9Remix
{
bool SwapChain::Present()
{
  if (!D3D9::device)
    return false;

  HRESULT hr = D3D9::device->Present(nullptr, nullptr, nullptr, nullptr);
  if (FAILED(hr))
  {
    if (hr == D3DERR_DEVICELOST)
    {
      WARN_LOG_FMT(VIDEO, "D3D9 device lost during present");
      return false;
    }
    ERROR_LOG_FMT(VIDEO, "D3D9 present failed: {:#010x}", static_cast<u32>(hr));
    return false;
  }
  return true;
}

bool SwapChain::Resize(u32 width, u32 height)
{
  m_width = width;
  m_height = height;
  // D3D9 device reset would be needed for a real resize.
  // For RTX Remix, the window size is generally fixed.
  return true;
}
}  // namespace DX9Remix
