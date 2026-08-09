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
  // Draws dropped because stage 0 sampled an EFB copy destination this backend
  // deliberately discarded. Answers "how many blank rectangles did we stop the
  // game drawing over the traced scene". Distinct from skipped_efb_texture,
  // which counts the older case of a texture that failed to load at all - a
  // zero-filled destination loads perfectly happily, so that counter never
  // fires on this path.
  u32 skipped_efb_discarded = 0;
  u32 skipped_degenerate = 0;
  // Draws that write neither colour nor alpha, or whose alpha test can never
  // pass. Depth-only geometry in a backend that has no depth buffer.
  u32 skipped_invisible = 0;
  u32 skipped_alpha_only = 0;
  u32 dst_alpha_substituted = 0;
  u32 dst_alpha_masked = 0;
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

  // The TEV COLOUR chain fold, counted over the draws it is eligible for (world
  // draws whose colour comes from the vertex stream). `folded` is the number whose
  // submitted vertex colour now carries the chain's resolved output instead of its
  // raw input weight - on Wind Waker that is the ocean. `identity` is the chains
  // that pass the rasterized colour straight through, where folding would be a
  // no-op and is skipped so the mesh hash cannot churn. `bailed` is chains the
  // evaluator refused; a rising number there is the thing to investigate.
  u32 tev_color_folded = 0;
  u32 tev_color_identity = 0;
  // Draws whose chain resolved to a per-draw CONSTANT colour, delivered through
  // tFactor. The dominant Wind Waker case is `TexColor * Reg[Color0]` - a TEV
  // register used as a multiplier, which resolved to "no tint" before and
  // rendered as the bare texture.
  u32 tev_color_tinted = 0;
  u32 tev_color_bailed = 0;
  // Draws whose albedo texture came from a TEV stage after stage 0, because
  // stage 0 samples nothing. Wind Waker's sea is the case this exists for: a
  // register lerp on stage 0 and the water texture on stage 1.
  u32 texture_later_stage = 0;
  u32 texture_ramp_skipped = 0;

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
  // Directional lights dropped so an atmosphere mod owns the key light. Answers
  // "is the game still adding a sun on top of the sky" - if the scene is too
  // bright with the knob on, this reading zero says the brightness is coming
  // from somewhere else.
  u32 lights_dropped_distant = 0;
  u32 lights_sphere = 0;
  u32 lights_spot = 0;
  // Spot lights whose distance AND angular attenuation polynomials are both the
  // constant 1, i.e. a light with no falloff of any kind. GX has no directional
  // type, so this is how a GC title spells "sun": an ordinary light parked far
  // enough away that its direction barely varies across the scene. Routing one
  // to a Remix sphere applies a 1/r^2 the console never applied and the scene
  // renders black WITH a light in it, which also suppresses the fallback light.
  u32 lights_falloff_free = 0;

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
  // Classified sky instances whose transform was scaled about the camera to push
  // them behind all world geometry. Non-zero is the whole of RemixSkyAtInfinity
  // working; zero with a non-zero classified set means the knob is off.
  u32 sky_pushed = 0;
  // Instances drawn with the unlit sky material. Should track `pushed` once the
  // classifier has settled; a gap between the two means a classified mesh is
  // still being drawn through a shaded material.
  u32 sky_emissive = 0;
  // Classified sky meshes removed from the camera electorate. A skybox votes for
  // the camera's rotation delta with the translation missing, which is an
  // actively wrong hypothesis rather than merely a useless one.
  u32 view_sky_excluded = 0;
  // Modelview histogram. `modelview_samples` counts draws that offered a single
  // modelview to bucket; `modelview_palette` counts the ones that had none, so a
  // dominant matrix's share can be read against the right denominator.
  u32 modelview_samples = 0;
  u32 modelview_palette = 0;
  // Of those palette draws, how many were submitted GPU-skinned rather than
  // CPU-baked. Read it against `meshes created`: skinned draws are the ones that
  // used to mint a fresh mesh every frame, so a non-zero `skinned` with a
  // collapsed mesh count is the whole feature working. `skinned` non-zero while
  // meshes stay high means the game re-partitions which vertices name which
  // palette slot, so even compact indices re-hash - the one thing that cannot be
  // established without running a game.
  u32 draws_skinned = 0;
  // How many of those skinned draws had to MINT a mesh rather than reuse one.
  // This is the splitter the plain `meshes` count cannot give: that number mixes
  // skinned draws with regenerated geometry (WW-sea-style), so it can stay high
  // for reasons that have nothing to do with skinning. Read it as:
  //   0            - hashes are stable, handles are being reused, and the
  //                  runtime is taking kUpdateBVH (which is what populates
  //                  previousPositionBuffer). If motion vectors are still black
  //                  here, the fault is downstream of identity, not in the hash.
  //   == skinned   - a new handle every frame, so the runtime takes KBuildBVH.
  //                  Skinning STILL dispatches on that path, so characters look
  //                  correct while carrying no history at all - exactly the
  //                  "geometry fine, motion vectors zero" symptom.
  u32 draws_skinned_new_mesh = 0;
  // Draws served by rewriting an existing mesh handle's vertex bytes in place
  // (UpdateMeshBatched) instead of minting a fresh handle - the regenerated-
  // geometry counterpart of `skinned`. Read it against `meshes created`: in a
  // steady scene this should carry the per-frame regenerated draw count while
  // `meshes created` collapses toward zero, because those draws are exactly
  // the ones that used to create a new mesh every frame.
  u32 meshes_updated = 0;
  // Orthographic draws placed on the world-space UI plane, and those that could
  // not be (no camera yet, or a degenerate ortho projection). A game whose menus
  // are missing while `ui_placed` is non-zero has a placement bug, not a
  // classification one.
  u32 ui_placed = 0;
  u32 ui_unplaceable = 0;
  // Untextured ortho draws dropped because they arrived before any world
  // geometry - EFB clears and scratch-region fills, which the overlay would
  // composite on top of the scene instead of underneath it. Answers "how much
  // of the screen did we stop being painted flat".
  u32 ui_dropped_pre_world = 0;
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
  // Tag-driven routing, one counter per decision path, so "the tag did not
  // work" is a question the log answers rather than a thing to theorize about.
  //
  // `ui_tagged_ui`     - draws composited because the user tagged them UI, with
  //   every drop heuristic bypassed.
  // `ui_tagged_ignore` - draws discarded on an explicit Ignore tag.
  // `ui_tagged_world`  - draws sent world-side on an explicit World Space UI tag.
  // `ui_tagmode_world` - draws sent world-side only because the dev menu is
  //   open. Non-zero ONLY while tagging; if it is non-zero with the menu shut,
  //   the UI-state read is wrong.
  // `ui_dropped_strict`     - untagged draws dropped by RemixUiStrict.
  // `ui_dropped_preworld`   - draws dropped by RemixUiDropPreWorld (the
  //   texture-agnostic pre-world filter). Distinct from ui_dropped_pre_world
  //   above, which is the older untextured-only rule; the two coexist.
  // `ui_dropped_fullscreen` - draws dropped by RemixUiDropFullScreenOpaque.
  // `ui_overlay_tex_registered` - first-time uploads of an overlay-only texture
  //   into the dev-menu grid. Rises on a new screen and then settles; if it
  //   never leaves zero, nothing is taggable and the routing has no input.
  u32 ui_tagged_ui = 0;
  u32 ui_tagged_ignore = 0;
  u32 ui_tagged_world = 0;
  u32 ui_tagmode_world = 0;
  u32 ui_dropped_strict = 0;
  u32 ui_dropped_preworld = 0;
  u32 ui_dropped_fullscreen = 0;
  u32 ui_overlay_tex_registered = 0;
  // The perspective arm of tag routing - a HUD the game draws through the 3D
  // frustum rather than as 2D. Same one-counter-per-decision-path rule as above.
  //
  // `ui_tagged_persp`    - perspective draws diverted into the screen overlay by
  //   a UI tag. Counts the DIVERT DECISION, in draws, not in tagged textures:
  //   one tagged texture drawn eight times reads 8, consistently with every
  //   other counter in this group. Zero while anything in the chain is off (the
  //   knob, the routing master, mode != 1) or while the dev menu is open, and
  //   zero on any game that has tagged nothing - which is what makes this group
  //   the regression floor for the perspective path too. A diverted draw is also
  //   counted in ui_placed, deliberately: it IS an overlay draw now, and the
  //   overlay's raster/upload timings include it.
  // `ui_persp_refused_w` - diverted draws handed BACK to the world because a
  //   vertex sat at or behind the eye plane, where the perspective divide has no
  //   answer. Should read zero forever: a quad parked in front of the camera
  //   never reaches the eye. Non-zero says the tagged element is not actually
  //   screen-parked, so the tag is on the wrong texture - the draw is not lost,
  //   it renders as world geometry as before.
  // `ui_persp_skinned`   - UI-tagged perspective draws kept world-side because
  //   they are GPU-skinned. Their vertices are bone-local and the pose lives in
  //   a palette the runtime applies, so there is no single modelview the overlay
  //   could place them with. Non-zero means someone tagged a skinned object.
  u32 ui_tagged_persp = 0;
  u32 ui_persp_refused_w = 0;
  u32 ui_persp_skinned = 0;
  // Overlay draws recorded with their z test enabled - the draws the depth
  // plane exists for (RemixUiDepth). Zero on every ordinary 2D HUD measured so
  // far; non-zero says this game's overlay genuinely resolves by depth and the
  // plane is engaged. Counted at record time, whether or not any pixel of the
  // draw survives.
  u32 ui_depth_tested = 0;
  // EFB copies the game triggered this frame, and the subset that could hide a
  // UI draw: not the XFB copy, and carrying the clear bit, so the region it
  // took is wiped off the EFB before anything reaches the screen. A frame whose
  // `efb_copies_scratch` is zero cannot be composing anything off-screen, which
  // is the whole hypothesis in one number.
  u32 efb_copies = 0;
  u32 efb_copies_scratch = 0;
  // The same copies broken down by CLASS, which is what decides whether each one
  // is executed against the CPU EFB or deliberately discarded. Each answers
  // "what did the classifier think this frame was doing?", and together they are
  // how a misclassification is spotted without a debugger:
  //   `efb_copies_xfb`        - the presentation copy. One per frame, normally.
  //   `efb_copies_depth`      - Z24 source, i.e. almost always a shadow map.
  //   `efb_copies_intensity`  - luminance format, i.e. a bloom/glow tap.
  //   `efb_copies_scene`      - colour copy taken after a world draw. World
  //       content this backend never rasterizes, so executing it would paint a
  //       flat rectangle. A non-zero count on a MENU screen is the tell that the
  //       frame-level world-draw signal is too blunt for that game.
  //   `efb_copies_composed2d` - colour copy taken before any world draw, whose
  //       content the CPU EFB holds exactly. The one class that executes by
  //       default; finding a screen where this is non-zero is how the payoff
  //       gets tested at all.
  u32 efb_copies_xfb = 0;
  u32 efb_copies_depth = 0;
  u32 efb_copies_intensity = 0;
  u32 efb_copies_scene = 0;
  u32 efb_copies_composed2d = 0;
  // Actions actually taken. `discarded` must equal the sum of the classes whose
  // knobs are off; `executed` the sum of those that are on. They are counted
  // separately from the classes on purpose - a knob flipped in GFX.ini is
  // invisible in the class counts alone.
  u32 efb_copies_executed = 0;
  u32 efb_copies_discarded = 0;
  // Microseconds spent inside the copy encoders. Zero on a frame where
  // everything is discarded (a discard pays only its zero-fill), so a non-zero
  // value here is also the confirmation that something executed at all. This is
  // the regression tripwire for a 2D-heavy title: sustained values in the
  // thousands mean the encode is eating the frame.
  u64 efb_encode_us = 0;
  // Times the 2D layer was composited into the EFB before an encode. Should
  // never exceed `efb_copies_executed`; if it does, the re-Begin bookkeeping in
  // FoldUiIntoEfb has broken and copied UI is being double-blended.
  u32 efb_ui_folds = 0;
  // CPU-thread EFB accesses through MMU (MMU.cpp:142-190), drained from the EFB
  // interface at frame end. Zero is a perfectly valid answer - most GC titles
  // never touch the EFB from the CPU - but a title that does (Super Mario
  // Galaxy's pointer) reads 0 forever if the emulation knob is off, which is the
  // difference these two make visible.
  u32 efb_peeks = 0;
  u32 efb_pokes = 0;
  // Microseconds spent rasterizing the overlay, and handing it to the runtime.
  // Split because they have different fixes: the first is this backend's inner
  // loop, the second is a per-frame staging-buffer create plus a multi-megabyte
  // memcpy inside the runtime.
  u64 ui_raster_us = 0;
  u64 ui_upload_us = 0;
  // Whether the raster above was served from the unchanged-frame cache. A menu
  // sitting still should read 1 with raster_us near zero; a 1 while the screen
  // visibly animates means the hash is missing an input and the cache is
  // showing a stale frame - the one failure mode worth watching for.
  bool ui_raster_cached = false;
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

  // Sub-screen viewports. GX POSITIONS every draw by xfmem.viewport
  // (Clipper.cpp:553-554, BPFunctions.cpp:192-193), and a game that renders
  // picture-in-picture or split screens moves it mid-frame - while Remix takes
  // one camera per frame, so the difference has to be folded into the instance
  // transform exactly the way a projection difference is. `viewport_changed`
  // counts world draws whose raw rect differs from the frame's REFERENCE rect,
  // which is latched by the same draw that latches the reference projection so
  // the fold and the camera can never be defined against different views.
  //
  // Of those, `viewport_corrected` reached the transform and was folded, and
  // `viewport_uncorrectable` was refused - a non-zero refusal is the
  // log-readable tell that a draw is knowingly being rendered mid-screen.
  // `viewport_mirrored` is a subset of corrected whose combined scale came out
  // negative: the placement follows the algebra, but no game is known to mix
  // viewport signs mid-frame, so the shading of one is unvalidated and the
  // counter is the tripwire.
  //
  // `viewport_depth_changed` counts draws whose zRange/farZ pair differs from
  // the reference's. Nothing folds that pair - it remaps NDC z into the EFB
  // depth ramp and this backend has no depth buffer for it to bias - but a
  // FULL-screen rect with a compressed zRange is the classic viewmodel tell, so
  // the count is the evidence a VIEW_MODEL follow-up would be built on.
  u32 viewport_changed = 0;
  u32 viewport_corrected = 0;
  u32 viewport_uncorrectable = 0;
  u32 viewport_mirrored = 0;
  u32 viewport_depth_changed = 0;
  // Draws arriving after the per-frame viewport variant table filled up.
  // Non-zero means the viewport log is incomplete, not that anything rendered
  // wrong: neither the reference nor the fold reads the table.
  u32 viewport_overflow = 0;
  // Perspective draws refused the reference latch because their viewport did
  // not cover the presented region (RemixViewportRefXfb). A steady non-zero
  // count is the helper-pass-first signature - Sonic Unleashed reads 27 - and
  // those draws render unfolded unless the aux-pass drop removes them first.
  u32 viewport_ref_deferred = 0;
  // Perspective draws dropped as a discarded helper pass (RemixEfbDropAuxPass):
  // their viewport rect was copied-out-and-cleared to a non-XFB destination
  // last frame and the copy was discarded. Counted into m_frame_world_draws
  // anyway, because the console's EFB did hold world pixels there and the
  // Scene-vs-Composed2D copy classifier must keep seeing them.
  u32 skipped_aux_pass = 0;
};

