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

  // Matrix-palette skinning scratch, for the same reason. `m_skinning` holds the
  // compact per-vertex bone indices and the snapshotted palette handed to
  // RemixApi::SubmitMesh; `m_flat_blend_indices` is its blend-index array
  // re-expanded to match the flat-normal vertex split, which forks the vertex
  // array and so has to fork this one identically.
  //
  // `m_palette_compact` maps a physical posMatrices row (0-63) to its compact
  // id and `m_palette_named` says whether that entry means anything yet;
  // `m_palette_slots` is the inverse, compact id -> row. The inverse is what
  // makes clearing the forward table cost only the rows a draw actually named
  // rather than a blind 64-entry wipe on a path that runs hundreds of times a
  // frame, and it is also the order the palette snapshot is taken in.
  //
  // Value-initialized `named` flags are exactly the right starting state - no
  // row is claimed before the first draw - which is why the claim is a separate
  // bool rather than a sentinel inside the index table.
  DrawSkinning m_skinning;
  std::vector<u32> m_flat_blend_indices;
  std::vector<u32> m_palette_slots;
  std::array<u8, 64> m_palette_compact = {};
  std::array<bool, 64> m_palette_named = {};
};

// Drop the EFB alpha mask a priming pass may have left pending.
//
// The mask is file-scope state in the translation unit, not a member of
// anything the backend destroys, so without this it survives a game being
// stopped and is still there when the next one boots - and a mask means "cut
// the next same-sized draw to this shape", which is not something to inherit
// from another title. Called from RemixApi::Shutdown alongside the synthesized
// albedos the same idiom produces.
void ResetPendingAlphaMask();
}  // namespace Remix
