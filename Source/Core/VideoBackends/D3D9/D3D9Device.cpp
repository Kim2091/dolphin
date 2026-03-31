// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D9/D3D9Device.h"

#include "Common/DynamicLibrary.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#pragma comment(lib, "d3d9.lib")

namespace DX9Remix
{
static Common::DynamicLibrary s_d3d9_library;

namespace D3D9
{
ComPtr<IDirect3D9> d3d9;
ComPtr<IDirect3DDevice9> device;

bool Create(HWND hwnd)
{
  using Direct3DCreate9Func = IDirect3D9*(WINAPI*)(UINT);

  Direct3DCreate9Func create_func;
  if (!s_d3d9_library.Open("d3d9.dll") ||
      !s_d3d9_library.GetSymbol("Direct3DCreate9", &create_func))
  {
    PanicAlertFmtT("Failed to load d3d9.dll");
    s_d3d9_library.Close();
    return false;
  }

  IDirect3D9* raw_d3d9 = create_func(D3D_SDK_VERSION);
  if (!raw_d3d9)
  {
    PanicAlertFmtT("Failed to create IDirect3D9");
    s_d3d9_library.Close();
    return false;
  }
  d3d9.Attach(raw_d3d9);

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 640;
  pp.BackBufferHeight = 528;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.MultiSampleType = D3DMULTISAMPLE_NONE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

  IDirect3DDevice9* raw_device = nullptr;
  HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                  D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                  &pp, &raw_device);
  if (FAILED(hr))
  {
    PanicAlertFmtT("Failed to create D3D9 device. HRESULT: {0:#010x}", static_cast<u32>(hr));
    d3d9.Reset();
    s_d3d9_library.Close();
    return false;
  }
  device.Attach(raw_device);

  NOTICE_LOG_FMT(VIDEO, "D3D9 device created successfully");
  return true;
}

void Destroy()
{
  device.Reset();
  d3d9.Reset();
  s_d3d9_library.Close();
  NOTICE_LOG_FMT(VIDEO, "D3D9 device destroyed");
}

}  // namespace D3D9
}  // namespace DX9Remix