// What an EFB copy is FOR, decided from state already in hand at the copy. The
// order of the enumerators is the order the classifier tests them in, and that
// order is load-bearing: a depth copy is also a copy with world draws behind it,
// an intensity tap is also a colour copy, and the first match wins.
//
// Only Composed2D executes by default. See GFX_REMIX_EFB_COPY_* for why each
// class does what it does.
enum class EfbCopyClass
{
  // EFBCopyFormat::XFB - the copy that ends the frame.
  Xfb,
  // params.depth: source pixel format is Z24 (BPStructs.cpp:308).
  Depth,
  // params.yuv (isIntensity): a luminance destination format.
  Intensity,
  // Colour copy with at least one perspective draw already submitted this frame.
  // The one HEURISTIC class.
  Scene,
  // Colour copy with none - so everything the console's EFB held is clear colour
  // plus ortho draws plus pokes, which is exactly what the CPU EFB holds.
  Composed2D,
};

const char* EfbCopyClassName(EfbCopyClass copy_class);

// One EFB copy the game triggered, stamped WHERE IT HAPPENED. Recorded rather
// than re-read at frame end for the usual reason: every field here is per-copy
// BP state, and a frame-end read of bpmem sees only the last copy's.
//
// Copies are now CLASSIFIED and then either executed against the CPU EFB or
// deliberately discarded (see EfbCopyClass). The console-side consequence
// recorded here still matters either way: a copy that takes a region and sets
// the clear bit wipes that region off the EFB (BPStructs.cpp:377-393), so
// anything drawn into it beforehand never reaches the XFB - and that is true of
// a discarded copy too, because the clear arrives through separate machinery.
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
  // Source pixel format was Z24, and destination format was a luminance one -
  // EFBCopyParams::depth and ::yuv (TextureCacheBase.h:72-87). The two exact
  // signals the classifier reads.
  bool depth = false;
  bool intensity = false;
  // Vertical scale of the copy. Recorded so the trace line can print every input
  // the decision saw, even the ones it did not use.
  float y_scale = 1.0f;
  // What the classifier decided, and what was done about it.
  EfbCopyClass copy_class = EfbCopyClass::Scene;
  bool executed = false;
  // The two counters that decided Scene vs Composed2D, as they stood AT THE
  // COPY. Frame-end values would be the last copy's view of the frame, which is
  // exactly the reading that would make a misclassification look justified.
  u32 world_draws = 0;
  u32 ui_draws = 0;
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

