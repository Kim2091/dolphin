// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d9.h>
#include <wrl/client.h>

#include "VideoCommon/NativeVertexFormat.h"

namespace DX9Remix
{
using Microsoft::WRL::ComPtr;

class D3D9VertexFormat final : public NativeVertexFormat
{
public:
  D3D9VertexFormat(const PortableVertexDeclaration& vtx_decl,
                   ComPtr<IDirect3DVertexDeclaration9> d3d_decl);

  IDirect3DVertexDeclaration9* GetDeclaration() const { return m_d3d_decl.Get(); }

  static std::unique_ptr<D3D9VertexFormat> Create(const PortableVertexDeclaration& vtx_decl);

private:
  ComPtr<IDirect3DVertexDeclaration9> m_d3d_decl;
};
}  // namespace DX9Remix
