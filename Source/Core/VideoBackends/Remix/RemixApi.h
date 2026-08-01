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

// For the XF Light register layout. The backend snapshots those registers on
// the draw path rather than reading them at frame end, so the struct has to be
// storable here.
#include "VideoCommon/XFMemory.h"

// Vendored from the user's Remix Plus fork; see Externals/remix/remix_c.h.
// Angle brackets are deliberate: MSVC's /external:anglebrackets keeps the
// third-party header out of Dolphin's /W4 /WX budget. It pulls in <windows.h>,
// which is why nothing else in this backend includes it.
#include <remix_c.h>

struct WindowSystemInfo;

namespace Remix
{
class RemixTexture;

// A 3x4 row-major affine transform - the layout GX's xfmem.posMatrices and
// remixapi_Transform::matrix both already use, so conversion is a copy. The
// implicit fourth row is [0 0 0 1].
using Affine = std::array<float, 12>;

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
  // Draws that write neither colour nor alpha, or whose alpha test can never
  // pass. Depth-only geometry in a backend that has no depth buffer.
  u32 skipped_invisible = 0;
  u32 meshes_created = 0;
  u32 instances_drawn = 0;
  u32 sky_draws = 0;
  // Draws whose projection differed from the frame's reference and were folded
  // back onto it, and how many of those carried an off-centre frustum. Both are
  // zero on a game that never switches projection mid-frame.
  u32 projection_corrected = 0;
  u32 projection_oblique = 0;
  // Draws whose projection could not be folded onto the reference at all (the
  // implied scale was absurd, or the reference was degenerate). These render
  // through the wrong frustum, so a non-zero count is a real defect - not the
  // same thing as a draw that simply needed no correction.
  u32 projection_uncorrectable = 0;
  // Draws arriving after the per-frame variant table filled up. Non-zero means
  // the projection log below is incomplete, not that anything rendered wrong.
  u32 projection_overflow = 0;

  // Camera recovery. `view_candidates` is how many draws persisted from last
  // frame and so could vote on the camera delta; `view_inliers` is how many
  // agreed with the winner. A healthy frame has inliers close to candidates -
  // most of the world is static. `w_stable / w_compared` is the real verdict:
  // it counts objects whose recovered world transform did NOT change between
  // frames, which is exactly what the decomposition is supposed to achieve.
  u32 view_samples = 0;
  u32 view_candidates = 0;
  u32 view_inliers = 0;
  u32 view_reanchors = 0;
  u32 w_stable = 0;
  u32 w_compared = 0;
  // Mesh hashes submitted more than once in the frame. Such a hash has no unique
  // cross-frame correspondence, so the delta built from it pairs two arbitrary
  // instances - Wind Waker's ocean tiles and repeated props are exactly this.
  u32 view_duplicates = 0;
  // How many of those were actually dropped from the electorate. Zero with the
  // electorate fix off, which is what makes the two counters worth separating.
  u32 view_dup_excluded = 0;

  // Where TEV stage 0's rasterized colour came from, by GX's own rules.
  // `color_register` is the case that was being dropped outright - a game
  // tinting geometry through GXSetChanMatColor - and `color_none` counts draws
  // whose stage 0 does not rasterize an enabled colour channel at all, which
  // read as untinted.
  u32 color_vertex = 0;
  u32 color_register = 0;
  u32 color_none = 0;

  // Draws whose stage-0 texture coordinate went through GX texgen, and how many
  // of those produced something a raw read of attribute 0 would not have. A game
  // reporting zero non-trivial texgens cannot have a texgen bug.
  u32 texgen_generated = 0;
  u32 texgen_nontrivial = 0;

  // Draws submitted as something other than plain opaque geometry. `blended` is
  // the whole point of translating blend state; `logic_op` counts draws using a
  // raster logic op, which has no Remix equivalent and stays opaque.
  u32 blended = 0;
  u32 alpha_tested = 0;
  u32 logic_op = 0;

  // Draws that had no normals and so got generated flat ones, and how many of
  // those needed the cross product negating - a positive viewport, a mirroring
  // modelview, or a game culling front faces rather than back.
  u32 normals_generated = 0;
  u32 normals_flipped = 0;

