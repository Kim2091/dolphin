// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Remix/RemixVertexManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "Common/Logging/Log.h"

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
// A GC/Wii skybox is geometry that everything else is meant to draw over: it
// carries no usable depth, so it is emitted with the depth test off (or set to
// Always) AND with depth writes off. Both halves are required. Depth-write-off
// on its own is the ordinary signature of alpha-blended geometry, which must
// stay world geometry; it is the absent depth *test* that says "nothing is ever
// behind this", which is exactly what sky means.
//
// Tagging matters beyond looks: an untagged skybox is near geometry to the path
// tracer, so it occludes Remix's own sky/atmosphere instead of being replaced
// by it.
bool IsSkyDraw(const ZMode& zmode)
{
  const bool depth_test_off = !zmode.test_enable || zmode.func == CompareMode::Always;
  return depth_test_off && !zmode.update_enable;
}

// GX alpha testing is two comparators combined by a logic op. Remix's material
// carries a single comparator, and GX's CompareMode numbering (Never=0 ..
// Always=7) is already the numbering Remix expects, so the common shapes
// translate directly rather than approximately.
//
// Pinning this to "always pass" - which is what v1 did - is what makes every
// cutout in a game render as a solid card: foliage, fences, chain-link, tree
// billboards and particles are all a quad plus an alpha test.
void ResolveAlphaTest(const AlphaTest& alpha_test, u8& type, u8& reference)
{
  type = 7;  // Always
  reference = 0;

  // Only And decomposes cleanly into one comparator. Or/Xor/Xnor with two live
  // comparators cannot be expressed at all, and guessing one of the two would
  // cut away geometry the game draws - leave those permissive.
  if (alpha_test.logic != AlphaTestOp::And)
    return;

  const bool comp0_trivial = alpha_test.comp0 == CompareMode::Always;
  const bool comp1_trivial = alpha_test.comp1 == CompareMode::Always;
  if (!comp0_trivial && comp1_trivial)
  {
    type = static_cast<u8>(alpha_test.comp0.Value());
    reference = static_cast<u8>(alpha_test.ref0.Value());
  }
  else if (comp0_trivial && !comp1_trivial)
  {
    type = static_cast<u8>(alpha_test.comp1.Value());
    reference = static_cast<u8>(alpha_test.ref1.Value());
  }
  else if (!comp0_trivial && !comp1_trivial)
  {
    // A genuine range test (typically Greater(lo) AND Less(hi)). Keeping the
    // first half preserves the cutout; the far edge is rarely load-bearing.
    type = static_cast<u8>(alpha_test.comp0.Value());
    reference = static_cast<u8>(alpha_test.ref0.Value());
  }
}

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

// Same matrix without the translation column, for use with a normal matrix from
// xfmem.normalMatrices. Note those are 3 rows of 3 rather than 3 of 4, so the
// caller passes a stride; see NormalMatrixFor.
void TransformNormal3(const float* matrix, const float* in, float* out)
{
  out[0] = in[0] * matrix[0] + in[1] * matrix[1] + in[2] * matrix[2];
  out[1] = in[0] * matrix[3] + in[1] * matrix[4] + in[2] * matrix[5];
  out[2] = in[0] * matrix[6] + in[1] * matrix[7] + in[2] * matrix[8];
}

// The normal matrix paired with a position matrix index. There are only 32 of
// them against 64 position matrices, so the index wraps - masking with 63 (or
// reusing the position matrix, as v1 did) reads past the end or silently
// applies the wrong basis to lighting. TransformUnit.cpp uses & 31.
const float* NormalMatrixFor(u32 position_matrix_index)
{
  return &xfmem.normalMatrices[(position_matrix_index & 31) * 3];
}

// Same matrix without the translation column. Kept for the position-matrix
// path, where the rotation part is the right transform for a rigid entry.
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

