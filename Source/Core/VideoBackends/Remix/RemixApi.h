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
// For MathUtil::Rectangle, the type VideoCommon hands EFB copy source rects in.
#include "Common/MathUtil.h"

#include "VideoBackends/Remix/RemixUiRaster.h"

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
  // The largest cluster that disagreed with the winner, and how often that was
  // close enough for the calm-camera prior to decide instead of raw size. A
  // runner-up near the winner's size means a big rigid moving thing is competing
  // with the static world; a genuinely turning camera has no runner-up at all,
  // because everything static votes together.
  u32 view_runnerup_inliers = 0;
  u32 view_tie_breaks = 0;

  // Where TEV stage 0's rasterized colour came from, by GX's own rules.
  // `color_register` is the case that was being dropped outright - a game
  // tinting geometry through GXSetChanMatColor - and `color_none` counts draws
  // whose stage 0 does not rasterize an enabled colour channel at all, which
  // read as untinted.
  u32 color_vertex = 0;
  u32 color_register = 0;
  u32 color_none = 0;
  // Draws whose ras-consuming COLOUR stage and ras-consuming ALPHA stage named
  // different XF channels, and those channels different vertex attributes. One
  // u32 of vertex colour cannot carry both, so the colour half wins - this
  // counts how often that compromise was made.
  u32 ras_channel_split = 0;

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
  // Sky auto-detection. `sky_auto_candidates` is how many meshes satisfied the
  // transform signature this frame; `sky_auto_tagged` is how many instances were
  // actually given the SKY bit because of it. The sticky classified-set size is
  // reported alongside them and holding constant frame to frame IS the stability
  // check - a set that keeps growing is a classifier chasing noise.
  u32 sky_auto_candidates = 0;
  u32 sky_auto_classified = 0;
  u32 sky_auto_tagged = 0;
  // Of the tagged instances, how many took IGNORE instead of SKY because the draw
  // had no texture. Splitting the counter is what makes the experiment readable:
  // tagged 7 / ignored 4 says the four untextured Wind Waker domes were dropped
  // outright rather than handed to the sky path.
  u32 sky_auto_ignored = 0;
  // Classified sky meshes removed from the camera electorate. A skybox votes for
  // the camera's rotation delta with the translation missing, which is an
  // actively wrong hypothesis rather than merely a useless one.
  u32 view_sky_excluded = 0;
  // Modelview histogram. `modelview_samples` counts draws that offered a single
  // modelview to bucket; `modelview_palette` counts the ones that had none, so a
  // dominant matrix's share can be read against the right denominator.
  u32 modelview_samples = 0;
  u32 modelview_palette = 0;
  // Orthographic draws placed on the world-space UI plane, and those that could
  // not be (no camera yet, or a degenerate ortho projection). A game whose menus
  // are missing while `ui_placed` is non-zero has a placement bug, not a
  // classification one.
  u32 ui_placed = 0;
  u32 ui_unplaceable = 0;
  // UI draws refused, and why. Both are removals of content that WOULD have been
  // painted, so they belong in the frame line next to ui_placed rather than in a
  // trace-only log: a menu that vanishes in some other game is diagnosed by
  // reading these, and by nothing else.
  //
  // `ui_skipped_dst_alpha`  - blend factors that read EFB alpha, which the
  //   overlay does not have. Expect at most about one per frame; more than that
  //   and the rule is eating legitimate UI.
  // `ui_skipped_efb_copy_tex` - textured from an EFB copy's destination, which
  //   this backend never writes, so the texels are stale memory.
  u32 ui_skipped_dst_alpha = 0;
  u32 ui_skipped_efb_copy_tex = 0;
  // EFB copies the game triggered this frame, and the subset that could hide a
  // UI draw: not the XFB copy, and carrying the clear bit, so the region it
  // took is wiped off the EFB before anything reaches the screen. A frame whose
  // `efb_copies_scratch` is zero cannot be composing anything off-screen, which
  // is the whole hypothesis in one number.
  u32 efb_copies = 0;
  u32 efb_copies_scratch = 0;
  // Microseconds spent rasterizing the overlay, and handing it to the runtime.
  // Split because they have different fixes: the first is this backend's inner
  // loop, the second is a per-frame staging-buffer create plus a multi-megabyte
  // memcpy inside the runtime.
  u64 ui_raster_us = 0;
  u64 ui_upload_us = 0;
  // UI draws whose final alpha was resolved from the TEV chain rather than
  // guessed from stage-0 texture alpha. A game whose HUD is drawn when it should
  // be hidden, with this at zero, is a game whose chain the evaluator gave up on.
  u32 ui_tev_alpha = 0;
  // Why the TEV alpha evaluator gave up, when it did.
  u32 tev_bail_stages = 0;
  u32 tev_bail_konst = 0;
  u32 tev_bail_compare = 0;
  u32 tev_bail_rasterized = 0;
  // World draws the console would have rasterized nothing of, because the game
  // scissored them down to an empty rect. Without the skip these are fully
  // visible geometry that also casts shadows; a non-zero count while something
  // vanishes is what says the rule is over-reaching.
  u32 skipped_scissor = 0;

  // A lit draw whose channel ambient register was bright enough to matter. GX
  // ambient has no Remix analogue at all - the path tracer's GI has to stand in
  // for it - so this quantifies how much of the frame's light was ambient before
  // anyone reaches for RemixLightScale.
  bool ambient_bright = false;

  // Sub-screen viewports, instrument only. The world path reads xfmem.viewport
  // for the winding sign alone (RemixVertexManager.cpp:319), while every
  // hardware backend POSITIONS the draw by it (BPFunctions.cpp:192-193). A game
  // that renders split screens or picture-in-picture switches the viewport
  // mid-frame and would be misplaced here; one that never does cannot have a
  // viewport bug. `viewport_changed` counts world draws whose viewport rect
  // differs from the frame's first-seen one.
  //
  // Deliberately no knob and no behaviour: geometric handling waits for a game
  // that demonstrates the symptom.
  u32 viewport_changed = 0;
  bool viewport_seen = false;
  std::array<float, 4> viewport_first = {};
};

