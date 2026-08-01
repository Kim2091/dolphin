// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

// Vendored from the user's Remix Plus fork; see Externals/remix/remix_c.h.
// Angle brackets are deliberate: MSVC's /external:anglebrackets keeps the
// third-party header out of Dolphin's /W4 /WX budget. It pulls in <windows.h>,
// which is why nothing else in this backend includes it.
#include <remix_c.h>

struct WindowSystemInfo;

namespace Remix
{
class RemixTexture;

// Per-frame draw-classification counters. Logged once per frame when
// Config::GFX_REMIX_LOG_STATS is enabled, so classification is observable
// without a debugger.
struct FrameStats
{
  u32 draws_seen = 0;
  u32 skipped_non_triangle = 0;
  u32 skipped_ortho = 0;
  u32 skipped_efb_texture = 0;
  u32 skipped_degenerate = 0;
  u32 meshes_created = 0;
  u32 instances_drawn = 0;
  u32 sky_draws = 0;
};

// Result of resolving a draw's stage-0 texture to a Remix material. The hash is
// returned alongside the handle because mesh hashes have to fold it: Remix
// handles ARE hashes, and a mesh bakes its material at create time, so two
// material variants of identical geometry must not alias one mesh handle.
struct MaterialRef
{
  remixapi_MaterialHandle handle = nullptr;
  u64 hash = 0;
};

// Owner of everything that talks to the Remix runtime. Created by
// Remix::VideoBackend::Initialize and destroyed by its Shutdown, so its
// lifetime is exactly the backend's.
//
// Threading: every remixapi call made here happens on Dolphin's GPU (video)
// thread - the draw path runs inside VertexManagerBase::Flush, and the frame
// boundary comes from after_frame_event, which is triggered from the same
// thread. That makes the whole thing single-threaded by construction; no
// locking is needed and no create can race the runtime's Present.
class RemixApi
{
public:
  RemixApi();
  ~RemixApi();

  bool Initialize(const WindowSystemInfo& wsi);
  void Shutdown();
  bool IsValid() const { return m_valid; }

  // --- Called from RemixGfx ---
  void SetBoundTexture(u32 index, const RemixTexture* texture);
  void UnbindTexture(const RemixTexture* texture);
  const RemixTexture* GetBoundTexture(u32 index) const;

  // --- Called from RemixVertexManager ---

  // Latches the frame's camera projection. First perspective draw of a frame
  // wins; a frame with only orthographic draws keeps the previous camera.
  void LatchProjection(const std::array<float, 6>& raw_projection);

  // Uploads the texture (once per content hash) and returns the material that
  // references it. A null texture yields the untextured fallback material.
  MaterialRef EnsureMaterial(const RemixTexture* texture, u8 filter_mode, u8 wrap_mode_u,
                             u8 wrap_mode_v);

  // Creates the mesh on a cache miss and always draws one instance of it.
  // category_flags is per-instance (REMIXAPI_INSTANCE_CATEGORY_BIT_*) and so is
  // deliberately NOT folded into the mesh hash - the same geometry tagged two
  // ways still shares one mesh handle, which is what we want.
  void SubmitMesh(const MaterialRef& material,
                  const std::vector<remixapi_HardcodedVertex>& vertices,
                  const std::vector<u32>& indices, const remixapi_Transform& transform,
                  remixapi_InstanceCategoryFlags category_flags);

  FrameStats& Stats() { return m_stats; }

  // Cached at Initialize rather than read per draw - the classifier runs on
  // every batch, and Config::Get is not free at that rate.
  // 0 = ignore the depth heuristic, 1 = tag as sky, 2 = drop sky draws. The
  // heuristic is off by default: it matched the wrong draws in practice (clouds
  // and overlays rather than the horizon dome, which is drawn depth-tested).
  int SkyMode() const { return m_sky_mode; }

  // True when this stage-0 texture was listed in GFX_REMIX_SKY_TEXTURES. This
  // is the reliable route to sky: the runtime's texture-grid categories never
  // reach API draws, but the SKY bit passed on the instance does.
  bool IsSkyTexture(u64 content_hash) const
  {
    return !m_sky_textures.empty() && m_sky_textures.count(content_hash) != 0;
  }

  // True on the occasional frame we dump per-draw depth state on, so the sky
  // heuristic can be checked against what the game actually emits instead of
  // being guessed at. Bounded to one frame in 120, first few draws only.
  bool ShouldTraceDraws() const { return m_log_stats && (m_frame_index % 120) == 0; }

private:
  struct MeshEntry
  {
    remixapi_MeshHandle handle = nullptr;
    u64 last_used_frame = 0;
  };

  struct LightEntry
  {
    remixapi_LightHandle handle = nullptr;
  };

  void OnAfterFrame();
  void SetupCamera();
  void SubmitLights();
  void SubmitFallbackTriangle();
  void ReapIdleMeshes();
  bool UploadTexture(const RemixTexture& texture);
  bool EnsureFallbackMesh();
  void DestroyAllHandles();

  remixapi_Interface m_interface = {};
  remixapi_HMODULE m_dll = nullptr;
  IDirect3D9Ex* m_d3d9 = nullptr;
  IDirect3DDevice9Ex* m_d3d9_device = nullptr;
  bool m_valid = false;

  Common::EventHook m_after_frame_event;

  std::array<const RemixTexture*, 8> m_bound_textures = {};

  std::unordered_map<u64, remixapi_TextureHandle> m_textures;
  std::unordered_map<u64, remixapi_MaterialHandle> m_materials;
  std::unordered_map<u64, MeshEntry> m_meshes;
  // Mesh hashes whose create or draw faulted inside the runtime. Never retried:
  // a handle that made the runtime throw once will do it every frame.
  std::unordered_set<u64> m_poisoned_meshes;

  std::array<float, 6> m_raw_projection = {};
  bool m_projection_latched = false;
  bool m_camera_valid = false;

  std::array<LightEntry, 8> m_lights = {};
  remixapi_LightHandle m_fallback_light = nullptr;
  remixapi_MeshHandle m_fallback_mesh = nullptr;

  FrameStats m_stats;
  u64 m_frame_index = 0;
  bool m_log_stats = true;
  int m_sky_mode = 0;
  std::unordered_set<u64> m_sky_textures;
  // Per-frame object-picking id. Must be non-zero for the runtime to record a
  // pick, and must differ per draw for clicks to resolve to one surface.
  u32 m_next_picking_value = 1;
  float m_light_scale = 1.0f;
};

extern std::unique_ptr<RemixApi> g_remix_api;

}  // namespace Remix
