// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D9/D3D9TextureCache.h"

#include <d3d9.h>

#include "Common/Logging/Log.h"
#include "VideoBackends/D3D9/D3D9Device.h"
#include "VideoBackends/D3D9/D3D9Texture.h"

namespace DX9Remix
{
void TextureCache::CopyEFBToCacheEntry(RcTcacheEntry& entry, bool is_depth_copy,
                                       const MathUtil::Rectangle<int>& src_rect,
                                       bool scale_by_half, bool linear_filter,
                                       EFBCopyFormat dst_format, bool is_intensity, float gamma,
                                       bool clamp_top, bool clamp_bottom,
                                       const std::array<u32, 3>& filter_coefficients)
{
  if (!D3D9::device || !entry || !entry->texture)
    return;

  // Since all game draws go to the D3D9 backbuffer (for RTX Remix capture),
  // we copy from the backbuffer into the EFB copy texture.  This gives games
  // their EFB copy textures back (reflections, effects, XFB, etc.) instead
  // of leaving them black.

  // Skip depth copies — we don't have a D3D9 depth texture to read from.
  if (is_depth_copy)
    return;

  auto* d3d9_tex = static_cast<D3D9Texture*>(entry->texture.get());
  IDirect3DTexture9* dest_texture = d3d9_tex->GetD3DTexture();
  if (!dest_texture)
    return;

  // Get the backbuffer surface
  ComPtr<IDirect3DSurface9> backbuffer;
  HRESULT hr = D3D9::device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
  if (FAILED(hr) || !backbuffer)
    return;

  // Get the destination texture's surface (level 0)
  ComPtr<IDirect3DSurface9> dest_surface;
  hr = dest_texture->GetSurfaceLevel(0, &dest_surface);
  if (FAILED(hr) || !dest_surface)
    return;

  // Build source rect from the EFB coordinates
  RECT src_d3d;
  src_d3d.left = src_rect.left;
  src_d3d.top = src_rect.top;
  src_d3d.right = src_rect.right;
  src_d3d.bottom = src_rect.bottom;

  // StretchRect from backbuffer region into the entry texture.
  // D3DTEXF_LINEAR for smooth scaling, D3DTEXF_POINT if linear not requested.
  const D3DTEXTUREFILTERTYPE filter = linear_filter ? D3DTEXF_LINEAR : D3DTEXF_POINT;
  hr = D3D9::device->StretchRect(backbuffer.Get(), &src_d3d, dest_surface.Get(), nullptr, filter);

  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VIDEO, "EFB copy StretchRect failed: {:#010x}", static_cast<u32>(hr));
  }
}
}  // namespace DX9Remix