// One EFB copy the game triggered, stamped WHERE IT HAPPENED. Recorded rather
// than re-read at frame end for the usual reason: every field here is per-copy
// BP state, and a frame-end read of bpmem sees only the last copy's.
//
// This backend executes no copies (RemixTextureCache::CopyEFB writes nothing),
// but the console-side consequence of one still decides what is on screen: a
// copy that takes a region and sets the clear bit wipes that region off the EFB
// (BPStructs.cpp:377-393), so anything drawn into it beforehand never reaches
// the XFB.
struct EfbCopyEvent
{
  // FrameStats::draws_seen at the moment the copy fired. Every draw increments
  // that counter (RemixVertexManager.cpp:1016), so `copy.seq >= draw.seq`
  // orders the two events exactly, with no extra clock.
  u32 seq = 0;
  // EFB-native source rect, right/bottom exclusive - BPStructs.cpp:249-256.
  MathUtil::Rectangle<int> rect = {};
  u32 dst_addr = 0;
  u8 copy_format = 0;
  // EFBCopyFormat::XFB, i.e. the copy that ends the frame rather than one that
  // builds an off-screen image.
  bool xfb = false;
  bool clear = false;
  // A box-filtered 2:1 downsample (UPE_Copy::half_scale). Bloom and glow chains
  // use it; a pixel-exact compose of the same region cannot.
  bool half_scale = false;
};

// One UI draw's EFB-space footprint, recorded in the draw path so it can be
// compared against the frame's copies. In the SAME space and convention as
// EfbCopyEvent::rect - EFB units, right/bottom exclusive - because the whole
// point is the containment test between them.
struct UiDrawFootprint
{
  u32 seq = 0;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  u64 texture_hash = 0;
  bool tev_alpha_known = false;
  bool depth_test = false;
  bool depth_write = false;
};

