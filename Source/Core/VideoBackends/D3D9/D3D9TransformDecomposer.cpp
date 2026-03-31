// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D9/D3D9TransformDecomposer.h"

#include <cmath>
#include <cstring>

#include "VideoCommon/XFMemory.h"

namespace DX9Remix
{
TransformDecomposer::TransformDecomposer()
{
  // Initialize to identity
  std::memset(&m_view_matrix, 0, sizeof(D3DMATRIX));
  m_view_matrix._11 = m_view_matrix._22 = m_view_matrix._33 = m_view_matrix._44 = 1.0f;
  m_inverse_view_matrix = m_view_matrix;
}

D3DMATRIX TransformDecomposer::ExpandPosMatrix(const float* mtx34)
{
  // GC/Wii position matrices are 3x4, column-vector convention (M * v):
  //   [r00 r01 r02 tx]
  //   [r10 r11 r12 ty]
  //   [r20 r21 r22 tz]
  //
  // D3D9 uses row-vector convention (v * M), so translation goes in the 4th ROW.
  // We must TRANSPOSE the 3x3 rotation part and move translation to row 4.
  D3DMATRIX result;
  result._11 = mtx34[0];
  result._12 = mtx34[4];
  result._13 = mtx34[8];
  result._14 = 0.0f;
  result._21 = mtx34[1];
  result._22 = mtx34[5];
  result._23 = mtx34[9];
  result._24 = 0.0f;
  result._31 = mtx34[2];
  result._32 = mtx34[6];
  result._33 = mtx34[10];
  result._34 = 0.0f;
  result._41 = mtx34[3];   // tx
  result._42 = mtx34[7];   // ty
  result._43 = mtx34[11];  // tz
  result._44 = 1.0f;
  return result;
}

D3DMATRIX TransformDecomposer::MultiplyMatrix(const D3DMATRIX& a, const D3DMATRIX& b)
{
  D3DMATRIX result;
  const float* af = reinterpret_cast<const float*>(&a);
  const float* bf = reinterpret_cast<const float*>(&b);
  float* rf = reinterpret_cast<float*>(&result);

  for (int row = 0; row < 4; row++)
  {
    for (int col = 0; col < 4; col++)
    {
      rf[row * 4 + col] = af[row * 4 + 0] * bf[0 * 4 + col] +
                           af[row * 4 + 1] * bf[1 * 4 + col] +
                           af[row * 4 + 2] * bf[2 * 4 + col] +
                           af[row * 4 + 3] * bf[3 * 4 + col];
    }
  }
  return result;
}

D3DMATRIX TransformDecomposer::InvertMatrix(const D3DMATRIX& m)
{
  const float* src = reinterpret_cast<const float*>(&m);
  D3DMATRIX result;
  float* dst = reinterpret_cast<float*>(&result);

  // Compute cofactors and determinant using Cramer's rule
  float tmp[12];
  tmp[0] = src[10] * src[15];
  tmp[1] = src[11] * src[14];
  tmp[2] = src[9] * src[15];
  tmp[3] = src[11] * src[13];
  tmp[4] = src[9] * src[14];
  tmp[5] = src[10] * src[13];
  tmp[6] = src[8] * src[15];
  tmp[7] = src[11] * src[12];
  tmp[8] = src[8] * src[14];
  tmp[9] = src[10] * src[12];
  tmp[10] = src[8] * src[13];
  tmp[11] = src[9] * src[12];

  dst[0] = tmp[0] * src[5] + tmp[3] * src[6] + tmp[4] * src[7];
  dst[0] -= tmp[1] * src[5] + tmp[2] * src[6] + tmp[5] * src[7];
  dst[1] = tmp[1] * src[4] + tmp[6] * src[6] + tmp[9] * src[7];
  dst[1] -= tmp[0] * src[4] + tmp[7] * src[6] + tmp[8] * src[7];
  dst[2] = tmp[2] * src[4] + tmp[7] * src[5] + tmp[10] * src[7];
  dst[2] -= tmp[3] * src[4] + tmp[6] * src[5] + tmp[11] * src[7];
  dst[3] = tmp[5] * src[4] + tmp[8] * src[5] + tmp[11] * src[6];
  dst[3] -= tmp[4] * src[4] + tmp[9] * src[5] + tmp[10] * src[6];
  dst[4] = tmp[1] * src[1] + tmp[2] * src[2] + tmp[5] * src[3];
  dst[4] -= tmp[0] * src[1] + tmp[3] * src[2] + tmp[4] * src[3];
  dst[5] = tmp[0] * src[0] + tmp[7] * src[2] + tmp[8] * src[3];
  dst[5] -= tmp[1] * src[0] + tmp[6] * src[2] + tmp[9] * src[3];
  dst[6] = tmp[3] * src[0] + tmp[6] * src[1] + tmp[11] * src[3];
  dst[6] -= tmp[2] * src[0] + tmp[7] * src[1] + tmp[10] * src[3];
  dst[7] = tmp[4] * src[0] + tmp[9] * src[1] + tmp[10] * src[2];
  dst[7] -= tmp[5] * src[0] + tmp[8] * src[1] + tmp[11] * src[2];

  tmp[0] = src[2] * src[7];
  tmp[1] = src[3] * src[6];
  tmp[2] = src[1] * src[7];
  tmp[3] = src[3] * src[5];
  tmp[4] = src[1] * src[6];
  tmp[5] = src[2] * src[5];
  tmp[6] = src[0] * src[7];
  tmp[7] = src[3] * src[4];
  tmp[8] = src[0] * src[6];
  tmp[9] = src[2] * src[4];
  tmp[10] = src[0] * src[5];
  tmp[11] = src[1] * src[4];

  dst[8] = tmp[0] * src[13] + tmp[3] * src[14] + tmp[4] * src[15];
  dst[8] -= tmp[1] * src[13] + tmp[2] * src[14] + tmp[5] * src[15];
  dst[9] = tmp[1] * src[12] + tmp[6] * src[14] + tmp[9] * src[15];
  dst[9] -= tmp[0] * src[12] + tmp[7] * src[14] + tmp[8] * src[15];
  dst[10] = tmp[2] * src[12] + tmp[7] * src[13] + tmp[10] * src[15];
  dst[10] -= tmp[3] * src[12] + tmp[6] * src[13] + tmp[11] * src[15];
  dst[11] = tmp[5] * src[12] + tmp[8] * src[13] + tmp[11] * src[14];
  dst[11] -= tmp[4] * src[12] + tmp[9] * src[13] + tmp[10] * src[14];
  dst[12] = tmp[2] * src[10] + tmp[5] * src[11] + tmp[1] * src[9];
  dst[12] -= tmp[4] * src[11] + tmp[0] * src[9] + tmp[3] * src[10];
  dst[13] = tmp[8] * src[11] + tmp[0] * src[8] + tmp[7] * src[10];
  dst[13] -= tmp[6] * src[10] + tmp[9] * src[11] + tmp[1] * src[8];
  dst[14] = tmp[6] * src[9] + tmp[11] * src[11] + tmp[3] * src[8];
  dst[14] -= tmp[10] * src[11] + tmp[2] * src[8] + tmp[7] * src[9];
  dst[15] = tmp[10] * src[10] + tmp[4] * src[8] + tmp[9] * src[9];
  dst[15] -= tmp[8] * src[9] + tmp[11] * src[10] + tmp[5] * src[8];

  float det = src[0] * dst[0] + src[1] * dst[1] + src[2] * dst[2] + src[3] * dst[3];
  if (std::abs(det) < 1e-10f)
  {
    // Singular matrix, return identity
    std::memset(&result, 0, sizeof(D3DMATRIX));
    result._11 = result._22 = result._33 = result._44 = 1.0f;
    return result;
  }

  float inv_det = 1.0f / det;
  for (int i = 0; i < 16; i++)
    dst[i] *= inv_det;

  return result;
}

void TransformDecomposer::UpdateViewMatrix(const float* pos_matrices)
{
  // posMatrices is a flat array of 256 floats (xfmem.posMatrices[256]).
  // PosNormalMtxIdx is a 6-bit value (0-63). The float offset is PosNormalMtxIdx * 4.
  // Each 3x4 matrix occupies 12 floats (3 rows x 4 cols).
  // Common PosNormalMtxIdx values: 0, 3, 6, 9, ... (stepping by 3 = one matrix).
  // m_view_matrix_index is in the same units (default 0).
  const u32 float_offset = m_view_matrix_index * 4;
  if (float_offset + 12 > 256)
  {
    m_view_valid = false;
    return;
  }

  m_view_matrix = ExpandPosMatrix(&pos_matrices[float_offset]);
  m_inverse_view_matrix = InvertMatrix(m_view_matrix);
  m_view_valid = true;
}

D3DMATRIX TransformDecomposer::GetViewMatrix() const
{
  return m_view_matrix;
}

D3DMATRIX TransformDecomposer::GetWorldMatrix(u32 pos_mtx_index, const float* pos_matrices) const
{
  if (!m_view_valid)
  {
    // Return identity if no valid view matrix
    D3DMATRIX identity;
    std::memset(&identity, 0, sizeof(D3DMATRIX));
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    return identity;
  }

  // pos_mtx_index is the raw PosNormalMtxIdx from CP state (0-63, 6 bits).
  // The float offset into posMatrices[256] is pos_mtx_index * 4.
  // Each 3x4 matrix occupies 12 floats. Common values: 0, 3, 6, 9, ...

  const u32 float_offset = pos_mtx_index * 4;
  if (float_offset + 12 > 256)
  {
    D3DMATRIX identity;
    std::memset(&identity, 0, sizeof(D3DMATRIX));
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    return identity;
  }

  D3DMATRIX combined = ExpandPosMatrix(&pos_matrices[float_offset]);

  // In GC space (column-vector): combined = view * model (view applied first in matrix multiply)
  // So model = inverse(view) * combined
  // In D3D9 space (transposed): model_d3d = combined_d3d * inverse_view_d3d
  return MultiplyMatrix(combined, m_inverse_view_matrix);
}

D3DMATRIX TransformDecomposer::GetProjectionMatrix(const Projection& proj) const
{
  D3DMATRIX result;
  std::memset(&result, 0, sizeof(D3DMATRIX));

  const auto& raw = proj.rawProjection;

  // GC/Wii projections are column-vector (M * v). D3D9 uses row-vector (v * M).
  // We must transpose the projection matrix.

  // GC/Wii produces Z in [-1,1] (OpenGL NDC), but D3D9 uses [0,1].
  // Remap: z_d3d = 0.5 * z_gl + 0.5
  // This modifies the Z column: new_z_col = 0.5 * old_z_col + 0.5 * old_w_col

  if (proj.type == ProjectionType::Perspective)
  {
    // Transposed for D3D9 row-vector, with Z remapped to [0,1]:
    result._11 = raw[0];
    result._12 = 0.0f;
    result._13 = 0.0f;
    result._14 = 0.0f;
    result._21 = 0.0f;
    result._22 = raw[2];
    result._23 = 0.0f;
    result._24 = 0.0f;
    result._31 = raw[1];
    result._32 = raw[3];
    result._33 = (raw[4] - 1.0f) * 0.5f;  // remap Z
    result._34 = -1.0f;
    result._41 = 0.0f;
    result._42 = 0.0f;
    result._43 = raw[5] * 0.5f;  // remap Z
    result._44 = 0.0f;
  }
  else
  {
    // Transposed for D3D9 row-vector, with Z remapped to [0,1]:
    result._11 = raw[0];
    result._12 = 0.0f;
    result._13 = 0.0f;
    result._14 = 0.0f;
    result._21 = 0.0f;
    result._22 = raw[2];
    result._23 = 0.0f;
    result._24 = 0.0f;
    result._31 = 0.0f;
    result._32 = 0.0f;
    result._33 = raw[4] * 0.5f;            // remap Z
    result._34 = 0.0f;
    result._41 = raw[1];
    result._42 = raw[3];
    result._43 = raw[5] * 0.5f + 0.5f;     // remap Z
    result._44 = 1.0f;
  }

  return result;
}
}  // namespace DX9Remix