  // Union of every draw's XF light enable mask over the frame. SubmitLights
  // samples that mask once, at frame end, from whatever channel state the last
  // draw happened to leave behind - so this differing from what SubmitLights
  // saw means lights are being dropped by a per-frame read of per-draw state.
  u32 draw_light_mask = 0;

  // XF lights by the kind they resolved to. Distant vs sphere is the whole
  // point of reading the attenuation function - a game whose suns show up as
  // spheres is a game rendering nearly black.
  u32 lights_distant = 0;
  u32 lights_sphere = 0;
  u32 lights_spot = 0;

  // How per-draw the light REGISTERS turn out to be. `lights_rewritten` counts
  // claims of a slot whose 64 register bytes had changed since the first draw
  // that claimed it this frame; `lights_conflicted` counts claims that disagree
  // about the attenuation function. First claim wins either way - these exist so
  // that a game which animates a light slot mid-frame stops being invisible.
  // `lights_alpha_only` counts submitted lights that no COLOUR channel ever
  // claimed: GX feeds those only into a channel's alpha (TransformUnit.cpp
  // LightAlpha reads color[0] alone), so submitting them as full RGB lights is
  // an over-translation. Counter first, no behaviour change.
  u32 lights_rewritten = 0;
  u32 lights_conflicted = 0;
  u32 lights_alpha_only = 0;

  // Channel semantics Remix has no way to reproduce, counted so a "the scene is
  // too dark" report can be answered from the log. Remix always renders a
  // clamped N.L; a diffusefunc of None makes a GX light behave as pure ambient
  // and Sign lets it go negative, neither of which survives translation. A Spec
  // light is a specular-only contribution GX adds through a second channel, so
  // rendering it as an ordinary diffuse light double-counts it - kept, but no
  // longer invisible.
  u32 lights_diffuse_none = 0;
  u32 lights_diffuse_sign = 0;
  u32 lights_spec = 0;
  // A lit draw whose channel ambient register was bright enough to matter. GX
  // ambient has no Remix analogue at all - the path tracer's GI has to stand in
  // for it - so this quantifies how much of the frame's light was ambient before
  // anyone reaches for RemixLightScale.
  bool ambient_bright = false;
};

// What one draw said about the XF lights it switched on. Every field here is
// per-DRAW state: the enable mask, the attenuation function and the diffuse
// function all live on the referencing channel rather than on the light, and
// reading any of them once at frame end sees only the frame's last draw.
struct DrawLightState
{
  u32 mask = 0;
  // Subset of `mask` claimed by a COLOUR channel rather than an alpha one.
  u32 color_mask = 0;
  std::array<u8, 8> attenuation = {};
  std::array<u8, 8> diffuse = {};
  // Any lit channel of this draw whose ambient register was bright.
  bool ambient_bright = false;
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

// Remix's RtTextureArgSource, as passed through remixapi_InstanceInfoBlendEXT.
// Named here because the C header carries the fields as bare uint32_t.
enum : u8
{
  REMIX_TEX_ARG_NONE = 0,
  REMIX_TEX_ARG_TEXTURE = 1,
  REMIX_TEX_ARG_VERTEX_COLOR0 = 2,
  REMIX_TEX_ARG_TFACTOR = 3,
};

// Remix's DxvkRtTextureOperation (D3DTEXTUREOP by another name).
enum : u8
{
  REMIX_TEX_OP_DISABLE = 0,
  REMIX_TEX_OP_SELECT_ARG1 = 1,
  REMIX_TEX_OP_SELECT_ARG2 = 2,
  REMIX_TEX_OP_MODULATE = 3,
};

// The parts of a draw's GX state that Remix consumes per INSTANCE rather than
// per material, through remixapi_InstanceInfoBlendEXT. Deliberately outside
// material and mesh identity: two draws differing only here still share one mesh
// handle, which is the whole reason a per-draw register tint goes in tfactor
// rather than being baked into the vertex colours.
//
// The defaults reproduce the runtime's own defaults for an instance with no
// BlendEXT attached, so attaching one with this struct untouched changes
// nothing.
struct DrawBlendState
{
  u8 color_arg1 = REMIX_TEX_ARG_TEXTURE;
  u8 color_arg2 = REMIX_TEX_ARG_NONE;
  u8 color_operation = REMIX_TEX_OP_MODULATE;
  u8 alpha_arg1 = REMIX_TEX_ARG_TEXTURE;
  u8 alpha_arg2 = REMIX_TEX_ARG_NONE;
  u8 alpha_operation = REMIX_TEX_OP_SELECT_ARG1;
  // Packed 0xAARRGGBB - Remix reads tFactor as B8G8R8A8, the same packing as
  // remixapi_HardcodedVertex::color.
  u32 tfactor = 0xFFFFFFFFu;
  // GC/Wii geometry very often carries lighting baked into its vertex colours.
  // The runtime divides such a colour by its own largest component so only the
  // hue survives and the path tracer supplies the brightness; that is wrong when
  // the vertex colour is a genuine material colour instead.
  bool vertex_color_is_baked_lighting = false;