// Remix takes exactly one camera per frame, but GX projection state is per
// draw - and Dolphin flushes the batch whenever it changes, so mid-frame
// switches are real and common. Worse, the parameterized camera has no field
// that can express raw[1]/raw[3], the terms that shear the frustum off-centre.
//
// Both losses are recoverable, because relative to the frustum Remix actually
// renders with, the difference is exactly affine in view space. GX perspective
// clip space is
//   clip.x = raw[0]*x + raw[1]*z
//   clip.y = raw[2]*y + raw[3]*z
//   clip.w = -z
// and the camera SetupCamera submits is the reference projection made CENTRED -
// fovY and aspect carry raw[0]/raw[2], and nothing carries raw[1]/raw[3]. So
// the target to solve against is (raw0_r, 0, raw2_r, 0), not the reference's
// own raw terms. Requiring that camera to land the point where the draw's
// projection would, with z (and therefore w) untouched, gives
//   raw0_r*x' == raw0_d*x + raw1_d*z
//   =>  x' = (raw0_d/raw0_r)*x + (raw1_d/raw0_r)*z
// and likewise for y through raw[2]/raw[3]. A scale plus a shear along z, with
// no perspective term, so it composes straight onto the instance transform.
//
// Note what that means for a frame with a single off-centre projection: the
// reference draw does not correct to identity, it corrects by its own shear.
// That is the point - the camera dropped raw[1]/raw[3], so even the draw that
// defined the camera has to have them folded back in.
struct ProjectionCorrection
{
  float x_scale = 1.0f;
  float x_shear = 0.0f;
  float y_scale = 1.0f;
  float y_shear = 0.0f;
};

// Past this ratio the reference is not a plausible frustum for the draw, and
// folding onto it would smear geometry across the screen rather than place it.
// Leaving such a draw uncorrected is the recoverable failure.
constexpr float MAX_PROJECTION_SCALE = 64.0f;

// "Nothing to do" and "refused to do it" both leave the transform alone, but
// they mean opposite things when reading the frame log - one says the frame is
// consistent, the other says a draw is knowingly being rendered through the
// wrong frustum. Counted separately for that reason.
enum class CorrectionResult
{
  NotNeeded,
  Apply,
  Unrepresentable,
};

CorrectionResult BuildProjectionCorrection(const std::array<float, 6>& draw,
                                           const std::array<float, 6>& reference,
                                           ProjectionCorrection& out)
{
  // A reference with no x or y scale is not a frustum at all; there is nothing
  // meaningful to fold onto.
  if (std::abs(reference[0]) < 1e-6f || std::abs(reference[2]) < 1e-6f)
    return CorrectionResult::Unrepresentable;

  // reference[1] / reference[3] deliberately do not appear: the camera never
  // applied them, so the draw's own off-centre terms fold in whole.
  out.x_scale = draw[0] / reference[0];
  out.x_shear = draw[1] / reference[0];
  out.y_scale = draw[2] / reference[2];
  out.y_shear = draw[3] / reference[2];

  if (!std::isfinite(out.x_scale) || !std::isfinite(out.x_shear) ||
      !std::isfinite(out.y_scale) || !std::isfinite(out.y_shear))
  {
    return CorrectionResult::Unrepresentable;
  }

  if (std::abs(out.x_scale) > MAX_PROJECTION_SCALE ||
      std::abs(out.y_scale) > MAX_PROJECTION_SCALE ||
      std::abs(out.x_scale) < 1.0f / MAX_PROJECTION_SCALE ||
      std::abs(out.y_scale) < 1.0f / MAX_PROJECTION_SCALE)
  {
    return CorrectionResult::Unrepresentable;
  }

  // Identity is the common case on a well-behaved game - one centred
  // projection all frame - and skipping it keeps float noise off that path.
  const bool needed = std::abs(out.x_scale - 1.0f) > 1e-6f ||
                      std::abs(out.y_scale - 1.0f) > 1e-6f || std::abs(out.x_shear) > 1e-6f ||
                      std::abs(out.y_shear) > 1e-6f;
  return needed ? CorrectionResult::Apply : CorrectionResult::NotNeeded;
}

