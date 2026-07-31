// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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
  // Reused across draws so a 60 Hz translation path does not allocate.
  std::vector<remixapi_HardcodedVertex> m_vertices;
  std::vector<u32> m_indices;
  std::vector<remixapi_HardcodedVertex> m_flat_vertices;
  std::vector<u32> m_flat_indices;
};
}  // namespace Remix