  // Alpha test and blending, in Vulkan's numbering because that is what the
  // runtime's legacy classifier (rtx_instance_manager.cpp:716-820) matches
  // against to decide translucent vs emissive vs multiplicative. Reaching that
  // classifier at all needs the material to set useDrawCallAlphaState; with it
  // clear the runtime answers from the material instead and these are inert.
  //
  // Defaults are "opaque, no test", i.e. what the material used to say.
  bool alpha_test_enabled = false;
  u8 alpha_test_compare = 7;   // VK_COMPARE_OP_ALWAYS, and GX's Always
  u8 alpha_test_reference = 0;
  bool blend_enabled = false;
  u8 src_color_factor = 1;  // VK_BLEND_FACTOR_ONE
  u8 dst_color_factor = 0;  // VK_BLEND_FACTOR_ZERO
  u8 color_blend_op = 0;    // VK_BLEND_OP_ADD
  u8 src_alpha_factor = 1;
  u8 dst_alpha_factor = 0;
  u8 alpha_blend_op = 0;
  u8 write_mask = 0xF;  // R | G | B | A
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

  // Accounts for one perspective draw's projection and returns its slot in the
  // frame's variant table, or -1 if the table is full. The first perspective
  // draw of a frame also latches the reference projection that SetupCamera
  // turns into the camera; a frame with only orthographic draws keeps the
  // previous camera.
  int ObserveProjection(const std::array<float, 6>& raw_projection);

  // Records that a draw which actually reached SubmitMesh used variant `slot`.
  // The reference is "first perspective draw of the frame", and these counts
  // are how we find out whether that draw is the one carrying the scene.
  void NoteProjectionUse(int slot, u32 vertex_count);

  // The frame's reference projection - the one SetupCamera is built from, and
  // therefore the one every other draw has to be folded onto. False until a
  // perspective draw has been seen this frame, and also false when that draw's
  // frustum was too degenerate for the camera to represent: SetupCamera then
  // falls back to a fabricated default that encodes none of these raw terms, so
  // folding draws onto them would aim at a camera that is not there.
  bool HasReferenceProjection() const { return m_projection_latched && m_reference_usable; }
  const std::array<float, 6>& ReferenceProjection() const { return m_raw_projection; }

  // False restores the pre-fix behaviour: submit each draw's modelview as-is
  // and let the frame's single camera misframe everything that does not share
  // the reference projection.
  bool ProjectionFixEnabled() const { return m_projection_fix; }

  // False submits the raw vertex colour and no texture-stage state at all, which
  // is the pre-fix behaviour: the runtime's defaults never read a vertex colour
  // and have no way to hear about xfmem.matColor.
  bool GxColorEnabled() const { return m_gx_color; }

  // False passes vertex attribute 0's texture coordinate through untouched,
  // which is the pre-fix behaviour: right for the common identity-matrix case
  // and wrong for every animated or scaled texture matrix.
  bool GxTexGenEnabled() const { return m_gx_texgen; }

