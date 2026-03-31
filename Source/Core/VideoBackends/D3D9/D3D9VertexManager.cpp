// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D9/D3D9VertexManager.h"

#include "Common/Assert.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/D3D9/D3D9Device.h"
#include "VideoBackends/D3D9/D3D9NativeVertexFormat.h"
#include "VideoBackends/D3D9/D3D9TEVMapper.h"
#include "VideoBackends/D3D9/D3D9Texture.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace DX9Remix
{
u32 VertexManager::s_draw_calls_this_frame = 0;
u32 VertexManager::s_upload_calls_this_frame = 0;
u32 VertexManager::s_total_indices_this_frame = 0;

VertexManager::VertexManager()
{
  m_vertex_buffer.resize(MAXVBUFFERSIZE);
  m_index_buffer.resize(MAXIBUFFERSIZE);
}

VertexManager::~VertexManager() = default;

bool VertexManager::Initialize()
{
  if (!VertexManagerBase::Initialize())
    return false;

  if (!D3D9::device)
    return false;

  // Create dynamic vertex buffer
  HRESULT hr = D3D9::device->CreateVertexBuffer(
      MAXVBUFFERSIZE, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
      &m_d3d9_vertex_buffer, nullptr);
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create D3D9 vertex buffer: {:#010x}", static_cast<u32>(hr));
    return false;
  }

  // Create dynamic index buffer
  hr = D3D9::device->CreateIndexBuffer(MAXIBUFFERSIZE * sizeof(u16),
                                       D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                       D3DPOOL_DEFAULT, &m_d3d9_index_buffer, nullptr);
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create D3D9 index buffer: {:#010x}", static_cast<u32>(hr));
    return false;
  }

  return true;
}

void VertexManager::ResetBuffer(u32 vertex_stride)
{
  m_current_stride = vertex_stride;
  m_cur_buffer_pointer = m_base_buffer_pointer = m_vertex_buffer.data();
  m_end_buffer_pointer = m_vertex_buffer.data() + m_vertex_buffer.size();
  m_index_generator.Start(m_index_buffer.data());
}

void VertexManager::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                 u32* out_base_vertex, u32* out_base_index)
{
  *out_base_vertex = 0;
  *out_base_index = 0;

  if (!D3D9::device || num_vertices == 0)
    return;

  // Swizzle vertex colors from RGBA to BGRA (D3DCOLOR format) before upload.
  // D3D9's D3DDECLTYPE_D3DCOLOR expects BGRA byte order.
  const auto* vtx_format = VertexLoaderManager::GetCurrentVertexFormat();
  if (vtx_format)
  {
    const auto& decl = vtx_format->GetVertexDeclaration();
    for (int c = 0; c < 2; c++)
    {
      if (decl.colors[c].enable && decl.colors[c].components == 4 &&
          decl.colors[c].type == ComponentFormat::UByte)
      {
        const u32 offset = decl.colors[c].offset;
        for (u32 v = 0; v < num_vertices; v++)
        {
          u8* color = m_vertex_buffer.data() + v * vertex_stride + offset;
          std::swap(color[0], color[2]);  // R <-> B
        }
      }
    }
  }

  // Upload vertex data
  const u32 vertex_data_size = num_vertices * vertex_stride;
  if (m_d3d9_vertex_buffer && vertex_data_size > 0)
  {
    void* vb_data = nullptr;
    HRESULT hr = m_d3d9_vertex_buffer->Lock(0, vertex_data_size, &vb_data, D3DLOCK_DISCARD);
    if (SUCCEEDED(hr))
    {
      std::memcpy(vb_data, m_vertex_buffer.data(), vertex_data_size);
      m_d3d9_vertex_buffer->Unlock();
    }
  }

  // Upload index data
  const u32 index_data_size = num_indices * sizeof(u16);
  if (m_d3d9_index_buffer && index_data_size > 0)
  {
    void* ib_data = nullptr;
    HRESULT hr = m_d3d9_index_buffer->Lock(0, index_data_size, &ib_data, D3DLOCK_DISCARD);
    if (SUCCEEDED(hr))
    {
      std::memcpy(ib_data, m_index_buffer.data(), index_data_size);
      m_d3d9_index_buffer->Unlock();
    }
  }
}

