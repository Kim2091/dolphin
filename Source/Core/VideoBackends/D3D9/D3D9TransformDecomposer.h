// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d9.h>

#include "Common/CommonTypes.h"
#include "Common/Matrix.h"

struct Projection;
enum class ProjectionType : u32;

namespace DX9Remix
{
// Decomposes Dolphin's combined world*view position matrices into separate
// World, View, and Projection matrices for RTX Remix compatibility.
//
// Dolphin's posMatrix[i] = model_transform * view_transform (combined).
// We use posMatrix[view_index] as the reference view matrix (default index 0),
// then compute: world_i = posMatrix[i] * inverse(posMatrix[view_index])
class TransformDecomposer
{
public:
  TransformDecomposer();

  // Update the cached view matrix from the position matrices.
  // Should be called when the view matrix index's data changes.
  void UpdateViewMatrix(const float* pos_matrices);

  // Get the D3D9 world matrix for a given position matrix index.
  D3DMATRIX GetWorldMatrix(u32 pos_mtx_index, const float* pos_matrices) const;

  // Get the cached view matrix.
  D3DMATRIX GetViewMatrix() const;

  // Convert Dolphin's projection to a D3D9 projection matrix.
  D3DMATRIX GetProjectionMatrix(const Projection& proj) const;

  // Set which position matrix index to use as the view/camera matrix.
  void SetViewMatrixIndex(u32 index) { m_view_matrix_index = index; }
  u32 GetViewMatrixIndex() const { return m_view_matrix_index; }

private:
  // Expand a 3x4 row-major position matrix (12 floats) to a D3D9 4x4 matrix
  static D3DMATRIX ExpandPosMatrix(const float* mtx34);

  // Compute the inverse of a 4x4 matrix
  static D3DMATRIX InvertMatrix(const D3DMATRIX& m);

  // Multiply two 4x4 matrices
  static D3DMATRIX MultiplyMatrix(const D3DMATRIX& a, const D3DMATRIX& b);

  u32 m_view_matrix_index = 0;
  D3DMATRIX m_view_matrix;
  D3DMATRIX m_inverse_view_matrix;
  bool m_view_valid = false;
};
}  // namespace DX9Remix