  // False keeps every draw opaque with its alpha test on the material, which is
  // the pre-fix behaviour. On, the material defers to the draw call and the
  // per-instance blend state below becomes the thing the runtime reads.
  bool GxBlendEnabled() const { return m_gx_blend; }

  // Uploads the texture (once per content hash) and returns the material that
  // references it. A null texture yields the untextured fallback material.
  // alpha_test_type / alpha_reference come from the draw's GX alpha test and
  // fold into material identity along with the sampler state, since Remix bakes
  // both into the material.
  MaterialRef EnsureMaterial(const RemixTexture* texture, u8 filter_mode, u8 wrap_mode_u,
                             u8 wrap_mode_v, u8 alpha_test_type, u8 alpha_reference);

  // Creates the mesh on a cache miss and queues one instance of it. The draw is
  // NOT emitted here: the recovered camera is not known until the frame's draws
  // have all been seen, so instances are held and flushed after it is. See
  // FlushPendingInstances.
  //
  // category_flags is per-instance (REMIXAPI_INSTANCE_CATEGORY_BIT_*) and so is
  // deliberately NOT folded into the mesh hash - the same geometry tagged two
  // ways still shares one mesh handle, which is what we want.
  //
  // raw_modelview is the draw's untouched GX modelview, or nullptr on the
  // matrix-palette path where there is no single one. It is what the camera
  // estimator votes on, and it must be the RAW matrix: a projection correction
  // present in both frames conjugates the inter-frame delta instead of
  // cancelling out of it, which yields the right rotation in the wrong basis.
  void SubmitMesh(const MaterialRef& material,
                  const std::vector<remixapi_HardcodedVertex>& vertices,
                  const std::vector<u32>& indices, const remixapi_Transform& transform,
                  remixapi_InstanceCategoryFlags category_flags, const DrawBlendState& blend,
                  const float* raw_modelview);

  // Records which XF lights a draw switched on, and what each one MEANS to that
  // draw - GX puts the attenuation function on the referencing channel, not on
  // the light, so the same eight light registers are directional or positional
  // depending on who is looking. Accumulated across the frame because both are
  // per-draw state: reading them once at frame end sees only whatever the last
  // draw left behind, which in Wind Waker is a channel with lighting off, so
  // every light in the scene was being dropped.
  //
  // First draw to claim a light decides its kind; a light referenced twice with
  // conflicting functions is not something GX geometry can express anyway.
  //
  // The light REGISTERS are per-draw state for the same reason, and are
  // snapshotted here on first claim rather than read at frame end: XFStructs.cpp
  // flushes the vertex manager on every XF write, so the register values at draw
  // time are well defined, while the frame-end values are whatever the last
  // draw happened to leave behind.
  //
  void NoteDrawLights(const DrawLightState& state);

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
    // Which Remix light kind the handle was created as. Baked in at create
    // time, so a GX light that switches attenuation function needs a new one.
    bool positional = false;
    // Fingerprint of everything the trace line reports except the position, so
    // the trace fires on a create or a genuine parameter change and stays quiet
    // while a light merely moves.
    u64 trace_key = 0;
  };

  // One distinct projection seen during a frame, with how much geometry rode on
  // it. Bounded because a runaway game must not be able to grow this per frame.
  struct ProjectionVariant
  {
    std::array<float, 6> raw = {};
    u32 draws = 0;
    u32 vertices = 0;
  };
  static constexpr size_t MAX_PROJECTION_VARIANTS = 8;