// One matrix-palette draw's skinning data, carried from the vertex manager into
// SubmitMesh. Split in two because the halves have different lifetimes: the
// blend indices describe the MESH and are consumed once, at create time, while
// the palette is per-draw state that has to survive until the frame's camera is
// known and the instance is finally emitted.
//
// This exists at all because GX has no bones. A matrix-palette draw names one of
// 64 xfmem.posMatrices rows per vertex, so the console's "palette" is a set of
// complete object-to-VIEW modelviews rather than a rig - which is exactly what
// the Remix skinning kernel wants (positionOut = bone[idx] * v), one bone per
// vertex, weight 1.
struct DrawSkinning
{
  // One compact bone index per SUBMITTED vertex, in the same order and of the
  // same length as the vertex array that goes to CreateMesh. Compact means
  // 0..palette.size()-1 in order of first appearance in the vertex stream, NOT
  // the physical posMatrices row: which rows a game hands a character is an
  // allocation detail that can change between frames, and the mesh hash folds
  // these indices in, so anything less canonical would re-hash for free.
  std::vector<u32> blend_indices;
  // The referenced modelviews, in that same compact order, snapshotted AT DRAW
  // TIME. xfmem is rewritten mid-frame, so reading it any later would give every
  // skinned draw in the frame the last one's pose.
  std::vector<remixapi_Transform> palette;
};

// One draw's viewport, as a screen mapping rather than as registers. GX maps
// clip space to the EFB per draw as
//   screen.x = (clip.x / clip.w) * wd + xOrig
//   screen.y = (clip.y / clip.w) * ht + yOrig
// (Clipper.cpp:553-554 - the plain-math reference), so wd/ht are HALF extents
// and xOrig/yOrig the centre. That is the whole mapping the fold has to
// reproduce through one camera.
struct DrawViewport
{
  // xOrig, yOrig, wd, ht, exactly as the game wrote them into xfmem.viewport.
  // Compared bit-exactly: "did the game move the viewport" is a question about
  // register writes, and an epsilon here would only invent variants that differ
  // by nothing.
  std::array<float, 4> rect = {};
  // The scissor-adjusted centre - xOrig - x_off, yOrig - y_off - which is what
  // every hardware backend positions by (BPFunctions.cpp:192-193). The
  // subtraction is what turns a viewport written in the scissor's coordinate
  // space into an EFB position, so it is the centre the fold must use and not
  // the raw origin.
  float cx = 0.0f;
  float cy = 0.0f;
  // The depth remap (Clipper.cpp:555). Recorded, never folded: it biases
  // depth-test comparisons on a buffer this backend does not have, and
  // compressing view-space z instead would physically pull geometry into the
  // camera. See FrameStats::viewport_depth_changed.
  float zrange = 0.0f;
  float farz = 0.0f;

  float wd() const { return rect[2]; }
  float ht() const { return rect[3]; }
  bool SameRect(const DrawViewport& other) const { return rect == other.rect; }
  bool SameDepth(const DrawViewport& other) const
  {
    return zrange == other.zrange && farz == other.farz;
  }
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

  // bpmem.zmode. Originally carried purely for the UI footprint audit; since
  // the overlay gained a depth plane (RemixUiDepth) these are what SubmitUiDraw
  // stamps into the rasterizer's DrawCall, so a draw whose z test the console
  // would honour is honoured here too. Every ordinary 2D HUD measured so far
  // has the test off and never touches the plane.
  bool depth_test = false;
  u8 depth_func = 7;  // CompareMode numbering, Never = 0 .. Always = 7.
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

  // Accounts for one perspective draw's projection and viewport and returns the
  // projection's slot in the frame's variant table, or -1 if the table is full.
  // The first perspective draw of a frame that passes the RemixViewportRefXfb
  // gate - one whose viewport covers most of the previously presented region,
  // or simply the first draw while the gate is down - also latches the
  // reference projection that SetupCamera turns into the camera; a frame with
  // only orthographic draws keeps the previous camera.
  //
  // The reference VIEWPORT is latched by the same draw, in the same call, on
  // purpose: the correction is defined relative to what SetupCamera shows, and
  // SetupCamera is built from the reference projection. Letting the two
  // references come from different draws would be this backend's fifth
  // per-frame-state-collapse bug wearing a new hat.
  int ObserveProjection(const std::array<float, 6>& raw_projection, const DrawViewport& viewport);

  // Records that a draw which actually reached SubmitMesh used variant `slot`.
  // The reference is the first perspective draw the RemixViewportRefXfb gate
  // admits (first draw outright when the gate is down), and these counts are
  // how we find out whether that draw is the one carrying the scene.
  void NoteProjectionUse(int slot, u32 vertex_count);

  // The frame's reference projection - the one SetupCamera is built from, and
  // therefore the one every other draw has to be folded onto. False until a
  // perspective draw has been seen this frame, and also false when that draw's
  // frustum was too degenerate for the camera to represent: SetupCamera then
  // falls back to a fabricated default that encodes none of these raw terms, so
  // folding draws onto them would aim at a camera that is not there.
  bool HasReferenceProjection() const { return m_projection_latched && m_reference_usable; }
  const std::array<float, 6>& ReferenceProjection() const { return m_raw_projection; }

  // The frame's reference viewport - the screen rect the reference projection's
  // draw mapped into, and therefore the rect the single camera is implicitly
  // rendering. False until a perspective draw has been seen this frame, and
  // also false when that draw's rect has no extent to divide by: a zero-width
  // or zero-height viewport is not a screen mapping, and folding onto it would
  // be a division by nothing.
  //
  // The rect itself is readable either way, because "did this draw's viewport
  // move" is answerable even when the reference is unusable - that is exactly
  // the case FrameStats::viewport_uncorrectable exists to report.
  bool HasReferenceViewport() const { return m_projection_latched && m_reference_viewport_usable; }
  const DrawViewport& ReferenceViewport() const { return m_reference_viewport; }