// Per-draw facts the sky classifier RECORDS but never classifies on. They exist
// so the weaker signals - depth state, submission order, texture identity - can
// be checked against the transform signature's verdicts instead of guessed at,
// and so a classification log line names something the user can paste into
// rtx.skyBoxGeometries or RemixSkyTextures to make it permanent.
struct DrawDiagnostics
{
  // The stage-0 TEXTURE content hash, not the material hash: the material hash
  // folds in sampler state and would never match a hand-written list.
  u64 texture_hash = 0;
  bool depth_test = false;
  u8 depth_func = 0;
  bool depth_write = false;
  u32 draw_index = 0;
  // Which xfmem.posMatrices slot supplied this draw's modelview, or
  // NO_POSITION_MATRIX on the matrix-palette path where the vertices name their
  // own and there is no single one. Carried for the modelview histogram: "which
  // slot holds the view matrix" is the question that instrument exists to answer,
  // and the slot is known here and nowhere downstream.
  static constexpr u32 NO_POSITION_MATRIX = 0xFFFFFFFFu;
  u32 position_matrix = NO_POSITION_MATRIX;
};

// One distinct modelview VALUE seen during a frame, and everything that used it.
// Keyed on the exact 12 floats: a game that computes V*M for an identity M gets
// V back bit for bit (multiplying by one and adding zero is exact), so every
// draw sharing a view matrix lands in one bucket with no tolerance needed.
struct ModelviewBucket
{
  Affine matrix = {};
  // Distinct mesh hashes, not draws. A hundred instances of one prop is one mesh
  // sharing one matrix; a hundred DIFFERENT meshes sharing one matrix is the
  // signature that says they were authored in a common space.
  std::unordered_set<u64> meshes;
  u32 draws = 0;
  u32 vertices = 0;
  // Bit per xfmem.posMatrices slot that held this value during the frame.
  u64 slots = 0;
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

  // The RAW GX alpha test, both comparators and the op joining them. The three
  // fields above are the single-comparator reduction Remix's material state can
  // express; the software UI raster can run the real thing, and a draw the
  // console rejects through an Or/Xor configuration must not be drawn just
  // because the reduction had to give up and say Always.
  u8 raw_alpha_compare0 = 7;
  u8 raw_alpha_compare1 = 7;
  u8 raw_alpha_logic = 0;
  u8 raw_alpha_reference0 = 0;
  u8 raw_alpha_reference1 = 0;

  // Final alpha as the TEV chain resolves it, sampled at the four corners of
  // (texture alpha, rasterized alpha) in the order (0,0) (1,0) (0,1) (1,1) and
  // bilinearly interpolated between - which is exact for GX's modulate chains.
  // False when the chain is not bilinear in that pair, in which case nothing
  // downstream may rely on these.
  bool tev_alpha_known = false;
  std::array<float, 4> tev_alpha_corners = {1.0f, 1.0f, 1.0f, 1.0f};
  bool blend_enabled = false;
  u8 src_color_factor = 1;  // VK_BLEND_FACTOR_ONE
  u8 dst_color_factor = 0;  // VK_BLEND_FACTOR_ZERO
  u8 color_blend_op = 0;    // VK_BLEND_OP_ADD
  u8 src_alpha_factor = 1;
  u8 dst_alpha_factor = 0;
  u8 alpha_blend_op = 0;
  u8 write_mask = 0xF;  // R | G | B | A

  // bpmem.zmode, carried purely so the UI footprint audit can report it. The
  // overlay rasterizer has no depth buffer by design (RemixUiRaster.h:34-37), so
  // a UI draw that the console's Z test would have rejected is drawn here
  // regardless - these two say whether that is even possible for a given draw.
  // Nothing reads them to make a decision.
  bool depth_test = false;
  bool depth_write = false;

  // Where stage 0's texture came from in GC memory, and whether the cache calls
  // it a copy. Stamped from the bound TCacheEntry, which is the only thing that
  // knows: RemixTexture cannot tell a texture decoded out of an EFB copy's
  // destination apart from any other texture, because on this backend nothing
  // ever wrote that destination and the decode of the stale bytes succeeds.
  u32 texture_addr = 0;
  bool texture_is_efb_copy = false;
  bool texture_is_xfb_copy = false;
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

