// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Remix/RemixVertexManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "VideoBackends/Remix/RemixApi.h"
#include "VideoBackends/Remix/RemixTexture.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/XFMemory.h"

namespace Remix
{
namespace
{
// Dolphin's vertex loader writes vertex colors as a u32 whose memory order is
// R, G, B, A. Remix reads remixapi_HardcodedVertex::color as
// VK_FORMAT_B8G8R8A8_UNORM, i.e. memory order B, G, R, A. Swap the two ends.
u32 ToRemixVertexColor(u32 dolphin_color)
{
  return (dolphin_color & 0xFF00FF00u) | ((dolphin_color >> 16) & 0x000000FFu) |
         ((dolphin_color & 0x000000FFu) << 16);
}

void ReadFloats(const u8* src, float* dst, int count)
{
  std::memcpy(dst, src, sizeof(float) * static_cast<size_t>(count));
}

// Transform an object-space position by a GX 3x4 (row-major) modelview matrix.
void TransformPosition(const float* matrix, const float* in, float* out)
{
  out[0] = in[0] * matrix[0] + in[1] * matrix[1] + in[2] * matrix[2] + matrix[3];
  out[1] = in[0] * matrix[4] + in[1] * matrix[5] + in[2] * matrix[6] + matrix[7];
  out[2] = in[0] * matrix[8] + in[1] * matrix[9] + in[2] * matrix[10] + matrix[11];
}

// Same matrix without the translation column. GX matrix-palette entries are
// rigid in practice, so the rotation part of the position matrix is a good
// enough normal transform for v1; a genuinely non-uniformly scaled palette
// entry would want xfmem.normalMatrices instead.
void TransformNormal(const float* matrix, const float* in, float* out)
{
  out[0] = in[0] * matrix[0] + in[1] * matrix[1] + in[2] * matrix[2];
  out[1] = in[0] * matrix[4] + in[1] * matrix[5] + in[2] * matrix[6];
  out[2] = in[0] * matrix[8] + in[1] * matrix[9] + in[2] * matrix[10];
}

void Normalize(float* v)
{
  const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (length > 1e-8f)
  {
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
  }
}
}  // namespace

VertexManager::VertexManager() = default;

VertexManager::~VertexManager() = default;

void VertexManager::DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex)
{
  if (!g_remix_api || !g_remix_api->IsValid())
    return;

  FrameStats& stats = g_remix_api->Stats();
  ++stats.draws_seen;

  // ---- Classify -----------------------------------------------------------

  // Orthographic projection is HUD/UI/menus. It is never submitted; v1 has no
  // 2D compositing path at all, so those draws are simply absent from the
  // path-traced output.
  if (xfmem.projection.type != ProjectionType::Perspective)
  {
    ++stats.skipped_ortho;
    return;
  }

  // First perspective draw of the frame defines the camera.
  g_remix_api->LatchProjection(xfmem.projection.rawProjection);

  // bSupportsPrimitiveRestart is false for this backend, so every quad/strip/fan
  // has already been expanded into a plain triangle list by the index generator
  // and there are no restart tokens to parse. Anything that is still not
  // Triangles is a line or point primitive, which Remix has no surface for.
  if (m_current_primitive_type != PrimitiveType::Triangles)
  {
    ++stats.skipped_non_triangle;
    return;
  }

  if (num_indices < 3)
  {
    ++stats.skipped_degenerate;
    return;
  }

  const NativeVertexFormat* format = VertexLoaderManager::GetCurrentVertexFormat();
  if (format == nullptr)
  {
    ++stats.skipped_degenerate;
    return;
  }
  const PortableVertexDeclaration& decl = format->GetVertexDeclaration();
  const u32 stride = static_cast<u32>(decl.stride);
  if (stride == 0 || m_cur_buffer_pointer <= m_base_buffer_pointer)
  {
    ++stats.skipped_degenerate;
    return;
  }
  const u32 vertex_count =
      static_cast<u32>(m_cur_buffer_pointer - m_base_buffer_pointer) / stride;
  if (vertex_count == 0)
  {
    ++stats.skipped_degenerate;
    return;
  }

  // ---- Resolve the albedo material ---------------------------------------

  // v1 materials are "whatever TEV stage 0 samples". Every other stage - and
  // therefore every multi-stage combiner effect - is ignored.
  const bool stage0_textured = bpmem.tevorders[0].getEnable(0) != 0;
  const u32 stage0_texmap = bpmem.tevorders[0].getTexMap(0);
  const RemixTexture* albedo = nullptr;
  u8 filter_mode = 1;   // MDL Filter::Linear
  u8 wrap_mode_u = 1;   // MDL WrapMode::Repeat
  u8 wrap_mode_v = 1;

  if (stage0_textured)
  {
    albedo = g_remix_api->GetBoundTexture(stage0_texmap);
    // Our texture cache does not execute EFB/XFB copies, so an entry produced
    // by one never receives pixels through Load(). That is exactly the set of
    // draws we cannot translate (dynamic shadow maps, heat haze, water
    // reflections) - and also the set of custom-texture formats we do not
    // decode in v1.
    if (albedo == nullptr || !albedo->HasData())
    {
      ++stats.skipped_efb_texture;
      return;
    }

    const TexMode0& mode = bpmem.tex.GetUnit(stage0_texmap).texMode0;
    // GX and Remix (MDL) agree numerically: Near/Linear == 0/1 and
    // Clamp/Repeat/Mirror == 0/1/2. The invalid GX wrap value 3 behaves as
    // clamp on hardware.
    filter_mode = static_cast<u8>(mode.mag_filter.Value());
    wrap_mode_u = static_cast<u8>(std::min<u32>(static_cast<u32>(mode.wrap_s.Value()), 2));
    wrap_mode_v = static_cast<u8>(std::min<u32>(static_cast<u32>(mode.wrap_t.Value()), 2));
  }

  const MaterialRef material =
      g_remix_api->EnsureMaterial(albedo, filter_mode, wrap_mode_u, wrap_mode_v);
  if (material.handle == nullptr)
    return;

  // ---- Decode the vertex stream ------------------------------------------

  const bool per_vertex_matrix = decl.posmtx.enable;
  const int position_components = std::min(decl.position.components, 3);
  const bool has_normals = decl.normals[0].enable;
  const bool has_texcoord = decl.texcoords[0].enable;
  const int texcoord_components = has_texcoord ? std::min(decl.texcoords[0].components, 2) : 0;
  const bool has_color = decl.colors[0].enable;

  m_vertices.clear();
  m_vertices.resize(vertex_count);

  for (u32 i = 0; i < vertex_count; ++i)
  {
    const u8* const src = m_base_buffer_pointer + static_cast<size_t>(i) * stride;
    remixapi_HardcodedVertex& dst = m_vertices[i];

    float position[3] = {0.0f, 0.0f, 0.0f};
    ReadFloats(src + decl.position.offset, position, position_components);

    float normal[3] = {0.0f, 0.0f, 0.0f};
    if (has_normals)
      ReadFloats(src + decl.normals[0].offset, normal, 3);

    if (per_vertex_matrix)
    {
      // Matrix-palette draw (skinned / multi-matrix geometry): each vertex
      // names its own modelview. There is no per-vertex transform on the Remix
      // side without real skinning data, so bake the transform in and submit
      // with an identity instance transform. The mesh hash then covers the
      // transformed bytes, so animated meshes re-create every frame and lean on
      // the idle-mesh LRU - an accepted v1 cost.
      u32 matrix_index = 0;
      std::memcpy(&matrix_index, src + decl.posmtx.offset, sizeof(u32));
      const float* const matrix = &xfmem.posMatrices[(matrix_index & 0x3f) * 4];

      float transformed[3];
      TransformPosition(matrix, position, transformed);
      dst.position[0] = transformed[0];
      dst.position[1] = transformed[1];
      dst.position[2] = transformed[2];

      if (has_normals)
      {
        TransformNormal(matrix, normal, transformed);
        Normalize(transformed);
        dst.normal[0] = transformed[0];
        dst.normal[1] = transformed[1];
        dst.normal[2] = transformed[2];
      }
    }
    else
    {
      dst.position[0] = position[0];
      dst.position[1] = position[1];
      dst.position[2] = position[2];
      dst.normal[0] = normal[0];
      dst.normal[1] = normal[1];
      dst.normal[2] = normal[2];
    }

    if (texcoord_components > 0)
      ReadFloats(src + decl.texcoords[0].offset, dst.texcoord, texcoord_components);

    if (has_color)
    {
      u32 color = 0;
      std::memcpy(&color, src + decl.colors[0].offset, sizeof(u32));
      dst.color = ToRemixVertexColor(color);
    }
    else
    {
      dst.color = 0xFFFFFFFFu;
    }
  }

  m_indices.clear();
  m_indices.reserve(num_indices);
  for (u32 i = 0; i < num_indices; ++i)
  {
    const u32 index = static_cast<u32>(m_cpu_index_buffer[base_index + i]) + base_vertex;
    if (index >= vertex_count)
    {
      ++stats.skipped_degenerate;
      return;
    }
    m_indices.push_back(index);
  }

  const std::vector<remixapi_HardcodedVertex>* out_vertices = &m_vertices;
  const std::vector<u32>* out_indices = &m_indices;

  if (!has_normals)
  {
    // The game supplied no normals (very common: GC titles bake lighting into
    // vertex colors). Give every triangle its own geometric normal, which
    // requires splitting shared vertices - a shared vertex cannot carry two
    // face normals.
    //
    // Winding convention: cross(p1 - p0, p2 - p0) assumes counter-clockwise
    // front faces. If lighting reads inverted across the whole scene, swapping
    // the two edge vectors here is the one-line fix. Instances are submitted
    // double-sided, so a wrong guess costs shading, never visibility.
    const size_t triangle_count = m_indices.size() / 3;
    m_flat_vertices.clear();
    m_flat_vertices.reserve(triangle_count * 3);
    m_flat_indices.clear();
    m_flat_indices.reserve(triangle_count * 3);

    for (size_t tri = 0; tri < triangle_count; ++tri)
    {
      const remixapi_HardcodedVertex& v0 = m_vertices[m_indices[tri * 3 + 0]];
      const remixapi_HardcodedVertex& v1 = m_vertices[m_indices[tri * 3 + 1]];
      const remixapi_HardcodedVertex& v2 = m_vertices[m_indices[tri * 3 + 2]];

      const float e0[3] = {v1.position[0] - v0.position[0], v1.position[1] - v0.position[1],
                           v1.position[2] - v0.position[2]};
      const float e1[3] = {v2.position[0] - v0.position[0], v2.position[1] - v0.position[1],
                           v2.position[2] - v0.position[2]};
      float normal[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                         e0[0] * e1[1] - e0[1] * e1[0]};
      Normalize(normal);

      for (const remixapi_HardcodedVertex* source : {&v0, &v1, &v2})
      {
        remixapi_HardcodedVertex vertex = *source;
        vertex.normal[0] = normal[0];
        vertex.normal[1] = normal[1];
        vertex.normal[2] = normal[2];
        m_flat_indices.push_back(static_cast<u32>(m_flat_vertices.size()));
        m_flat_vertices.push_back(vertex);
      }
    }

    out_vertices = &m_flat_vertices;
    out_indices = &m_flat_indices;
  }

  // ---- Instance transform -------------------------------------------------

  remixapi_Transform transform = {};
  if (per_vertex_matrix)
  {
    transform.matrix[0][0] = 1.0f;
    transform.matrix[1][1] = 1.0f;
    transform.matrix[2][2] = 1.0f;
  }
  else
  {
    // GX modelview matrices are 3 rows of 4 floats, row-major - byte-identical
    // to remixapi_Transform::matrix[3][4]. Because there is no separate view
    // matrix in the GX pipeline, this object-to-VIEW transform is submitted as
    // if it were object-to-world; the camera then sits at the origin (see
    // RemixApi::SetupCamera).
    const float* const matrix =
        &xfmem.posMatrices[g_main_cp_state.matrix_index_a.PosNormalMtxIdx * 4];
    std::memcpy(&transform.matrix[0][0], matrix, sizeof(float) * 12);
  }

  g_remix_api->SubmitMesh(material, *out_vertices, *out_indices, transform);
}

}  // namespace Remix