  // False restores the pre-fix behaviour: submit each draw's modelview as-is
  // and let the frame's single camera misframe everything that does not share
  // the reference projection.
  bool ProjectionFixEnabled() const { return m_projection_fix; }

  // False restores the pre-fix behaviour for the VIEWPORT half of the same
  // fold: sub-screen draws - picture-in-picture panels, position ladders - are
  // rendered through the reference draw's screen rect and so land in the middle
  // of the world. On, the difference between the two rects is folded into the
  // instance transform alongside the projection difference. Independent of
  // ProjectionFixEnabled only in name: the viewport terms ride the projection
  // correction, so this knob does nothing while that one is off.
  bool ViewportFixEnabled() const { return m_viewport_fix; }

  // True when this perspective draw is a discarded helper pass: its viewport
  // rect was EFB-copied to a non-XFB destination WITH clear and the copy was
  // discarded, all learned from the previous presented frame. Never true for a
  // viewport covering most of the presented region, and never true on frames
  // where no viewport covered it (split screen) - the same stand-down rule the
  // reference gate uses, so a false cull needs visible content in a
  // copied-and-cleared sub-rect, which is the already-blank mirror-composite
  // pattern.
  bool ShouldDropAuxPass(const DrawViewport& viewport) const;

  // The bookkeeping for a draw ShouldDropAuxPass rejected. Counts the skip AND
  // m_frame_world_draws: the console's EFB held world pixels there, so the EFB
  // copy classifier's Scene signal must not lose them, or the helper pass's own
  // copy would reclassify Composed2D and execute.
  void NoteAuxPassDropped();

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
  bool GxRampAlbedoSkipEnabled() const { return m_gx_ramp_albedo_skip; }
  bool GxLitChannelTexGenEnabled() const { return m_gx_lit_channel_texgen; }
  bool GxEfbAlphaPassesEnabled() const { return m_gx_efb_alpha_passes; }
  bool UiRasChannelEnabled() const { return m_ui_ras_channel; }

  // False submits world draws the console scissored down to nothing, which is
  // the pre-fix behaviour: the world path read scissor state nowhere.
  bool WorldScissorSkipEnabled() const { return m_world_scissor_skip; }

  // False restores the CPU bake for matrix-palette draws exactly, which is the
  // pre-fix behaviour: every vertex is transformed by its own modelview here and
  // the mesh is submitted in view space, so its bytes - and therefore its hash,
  // its handle and its instance - are new every frame and it carries no motion.
  // On, the raw object-space mesh goes over with one bone per vertex and the
  // palette rides the instance, so the hash is stable and the runtime skins it.
  // It participates in mesh identity, so it is read once at init and never
  // re-read: flipping it mid-game would leave the two kinds of mesh sharing one
  // cache.
  bool GpuSkinningEnabled() const { return m_gpu_skinning; }

  // Per-draw world colour trace. Heavier than the other trace knobs - a line per
  // draw rather than per frame - so it is off by default.
  bool TraceColorsEnabled() const { return m_trace_colors; }

  // False submits the raw rasterized colour as the albedo tint, which is the
  // pre-fix behaviour: the TEV colour registers a GC title keeps its palette in
  // were dropped entirely. See ResolveTevColor.
  bool GxTevColorEnabled() const { return m_gx_tev_color; }

  // False takes the albedo texture from TEV stage 0 only, which is the pre-fix
  // behaviour: a draw whose stage 0 samples nothing arrived untextured even when a
  // later stage sampled the texture that shades it.
  bool GxTextureStageEnabled() const { return m_gx_texture_stage; }

  // Diagnostic: paint every world draw a flat colour naming which colour route
  // it took, texture and register selected out. Answers "which pixels does this
  // route own", which no counter can. Never on by default.
  bool DebugColorRoutesEnabled() const { return m_debug_color_routes; }

  // Uploads the texture (once per content hash) and returns the material that
  // references it. A null texture yields the untextured fallback material.
  // alpha_test_type / alpha_reference come from the draw's GX alpha test and
  // fold into material identity along with the sampler state, since Remix bakes
  // both into the material.
  //
  // `emissive` builds the unlit variant used for the game's own sky. GX draws a
  // skybox with lighting off - Wind Waker's dome resolves to literally
  // `K0 = [80 120 255]`, a TEV chain with no rasterized-colour input at all - so
  // on console that colour IS the final pixel. Handing it to Remix as diffuse
  // albedo and asking a light to reveal it is the wrong model: it shades, it
  // goes dark away from the sun, and it can never match. An emissive surface
  // reproduces "this colour, regardless of lighting" exactly.
  //
  // `emissive_rgb` is 0x00RRGGBB, the folded TEV constant, and is consulted only
  // for untextured draws; a textured sky takes its own albedo as the emissive
  // texture instead, which is the same patch the runtime applies to WorldUI
  // (rtx_instance_manager.cpp:1116-1123). Both fold into material identity, so
  // the emissive variant is a distinct material and a distinct mesh.
  MaterialRef EnsureMaterial(const RemixTexture* texture, u8 filter_mode, u8 wrap_mode_u,
                             u8 wrap_mode_v, u8 alpha_test_type, u8 alpha_reference,
                             bool emissive = false, u32 emissive_rgb = 0);

  // Identity of the geometry bytes alone. Split out of SubmitMesh so the draw
  // path can ask "is this mesh classified sky?" BEFORE it picks a material -
  // which it must, because the answer decides whether the material is emissive,
  // and the material then feeds the mesh hash.
  // Colour texture with the mask's alpha substituted in, cached so the same pair
  // resolves to one texture every frame. Null when the two disagree on size, or
  // when the combination cannot be uploaded.
  //
  // The mask arrives as BYTES, not as a texture: its producer and its consumer
  // are different draws, and the texture cache may evict it in between.
  const RemixTexture* MaskedAlbedo(const RemixTexture& colour,
                                   const std::vector<u8>& mask_pixels, u32 mask_width,
                                   u32 mask_height, u64 mask_hash);

  static u64 GeometryHash(const std::vector<remixapi_HardcodedVertex>& vertices,
                          const std::vector<u32>& indices);

  // Folds a skinned draw's per-vertex bone indices into that geometry identity.
  // Object-space positions stop being a unique description of the mesh the
  // moment skinning exists: two draws can share every vertex and every triangle
  // and still partition those vertices across bones differently, and they must
  // not collapse onto one handle, because the partition is baked into the mesh
  // at create time and whichever registered first would decide the other's pose.
  // It also keeps a skinned mesh apart from an unskinned twin. Lives here rather
  // than at the call site so every rule about how geometry identity is computed
  // stays in one file.
  static u64 FoldSkinningHash(u64 geometry_hash, const std::vector<u32>& blend_indices);

  // Material identity from the state that Remix bakes into a material. Lifted
  // out of EnsureMaterial so there is exactly ONE definition of the fold, and
  // two callers that cannot drift apart: EnsureMaterial itself, and
  // UntexturedOrthoKey, which has to reproduce a mesh hash the world path
  // produced. `texture_hash` is the albedo's content hash, or 0 for an
  // untextured draw (which folds the fallback material's hash instead).
  static u64 ComputeMaterialHash(u64 texture_hash, u8 filter_mode, u8 wrap_mode_u, u8 wrap_mode_v,
                                 u8 alpha_test_type, u8 alpha_reference, bool emissive,
                                 u32 emissive_rgb);

  // THE mesh identity: geometry bytes folded with the material hash, de-zeroed.
  //
  // Not an implementation detail of SubmitMesh - it is the value the runtime
  // sees as the mesh HANDLE (SubmitMesh mints the mesh with
  // `info.hash = ComputeMeshHash(...)`), which is in turn the key the dev menu's
  // click-to-tag popup records for an UNTEXTURED API draw (rtx_fork_submit.cpp's
  // keyIsMeshHash path). Every place that has to reproduce that key - the ortho
  // tag lookup, the perspective one - therefore has to compute it the same way,
  // from the same inputs, forever.
  //
  // One function rather than the expression repeated at each site, because the
  // requirement is not "these agree today" but "these cannot be made to
  // disagree". Skinning and sky-emissive folds need no mention here: both are
  // already inside `geometry_hash` and `material_hash` respectively by the time
  // anyone calls this.
  static u64 ComputeMeshHash(u64 geometry_hash, u64 material_hash);