  // False resolves the rasterized colour channel from TEV stage 0 whether or not
  // stage 0 consumes it, which is the pre-fix behaviour. On, each half takes the
  // channel named by the stage that actually reads ras.
  bool GxRasChannelEnabled() const { return m_gx_ras_channel; }

  // False submits world draws the console scissored down to nothing, which is
  // the pre-fix behaviour: the world path read scissor state nowhere.
  bool WorldScissorSkipEnabled() const { return m_world_scissor_skip; }

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
  // world_ui_projection non-null marks an orthographic draw: `transform` is then
  // the draw's plain modelview and the mapping onto the UI plane is applied at
  // flush time, when the camera exists. Such a draw must pass raw_modelview as
  // null - an ortho modelview is not a view matrix, and letting one into the
  // histogram would corrupt the very camera the UI is placed against.
  void SubmitMesh(const MaterialRef& material,
                  const std::vector<remixapi_HardcodedVertex>& vertices,
                  const std::vector<u32>& indices, const remixapi_Transform& transform,
                  remixapi_InstanceCategoryFlags category_flags, const DrawBlendState& blend,
                  const float* raw_modelview, const DrawDiagnostics& diagnostics,
                  const std::array<float, 6>* world_ui_projection = nullptr);

  // How orthographic draws - HUD, menus, 2D screens - are handled.
  //   0 = dropped, the pre-feature behaviour
  //   1 = software-rasterized into a screen overlay composited at present
  //   2 = submitted as world-space geometry on a plane in front of the camera
  // Mode 2 is kept because it is the only one that puts UI inside the traced
  // world (it can light the scene, reflect, and be looked at from an angle), but
  // it is NOT passthrough: the runtime treats it as geometry, so it is denoised
  // and moves with the camera. Mode 1 is what looks like a normal UI.
  int UiMode() const { return m_ui_mode; }

  // Rasterizes one orthographic draw into the screen overlay. Vertices are the
  // decoded remixapi ones; `modelview` is the draw's GX modelview, or null when
  // the vertices have already been baked into view space. `viewport` is the
  // draw's viewport as an EFB-space rect (x, y, width, height) - a game is free
  // to put its HUD in a sub-rect and a full-screen assumption would misplace it.
  // `clip` is the draw's scissor rect (left, top, right, bottom) in EFB space,
  // from the same ComputeScissorRects every other backend uses.
  void SubmitUiDraw(const std::vector<remixapi_HardcodedVertex>& vertices,
                    const std::vector<u32>& indices, const float* modelview,
                    const std::array<float, 6>& ortho_projection,
                    const std::array<float, 4>& viewport, const std::array<float, 4>& clip,
                    const RemixTexture* texture, u8 filter_mode, u8 wrap_mode_u, u8 wrap_mode_v,
                    const DrawBlendState& blend);

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

  // --- Called from RemixTextureCache ---

  // Records one EFB copy. Called synchronously from
  // Remix::TextureCache::CopyEFB, which VideoCommon invokes from
  // TextureCacheBase::CopyRenderTargetToTexture (:2399) while the triggering BP
  // state is still live - that is what makes reading bpmem.triggerEFBCopy at the
  // call site correct rather than a frame-end guess.
  //
  // Recording is on whenever the audit or the filter is, and off otherwise, so
  // an untraced run pays nothing.
  void NoteEfbCopy(const MathUtil::Rectangle<int>& src_rect, u32 dst_addr, u8 copy_format, bool xfb,
                   bool clear, bool half_scale);

  // True when NoteEfbCopy should record. Read by the texture cache before it
  // touches bpmem at all.
  bool RecordEfbCopies() const { return m_trace_efb_copies; }

  FrameStats& Stats() { return m_stats; }

