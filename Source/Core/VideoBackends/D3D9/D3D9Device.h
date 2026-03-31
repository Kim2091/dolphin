// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d9.h>
#include <wrl/client.h>

#include "Common/CommonTypes.h"

namespace DX9Remix
{
using Microsoft::WRL::ComPtr;

namespace D3D9
{
extern ComPtr<IDirect3D9> d3d9;
extern ComPtr<IDirect3DDevice9> device;

bool Create(HWND hwnd);
void Destroy();

}  // namespace D3D9
}  // namespace DX9Remix