  // Whether a geometry hash has been classified as sky. Keyed on geometry rather
  // than on the mesh hash precisely so that asking this question cannot be
  // perturbed by the answer - see MeshEntry::geometry_hash.
  bool IsSkyGeometry(u64 geometry_hash) const
  {
    return m_sky_auto_detect >= 1 && m_sky_classified.count(geometry_hash) != 0;
  }
  bool SkyEmissiveEnabled() const { return m_sky_emissive; }

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
  //
  // skinning non-null marks a GPU-skinned matrix-palette draw. `vertices` are
  // then the game's OBJECT-space ones and `transform` is the identity (plus the
  // projection correction), because the pose lives entirely in the bone palette:
  // the runtime skins into the BLAS first and applies the instance transform on
  // top, so the flush fold by V^-1 lands the result in exactly the place the CPU
  // bake used to put it. Caller-owned and read within the call - the palette is
  // copied into the pending instance, the indices go straight into CreateMesh,
  // which deep-copies them - so scratch vectors are fine.
  void SubmitMesh(const MaterialRef& material, u64 geometry_hash,
                  const std::vector<remixapi_HardcodedVertex>& vertices,
                  const std::vector<u32>& indices, const remixapi_Transform& transform,
                  remixapi_InstanceCategoryFlags category_flags, const DrawBlendState& blend,
                  const float* raw_modelview, const DrawDiagnostics& diagnostics,
                  const std::array<float, 6>* world_ui_projection = nullptr,
                  const DrawSkinning* skinning = nullptr);

  // How orthographic draws - HUD, menus, 2D screens - are handled.
  //   0 = dropped, the pre-feature behaviour
  //   1 = software-rasterized into a screen overlay composited at present
  //   2 = submitted as world-space geometry on a plane in front of the camera
  // Mode 2 is kept because it is the only one that puts UI inside the traced
  // world (it can light the scene, reflect, and be looked at from an angle), but
  // it is NOT passthrough: the runtime treats it as geometry, so it is denoised
  // and moves with the camera. Mode 1 is what looks like a normal UI.
  int UiMode() const { return m_ui_mode; }

  // What the user tagged a 2D draw as, in the Remix dev menu's texture grid.
  // Mirrors the three texture-category hash sets the runtime keeps
  // (rtx.uiTextures / rtx.ignoreTextures / rtx.worldSpaceUiTextures); `None` is
  // the untagged majority, which the heuristics are then free to judge.
  enum class UiTagClass
  {
    None,
    Ui,
    Ignore,
    WorldUi,
  };

  // Whether tag-driven routing is switched on at all. Off is the pre-feature
  // behaviour: no polling, no world-side detour while the menu is open, no
  // grid registration.
  bool UiTagRoutingEnabled() const { return m_ui_tag_routing; }

  // Whether a UI tag also diverts PERSPECTIVE draws into the screen overlay.
  // Subordinate to UiTagRoutingEnabled - the divert requires both - so the
  // routing master switch still kills everything tag-driven, while this one
  // alone restores exactly the ortho-only behaviour that preceded it.
  //
  // Only the UI tag is involved. Ignore and World Space UI on a perspective
  // draw are applied by the runtime's own category hook (it gets a real API
  // draw for those), and duplicating that here would double-apply.
  bool UiTagPerspectiveEnabled() const { return m_ui_tag_perspective; }

  // The click-to-tag view: every 2D draw is routed world-side so the runtime's
  // picker can reach it - an overlay draw is finished pixels by the time the
  // runtime sees it and has no object to pick. A user toggle
  // (RemixUiWorldView), NOT derived from the dev menu being open: the menu is
  // opened for plenty of non-tagging reasons and every one of them used to
  // yank the HUD into the world and back. m_runtime_ui_state is still polled,
  // but only as the `uistate` diagnostic on the frame line.
  bool UiTaggingModeActive() const { return m_ui_world_view; }

  // "Keep only what was tagged UI." Consulted for untagged draws only; an
  // explicit tag of any kind outranks it.
  bool UiStrictEnabled() const { return m_ui_strict; }

  // Looks a 2D draw's key up in the three polled tag sets. Precedence is
  // UI > Ignore > World UI: a hash tagged twice is a user mistake, and the
  // reading that keeps content on screen is the one to take.
  UiTagClass ClassifyUiDraw(u64 key) const;

  // Uploads a 2D draw's texture to the runtime purely so it appears in the
  // dev-menu texture grid with a thumbnail - there is no material and no mesh
  // behind it. Without this, a HUD element that only ever goes through the
  // overlay path is untaggable: it is never in the grid, because nothing ever
  // created a Remix texture for it.
  //
  // No-op for a null or empty texture, and cheap on repeat sightings (the
  // upload itself is deduplicated by content hash in UploadTexture).
  void RegisterOverlayTexture(const RemixTexture* texture);

  // The lookup key for an UNTEXTURED 2D draw, and the reason this is a named
  // helper rather than an expression at the call site.
  //
  // The runtime identifies an untextured API draw by its MESH HANDLE (see
  // rtx_fork_submit.cpp's keyIsMeshHash path, and the dev menu's click-to-tag
  // popup, which tags exactly that value). Our mesh handle IS
  // NonZeroHash(FoldHash(geometry_hash, material.hash)) - SubmitMesh mints the
  // mesh with `info.hash = mesh_hash`. So for the tag the user sets by clicking
  // a white box in tagging mode to match the key we look up in normal mode,
  // the two have to be computed the same way, from the same inputs, forever.
  //
  // They are: ortho draws are never skinned (skinning requires !is_ortho), and
  // the world path passes sky_emissive = false for ortho draws with these same
  // filter/wrap/alpha-test arguments. Hence emissive = false, 0 below.
  u64 UntexturedOrthoKey(const std::vector<remixapi_HardcodedVertex>& vertices,
                         const std::vector<u32>& indices, u8 filter_mode, u8 wrap_mode_u,
                         u8 wrap_mode_v, u8 alpha_test_type, u8 alpha_reference);