  // Cached at Initialize rather than read per draw - the classifier runs on
  // every batch, and Config::Get is not free at that rate.
  // 0 = ignore the depth heuristic, 1 = tag as sky, 2 = drop sky draws. The
  // heuristic is off by default: it matched the wrong draws in practice (clouds
  // and overlays rather than the horizon dome, which is drawn depth-tested).
  int SkyMode() const { return m_sky_mode; }

  // True when this stage-0 texture was listed in GFX_REMIX_SKY_TEXTURES. This
  // is the reliable route to sky: the runtime's texture-grid categories never
  // reach API draws, but the SKY bit passed on the instance does. The veto list
  // outranks it, so one setting can turn off a wrong verdict from any source.
  bool IsSkyTexture(u64 content_hash) const
  {
    if (IsSkyVetoed(content_hash))
      return false;
    return !m_sky_textures.empty() && m_sky_textures.count(content_hash) != 0;
  }

  // Precedence is veto > manual > auto: a hash named here is never sky, however
  // it was nominated.
  bool IsSkyVetoed(u64 hash) const
  {
    return !m_sky_veto_hashes.empty() && m_sky_veto_hashes.count(hash) != 0;
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
    // Object-space bounding radius, measured once at create time from the
    // decoded vertices. Scaled by the modelview at classification time, this is
    // what separates a sky dome from small camera-welded geometry.
    float object_radius = 0.0f;
    // Last draw of this mesh, for the classification log line only.
    DrawDiagnostics diagnostics;
  };

  // A mesh that has been matching the sky transform signature. Only frames on
  // which the camera actually translated count - see the informative-frame gate
  // in EstimateView.
  struct SkyCandidate
  {
    u32 streak = 0;
    u32 informative_frames = 0;
    float scaled_extent = 0.0f;
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
  // Ceilings on the frame's audit event logs. A frame legitimately triggers a
  // handful of EFB copies and up to a couple of hundred UI draws; these exist
  // only so a runaway game cannot grow either vector without bound. Overflow is
  // conservative in both directions - an unrecorded copy hides nothing and an
  // unrecorded footprint is never suppressed.
  static constexpr size_t MAX_EFB_COPY_EVENTS = 256;
  static constexpr size_t MAX_UI_FOOTPRINTS = 1024;
  // Ceiling on the session-long set of EFB copy DESTINATIONS. Games reuse a
  // small pool of render-target addresses - Wind Waker's whole run uses three -
  // so this is generous, and overflow is conservative: an unrecorded destination
  // means a garbage-textured draw is drawn rather than a good one refused.
  static constexpr size_t MAX_EFB_COPY_DESTINATIONS = 64;
  // Ceiling on distinct modelviews tracked per frame, so a game that gives every
  // draw its own matrix cannot grow the map without bound. Overflowing draws are
  // counted and dropped - the histogram is then incomplete, which is itself the
  // answer to "does one matrix dominate".
  static constexpr size_t MAX_MODELVIEW_BUCKETS = 4096;
  // What the histogram's winner has to show before it is trusted AS the camera.
  // Not a close call to arbitrate - a scene either has a shared authoring space
  // or it does not - so these are floors far under any real positive rather than
  // tuned values. Wind Waker measures 160-293 meshes against a runner-up pinned
  // at 14, an 11-20x margin.
  static constexpr u32 MIN_MODELVIEW_CAMERA_MESHES = 8;
  static constexpr u32 MODELVIEW_CAMERA_DOMINANCE = 2;
  // Consecutive gate passes before the winner is believed. Guards the world
  // offset latch specifically: the gate is a per-frame test and a startup logo
  // can satisfy it by accident at t ~ 0, which is exactly how the first attempt
  // latched an identity offset and left the world out at 1e5.
  static constexpr u32 MODELVIEW_CAMERA_WARMUP = 8;