// transform <- C * transform, both 3x4 row-major with an implicit [0 0 0 1]
// fourth row. C's fourth column is zero, so one expression covers the
// translation column as well, and row 2 passes through untouched.
//
// Caveat: the shear makes C non-rigid, so strictly the normals want C's inverse
// transpose. The correction is near-identity for everything except a genuine
// FOV switch, so this reads as slightly skewed shading on mismatched draws
// rather than as a visible fault - worth revisiting if a game leans on it.
void ApplyProjectionCorrection(const ProjectionCorrection& c, remixapi_Transform& transform)
{
  for (int j = 0; j < 4; ++j)
  {
    const float row2 = transform.matrix[2][j];
    transform.matrix[0][j] = c.x_scale * transform.matrix[0][j] + c.x_shear * row2;
    transform.matrix[1][j] = c.y_scale * transform.matrix[1][j] + c.y_shear * row2;
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

  // First perspective draw of the frame defines the camera; every distinct
  // projection after it is tracked so the frame log can say whether the
  // reference is the one actually carrying the scene.
  const int projection_slot = g_remix_api->ObserveProjection(xfmem.projection.rawProjection);

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

  // Draws that cannot put colour on screen. A rasterizer still runs these for
  // their depth side effects - Z-prepasses, water and shadow masks, occlusion
  // proxies - but there is no depth buffer here for them to write to, so
  // submitting them just adds solid geometry the game never meant to be seen.
  // Mirrors the rule in RenderState.cpp: color_update && TestResult() != Fail.
  if ((!bpmem.blendmode.color_update && !bpmem.blendmode.alpha_update) ||
      bpmem.alpha_test.TestResult() == AlphaTestResult::Fail)
  {
    ++stats.skipped_invisible;
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

  u8 alpha_test_type = 7;
  u8 alpha_reference = 0;
  ResolveAlphaTest(bpmem.alpha_test, alpha_test_type, alpha_reference);

  const MaterialRef material = g_remix_api->EnsureMaterial(albedo, filter_mode, wrap_mode_u,
                                                           wrap_mode_v, alpha_test_type,
                                                           alpha_reference);
  if (material.handle == nullptr)
    return;

  // ---- Decode the vertex stream ------------------------------------------

  const bool per_vertex_matrix = decl.posmtx.enable;

  // A matrix-palette draw very often names only ONE matrix: the vertex format
  // carries the attribute because the game set it up that way, but the object
  // is rigid. Those do not need baking, and baking them is expensive - a baked
  // draw hashes transformed vertex bytes, so it mints a fresh mesh handle every
  // frame the camera moves and leans on the 300-frame reaper to clean up after
  // it. Left in object space with the single matrix on the instance instead,
  // the hash is stable: one handle, reused, and with usable motion vectors.
  //
  // Costs one extra pass over the posmtx attribute, which is 4 bytes a vertex.
  u32 uniform_matrix_index = 0;
  bool uniform_matrix = per_vertex_matrix;
  if (per_vertex_matrix)
  {
    std::memcpy(&uniform_matrix_index, m_base_buffer_pointer + decl.posmtx.offset, sizeof(u32));
    uniform_matrix_index &= 0x3f;
    for (u32 i = 1; i < vertex_count; ++i)
    {
      u32 index = 0;
      std::memcpy(&index, m_base_buffer_pointer + static_cast<size_t>(i) * stride + decl.posmtx.offset,
                  sizeof(u32));
      if ((index & 0x3f) != uniform_matrix_index)
      {
        uniform_matrix = false;
        break;
      }
    }
  }
  // Only genuinely multi-matrix geometry gets its vertices transformed.
  const bool bake_vertices = per_vertex_matrix && !uniform_matrix;

  const int position_components = std::min(decl.position.components, 3);
  const bool has_normals = decl.normals[0].enable;
  const bool has_texcoord = decl.texcoords[0].enable;
  const int texcoord_components = has_texcoord ? std::min(decl.texcoords[0].components, 2) : 0;
  // A vertex carrying only color1 feeds channel 0 - the hardware does not
  // require the channels to be populated in order (VertexShaderGen.cpp does the
  // same substitution). Reading colors[0] unconditionally turned that geometry
  // white.
  const int color_slot = decl.colors[0].enable ? 0 : (decl.colors[1].enable ? 1 : -1);

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

    if (bake_vertices)
    {
      // Genuinely multi-matrix geometry: each vertex names its own modelview.
      // There is no per-vertex transform on the Remix side without real skinning
      // data, so bake the transform in and submit with an identity instance
      // transform. The mesh hash then covers the transformed bytes, so these
      // re-create every frame and lean on the idle-mesh LRU - an accepted cost,
      // now paid only by the draws that actually need it.
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
        // Normals ride their own matrix, not the position one. GX pairs them by
        // index but there are only 32 normal matrices to 64 position matrices,
        // so the index wraps.
        TransformNormal3(NormalMatrixFor(matrix_index), normal, transformed);
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

    if (color_slot >= 0)
    {
      u32 color = 0;
      std::memcpy(&color, src + decl.colors[color_slot].offset, sizeof(u32));
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
  // Kept separate from `transform` because the camera estimator has to vote on
  // the RAW modelview: the projection correction folded in below is a view-space
  // shear, and one present in both frames conjugates the inter-frame delta
  // rather than cancelling out of it. Null on the matrix-palette path, which has
  // no single modelview to offer.
  const float* raw_modelview = nullptr;
  if (bake_vertices)
  {
    transform.matrix[0][0] = 1.0f;
    transform.matrix[1][1] = 1.0f;
    transform.matrix[2][2] = 1.0f;
  }
  else
  {
    // GX modelview matrices are 3 rows of 4 floats, row-major - byte-identical
    // to remixapi_Transform::matrix[3][4]. There is no separate view matrix in
    // the GX pipeline, so this is an object-to-VIEW transform; either the camera
    // sits at the origin and it doubles as object-to-world, or camera recovery
    // is on and RemixApi turns it into one (see RemixApi::SetupCamera).
    //
    // A single-matrix palette draw takes the matrix its vertices all named;
    // everything else takes the CP-state one.
    const u32 matrix_index =
        uniform_matrix ? uniform_matrix_index :
                         static_cast<u32>(g_main_cp_state.matrix_index_a.PosNormalMtxIdx);
    raw_modelview = &xfmem.posMatrices[matrix_index * 4];
    std::memcpy(&transform.matrix[0][0], raw_modelview, sizeof(float) * 12);
  }

  // Fold this draw's projection onto the frame's reference, so the one camera
  // SetupCamera submits still frames every draw the way the game did. Without
  // it, anything that did not set the reference projection is rendered through
  // someone else's frustum: a different FOV puts it at the wrong screen angle,
  // and a dropped off-centre term reads almost exactly like a small camera
  // rotation - so that geometry swings against the rest of the scene as the
  // view turns, which is the whole symptom this exists to kill.
  if (xfmem.projection.rawProjection[1] != 0.0f || xfmem.projection.rawProjection[3] != 0.0f)
    ++stats.projection_oblique;

  if (g_remix_api->ProjectionFixEnabled() && g_remix_api->HasReferenceProjection())
  {
    ProjectionCorrection correction;
    switch (BuildProjectionCorrection(xfmem.projection.rawProjection,
                                      g_remix_api->ReferenceProjection(), correction))
    {
    case CorrectionResult::Apply:
      ApplyProjectionCorrection(correction, transform);
      ++stats.projection_corrected;
      break;
    case CorrectionResult::Unrepresentable:
      ++stats.projection_uncorrectable;
      break;
    case CorrectionResult::NotNeeded:
      break;
    }
  }

  // Two routes to sky. The explicit texture list is the reliable one - it names
  // the exact skybox texture, so it cannot mistake clouds or overlays for the
  // horizon dome the way the depth heuristic did. The heuristic remains behind
  // RemixSkyMode for games where the hash is not known yet, defaulted off.
  const int sky_mode = g_remix_api->SkyMode();
  const bool is_sky = (albedo != nullptr && g_remix_api->IsSkyTexture(albedo->GetContentHash())) ||
                      (sky_mode != 0 && IsSkyDraw(bpmem.zmode));
  if (is_sky)
    ++stats.sky_draws;

  // Mode 2 drops sky geometry outright. Tagging it as sky still hands Remix
  // something to draw where the sky is; removing it is the only way to leave
  // that volume genuinely empty for a replacement atmosphere.
  if (is_sky && sky_mode == 2)
    return;

  remixapi_InstanceCategoryFlags category_flags = 0;
  if (is_sky)
    category_flags |= REMIXAPI_INSTANCE_CATEGORY_BIT_SKY;

  // Trace the draws the heuristic MATCHED (plus a couple that it did not, for
  // contrast) - knowing what got tagged is what says whether the heuristic is
  // picking out the skybox or just hoovering up every depth-test-less draw.
  if (g_remix_api->ShouldTraceDraws() &&
      ((is_sky && stats.sky_draws <= 8) || (!is_sky && stats.instances_drawn < 3)))
  {
    // Log the TEXTURE content hash, not the material hash - the texture hash is
    // what RemixSkyTextures matches on. The material hash folds in sampler bits
    // and would silently never match.
    INFO_LOG_FMT(VIDEO,
                 "Remix {} draw: ztest {} zfunc {} zwrite {} | blend {} | verts {} tris {} | "
                 "tex {:#018x}",
                 is_sky ? "SKY" : "world", bpmem.zmode.test_enable ? 1 : 0,
                 static_cast<u32>(bpmem.zmode.func.Value()), bpmem.zmode.update_enable ? 1 : 0,
                 bpmem.blendmode.blend_enable ? 1 : 0, out_vertices->size(),
                 out_indices->size() / 3,
                 albedo != nullptr ? albedo->GetContentHash() : 0);
  }

  g_remix_api->NoteProjectionUse(projection_slot, static_cast<u32>(out_vertices->size()));
  g_remix_api->SubmitMesh(material, *out_vertices, *out_indices, transform, category_flags,
                          raw_modelview);
}

}  // namespace Remix