void VertexManager::UploadUniforms()
{
  if (!D3D9::device)
    return;

  s_upload_calls_this_frame++;

  // Set up transforms for RTX Remix
  // Get the active position matrix index
  const u32 pos_mtx_idx = g_main_cp_state.matrix_index_a.PosNormalMtxIdx;

  // Update the view matrix from posMatrices
  m_transform_decomposer.UpdateViewMatrix(xfmem.posMatrices);

  // Set View matrix (camera transform for RTX Remix)
  D3DMATRIX view = m_transform_decomposer.GetViewMatrix();
  D3D9::device->SetTransform(D3DTS_VIEW, &view);

  // Set World matrix (object transform relative to camera)
  D3DMATRIX world = m_transform_decomposer.GetWorldMatrix(pos_mtx_idx, xfmem.posMatrices);
  D3D9::device->SetTransform(D3DTS_WORLD, &world);

  // Set Projection matrix
  D3DMATRIX proj = m_transform_decomposer.GetProjectionMatrix(xfmem.projection);
  D3D9::device->SetTransform(D3DTS_PROJECTION, &proj);

  // Debug: write transforms to file
  static u32 s_upload_count = 0;
  s_upload_count++;
  if (s_upload_count <= 5)
  {
    FILE* f = fopen("d3d9_debug.txt", "a");
    if (f)
    {
      fprintf(f,
              "UploadUniforms #%u: mtx_idx=%u, proj_type=%u\n"
              "  View: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f "
              "%.3f %.3f %.3f]\n"
              "  World: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f "
              "%.3f %.3f %.3f]\n"
              "  Proj: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f "
              "%.3f %.3f %.3f]\n\n",
              s_upload_count, pos_mtx_idx, static_cast<u32>(xfmem.projection.type),
              view._11, view._12, view._13, view._14, view._21, view._22, view._23, view._24,
              view._31, view._32, view._33, view._34, view._41, view._42, view._43, view._44,
              world._11, world._12, world._13, world._14, world._21, world._22, world._23,
              world._24, world._31, world._32, world._33, world._34, world._41, world._42,
              world._43, world._44,
              proj._11, proj._12, proj._13, proj._14, proj._21, proj._22, proj._23, proj._24,
              proj._31, proj._32, proj._33, proj._34, proj._41, proj._42, proj._43, proj._44);
      fclose(f);
    }
  }

  // Set up texture stage states from TEV configuration
  TEVMapper::D3D9StageConfig stages[8];
  TEVMapper::MapTEVToD3D9(bpmem, stages);
  TEVMapper::ApplyStages(stages, 8);
}

void VertexManager::DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex)
{
  if (!D3D9::device || num_indices == 0)
    return;

  s_draw_calls_this_frame++;
  s_total_indices_this_frame += num_indices;

  // Set the vertex declaration from the current vertex format
  const auto* vtx_format = VertexLoaderManager::GetCurrentVertexFormat();
  if (vtx_format)
  {
    const auto* d3d9_format = static_cast<const D3D9VertexFormat*>(vtx_format);
    if (d3d9_format->GetDeclaration())
      D3D9::device->SetVertexDeclaration(d3d9_format->GetDeclaration());
  }

  // Set stream source and indices
  D3D9::device->SetStreamSource(0, m_d3d9_vertex_buffer.Get(), 0, m_current_stride);
  D3D9::device->SetIndices(m_d3d9_index_buffer.Get());

  // Determine primitive count from indices
  const u32 prim_count = num_indices / 3;
  if (prim_count == 0)
    return;

  // Get the number of vertices from the index generator
  const u32 num_vertices = m_index_generator.GetNumVerts();

  HRESULT hr = D3D9::device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, base_vertex, 0, num_vertices,
                                                   base_index, prim_count);

  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VIDEO, "DrawIndexedPrimitive failed: {:#010x}", static_cast<u32>(hr));
  }
}
}  // namespace DX9Remix