  // A draw held back until the frame's camera is known.
  struct PendingInstance
  {
    remixapi_MeshHandle mesh = nullptr;
    remixapi_Transform transform = {};
    remixapi_InstanceCategoryFlags category_flags = 0;
    DrawBlendState blend;
    u64 mesh_hash = 0;
    // An orthographic draw, to be placed on the world-space UI plane at flush
    // time. `transform` then holds only the draw's own modelview - the mapping
    // onto the plane is built from the camera, which is not known while draws
    // are arriving, and is composed onto it in FlushPendingInstances.
    bool world_ui = false;
    std::array<float, 6> ortho_projection = {};
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
  // Union of every projection variant's depth range this frame. Shared by
  // SetupCamera and the sky classifier's size gate so the two cannot disagree
  // about how far away "far" is.
  bool ComputeDepthRange(float& near_plane, float& far_plane) const;
  void EstimateView();
  // The transform-signature test, run inside EstimateView's W-delta loop where
  // the ratio W(t)*W(t-1)^-1 is already in hand.
  void ClassifySky(u64 mesh_hash, const Affine& world_ratio, const Affine& modelview,
                   const float camera_delta[3], float far_plane);
  void FlushPendingInstances();
  void LogCameraRecovery();
  // Picks the frame's dominant modelview and, if it clears the confidence gate,
  // turns it into the camera. Runs at the TOP of OnAfterFrame - before
  // EstimateView - because with RemixCameraFromModelview on this IS the camera
  // and everything downstream is placed relative to it.
  void ResolveDominantModelview();
  // Runs EVERY frame, not just on the frames it prints: it carries the dominant
  // bucket across the frame boundary, which is what makes the candidate's own
  // inter-frame delta - the thing that would BE the camera delta if the candidate
  // is the view matrix - measurable at all. Clears the histogram on the way out.
  void LogModelviewHistogram();
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
  bool m_gx_ras_channel = true;
  bool m_world_scissor_skip = true;

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
  bool m_view_tie_break = true;

  // Modelview histogram - a pure instrument, read by nothing but its own log
  // line. Rebuilt from scratch every frame; only the dominant bucket survives the
  // frame boundary, so that the candidate can be differenced against itself.
  std::unordered_map<u64, ModelviewBucket> m_modelviews;
  u32 m_modelview_overflow = 0;
  // The frame's winner, held as VALUES rather than a pointer into the map above:
  // it is resolved at the top of OnAfterFrame and read again at the bottom, and
  // a member pointing into a container that is cleared in between is a trap.
  Affine m_modelview_top = {};
  std::unordered_set<u64> m_modelview_top_mesh_set;
  u32 m_modelview_top_meshes = 0;
  u32 m_modelview_top_draws = 0;
  u32 m_modelview_top_vertices = 0;
  u64 m_modelview_top_slots = 0;
  u32 m_modelview_second_meshes = 0;
  u32 m_modelview_second_draws = 0;
  u32 m_modelview_third_meshes = 0;
  u32 m_modelview_third_draws = 0;
  u32 m_modelview_mesh_uses = 0;
  bool m_modelview_top_valid = false;
  // The winner turned into a camera, once it has cleared the confidence gate.
  Affine m_modelview_camera = {};
  bool m_modelview_camera_valid = false;
  // Constant world offset latched at the first accepted camera, so world space
  // starts at the origin instead of at the game's own 1e5-scale coordinates.
  // See ResolveDominantModelview for why post-multiplying is exact.
  Affine m_modelview_world_offset = {};
  bool m_modelview_world_offset_latched = false;
  u32 m_modelview_camera_streak = 0;
  u32 m_modelview_camera_accepted = 0;
  u32 m_modelview_camera_held = 0;
  Affine m_modelview_top_previous = {};
  std::unordered_set<u64> m_modelview_top_meshes_previous;
  bool m_modelview_top_previous_valid = false;
  bool m_trace_modelviews = true;
  bool m_camera_from_modelview = true;

