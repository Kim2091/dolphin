// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D9/D3D9NativeVertexFormat.h"

#include <array>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "VideoBackends/D3D9/D3D9Device.h"

namespace DX9Remix
{
static D3DDECLTYPE VarToD3D9Type(ComponentFormat type, int components, bool integer)
{
  // Map Dolphin's ComponentFormat to D3D9 declaration types
  if (type == ComponentFormat::Float)
  {
    switch (components)
    {
    case 1:
      return D3DDECLTYPE_FLOAT1;
    case 2:
      return D3DDECLTYPE_FLOAT2;
    case 3:
      return D3DDECLTYPE_FLOAT3;
    case 4:
      return D3DDECLTYPE_FLOAT4;
    }
  }
  else if (type == ComponentFormat::UByte)
  {
    if (components == 4)
    {
      // For D3D9 fixed-function, diffuse color must be D3DCOLOR (BGRA byte order).
      // Dolphin vertex loaders output RGBA bytes, so we use D3DDECLTYPE_D3DCOLOR
      // which interprets bytes as BGRA. We'll need to swizzle R<->B in the vertex data.
      return integer ? D3DDECLTYPE_UBYTE4 : D3DDECLTYPE_D3DCOLOR;
    }
  }
  else if (type == ComponentFormat::Short)
  {
    if (components == 2)
      return integer ? D3DDECLTYPE_SHORT2 : D3DDECLTYPE_SHORT2N;
    if (components == 4)
      return integer ? D3DDECLTYPE_SHORT4 : D3DDECLTYPE_SHORT4N;
  }
  else if (type == ComponentFormat::UShort)
  {
    if (components == 2)
      return D3DDECLTYPE_USHORT2N;
    if (components == 4)
      return D3DDECLTYPE_USHORT4N;
  }

  // Fallback
  return D3DDECLTYPE_FLOAT3;
}

static void AddAttribute(std::array<D3DVERTEXELEMENT9, 16>& elems, u32& count,
                         const AttributeFormat& format, BYTE usage, BYTE usage_index)
{
  if (!format.enable || count >= 15)  // Leave room for D3DDECL_END
    return;

  D3DVERTEXELEMENT9& elem = elems[count++];
  elem.Stream = 0;
  elem.Offset = static_cast<WORD>(format.offset);
  elem.Type = static_cast<BYTE>(VarToD3D9Type(format.type, format.components, format.integer));
  elem.Method = D3DDECLMETHOD_DEFAULT;
  elem.Usage = usage;
  elem.UsageIndex = usage_index;
}

D3D9VertexFormat::D3D9VertexFormat(const PortableVertexDeclaration& vtx_decl,
                                   ComPtr<IDirect3DVertexDeclaration9> d3d_decl)
    : NativeVertexFormat(vtx_decl), m_d3d_decl(std::move(d3d_decl))
{
}

std::unique_ptr<D3D9VertexFormat> D3D9VertexFormat::Create(const PortableVertexDeclaration& vtx_decl)
{
  if (!D3D9::device)
    return nullptr;

  std::array<D3DVERTEXELEMENT9, 16> elems = {};
  u32 count = 0;

  // Position - always present
  AddAttribute(elems, count, vtx_decl.position, D3DDECLUSAGE_POSITION, 0);

  // Normals
  if (vtx_decl.normals[0].enable)
    AddAttribute(elems, count, vtx_decl.normals[0], D3DDECLUSAGE_NORMAL, 0);
  if (vtx_decl.normals[1].enable)
    AddAttribute(elems, count, vtx_decl.normals[1], D3DDECLUSAGE_TANGENT, 0);
  if (vtx_decl.normals[2].enable)
    AddAttribute(elems, count, vtx_decl.normals[2], D3DDECLUSAGE_BINORMAL, 0);

  // Colors
  for (int i = 0; i < 2; i++)
  {
    if (vtx_decl.colors[i].enable)
      AddAttribute(elems, count, vtx_decl.colors[i], D3DDECLUSAGE_COLOR, static_cast<BYTE>(i));
  }

  // Texture coordinates
  for (int i = 0; i < 8; i++)
  {
    if (vtx_decl.texcoords[i].enable)
      AddAttribute(elems, count, vtx_decl.texcoords[i], D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(i));
  }

  // Add end marker
  elems[count] = D3DDECL_END();

  ComPtr<IDirect3DVertexDeclaration9> d3d_decl;
  HRESULT hr = D3D9::device->CreateVertexDeclaration(elems.data(), &d3d_decl);
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create D3D9 vertex declaration: {:#010x}", static_cast<u32>(hr));
    return nullptr;
  }

  return std::make_unique<D3D9VertexFormat>(vtx_decl, std::move(d3d_decl));
}
}  // namespace DX9Remix