  // Rasterizes one draw into the screen overlay. Vertices are the decoded
  // remixapi ones; `modelview` is the draw's GX modelview, or null when the
  // vertices have already been baked into view space. `raw_projection` is the
  // draw's own untouched xfmem projection - read with ORTHO semantics by
  // default, and with PERSPECTIVE semantics when `perspective` is set.
  // `viewport` is the draw's viewport as an EFB-space rect (x, y, width,
  // height) - a game is free to put its HUD in a sub-rect and a full-screen
  // assumption would misplace it. `clip` is the draw's scissor rect (left, top,
  // right, bottom) in EFB space, from the same ComputeScissorRects every other
  // backend uses.
  //
  // `tag_bypass` marks a draw the user explicitly tagged "UI Texture". Such a
  // draw skips every drop heuristic - that tag IS the rescue lever, so an
  // automated rule must not be able to overrule it - and is marked in the
  // recorded call so the flush-time pre-world filter passes over it too. The
  // write-mask early-out is NOT bypassed: it is algebra, not a heuristic.
  //
  // `perspective` is for a HUD the game draws through the ordinary 3D frustum
  // and parks in front of the camera - only ever reached by an explicit UI tag,
  // never by a heuristic. It switches the View->NDC step to the GX perspective
  // convention and adds a w guard. Its standing assumption: the rasterizer
  // interpolates u/v/colour AFFINELY in screen space (RemixUiRaster::Vertex
  // carries no w), which is exact only while w is effectively constant across
  // the primitive - true of a screen-parked HUD quad, which is the only class
  // this is for. Violated, the tell is texture skew on the tagged element,
  // bending at the quad's diagonal; the remedy is to remove the tag.
  //
  // Returns whether the overlay TOOK the draw. False means "not consumed - the
  // caller decides what happens to it", and is returned in exactly two cases:
  // the overlay is structurally unavailable (wrong UI mode, no runtime entry
  // point, empty geometry), or a perspective draw had a vertex at or behind the
  // eye plane. Both leave the caller free to submit the draw as world geometry
  // instead. A draw refused by a POLICY rule returns true: that draw was
  // consumed and deliberately discarded, and re-submitting it world-side would
  // undo the policy.
  // `viewport` is (x, y, width, height, zRange, farZ) in EFB units - the last
  // two are the draw's own viewport z mapping, which is what turns ndc z into
  // the console's screen z for the overlay's depth plane (RemixUiDepth).
  bool SubmitUiDraw(const std::vector<remixapi_HardcodedVertex>& vertices,
                    const std::vector<u32>& indices, const float* modelview,
                    const std::array<float, 6>& raw_projection,
                    const std::array<float, 6>& viewport, const std::array<float, 4>& clip,
                    const RemixTexture* texture, u8 filter_mode, u8 wrap_mode_u, u8 wrap_mode_v,
                    const DrawBlendState& blend, bool tag_bypass = false,
                    bool perspective = false);

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
  //
  // Also CLASSIFIES the copy and returns the action to take: true = execute it
  // against the CPU EFB, false = discard it. Discard is the fall-through and is
  // bit-for-bit what this backend did before any of this existed, so an
  // unrecognised or misclassified copy can never look worse than it used to.
  // Returns false unconditionally when EFB emulation is off.
  bool NoteEfbCopy(const MathUtil::Rectangle<int>& src_rect, u32 dst_addr, u8 copy_format, bool xfb,
                   bool clear, bool half_scale, bool is_depth, bool is_intensity, float y_scale);

  // True when NoteEfbCopy should record. Read by the texture cache before it
  // touches bpmem at all.
  bool RecordEfbCopies() const { return m_trace_efb_copies; }

  // GFX_REMIX_EFB_EMULATION. Off means the texture cache takes the pre-change
  // path out of CopyEFB - record and return, touching no staging memory at all.
  bool EfbEmulationEnabled() const { return m_efb_emulation; }

  // True when `addr` is an EFB copy destination whose copy this backend
  // deliberately discarded, so the memory there holds no meaningful pixels.
  //
  // The draw path asks this to drop draws that sample such a destination. That
  // is the difference between a game's screen-space fake being ABSENT - letting
  // the traced result behind it show - and it being drawn as a blank rectangle
  // over the scene, which is what a zero-filled destination renders as. Returns
  // false when the skip is knobbed off or the emulation is off, so both restore
  // the draw-it-anyway behaviour.
  bool EfbDestinationDiscarded(u32 addr) const;

  // Composites the frame's 2D layer into the CPU EFB's colour plane, so an
  // executed copy encodes the UI the console would have had there rather than
  // bare clear colour. Called ONLY from the execute arm of
  // Remix::TextureCache::CopyEFB: a discarded copy must pay nothing for it.
  //
  // Safe to call repeatedly in one frame - it re-Begins the recorder afterwards,
  // so a second executed copy folds only what was submitted since the first.
  void FoldUiIntoEfb();

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
  u64 FrameIndex() const { return m_frame_index; }

private:
  struct MeshEntry
  {
    remixapi_MeshHandle handle = nullptr;
    u64 last_used_frame = 0;
    // Object-space bounding radius, measured once at create time from the
    // decoded vertices. Scaled by the modelview at classification time, this is
    // what separates a sky dome from small camera-welded geometry.
    float object_radius = 0.0f;
    // Identity of the GEOMETRY alone, without the material folded in. The sky
    // classifier keys on this rather than on the mesh hash, and the difference
    // is load-bearing: a Remix mesh bakes its surface material at create time
    // and the runtime keys meshes by info.hash, so the emissive sky variant MUST
    // have a different mesh hash from the non-emissive one. If classification
    // also keyed on the mesh hash, classifying a mesh would change the very hash
    // its classification is stored under - it would come back unclassified,
    // render non-emissive, return to the old hash, and oscillate forever.
    u64 geometry_hash = 0;
    // Last draw of this mesh, for the classification log line only.
    DrawDiagnostics diagnostics;
    // The pose-invariant key this mesh registered under in m_topology, or 0
    // for a mesh the dynamic-identity feature is not tracking (feature off,
    // skinned, world-UI). Carried here so a full-hash HIT can refresh its topo
    // entry in O(1) without regathering and rehashing the vertex stream, and
    // so ReapIdleMeshes can drop the topo entry alongside the mesh.
    u64 topology_key = 0;
  };