  // World-space UI. BuildWorldUiTransform maps a draw's orthographic view space
  // onto a plane in front of the camera; it needs the camera, so it is called
  // from FlushPendingInstances rather than from the draw path.
  bool BuildWorldUiTransform(const std::array<float, 6>& raw, Affine& out) const;
  int m_ui_mode = 1;
  float m_world_ui_distance = 2.0f;
  bool m_world_ui_flip_y = false;
  // Screen overlay. Sized to the swapchain, cleared at the first UI draw of each
  // frame and handed to the runtime just before Present.
  UiRasterizer m_ui_raster;
  // Scratch, reused across draws so a per-frame HUD does not allocate ~140 times.
  std::vector<UiRasterizer::Vertex> m_ui_vertices;
  bool m_ui_frame_begun = false;
  u32 m_surface_width = 0;
  u32 m_surface_height = 0;
  // Non-zero dumps the composited overlay at that frame index to
  // Logs/remix-ui-overlay.bmp, once. Diagnostic only; nothing reads it back.
  int m_ui_dump_frame = 0;
  // Frame-scoped event logs for the EFB-copy audit. Both are appended to where
  // the event happens - copies in the texture cache, footprints in the UI draw
  // path - and consumed together at frame end. Cleared in OnAfterFrame.
  //
  // The frame's own XFB copy IS in here by the time they are read:
  // after_frame_event is triggered from inside the XFB copy handler
  // (BPStructs.cpp:353), after CopyRenderTargetToTexture has already run.
  std::vector<EfbCopyEvent> m_efb_copies;
  std::vector<UiDrawFootprint> m_ui_footprints;
  // NOT cleared in OnAfterFrame, unlike the two above - see NoteEfbCopy for why
  // this one is session-long. Small enough that a linear scan is the right
  // lookup: Wind Waker's entire run puts three addresses in it.
  std::vector<u32> m_efb_copy_destinations;
  bool m_trace_efb_copies = true;
  // Read once in Initialize, never in the draw path. OFF for either is exactly
  // the pre-change behaviour: the draw is classified and submitted as before.
  bool m_ui_drop_dst_alpha = true;
  bool m_ui_drop_efb_copy_textures = false;
  // The EFB region the console actually presents, in EFB units. UI draw
  // coordinates are in EFB units and the overlay is the swapchain, so this is
  // the denominator that maps one onto the other; using EFB_WIDTH/EFB_HEIGHT
  // instead assumes the game presents all 640x528, which Wind Waker (480 rows)
  // does not. See GFX_REMIX_UI_SCALE_TO_XFB.
  //
  // `m_xfb_frame_rect` accumulates this frame's XFB copy source rects and is
  // promoted into the two sizes at frame end; zero sizes mean "no XFB copy seen
  // yet", in which case SubmitUiDraw falls back to the EFB constants.
  MathUtil::Rectangle<int> m_xfb_frame_rect = {};
  bool m_xfb_frame_valid = false;
  u32 m_presented_width = 0;
  u32 m_presented_height = 0;
  bool m_presented_logged = false;
  bool m_ui_scale_to_xfb = true;
  void AuditUiFootprints();
  void SubmitScreenOverlay();
  // The frustum SetupCamera actually submitted, published so the UI plane fills
  // the same one. Defaults match SetupCamera's own fallback, which is what a
  // screen with no perspective draw at all gets.
  float m_camera_fov_y_deg = 60.0f;
  float m_camera_aspect = 16.0f / 9.0f;

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
  // Sky auto-detection: 0 off, 1 detect and log only, 2 detect and tag.
  int m_sky_auto_detect = 1;
  u32 m_sky_auto_frames = 30;
  float m_sky_auto_min_extent = 0.05f;
  // Untextured classified draws take IGNORE rather than SKY, so a dome that the
  // sky path would have left in the scene cannot occlude the Numos sun.
  bool m_sky_auto_untextured_ignore = true;
  std::unordered_map<u64, SkyCandidate> m_sky_candidates;
  // Sticky for the session. A classified mesh is never un-classified: sky that
  // flickers in and out is worse than sky that is occasionally wrong, and a
  // restart clears it.
  std::unordered_set<u64> m_sky_classified;
  std::unordered_set<u64> m_sky_veto_hashes;
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
