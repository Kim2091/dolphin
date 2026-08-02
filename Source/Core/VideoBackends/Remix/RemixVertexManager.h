// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

#include "VideoBackends/Remix/RemixApi.h"

#include "VideoCommon/VertexManagerBase.h"

namespace Remix
{
// The GX -> remixapi translation point.
//
// By the time DrawCurrentBatch runs, VertexManagerBase has already written the
// flush batch's post-vertex-loader vertices into plain CPU memory (the base
// class's ResetBuffer/CommitBuffer are no-ops for us, exactly as they are for
// the Null backend), so this is where a draw call is readable as data rather
// than as GPU commands.
class VertexManager final : public VertexManagerBase
{
public:
  VertexManager();
  ~VertexManager() override;

protected:
  void DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex) override;

private:
  // Whether the console's scissor for the current register values rasterizes
  // nothing at all. Cached because BPFunctions::ComputeScissorRects allocates a
  // vector, and a frame has hundreds of world draws against a handful of scissor
  // changes. Keyed on the three registers emptiness actually depends on: the
  // viewport only ORDERS the resulting rectangles (ScissorResult::IsWorse), it
  // can never empty the list, so leaving it out of the key is exact rather than
  // approximate.
  bool ScissorIsEmpty();
  u32 m_scissor_key_tl = 0;
  u32 m_scissor_key_br = 0;
  u32 m_scissor_key_off = 0;
  bool m_scissor_key_valid = false;
  bool m_scissor_empty = false;

  // The current draw's viewport as a screen mapping - the raw rect plus the
  // scissor-adjusted centre every hardware backend positions by. Cached for the
  // same reason ScissorIsEmpty is: deriving the centre needs
  // BPFunctions::ComputeScissorRects, which allocates, and a well-behaved game
  // holds one viewport for the whole frame. Keyed on every register the
  // derivation reads - the four viewport floats AND the three scissor registers
  // - so the cache is exact rather than approximate.
  const DrawViewport& CurrentDrawViewport();
  DrawViewport m_viewport_cached = {};
  std::array<float, 4> m_viewport_key_rect = {};
  u32 m_viewport_key_tl = 0;
  u32 m_viewport_key_br = 0;
  u32 m_viewport_key_off = 0;
  bool m_viewport_key_valid = false;

  // Reused across draws so a 60 Hz translation path does not allocate.
  std::vector<remixapi_HardcodedVertex> m_vertices;
  std::vector<u32> m_indices;
  std::vector<remixapi_HardcodedVertex> m_flat_vertices;
  std::vector<u32> m_flat_indices;
};
}  // namespace Remix