  // Cross-frame lineage of one topology - everything about a mesh that a
  // re-pose does NOT change (index bytes, per-vertex UVs and colours, vertex
  // count, material). When the full mesh hash observed under one topology key
  // changes across two distinct frames, the geometry is being regenerated per
  // frame, and the lineage is promoted: one handle, updated in place through
  // UpdateMeshBatched, instead of a fresh handle per pose.
  struct TopoEntry
  {
    // Full mesh hash seen on this topology's most recent submission. A
    // promoted entry whose incoming hash EQUALS this one is a paused
    // animation: the handle is reused as-is and no update call is made.
    u64 last_full_hash = 0;
    u64 last_seen_frame = 0;
    // Distinct frames on which the full hash changed under this key. Two are
    // required before promotion so a one-off spawn/despawn collision between
    // different objects sharing a topology cannot promote; any full-hash HIT
    // resets it, because a hash that repeats is cached geometry working.
    u32 change_streak = 0;
    // Non-zero once promoted: the synthetic m_meshes key the lineage's single
    // MeshEntry lives under (the full hash churns per frame, so it cannot
    // serve as the key anymore).
    u64 dynamic_mesh_key = 0;
    // Recorded so a count change under a stable key (an LOD switch is the
    // realistic case) demotes instead of feeding UpdateMeshBatched an update
    // the runtime would reject.
    u32 vertex_count = 0;
    u32 index_count = 0;
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
  // One distinct viewport RECT seen during a frame, and how many draws used it.
  // Keyed on the raw rect alone so the table reads as "where on screen did this
  // frame draw", with the depth pair carried along for the viewmodel question
  // rather than splitting the table by it.
  struct ViewportVariant
  {
    DrawViewport viewport = {};
    u32 draws = 0;
  };
  static constexpr size_t MAX_VIEWPORT_VARIANTS = 8;
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
    // Material-independent geometry identity - the key every sky decision uses.
    u64 geometry_hash = 0;
    // An orthographic draw, to be placed on the world-space UI plane at flush
    // time. `transform` then holds only the draw's own modelview - the mapping
    // onto the plane is built from the camera, which is not known while draws
    // are arriving, and is composed onto it in FlushPendingInstances.
    bool world_ui = false;
    std::array<float, 6> ortho_projection = {};
    // This draw's bone palette, in the compact order its mesh's blend indices
    // name, or empty for an unskinned draw. OWNED rather than borrowed: the
    // vertex manager's scratch is reused by the very next draw, and this
    // instance is not emitted until the whole frame has been seen.
    std::vector<remixapi_Transform> bones;
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
  // The tail OnAfterFrame runs whether the frame was presented or dropped as
  // minor: per-frame state resets, the sample-history roll, the frame counter
  // and the live-config refresh. Minor frames discard their view samples
  // instead of rolling them, so the next full frame's deltas are measured
  // against the previous full frame and not against a 2D pass.
  void FinishFrame(bool minor_frame);
  // Re-reads the handful of knobs that are allowed to change while a game is
  // running. Everything else is read once in Initialize and then baked into
  // meshes, materials, lights and classification state, so re-reading it would
  // be a lie; see the Liveness column of the metadata table in
  // Core/Config/RemixSettings.cpp, which is what the settings GUI greys out on.
  // Called at the very end of OnAfterFrame so a frame never straddles an edit.
  void RefreshLiveConfig();
  // Re-reads the runtime's three texture-category sets and its dev-menu state
  // into the members below, once per frame at the frame boundary. Polling
  // rather than a callback because the runtime has no push channel for this;
  // the cost is one API call per set and about two frames of latency between
  // tagging something and seeing it take effect.
  void PollRuntimeTagState();
  // Two-way sync of the tagging view with the Remix dev menu, via the
  // game-state store. See the definition for why the Dolphin setting is
  // compared against its own previous value rather than against the live flag.
  void SyncTaggingWorldView();
  // Reads a "1"/"0" game value. False means "no usable answer" - a missing key
  // reads as SUCCESS with size 0, so absence must not be mistaken for an off.
  bool ReadGameFlag(const char* key, bool& out);
  // One set's worth of that. Returns false and leaves `out` untouched when the
  // runtime refuses the read, so a transient failure cannot silently un-tag
  // everything for a frame. Grows and retries once on truncation.
  bool PollTagSet(const char* option_name, std::unordered_set<u64>& out);
  void LogProjectionVariants();
  // Union of every projection variant's depth range this frame. Shared by
  // SetupCamera and the sky classifier's size gate so the two cannot disagree
  // about how far away "far" is.
  bool ComputeDepthRange(float& near_plane, float& far_plane) const;
  void EstimateView();
  // The transform-signature test, run inside EstimateView's W-delta loop where
  // the ratio W(t)*W(t-1)^-1 is already in hand.
  void ClassifySky(u64 geometry_hash, u64 mesh_hash, const Affine& world_ratio,
                   const Affine& modelview,
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
  // Pose-invariant identity for the dynamic-mesh-identity feature: folds the
  // index bytes, the per-vertex texcoords and colours, the vertex count and
  // the material hash - and deliberately NOT positions or normals, which are
  // exactly what a re-pose changes. Lives next to the other identity helpers
  // so every rule about how identity is computed stays in one file. A member
  // rather than static because the texcoord+colour gather reuses
  // m_topology_scratch to keep heap allocation off the per-draw path.
  u64 TopologyHash(const std::vector<remixapi_HardcodedVertex>& vertices,
                   const std::vector<u32>& indices, u64 material_hash);

  remixapi_Interface m_interface = {};
  remixapi_HMODULE m_dll = nullptr;
  IDirect3D9Ex* m_d3d9 = nullptr;
  IDirect3DDevice9Ex* m_d3d9_device = nullptr;
  // Whether dxvk_RegisterD3D9Device accepted the pair above. That call transfers
  // ownership to the runtime, so it decides who releases them - see Shutdown.
  bool m_device_registered = false;
  bool m_valid = false;

  Common::EventHook m_after_frame_event;

  std::array<const RemixTexture*, 8> m_bound_textures = {};

  std::unordered_map<u64, remixapi_TextureHandle> m_textures;
  // Textures that exist ONLY so the overlay path's draws are taggable, mapped
  // to the frame each was last seen on. A material-backed texture is never in
  // here: EnsureMaterial erases the hash on its way past, because a texture the
  // scene actually references must outlive any 2D sighting of it. The sweep in
  // ReapIdleMeshes destroys the rest on the same 300-frame idleness rule the
  // meshes use, so a menu visited once does not hold its thumbnails forever. A
  // reaped hash keeps its tag - tags are hash-keyed config, not texture state -
  // and gets its thumbnail back the next time the draw appears.
  std::unordered_map<u64, u64> m_overlay_texture_frames;
  std::unordered_map<u64, remixapi_MaterialHandle> m_materials;
  std::unordered_map<u64, MeshEntry> m_meshes;
  // Topology-keyed lineages for the dynamic-mesh-identity feature. Only draws
  // eligible for it enter (feature on, unskinned, not world-UI); swept
  // alongside m_meshes in ReapIdleMeshes so one-off meshes cannot grow it.
  std::unordered_map<u64, TopoEntry> m_topology;
  // Scratch for TopologyHash's per-vertex texcoord+colour gather. A member so
  // the 60 Hz submit path does not heap-allocate per draw - the same idiom as
  // the vertex manager's reused vectors.
  std::vector<u8> m_topology_scratch;
  // Mesh hashes whose create or draw faulted inside the runtime. Never retried:
  // a handle that made the runtime throw once will do it every frame.
  std::unordered_set<u64> m_poisoned_meshes;

  std::array<float, 6> m_raw_projection = {};
  bool m_projection_latched = false;
  bool m_reference_usable = false;
  bool m_camera_valid = false;
  std::array<ProjectionVariant, MAX_PROJECTION_VARIANTS> m_projection_variants = {};
  u32 m_projection_variant_count = 0;
  DrawViewport m_reference_viewport = {};
  bool m_reference_viewport_usable = false;
  std::array<ViewportVariant, MAX_VIEWPORT_VARIANTS> m_viewport_variants = {};
  u32 m_viewport_variant_count = 0;
  bool m_projection_fix = true;
  bool m_viewport_fix = true;
  // The reference-latch gate and the aux-pass drop both read the PREVIOUS
  // presented frame, because the XFB copy that defines "the screen" is the
  // event that ends a frame - during a frame the only rect knowable is the one
  // before it, and using it is correct for the same reason the UI overlay's
  // XFB mapping is: presented size changes at a mode switch, not between two
  // draws. All three survive FinishFrame; they are refreshed, not reset.
  MathUtil::Rectangle<int> m_presented_rect = {};
  bool m_presented_rect_valid = false;
  // True when some perspective viewport covered >50% of that rect last
  // presented frame. False - menus, split screen, boot - stands both the
  // reference gate and the aux-pass drop down to pre-fix behaviour.
  bool m_ref_gate_valid = false;
  // Non-XFB EFB-copy source rects that were discarded WITH the clear flag:
  // regions whose world pixels existed only to feed a texture this backend
  // refuses to produce. This frame's collection, and the previous presented
  // frame's - the one ShouldDropAuxPass consults.
  std::vector<MathUtil::Rectangle<int>> m_scratch_clear_rects;
  std::vector<MathUtil::Rectangle<int>> m_scratch_clear_rects_previous;
  bool m_viewport_ref_xfb = true;
  bool m_efb_drop_aux_pass = true;
  bool m_trace_projections = false;
  bool m_gx_color = true;
  bool m_gx_texgen = true;
  bool m_gx_blend = true;
  bool m_gx_light_fix = true;
  bool m_gx_ras_channel = true;
  bool m_gx_ramp_albedo_skip = true;
  bool m_gx_lit_channel_texgen = true;
  bool m_gx_efb_alpha_passes = true;
  // Textures this backend synthesized by combining a colour with a mask. Keyed
  // on the pair so a mesh does not re-materialise every frame. Released in
  // Shutdown: these are full decoded images, and without that they would
  // accumulate for the life of the process and survive into the next game.
  std::unordered_map<u64, std::unique_ptr<RemixTexture>> m_masked_textures;
  bool m_ui_ras_channel = true;
  bool m_world_scissor_skip = true;
  bool m_trace_colors = false;
  bool m_gx_tev_color = true;
  bool m_gx_texture_stage = true;
  bool m_debug_color_routes = false;
  bool m_gpu_skinning = true;
  // RemixDynamicMeshIdentity AND-ed with "the loaded runtime actually has the
  // UpdateMeshBatched slot". Read once at init and never refreshed: promoted
  // meshes live in m_meshes under synthetic keys, so flipping it mid-game
  // would strand cache entries under keys the other mode never looks up.
  bool m_dynamic_mesh_identity = true;

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
  // Drop presents that carry only the 2D layer between full world frames; see
  // GFX_REMIX_SKIP_MINOR_FRAMES for the whole story. The ring holds the last
  // few frames' pending-instance counts (stored pre-drop) so "minor" is judged
  // against what a real scene here recently looked like.
  bool m_skip_minor_frames = false;
  std::array<u32, 4> m_recent_pending{};
  // Screen overlay. Sized from the render window each frame boundary (see
  // UpdateOverlaySurface), begun at the first UI draw of each frame and handed
  // to the runtime just before Present.
  UiRasterizer m_ui_raster;
  // Scratch, reused across draws so a per-frame HUD does not allocate ~140 times.
  std::vector<UiRasterizer::Vertex> m_ui_vertices;
  bool m_ui_frame_begun = false;
  // Re-derives m_surface_width/height from the live client rect, the overlay
  // scale knob and the native cap. Called at Initialize and then at every frame
  // boundary, which is what makes window resizes and the scale knob take effect
  // without a game restart - the F-Zero GX lesson: the surface used to be
  // latched at boot, so shrinking the window mid-game changed nothing and the
  // menu stayed at 84 ms of CPU fill per frame.
  void UpdateOverlaySurface();
  HWND m_hwnd = nullptr;
  u32 m_surface_width = 0;
  u32 m_surface_height = 0;
  // Whether Flush may serve an unchanged frame from the previous composite.
  // Live via RefreshLiveConfig; handed to the rasterizer just before Flush.
  bool m_ui_frame_cache = true;
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
  // The subset of the above whose most recent copy was DISCARDED, i.e. the
  // addresses this backend has deliberately left without content. Session-long
  // for the same reason as the set above, but unlike it this one is maintained
  // in both directions: a destination that later executes is removed, because
  // by then it does hold real pixels and a draw sampling it must not be skipped.
  //
  // A game re-uses a handful of these, so the linear scan is the right lookup at
  // this size and matches how m_efb_copy_destinations is already searched.
  std::vector<u32> m_efb_discarded_destinations;
  // The SOURCE rects of discarded copies, in EFB units - the regions of the EFB
  // this backend treats as scratch. A draw clipped to exactly one of them is
  // rendering INTO that scratch, which on console is captured and then covered
  // over, and here would be composited on top of the traced frame instead.
  //
  // Session-long and deduped for the same reason as the destination set: the
  // game re-uses the same handful of rects every frame, and the draw arrives
  // BEFORE the copy that would classify it, so this frame's decision has to be
  // made from what previous frames established.
  //
  // Full-EFB rects are never recorded - the pre-world rule covers a full-screen
  // clear, and admitting one here could blank a legitimate full-screen 2D layer.
  std::vector<MathUtil::Rectangle<int>> m_efb_discarded_rects;
  bool m_trace_efb_copies = true;
  // EFB emulation. All read once in Initialize, never on the copy path.
  // m_efb_emulation false is the pre-change behaviour in every particular: no
  // classification happens, so every class counter reads zero exactly as it did
  // when they did not exist.
  bool m_efb_emulation = true;
  bool m_efb_copy_2d = true;
  bool m_efb_copy_scene = false;
  bool m_efb_copy_depth = false;
  bool m_efb_copy_intensity = false;
  bool m_efb_xfb_encode = false;
  bool m_efb_ui_compose = true;
  bool m_efb_skip_discarded_tex = true;
  bool m_ui_drop_pre_world_blank = true;
  bool m_gx_light_drop_distant = false;
  bool m_fallback_light_enabled = true;
  // Perspective draws that reached submission so far THIS frame. The whole of
  // the Scene-vs-Composed2D signal: a colour copy taken while this is zero
  // cannot contain world content, because none has been drawn yet. Incremented
  // in SubmitMesh on the non-world_ui path, reset with the stats.
  u32 m_frame_world_draws = 0;
  // A second rasterizer instance, running at the EFB's own 640x528 rather than
  // at the swapchain's size, so that UI draws - whose viewport and scissor
  // arrive in EFB units already (RemixVertexManager.cpp:2608-2617) - can be
  // composited into the EFB with no rescaling at all. Deliberately a second
  // INSTANCE of the existing class, not a second implementation.
  UiRasterizer m_efb_ui_raster;
  bool m_efb_ui_frame_begun = false;
  // Read once in Initialize, never in the draw path. OFF for either is exactly
  // the pre-change behaviour: the draw is classified and submitted as before.
  bool m_ui_drop_dst_alpha = true;
  bool m_ui_drop_efb_copy_textures = false;

  // Tag-driven 2D routing. All five are Live (RefreshLiveConfig re-reads them
  // every frame boundary): they gate per-draw routing decisions and nothing
  // built at Initialize time - mesh, material, light, overlay surface - is
  // derived from any of them, which is the bar RefreshLiveConfig's comment
  // sets. RemixUiStrict being live is the point of it: it is the A/B lever for
  // finding out which 2D elements are worth keeping, and the perspective knob
  // is the A/B lever for "is this element better as a HUD or as world geometry".
  bool m_ui_tag_routing = true;
  bool m_ui_tag_perspective = true;
  bool m_ui_drop_pre_world = false;
  bool m_ui_drop_fullscreen_opaque = false;
  bool m_ui_strict = false;
  // Honour each 2D draw's real z mode in the overlay rasterizer (also Live,
  // same argument). Off zeroes the depth fields in every recorded DrawCall, so
  // the rasterizer runs the pure painter's algorithm it always ran.
  bool m_ui_depth = true;
  // The click-to-tag world view (RemixUiWorldView), Live for the same reason:
  // flip on to tag, off to play, no restart.
  bool m_ui_world_view = false;
  // Tagging-view sync state. m_tagging_view_config is the last value seen from
  // the Dolphin setting, kept separately from m_ui_world_view so that a
  // dev-menu toggle is not mistaken for a config edit on the following frame.
  bool m_tagging_view_seeded = false;
  bool m_tagging_view_config = false;
  // The manual position, kept apart from m_ui_world_view so the auto-switch
  // preference can override what is in effect without destroying what the user
  // last chose by hand - turning auto back off returns to that, not to wherever
  // the menu left things.
  bool m_tagging_view_manual = false;
  // The runtime's three texture-category sets, re-read once a frame. Hashes are
  // either a texture's content hash (textured draws) or a mesh hash (untextured
  // ones) - the runtime's own two identities for an API draw, which is what
  // lets one set serve both.
  std::unordered_set<u64> m_tag_ui;
  std::unordered_set<u64> m_tag_ignore;
  std::unordered_set<u64> m_tag_world_ui;
  // remixapi_UIState as of the last poll. Non-zero means the dev menu is open,
  // which is what switches 2D draws to the world path so they can be clicked.
  int m_runtime_ui_state = 0;
  // Reused across polls so the three reads a frame never heap-allocate once the
  // sets have settled.
  std::vector<uint64_t> m_tag_poll_buffer;
  // The "runtime is too old for GetTextureHashList" warning is worth saying
  // once and never again.
  bool m_tag_list_warned = false;
  // Pre-world 2D draws recorded this frame that carry no UI tag, counted at
  // record time because that is the only point where both facts are known. The
  // frame-end filter turns this into the ui_dropped_preworld stat when it
  // actually engages - the draws were recorded either way, so counting them at
  // record time and reporting them at flush time is what makes the number mean
  // "dropped" rather than "eligible".
  u32 m_frame_preworld_unprotected = 0;
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
  // Push classified sky geometry out to "infinity" by scaling it about the
  // camera position, so it sits behind every world draw instead of in front of
  // the island. Independent of the tagging mode above: tagging DELETES the sky
  // (SKY resolves to hidden on the API path), this RENDERS it. With this on,
  // RemixSkyAutoDetect = 1 is no longer strictly "log only" - classification
  // still does not tag, but it does now move geometry.
  bool m_sky_at_infinity = true;
  // Multiplier applied about the camera. Wind Waker's dome sits 15k-25k units
  // out against a 160k far plane, so anything past ~11 clears the world; 16
  // leaves margin without pushing so far that float precision or a ray tmax
  // becomes the next problem.
  float m_sky_infinity_scale = 16.0f;
  // Route falloff-free Spot lights to the distant branch. Off reproduces the
  // pre-fix translation exactly, so it is a clean A/B.
  bool m_gx_light_no_falloff_distant = true;
  // Give classified sky geometry an unlit (emissive) material, so it renders at
  // the colour the console authored instead of being shaded like a wall.
  bool m_sky_emissive = true;
  float m_sky_emissive_intensity = 1.0f;
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