  // A draw held back until the frame's camera is known.
  struct PendingInstance
  {
    remixapi_MeshHandle mesh = nullptr;
    remixapi_Transform transform = {};
    remixapi_InstanceCategoryFlags category_flags = 0;
    DrawBlendState blend;
    u64 mesh_hash = 0;
  };
  // How many draws may vote on the camera delta, and how many of those the
  // O(n^2) consensus pass will consider as candidates.
  //
  // 256 was a cap on the ELECTORATE, and the samples live in an unordered_map -
  // so on a scene submitting well over a thousand instances it was an arbitrary
  // submission-order slice, and a slice full of moving objects outvotes a static
  // world. The vote is O(48*n), so 2048 costs about 80 KB and a few hundred
  // thousand small matrix compares. The old value is kept for the A/B.
  static constexpr size_t MAX_VIEW_SAMPLES = 2048;
  static constexpr size_t LEGACY_MAX_VIEW_SAMPLES = 256;
  static constexpr size_t MAX_VIEW_CANDIDATES = 48;
  // Draws below this vertex count are more likely to be effects or overlays
  // than world geometry, and a bad vote costs more than a missing one.
  static constexpr u32 MIN_VIEW_SAMPLE_VERTICES = 16;
  static constexpr u32 VIEW_MISS_STREAK_BEFORE_REANCHOR = 4;
  // Ceiling on how many agreeing draws the consensus may demand, so that a
  // scene full of moving objects cannot price the static ones out of the vote.
  // Six draws agreeing on one rigid delta is already decisive - they would have
  // to be moving in exact lockstep to fake it - and a genuine cut fails on
  // having almost no correspondences at all, not on this number.
  static constexpr u32 MAX_VIEW_CONSENSUS_REQUIRED = 6;

  void OnAfterFrame();
  void LogProjectionVariants();
  void EstimateView();
  void FlushPendingInstances();
  void LogCameraRecovery();
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
  bool m_reference_usable = false;
  bool m_camera_valid = false;
  std::array<ProjectionVariant, MAX_PROJECTION_VARIANTS> m_projection_variants = {};
  u32 m_projection_variant_count = 0;
  bool m_projection_fix = true;
  bool m_trace_projections = false;
  bool m_gx_color = true;
  bool m_gx_texgen = true;
  bool m_gx_blend = true;
  bool m_gx_light_fix = true;

  // Camera recovery state. m_view maps world -> view and is built by
  // integrating per-frame deltas from an arbitrary origin; m_view_inverse is
  // what turns each draw's modelview back into a world transform. Both are
  // identity while recovery is off, which is exactly the v1 behaviour of
  // treating view space as world space.
  std::vector<PendingInstance> m_pending_instances;
  std::unordered_map<u64, Affine> m_view_samples;
  std::unordered_map<u64, Affine> m_view_samples_previous;
  // Mesh hashes seen more than once this frame. Diagnostic here; the electorate
  // fix is what acts on them.
  std::unordered_set<u64> m_view_duplicate_hashes;
  // How much camera motion the estimator claimed this frame, and the running
  // maxima since the last camera log line. A rotation that spikes every frame
  // while the world visibly holds still is the estimator absorbing an object's
  // motion, which is the one thing that can make the sky rock on its own.
  float m_view_delta_rotation_deg = 0.0f;
  float m_view_delta_translation = 0.0f;
  float m_view_max_rotation_deg = 0.0f;
  float m_view_max_translation = 0.0f;
  Affine m_view = {};
  Affine m_view_inverse = {};
  Affine m_view_inverse_previous = {};
  u32 m_view_miss_streak = 0;
  bool m_camera_recovery = false;
  bool m_view_electorate_fix = true;
  bool m_view_hold_on_miss = true;

  // Per-frame accumulation of the above, cleared with the stats.
  u32 m_frame_light_mask = 0;
  // Subset of m_frame_light_mask claimed by a colour channel. Diagnostic only.
  u32 m_frame_light_color_mask = 0;
  std::array<u8, 8> m_frame_light_attenuation = {};
  std::array<u8, 8> m_frame_light_diffuse = {};
  // The XF light registers as they stood at the draw that first claimed each
  // slot this frame. SubmitLights consumes these, never xfmem directly.
  std::array<Light, 8> m_frame_lights = {};

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
  // Stand-in for D3D9's Light.Range, which the ported radiance conversion needs
  // and GX does not have. Only consulted when the distance-attenuation
  // polynomial never falls off (GX_DA_OFF).
  float m_light_range = 5000.0f;
};

extern std::unique_ptr<RemixApi> g_remix_api;

}  // namespace Remix
