// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d9.h>
#include <wrl/client.h>

#include "VideoBackends/D3D9/D3D9TransformDecomposer.h"
#include "VideoCommon/VertexManagerBase.h"

namespace DX9Remix
{
using Microsoft::WRL::ComPtr;

class VertexManager final : public VertexManagerBase
{
public:
  VertexManager();
  ~VertexManager() override;

  bool Initialize() override;

protected:
  void ResetBuffer(u32 vertex_stride) override;
  void CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices, u32* out_base_vertex,
                    u32* out_base_index) override;
  void UploadUniforms() override;
  void DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex) override;

private:
  // CPU-side buffers for vertex/index data
  std::vector<u8> m_vertex_buffer;
  std::vector<u16> m_index_buffer;

  // D3D9 vertex/index buffers
  ComPtr<IDirect3DVertexBuffer9> m_d3d9_vertex_buffer;
  ComPtr<IDirect3DIndexBuffer9> m_d3d9_index_buffer;

  // Transform decomposition for camera extraction
  TransformDecomposer m_transform_decomposer;

  // Current vertex stride for the batch
  u32 m_current_stride = 0;

  // Ring buffer state — pack multiple batches per frame into one VB/IB
  // to avoid D3DLOCK_DISCARD on every draw (which causes RTX Remix to
  // allocate a new internal copy of the full buffer each time).
  u32 m_vb_write_offset = 0;            // current byte position in VB
  u32 m_ib_write_offset = 0;            // current byte position in IB
  u32 m_current_vb_stream_offset = 0;   // byte offset for SetStreamSource

public:
  // Debug counters (reset each frame)
  static u32 s_draw_calls_this_frame;
  static u32 s_upload_calls_this_frame;
  static u32 s_total_indices_this_frame;
  static u32 s_total_vb_bytes_this_frame;
  static u32 s_total_ib_bytes_this_frame;
  static u32 s_textures_set_this_frame;
  static bool s_first_lock_of_frame;
};
}  // namespace DX9Remix
