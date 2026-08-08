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

// For the TEV register/konst alpha constants, which are where a GX game puts the
// per-draw fade that decides whether a HUD element is visible at all.
#include "Core/System.h"
#include "VideoCommon/PixelShaderManager.h"

// For ComputeScissorRects: the UI overlay has to clip exactly the way every
// other backend does, or geometry the game scissored away is drawn anyway.
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/RenderState.h"
// For the bound cache entry's GC address and copy flags - the only place that
// knows a texture was decoded out of an EFB copy's destination.
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/XFMemory.h"

namespace Remix
{
namespace
{
// Channel ambient bright enough to be worth reporting: 0.25 of full scale. GX
// ambient is a flat per-channel add that Remix cannot express, so a frame with a
// lot of it will look darker after translation no matter how well the lights
// themselves are converted, and the log should be able to say that.
constexpr u8 AMBIENT_BRIGHT_THRESHOLD = 63;

// How many UI draws the per-draw GX dump prints on a trace frame. Wind Waker's
// title screen is 104 draws and its busiest attract scene 170, so this is sized
// to cover a whole screen rather than the first handful - a window that stops at
// 12 records the title art and misses every HUD sprite behind it.
constexpr u32 UI_DRAW_DUMP_LIMIT = 160;

// How far into a frame the colour trace follows world draws. Wind Waker's title
// scene is ~1860 draws and its SEA - the geometry this instrument exists for -
// starts past draw 400, so a small cap answers a different question than the one
// being asked. Draws with no colour attribute and no lit channel are skipped
// instead, which is what keeps the volume down: they are two thirds of the frame
// and there is nothing to say about them.
constexpr u32 COLOR_TRACE_DRAW_LIMIT = 2048;

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

// xfmem.matColor is a u32 whose bytes are R, G, B, A from the high end down
// (TransformUnit.cpp:329 memcpys it into an ABGR-indexed array, and 372 takes
// alpha from the low byte). Remix wants the same B, G, R, A memory order the
// vertex colour uses.
u32 MatColorToRemix(u32 mat_color)
{
  const u32 r = (mat_color >> 24) & 0xFFu;
  const u32 g = (mat_color >> 16) & 0xFFu;
  const u32 b = (mat_color >> 8) & 0xFFu;
  const u32 a = mat_color & 0xFFu;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

// Which vertex colour attribute feeds XF colour channel `channel`, or -1 when
// nothing does. Mirrors ParseColorAttributes (SWVertexLoader.cpp:163-190): the
// hardware does not require the two attributes to be populated in order, so a
// vertex carrying only colour1 has it redirected to channel 0 - and channel 1
// then has no vertex source at all.
int VertexSlotForChannel(const PortableVertexDeclaration& decl, u32 channel)
{
  if (decl.colors[0].enable)
    return channel == 0 ? 0 : (decl.colors[1].enable ? 1 : -1);
  if (decl.colors[1].enable)
    return channel == 0 ? 1 : -1;
  return -1;
}

// What TEV stage 0 rasterizes as its colour, reduced to something Remix can
// apply. GX resolves this per vertex in the XF unit (TransformColor,
// TransformUnit.cpp:316-375); we borrow all of it except the lighting
// accumulation, which the path tracer replaces.
//
// Two rules are easy to miss and both were being missed. Stage 0 does not
// necessarily rasterize channel 0 - bpmem.tevorders names the channel. And when
// that channel's matsource is MatColorRegister the vertex colour is ignored
// ENTIRELY and xfmem.matColor supplies the colour, which is how games tint
// objects through GXSetChanMatColor; every such tint was being dropped.
//
// A channel past xfmem.numChan.numColorChans reads as zero in the real pipeline
// (VertexShaderGen.cpp:910-916). Passing that on as black would be faithful to a
// TEV we are not emulating and would turn geometry black wherever stage 0 does
// not actually consume the raster colour, so it resolves to "no tint" instead.
struct RasterColor
{
  // Vertex colour attribute to hand Remix, or -1 to write opaque white. Writing
  // white when the colour is unused is not just tidiness: vertex bytes are part
  // of the mesh hash, so a draw tinted through the register keeps one mesh
  // handle across every tint it is drawn with.
  int vertex_slot = -1;
  u8 color_arg2 = REMIX_TEX_ARG_NONE;
  // Resolved separately: colour and alpha are two independent LitChannels with
  // their own material sources, so a draw legitimately takes its tint from the
  // vertex stream and its opacity from the register, or the reverse.
  u8 alpha_arg2 = REMIX_TEX_ARG_NONE;
  u32 tfactor = 0xFFFFFFFFu;
  bool baked_lighting = false;
  // The colour half and the alpha half wanted different vertex attributes. One
  // u32 of vertex colour cannot carry two channels' bytes, so the colour half
  // wins; recorded so the compromise is countable rather than invisible.
  bool channel_split = false;
  // Everything below is for the trace only - the resolution's own working, kept
  // so a log line can show WHY a draw came out with no tint rather than only
  // that it did.
  int ras_color_stage = -1;
  int ras_alpha_stage = -1;
  int color_channel = -1;
  int alpha_channel = -1;
};

// The first TEV stage whose COLOR combiner references the rasterized colour or
// alpha, and the first whose ALPHA combiner references the rasterized alpha.
// -1 for "no stage consumes it".
//
// The rasterized channel is named PER STAGE by
// bpmem.tevorders[stage>>1].getColorChan(stage&1) (Tev.cpp:489,
// PixelShaderGen.cpp:257), and the stage that consumes ras is routinely not
// stage 0 - so reading stage 0's channel and calling it the draw's channel
// answers a different question than the one being asked.
struct RasStages
{
  int color = -1;
  int alpha = -1;
};

RasStages FindRasStages()
{
  RasStages out;
  const u32 stages = std::min<u32>(bpmem.genMode.numtevstages + 1, 16);
  const auto reads_ras_color = [](TevColorArg arg) {
    // TevColorArg::RasColor = 10, RasAlpha = 11 (BPMemory.h:179-180).
    return arg == TevColorArg::RasColor || arg == TevColorArg::RasAlpha;
  };
  const auto reads_ras_alpha = [](TevAlphaArg arg) {
    // TevAlphaArg::RasAlpha = 5 (BPMemory.h:204).
    return arg == TevAlphaArg::RasAlpha;
  };
  for (u32 stage = 0; stage < stages; ++stage)
  {
    const auto& cc = bpmem.combiners[stage].colorC;
    const auto& ac = bpmem.combiners[stage].alphaC;
    if (out.color < 0 && (reads_ras_color(cc.a) || reads_ras_color(cc.b) ||
                          reads_ras_color(cc.c) || reads_ras_color(cc.d)))
    {
      out.color = static_cast<int>(stage);
    }
    if (out.alpha < 0 && (reads_ras_alpha(ac.a) || reads_ras_alpha(ac.b) ||
                          reads_ras_alpha(ac.c) || reads_ras_alpha(ac.d)))
    {
      out.alpha = static_cast<int>(stage);
    }
    if (out.color >= 0 && out.alpha >= 0)
      break;
  }
  return out;
}

// The XF colour channel a given TEV stage rasterizes, or -1 when that is not a
// lit colour channel this can name: an alpha bump, a hardwired zero, or an index
// past xfmem.numChan.numColorChans, which reads as zero in the real pipeline
// (VertexShaderGen.cpp:910-916). Passing that on as black would be faithful to a
// TEV we are not emulating, so it resolves to "no tint" instead.
int ChannelForStage(int stage)
{
  if (stage < 0)
    return -1;
  const u32 index = static_cast<u32>(stage);
  u32 channel;
  switch (bpmem.tevorders[index >> 1].getColorChan(index & 1))
  {
  case RasColorChan::Color0:
    channel = 0;
    break;
  case RasColorChan::Color1:
    channel = 1;
    break;
  default:
    return -1;
  }
  return channel < xfmem.numChan.numColorChans ? static_cast<int>(channel) : -1;
}

RasterColor ResolveRasterColor(const PortableVertexDeclaration& decl, bool gx_semantics,
                               bool resolve_alpha, bool gx_ras_channel)
{
  RasterColor out;

  // Pre-fix behaviour, kept as a clean A/B: channel 0's vertex colour whatever
  // the draw actually rasterizes, and no texture-stage state - which leaves the
  // runtime at defaults that never read a vertex colour at all.
  if (!gx_semantics)
  {
    out.vertex_slot = VertexSlotForChannel(decl, 0);
    return out;
  }

  int color_channel;
  int alpha_channel;
  if (gx_ras_channel)
  {
    // Each half takes the channel named by the stage that actually consumes it.
    // A draw whose stage 0 rasterizes nothing at all - very common once the
    // chain starts with a plain texture fetch - used to resolve to "no tint"
    // however strongly a later stage tinted it.
    const RasStages ras = FindRasStages();
    color_channel = ChannelForStage(ras.color);
    alpha_channel = ChannelForStage(ras.alpha);
    out.ras_color_stage = ras.color;
    out.ras_alpha_stage = ras.alpha;
  }
  else
  {
    color_channel = ChannelForStage(0);
    alpha_channel = color_channel;
    out.ras_color_stage = 0;
    out.ras_alpha_stage = 0;
  }
  out.color_channel = color_channel;
  out.alpha_channel = alpha_channel;

  if (color_channel < 0 && alpha_channel < 0)
    return out;

  // The register tint, assembled per half: RGB belongs to the colour channel and
  // A to the alpha channel, and the two are independent LitChannels that a draw
  // is free to point at different XF channels.
  u32 tfactor = 0xFFFFFFFFu;
  if (color_channel >= 0)
  {
    tfactor = (tfactor & 0xFF000000u) |
              (MatColorToRemix(xfmem.matColor[color_channel]) & 0x00FFFFFFu);
  }
  if (alpha_channel >= 0)
  {
    tfactor = (tfactor & 0x00FFFFFFu) |
              (MatColorToRemix(xfmem.matColor[alpha_channel]) & 0xFF000000u);
  }
  out.tfactor = tfactor;

  if (color_channel >= 0)
  {
    const int color_slot = VertexSlotForChannel(decl, static_cast<u32>(color_channel));
    const LitChannel& channel = xfmem.color[color_channel];
    if (channel.matsource == MatSource::MatColorRegister)
    {
      out.color_arg2 = REMIX_TEX_ARG_TFACTOR;
    }
    else if (color_slot >= 0)
    {
      out.color_arg2 = REMIX_TEX_ARG_VERTEX_COLOR0;
      out.vertex_slot = color_slot;
      // With lighting off the channel colour IS the vertex colour, which on GC is
      // overwhelmingly baked lighting - that is why the games use it. With
      // lighting on, the vertex colour is the material term the hardware
      // multiplies the lights into, so it is a real material colour and must not
      // be normalized.
      out.baked_lighting = !channel.enablelighting;
    }
  }

  if (!resolve_alpha || alpha_channel < 0)
    return out;

  const int alpha_slot = VertexSlotForChannel(decl, static_cast<u32>(alpha_channel));
  const LitChannel& channel = xfmem.alpha[alpha_channel];
  if (channel.matsource == MatSource::MatColorRegister)
  {
    out.alpha_arg2 = REMIX_TEX_ARG_TFACTOR;
  }
  else if (alpha_slot >= 0)
  {
    out.alpha_arg2 = REMIX_TEX_ARG_VERTEX_COLOR0;
    if (out.vertex_slot < 0)
      out.vertex_slot = alpha_slot;
    else if (out.vertex_slot != alpha_slot)
      out.channel_split = true;
  }
  return out;
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

// Determinant of a GX 3x4 modelview's rotation part. Negative means the
// transform mirrors, and a mirror swaps which side of a triangle its
// cross-product normal comes out on.
float RotationDeterminant(const float* m)
{
  return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
         m[2] * (m[4] * m[9] - m[5] * m[8]);
}

// Whether cross(p1 - p0, p2 - p0) over this draw's vertices points AWAY from the
// side the game means to light, so a generated flat normal has to be negated.
//
// The rule comes out of Clipper.cpp:519-545. In GX clip space the backface test
// is a signed area, normalZDir <= 0, inverted when xfmem.viewport.ht > 0. Since
// clip.x = raw0*x + raw1*z, clip.y = raw2*y + raw3*z and clip.w = -z, that area
// equals -raw0*raw2 * (n . v0) with n = cross(e0, e1) and v0 the view-space
// position; raw0 and raw2 are positive on any real frustum, so a front face is
// exactly one whose n points back at the camera - which is what the existing
// cross product already computes.
//
// So the CCW assumption is RIGHT for the ordinary negative-viewport case, not
// backwards. (Reading it off Vulkan's VK_FRONT_FACE_CLOCKWISE gives the opposite
// answer only if you miss VertexShaderGen.cpp:876, which negates clip y before
// the API ever sees it; put that back and the two references agree exactly.)
// What was actually missing is everything that can flip it:
//
//   - the positive-viewport inversion Clipper applies,
//   - a mirroring modelview, on the path where the vertices are still in object
//     space and the sign has to survive the instance transform,
//   - and cull_mode Front, where the game is telling us the back is the side
//     meant to be seen. cull_mode All never arrives: VertexManagerBase::Flush
//     gates DrawCurrentBatch on !m_cull_all.
bool ShouldFlipGeneratedNormals(const float* modelview)
{
  bool flip = false;

  // Jimmie Johnson's Anything with an Engine is the known positive-viewport
  // title; everything else is negative.
  if (xfmem.viewport.ht > 0.0f)
    flip = !flip;

  if (modelview != nullptr && RotationDeterminant(modelview) < 0.0f)
    flip = !flip;

  if (bpmem.genMode.cull_mode == CullMode::Front)
    flip = !flip;

  return flip;
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

// GX blend factors in Vulkan's numbering, which is what the runtime's legacy
// blend classifier matches against. Deliberately the NON dual-source table from
// VKPipeline.cpp:168-180: Dolphin's own Vulkan pipeline uses SRC1_ALPHA to
// emulate GX's separate alpha output, but Remix recognises no such factor and
// would classify every one of those draws as unblended.
constexpr u8 VK_FACTOR_ZERO = 0;
constexpr u8 VK_FACTOR_ONE = 1;
constexpr u8 VK_BLEND_ADD = 0;
constexpr u8 VK_BLEND_REVERSE_SUBTRACT = 2;

u8 ToVkSrcFactor(SrcBlendFactor factor)
{
  // Zero, One, DstClr, InvDstClr, SrcAlpha, InvSrcAlpha, DstAlpha, InvDstAlpha
  static constexpr u8 table[8] = {0, 1, 4, 5, 6, 7, 8, 9};
  return table[static_cast<u32>(factor) & 7];
}

u8 ToVkDstFactor(DstBlendFactor factor)
{
  // Zero, One, SrcClr, InvSrcClr, SrcAlpha, InvSrcAlpha, DstAlpha, InvDstAlpha
  static constexpr u8 table[8] = {0, 1, 2, 3, 6, 7, 8, 9};
  return table[static_cast<u32>(factor) & 7];
}

// The two substitutions the hardware makes when the EFB has no alpha channel or
// when a colour factor is asked of the alpha equation. Straight out of
// RenderState.cpp:78-108, and load-bearing: leaving DstAlpha in place on an
// RGB8 target makes the runtime read a blend mode the console never performed.
SrcBlendFactor RemoveDstAlphaUsage(SrcBlendFactor factor)
{
  switch (factor)
  {
  case SrcBlendFactor::DstAlpha:
    return SrcBlendFactor::One;
  case SrcBlendFactor::InvDstAlpha:
    return SrcBlendFactor::Zero;
  default:
    return factor;
  }
}

DstBlendFactor RemoveDstAlphaUsage(DstBlendFactor factor)
{
  switch (factor)
  {
  case DstBlendFactor::DstAlpha:
    return DstBlendFactor::One;
  case DstBlendFactor::InvDstAlpha:
    return DstBlendFactor::Zero;
  default:
    return factor;
  }
}

SrcBlendFactor RemoveDstColorUsage(SrcBlendFactor factor)
{
  switch (factor)
  {
  case SrcBlendFactor::DstClr:
    return SrcBlendFactor::DstAlpha;
  case SrcBlendFactor::InvDstClr:
    return SrcBlendFactor::InvDstAlpha;
  default:
    return factor;
  }
}

DstBlendFactor RemoveSrcColorUsage(DstBlendFactor factor)
{
  switch (factor)
  {
  case DstBlendFactor::SrcClr:
    return DstBlendFactor::SrcAlpha;
  case DstBlendFactor::InvSrcClr:
    return DstBlendFactor::InvSrcAlpha;
  default:
    return factor;
  }
}

// The draw's blend state, translated the way BlendingState::Generate does it
// (RenderState.cpp:110-197). Everything else in the backend classifies; this one
// translates, because the runtime already owns a classifier for exactly this
// state and it is the same one every D3D9 title goes through - so a GX draw
// arrives at translucent, emissive or multiplicative by the same route a native
// one would, rather than by a second guess made here.
//
// Without it every fire, glow, light shaft, window and water surface is opaque
// geometry - and in a path tracer that is worse than in a rasterizer, because
// those cards also cast full shadows.
// Evaluate the TEV ALPHA chain far enough to learn what a draw's final alpha
// actually is, which is not what this backend has been assuming.
//
// It read alpha as "stage 0's texture alpha times the vertex/register colour
// alpha". GX resolves it through the whole stage chain, and the standard way a
// game hides or fades a HUD element is to multiply in a per-draw CONSTANT held
// in a TEV register (GXSetTevColor) or a konst - one BP write, no vertex edits.
// Wind Waker's title screen does exactly this: one stage computing
// lerp(ZERO, TEXA, A0), with an alpha test of >= 64. Set A0 to zero and every
// pixel fails; miss A0 and the entire gameplay HUD renders over the title.
//
// The chain is evaluated TWICE, with texture alpha pinned to 0 and to 255, and
// again with rasterized alpha at both extremes. If the result does not depend on
// rasterized alpha, then alpha is affine in texture alpha and
//   alpha(t) = bias + scale * t
// describes it exactly - which covers lerp(ZERO, TEXA, K) with bias 0, scale K.
// Anything this cannot resolve returns false and leaves the old behaviour alone;
// being wrong in the conservative direction only costs what we already had.
// Tev.h:196-198, copied rather than included so the software backend's private
// header is not pulled into this one.
constexpr std::array<int, 4> s_tev_bias = {0, 128, -128, 0};
constexpr std::array<int, 4> s_tev_scale_left = {0, 1, 2, 0};
constexpr std::array<int, 4> s_tev_scale_right = {0, 0, 0, 1};

// Why the evaluator gave up, when it did. Purely so a low resolve rate can be
// explained instead of guessed at.
enum class TevAlphaBail
{
  None,
  TooManyStages,
  Konst,
  CompareMode,
  DependsOnRasterized,
};
TevAlphaBail g_tev_alpha_bail = TevAlphaBail::None;

bool ResolveTevAlpha(std::array<float, 4>& out_corners)
{
  g_tev_alpha_bail = TevAlphaBail::None;
  auto& system = Core::System::GetInstance();
  const auto& constants = system.GetPixelShaderManager().constants;

  const u32 stages = bpmem.genMode.numtevstages + 1;
  if (stages > 16)
  {
    g_tev_alpha_bail = TevAlphaBail::TooManyStages;
    return false;
  }

  // GX konst alpha selections, in KonstSel order: the eight fixed fractions and
  // then the four K registers' components. Only the alpha-legal entries matter.
  const auto konst_alpha = [&](u32 stage) -> int {
    const u32 sel = static_cast<u32>(bpmem.tevksel.GetKonstAlpha(stage));
    if (sel <= 7)  // 1, 7/8, 6/8 ... 1/8
      return static_cast<int>(255 - sel * 32);
    if (sel >= 16 && sel < 32)
    {
      const u32 reg = (sel - 16) % 4;
      const u32 component = (sel - 16) / 4;  // 0 = R, 1 = G, 2 = B, 3 = A
      return constants.kcolors[reg][component];
    }
    return -1;  // reserved / not alpha-legal
  };

  const auto evaluate = [&](int texture_alpha, int ras_alpha, int& result) {
    // Registers start at whatever GXSetTevColor left, exactly as Tev.cpp:398-404
    // seeds them, and each stage writes its own destination.
    std::array<int, 4> reg = {constants.colors[0][3], constants.colors[1][3],
                              constants.colors[2][3], constants.colors[3][3]};
    for (u32 stage = 0; stage < stages; ++stage)
    {
      const auto& ac = bpmem.combiners[stage].alphaC;
      const int konst = konst_alpha(stage);
      const auto input = [&](u32 source) -> int {
        switch (source)
        {
        case 0:
        case 1:
        case 2:
        case 3:
          return reg[source];  // APREV, A0, A1, A2
        case 4:
          return texture_alpha;
        case 5:
          return ras_alpha;
        case 6:
          return konst;
        default:
          return 0;  // ZERO
        }
      };
      const int a = input(static_cast<u32>(ac.a.Value()));
      const int b = input(static_cast<u32>(ac.b.Value()));
      const int c = input(static_cast<u32>(ac.c.Value()));
      const int d = input(static_cast<u32>(ac.d.Value()));
      if (a < 0 || b < 0 || c < 0 || d < 0)
      {
        g_tev_alpha_bail = TevAlphaBail::Konst;
        return false;
      }
      // Comparison mode is a different formula entirely and is not worth
      // modelling for UI; bail rather than guess.
      if (ac.bias == TevBias::Compare)
      {
        g_tev_alpha_bail = TevAlphaBail::CompareMode;
        return false;
      }

      // Tev.cpp:140-155, in the same order and with the same rounding.
      const int cc = c + (c >> 7);
      int temp = a * (256 - cc) + b * cc;
      temp <<= s_tev_scale_left[static_cast<u32>(ac.scale.Value())];
      temp += (ac.scale == TevScale::Divide2) ? 0 : (ac.op == TevOp::Sub) ? 127 : 128;
      temp = ac.op == TevOp::Sub ? (-temp >> 8) : (temp >> 8);
      int value = ((d + s_tev_bias[static_cast<u32>(ac.bias.Value())])
                   << s_tev_scale_left[static_cast<u32>(ac.scale.Value())]) +
                  temp;
      value >>= s_tev_scale_right[static_cast<u32>(ac.scale.Value())];
      if (ac.clamp)
        value = std::clamp(value, 0, 255);
      else
        value = std::clamp(value, -1024, 1023);
      reg[static_cast<u32>(ac.dest.Value())] = value;
    }
    result = reg[static_cast<u32>(bpmem.combiners[stages - 1].alphaC.dest.Value())];
    return true;
  };

  // Sample the chain at the four corners of (texture alpha, rasterized alpha).
  // Both are free variables: GX's standard modulate is TEXA * RASA, which is
  // BILINEAR in the pair - treating only texture alpha as free bailed on 146 of
  // 149 draws, which is to say on essentially everything.
  int corner[4] = {};
  if (!evaluate(0, 0, corner[0]) || !evaluate(255, 0, corner[1]) ||
      !evaluate(0, 255, corner[2]) || !evaluate(255, 255, corner[3]))
  {
    return false;
  }

  // Four corners describe the chain only if it really is bilinear. Check the
  // midpoint against what bilinear interpolation predicts and give up if the
  // chain is doing something else (a square, a comparison, a clamp biting
  // mid-range). Cheap, and it keeps the evaluator honest rather than plausible.
  int middle = 0;
  if (!evaluate(128, 128, middle))
    return false;
  const float predicted =
      0.25f * (static_cast<float>(corner[0]) + static_cast<float>(corner[1]) +
               static_cast<float>(corner[2]) + static_cast<float>(corner[3]));
  if (std::abs(predicted - static_cast<float>(middle)) > 3.0f)
  {
    g_tev_alpha_bail = TevAlphaBail::DependsOnRasterized;
    return false;
  }

  for (int i = 0; i < 4; ++i)
    out_corners[i] = static_cast<float>(corner[i]) / 255.0f;
  return true;
}

// ---- The TEV COLOUR chain -------------------------------------------------
//
// The same argument as ResolveTevAlpha, on the colour side, and it turns out to
// be where Wind Waker keeps its entire palette.
//
// The backend's model of a draw's colour is "material albedo, modulated by the
// rasterized colour". GX's is a chain of up to 16 combiners, and the standard way
// a GC title colours a surface is
//
//   stage 0:  lerp(cReg_a, cReg_b, RasColor)     <- the colour lives in REGISTERS
//   stage 1:  prev * TexColor                    <- the texture only shades it
//
// so the rasterized colour is a per-vertex LERP WEIGHT between two per-draw
// constants written by GXSetTevColor, not a tint. Measured on Wind Waker's title
// scene: 3263 of 3865 vertex-coloured draws are exactly `lerp(c0, c1, ras)` and
// another 546 are `lerp(c0, konst, ras)` - together essentially all of them. The
// sea is 982 draws of it in one frame, with c0 = (36, 24, 59) deep blue and
// c1 = (255, 255, 245) near-white. Passing the raw weight through as an albedo
// tint drops both registers and submits a GREYSCALE material, which is precisely
// the reported symptom: the ocean renders white.
//
// What is resolved here is the chain as a function of the rasterized colour, with
// the TEXTURE PINNED WHITE. That factorization is the point: Remix applies the
// real texture itself through textureColorArg1Source, so evaluating with a white
// texture yields exactly the other factor, and
//   submitted vertex colour = chain(ras, tex = white)
//   Remix computes            texture * submitted
// reproduces the console's product. It is checked rather than assumed - a chain
// that is not a clean product with the texture is refused.
//
// The result is affine in ras, so two endpoint colours describe it:
//   colour(ras) = at_zero + (at_one - at_zero) * ras / 255
// which covers lerp(a, b, ras) exactly, with at_zero = a and at_one = b.
struct TevColorFold
{
  bool valid = false;
  // Whether the chain is the identity in ras (at_zero 0, at_one 255). Folding
  // that would rewrite every vertex to the value it already had, so it is
  // detected and skipped: vertex bytes are part of the mesh hash and a no-op
  // fold must not be allowed to churn it.
  bool identity = false;
  std::array<int, 3> at_zero = {0, 0, 0};
  std::array<int, 3> at_one = {255, 255, 255};
};

// Why the colour evaluator gave up, when it did - so a low resolve rate is a
// measurement rather than a guess. Same role as TevAlphaBail.
enum class TevColorBail
{
  None,
  TooManyStages,
  Konst,
  CompareMode,
  AlphaInput,
  NotAffine,
  NotMultiplicative,
};
TevColorBail g_tev_color_bail = TevColorBail::None;

// GX konst COLOUR selections, in KonstSel order (Software/Tev.h:158-190). The
// eight fixed fractions, four unusable slots that read zero on hardware, the four
// K registers as RGB, then each register's R, G, B and A broadcast to all three
// channels.
constexpr std::array<int, 8> s_tev_konst_fractions = {255, 223, 191, 159, 128, 96, 64, 32};

bool ResolveTevColor(bool has_texture, TevColorFold& out)
{
  g_tev_color_bail = TevColorBail::None;
  out = TevColorFold{};

  const auto& constants = Core::System::GetInstance().GetPixelShaderManager().constants;
  const u32 stages = bpmem.genMode.numtevstages + 1;
  if (stages > 16)
  {
    g_tev_color_bail = TevColorBail::TooManyStages;
    return false;
  }

  const auto konst_color = [&](u32 stage, std::array<int, 3>& value) -> bool {
    const u32 sel = static_cast<u32>(bpmem.tevksel.GetKonstColor(stage));
    if (sel < 8)
    {
      value.fill(s_tev_konst_fractions[sel]);
      return true;
    }
    if (sel < 12)
      return false;  // reads zero on hardware, but it is a game bug - refuse it
    if (sel < 16)
    {
      const u32 reg = sel - 12;
      for (u32 c = 0; c < 3; ++c)
        value[c] = constants.kcolors[reg][c];
      return true;
    }
    if (sel < 32)
    {
      const u32 reg = (sel - 16) % 4;
      const u32 component = (sel - 16) / 4;  // 0 = R, 1 = G, 2 = B, 3 = A
      value.fill(constants.kcolors[reg][component]);
      return true;
    }
    return false;
  };

  // One evaluation of the whole chain at a fixed (texture, rasterized) colour.
  // Per-channel arithmetic copied from Tev::DrawColorRegular
  // (Software/Tev.cpp:80-97) in the same order and with the same rounding, then
  // clamped the way Tev.cpp:515-520 clamps.
  const auto evaluate = [&](int texture, const std::array<int, 3>& ras,
                            std::array<int, 3>& result) -> bool {
    std::array<std::array<int, 3>, 4> reg{};
    for (u32 i = 0; i < 4; ++i)
      for (u32 c = 0; c < 3; ++c)
        reg[i][c] = constants.colors[i][c];

    for (u32 stage = 0; stage < stages; ++stage)
    {
      const auto& cc = bpmem.combiners[stage].colorC;
      std::array<int, 3> konst{};
      const bool konst_ok = konst_color(stage, konst);

      // TevColorArg (BPMemory.h:167-185). The ALPHA-valued inputs are refused
      // rather than approximated: prev.aaa is written by the alpha chain, which
      // this evaluator does not run, and ras.aaa is a second free variable that
      // the affine-in-ras model has no room for. Neither appears in any Wind
      // Waker combiner measured, so refusing them costs nothing real.
      const auto input = [&](u32 source, std::array<int, 3>& value) -> bool {
        switch (source)
        {
        case 0:  // prev.rgb
        case 2:  // c0.rgb
        case 4:  // c1.rgb
        case 6:  // c2.rgb
          value = reg[source / 2];
          return true;
        case 8:  // tex.rgb
        case 9:  // tex.aaa - the texture is pinned opaque, so this is the same
          value.fill(texture);
          return true;
        case 10:  // ras.rgb
          value = ras;
          return true;
        case 12:  // one
          value.fill(255);
          return true;
        case 13:  // half
          value.fill(128);
          return true;
        case 14:  // konst
          value = konst;
          return konst_ok;
        case 15:  // zero
          value.fill(0);
          return true;
        default:  // 1, 3, 5, 7 (register alphas), 11 (ras.aaa)
          g_tev_color_bail = TevColorBail::AlphaInput;
          return false;
        }
      };

      std::array<int, 3> a{}, b{}, c{}, d{};
      if (!input(static_cast<u32>(cc.a.Value()), a) ||
          !input(static_cast<u32>(cc.b.Value()), b) ||
          !input(static_cast<u32>(cc.c.Value()), c) || !input(static_cast<u32>(cc.d.Value()), d))
      {
        if (g_tev_color_bail == TevColorBail::None)
          g_tev_color_bail = TevColorBail::Konst;
        return false;
      }
      // Comparison mode is a different formula entirely (Tev.cpp:100-135) and is
      // not affine in anything; bail rather than guess.
      if (cc.bias == TevBias::Compare)
      {
        g_tev_color_bail = TevColorBail::CompareMode;
        return false;
      }

      const u32 scale = static_cast<u32>(cc.scale.Value());
      for (u32 ch = 0; ch < 3; ++ch)
      {
        const int cc_weight = c[ch] + (c[ch] >> 7);
        int temp = a[ch] * (256 - cc_weight) + b[ch] * cc_weight;
        temp <<= s_tev_scale_left[scale];
        temp += (cc.scale == TevScale::Divide2) ? 0 : (cc.op == TevOp::Sub) ? 127 : 128;
        temp >>= 8;
        temp = cc.op == TevOp::Sub ? -temp : temp;
        int value =
            ((d[ch] + s_tev_bias[static_cast<u32>(cc.bias.Value())]) << s_tev_scale_left[scale]) +
            temp;
        value >>= s_tev_scale_right[scale];
        if (cc.clamp)
          value = std::clamp(value, 0, 255);
        else
          value = std::clamp(value, -1024, 1023);
        reg[static_cast<u32>(cc.dest.Value())][ch] = value;
      }
    }
    result = reg[static_cast<u32>(bpmem.combiners[stages - 1].colorC.dest.Value())];
    return true;
  };

  const std::array<int, 3> ras_zero = {0, 0, 0};
  const std::array<int, 3> ras_one = {255, 255, 255};
  const std::array<int, 3> ras_half = {128, 128, 128};

  std::array<int, 3> at_zero{}, at_one{}, at_half{};
  if (!evaluate(255, ras_zero, at_zero) || !evaluate(255, ras_one, at_one) ||
      !evaluate(255, ras_half, at_half))
  {
    return false;
  }

  // Affine in ras, or refused. Two endpoints describe the chain only if the
  // midpoint lands where they say it does; a square, a clamp biting mid-range or
  // a second reference to ras elsewhere in the chain all show up here.
  for (u32 ch = 0; ch < 3; ++ch)
  {
    const int predicted = (at_zero[ch] + at_one[ch]) / 2;
    if (std::abs(predicted - at_half[ch]) > 2)
    {
      g_tev_color_bail = TevColorBail::NotAffine;
      return false;
    }
  }

  // A clean product with the texture, or refused. Remix multiplies the real
  // texture back in through argument 1, so the factorization is only valid when
  // the chain vanishes with a black texture. Tested only when a texture is
  // actually bound: a chain that never samples one is trivially independent of
  // it, and then the material's own albedo is the identity instead.
  if (has_texture)
  {
    std::array<int, 3> dark_zero{}, dark_one{};
    if (!evaluate(0, ras_zero, dark_zero) || !evaluate(0, ras_one, dark_one))
      return false;
    for (u32 ch = 0; ch < 3; ++ch)
    {
      if (dark_zero[ch] > 2 || dark_one[ch] > 2)
      {
        g_tev_color_bail = TevColorBail::NotMultiplicative;
        return false;
      }
    }
  }

  for (u32 ch = 0; ch < 3; ++ch)
  {
    out.at_zero[ch] = std::clamp(at_zero[ch], 0, 255);
    out.at_one[ch] = std::clamp(at_one[ch], 0, 255);
  }
  out.valid = true;
  out.identity = out.at_zero == std::array<int, 3>{0, 0, 0} &&
                 out.at_one == std::array<int, 3>{255, 255, 255};
  return true;
}

// Apply a resolved fold to one vertex colour. `color` is the raw attribute as
// Dolphin stores it - R, G, B, A in memory order, so the low byte is red - and the
// result keeps that packing and the original alpha. Alpha is deliberately
// untouched: it is resolved separately by ResolveTevAlpha, and the overlay
// rasterizer and the alpha-test fold both read it.
u32 ApplyTevColorFold(const TevColorFold& fold, u32 color)
{
  u32 out = color & 0xFF000000u;
  for (u32 ch = 0; ch < 3; ++ch)
  {
    const int ras = static_cast<int>((color >> (ch * 8)) & 0xFFu);
    const int value = fold.at_zero[ch] + (fold.at_one[ch] - fold.at_zero[ch]) * ras / 255;
    out |= static_cast<u32>(std::clamp(value, 0, 255)) << (ch * 8);
  }
  return out;
}

void ResolveBlend(DrawBlendState& out, FrameStats& stats)
{
  const BlendMode& mode = bpmem.blendmode;

  // GX can only write destination alpha when the EFB format actually has one.
  const bool target_has_alpha = bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24;
  const bool alpha_test_may_succeed = bpmem.alpha_test.TestResult() != AlphaTestResult::Fail;
  const bool color_update = mode.color_update && alpha_test_may_succeed;
  const bool alpha_update = mode.alpha_update && target_has_alpha && alpha_test_may_succeed;
  const bool dst_alpha = bpmem.dstalpha.enable && alpha_update;

  out.write_mask = static_cast<u8>((color_update ? 0x7 : 0x0) | (alpha_update ? 0x8 : 0x0));

  if (mode.blend_enable)
  {
    out.blend_enabled = true;
    ++stats.blended;

    if (mode.subtract)
    {
      // GX subtract ignores the factor registers entirely: dst - src.
      out.color_blend_op = VK_BLEND_REVERSE_SUBTRACT;
      out.src_color_factor = VK_FACTOR_ONE;
      out.dst_color_factor = VK_FACTOR_ONE;
      out.alpha_blend_op = dst_alpha ? VK_BLEND_ADD : VK_BLEND_REVERSE_SUBTRACT;
      out.src_alpha_factor = VK_FACTOR_ONE;
      out.dst_alpha_factor = dst_alpha ? VK_FACTOR_ZERO : VK_FACTOR_ONE;
      return;
    }

    SrcBlendFactor src = mode.src_factor;
    DstBlendFactor dst = mode.dst_factor;
    if (!target_has_alpha)
    {
      src = RemoveDstAlphaUsage(src);
      dst = RemoveDstAlphaUsage(dst);
    }
    // The alpha equation cannot reference a colour, and note the crossover: the
    // SOURCE factor loses its destination-colour term and vice versa.
    const SrcBlendFactor src_alpha = RemoveDstColorUsage(src);
    const DstBlendFactor dst_alpha_factor = RemoveSrcColorUsage(dst);

    out.color_blend_op = VK_BLEND_ADD;
    out.alpha_blend_op = VK_BLEND_ADD;
    out.src_color_factor = ToVkSrcFactor(src);
    out.dst_color_factor = ToVkDstFactor(dst);
    out.src_alpha_factor = dst_alpha ? VK_FACTOR_ONE : ToVkSrcFactor(src_alpha);
    out.dst_alpha_factor = dst_alpha ? VK_FACTOR_ZERO : ToVkDstFactor(dst_alpha_factor);
    return;
  }

  if (mode.logic_op_enable && mode.logic_mode != LogicOp::NoOp)
  {
    // A raster logic op has no blend-equation equivalent, so Remix has nowhere
    // to put it. Counted rather than approximated: guessing here would turn XOR
    // overlays into visible translucent geometry.
    ++stats.logic_op;
  }
}
// stream does not carry one of its own. Split across two XF registers, four
// coords each.
u32 DefaultTexMatrixIndex(u32 coord)
{
  switch (coord)
  {
  case 0:
    return xfmem.MatrixIndexA.Tex0MtxIdx;
  case 1:
    return xfmem.MatrixIndexA.Tex1MtxIdx;
  case 2:
    return xfmem.MatrixIndexA.Tex2MtxIdx;
  case 3:
    return xfmem.MatrixIndexA.Tex3MtxIdx;
  case 4:
    return xfmem.MatrixIndexB.Tex4MtxIdx;
  case 5:
    return xfmem.MatrixIndexB.Tex5MtxIdx;
  case 6:
    return xfmem.MatrixIndexB.Tex6MtxIdx;
  default:
    return xfmem.MatrixIndexB.Tex7MtxIdx;
  }
}

// Everything about a draw's texture coordinate generation that does not vary per
// vertex, resolved once. `enabled` false means "pass vertex attribute 0 through
// raw", which is both the pre-fix behaviour and the fallback whenever the state
// is something this does not model.
struct TexGenState
{
  bool enabled = false;
  u32 coord = 0;
  TexGenType type = TexGenType::Regular;
  SourceRow source_row = SourceRow::Geom;
  TexSize projection = TexSize::ST;
  TexInputForm input_form = TexInputForm::ABC1;
  // decl.texcoords slot the source row names, or -1 when the source is geometry.
  int source_slot = -1;
  u32 default_matrix = 0;
  // Per-vertex texture matrix indices arrive baked into the coordinate's THIRD
  // float (VertexLoader.cpp:191-198), already masked to 6 bits. Reading only two
  // components - which is what the raw passthrough did - silently discards them.
  int matrix_index_slot = -1;
  bool dual_transform = false;
  const float* post_matrix = nullptr;
  bool post_normalize = false;
};

TexGenState ResolveTexGen(const PortableVertexDeclaration& decl, bool gx_texgen, u32 tev_stage)
{
  TexGenState out;
  if (!gx_texgen)
    return out;

  // The SAMPLING stage names the texgen slot it samples; it is not always slot 0,
  // and the sampling stage is not always stage 0 either. Taking the coordinate
  // from stage 0 while taking the texture from a later stage samples the right
  // image through the wrong mapping - worse than either mistake alone.
  const u32 coord = bpmem.tevorders[tev_stage >> 1].getTexCoord(tev_stage & 1);
  if (coord >= xfmem.numTexGen.numTexGens || coord >= 8)
    return out;

  const TexMtxInfo& info = xfmem.texMtxInfo[coord];
  out.coord = coord;
  out.type = info.texgentype;
  out.source_row = info.sourcerow;
  out.projection = info.projection;
  out.input_form = info.inputform;
  out.default_matrix = DefaultTexMatrixIndex(coord);

  if (info.sourcerow >= SourceRow::Tex0 && info.sourcerow <= SourceRow::Tex7)
  {
    const u32 slot =
        static_cast<u32>(info.sourcerow.Value()) - static_cast<u32>(SourceRow::Tex0);
    out.source_slot = (slot < 8 && decl.texcoords[slot].enable) ? static_cast<int>(slot) : -1;
  }

  if (decl.texcoords[coord].enable && decl.texcoords[coord].components >= 3)
    out.matrix_index_slot = static_cast<int>(coord);

  if (xfmem.dualTexTrans.enabled)
  {
    const PostMtxInfo& post = xfmem.postMtxInfo[coord];
    out.dual_transform = true;
    out.post_matrix = &xfmem.postMatrices[post.index * 4];
    out.post_normalize = post.normalize;
  }

  out.enabled = true;
  return out;
}

// Whether this draw's texgen produces exactly what reading vertex attribute 0
// raw would have produced. Purely a diagnostic: a game reporting zero
// non-trivial texgens is a game where this whole path cannot be responsible for
// anything, which is worth being able to establish from a log.
bool IsIdentityTexMatrix(const float* m)
{
  return m[0] == 1.0f && m[1] == 0.0f && m[2] == 0.0f && m[3] == 0.0f && m[4] == 0.0f &&
         m[5] == 1.0f && m[6] == 0.0f && m[7] == 0.0f;
}

bool IsTrivialTexGen(const TexGenState& state)
{
  if (!state.enabled)
    return true;
  if (state.type != TexGenType::Regular || state.source_slot != 0 || state.matrix_index_slot >= 0)
    return false;
  if (!IsIdentityTexMatrix(&xfmem.posMatrices[state.default_matrix * 4]))
    return false;
  // Dual transform is a global XF enable, so games leave it on and point every
  // coordinate at the identity post-matrix. Testing the enable rather than the
  // matrix would call every draw in such a game non-trivial.
  return !state.dual_transform || IsIdentityTexMatrix(state.post_matrix);
}

// One vertex through TransformTexCoordRegular (TransformUnit.cpp:112-192), plus
// the non-Regular texgen types from its caller at 403-442.
//
// Deliberately NOT ported: the trailing bpmem.texcoords[].scale multiply at
// TransformUnit.cpp:444-449. That converts to the texel-space coordinates the
// software rasterizer samples in; every hardware backend keeps coordinates
// normalized and lets the sampler scale, and so does Remix.
void GenerateTexCoord(const TexGenState& state, const u8* vertex,
                      const PortableVertexDeclaration& decl, const float* position,
                      const float* normal, float* out_uv)
{
  float src[3] = {0.0f, 0.0f, 0.0f};
  switch (state.type)
  {
  case TexGenType::Color0:
  case TexGenType::Color1:
  {
    // The channel colour's first two components become the coordinate. Reading
    // the vertex attribute rather than a lit channel is the same shortcut taken
    // for the raster colour: the lighting term is the path tracer's job.
    const int slot = state.type == TexGenType::Color0 ?
                         (decl.colors[0].enable ? 0 : -1) :
                         (decl.colors[1].enable ? 1 : (decl.colors[0].enable ? 0 : -1));
    u32 color = 0xFFFFFFFFu;
    if (slot >= 0)
      std::memcpy(&color, vertex + decl.colors[slot].offset, sizeof(u32));
    // Vertex colours are stored R, G, B, A in memory order.
    out_uv[0] = static_cast<float>(color & 0xFFu) / 255.0f;
    out_uv[1] = static_cast<float>((color >> 8) & 0xFFu) / 255.0f;
    return;
  }
  case TexGenType::EmbossMap:
  {
    // Emboss adds a per-vertex offset derived from a light direction and the
    // binormals, which is a bump-mapping trick with no meaning to a path tracer
    // that lights the surface itself. Take the source coordinate without the
    // offset rather than dropping the draw's texturing entirely.
    if (state.source_slot >= 0)
    {
      const AttributeFormat& format = decl.texcoords[state.source_slot];
      ReadFloats(vertex + format.offset, out_uv, std::min(format.components, 2));
    }
    return;
  }
  case TexGenType::Regular:
  default:
    break;
  }

  switch (state.source_row)
  {
  case SourceRow::Geom:
    src[0] = position[0];
    src[1] = position[1];
    src[2] = position[2];
    break;
  case SourceRow::Normal:
    src[0] = normal[0];
    src[1] = normal[1];
    src[2] = normal[2];
    break;
  case SourceRow::BinormalT:
  case SourceRow::BinormalB:
  {
    // The tangent frame, when the vertex carries one. Dolphin substitutes
    // cached values from the last vertex that did otherwise; that state lives in
    // VertexShaderManager and is not worth reaching for here, since a texgen
    // sourcing an absent binormal is emboss bump mapping we do not evaluate.
    const int slot = state.source_row == SourceRow::BinormalT ? 1 : 2;
    if (decl.normals[slot].enable)
      ReadFloats(vertex + decl.normals[slot].offset, src, 3);
    break;
  }
  default:
    // Tex0..Tex7. The third component is 1, not whatever the attribute holds -
    // that slot carries the texture matrix index, not a coordinate.
    if (state.source_slot >= 0)
    {
      const AttributeFormat& format = decl.texcoords[state.source_slot];
      ReadFloats(vertex + format.offset, src, std::min(format.components, 2));
    }
    src[2] = 1.0f;
    break;
  }

  // Shadow the Hedgehog's cutscene eyelids depend on this (Dolphin issue 11458).
  for (float& component : src)
  {
    if (std::isnan(component))
      component = 1.0f;
  }

  u32 matrix_index = state.default_matrix;
  if (state.matrix_index_slot >= 0)
  {
    float baked = 0.0f;
    std::memcpy(&baked, vertex + decl.texcoords[state.matrix_index_slot].offset + 2 * sizeof(float),
                sizeof(float));
    matrix_index = static_cast<u32>(baked) & 0x3f;
  }
  const float* const matrix = &xfmem.posMatrices[matrix_index * 4];

  // AB11 feeds (a, b, 1, 1) rather than (a, b, c, 1), which is why the third and
  // fourth matrix columns both act as translation for it.
  const bool ab11 = state.input_form == TexInputForm::AB11;
  const float z_term = ab11 ? 1.0f : src[2];
  const float w_term = 1.0f;
  float dst[3];
  dst[0] = matrix[0] * src[0] + matrix[1] * src[1] + matrix[2] * z_term + matrix[3] * w_term;
  dst[1] = matrix[4] * src[0] + matrix[5] * src[1] + matrix[6] * z_term + matrix[7] * w_term;
  dst[2] = state.projection == TexSize::STQ ?
               matrix[8] * src[0] + matrix[9] * src[1] + matrix[10] * z_term + matrix[11] * w_term :
               1.0f;

  if (state.dual_transform)
  {
    float temp[3] = {dst[0], dst[1], dst[2]};
    if (state.post_normalize)
    {
      Normalize(temp);
    }
    const float* const post = state.post_matrix;
    dst[0] = post[0] * temp[0] + post[1] * temp[1] + post[2] * temp[2] + post[3];
    dst[1] = post[4] * temp[0] + post[5] * temp[1] + post[6] * temp[2] + post[7];
    dst[2] = post[8] * temp[0] + post[9] * temp[1] + post[10] * temp[2] + post[11];
  }

  if (dst[2] == 0.0f)
  {
    // Hardware special case, visible in Rogue Squadron 3's Hoth sky and The Last
    // Story's shadow culling.
    out_uv[0] = std::clamp(dst[0] / 2.0f, -1.0f, 1.0f);
    out_uv[1] = std::clamp(dst[1] / 2.0f, -1.0f, 1.0f);
    return;
  }

  // Projected coordinates divide by q. The console does this per pixel
  // (PixelShaderGen.cpp:1966-1969); doing it per vertex is the approximation a
  // mesh-cache design forces, and it is exact whenever q is constant across the
  // triangle - which covers everything but genuine projective texturing.
  out_uv[0] = dst[0] / dst[2];
  out_uv[1] = dst[1] / dst[2];
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
//
// The VIEWPORT is the same problem one step further out, and it folds into the
// same affine. GX finishes the mapping to the EFB per draw as
//   screen.x = (clip.x / clip.w) * wd + cx      (Clipper.cpp:553-554)
// with cx the scissor-adjusted centre. Remix's one camera renders the CENTRED
// reference projection into the full presented image - which is to say, into
// the REFERENCE draw's rect. Requiring that camera to land the corrected point
// x' on the same EFB pixel the draw's own projection and viewport would land x,
// with z (and therefore w) untouched:
//   raw0_r*x'*wd_r + cx_r*(-z) == (raw0_d*x + raw1_d*z)*wd_d + cx_d*(-z)
//   =>  x' = [(wd_d*raw0_d)/(wd_r*raw0_r)]*x
//          + [(wd_d*raw1_d - (cx_d - cx_r))/(wd_r*raw0_r)]*z
// and likewise for y through ht, cy, raw[2] and raw[3]. Still a scale plus a
// shear along z with no perspective term, so it composes onto the instance
// transform exactly as before.
//
// Two things worth reading off that result. First, it is the end-to-end
// solution rather than two corrections applied in sequence, so the composition
// ORDER cannot resurface as a bug: expanded, it is viewport OUTERMOST (scale
// a'*a, shear a'*b + b'), and the other order - which would scale the viewport
// offset by the draw's FOV ratio - is simply not what the algebra says.
// Second, cx_r appears only as a DIFFERENCE against cx_d, and wd_r only as the
// scale the camera implicitly renders at: the reference's own offset never
// enters the target side, for exactly the reason raw[1]_r/raw[3]_r do not - the
// camera does not apply it.
//
// With the draw's viewport equal to the reference's, wd_d/wd_r is 1 and
// cx_d - cx_r is 0, and every term reduces to the projection-only expression
// above. That is the regression guarantee, and it is why there is one
// correction here and not two mechanisms.
struct ProjectionCorrection
{
  float x_scale = 1.0f;
  float x_shear = 0.0f;
  float y_scale = 1.0f;
  float y_shear = 0.0f;
};

// One draw's viewport expressed against the frame's reference viewport, in the
// two dimensionless terms the correction above needs. The defaults are the
// identity: a draw sharing the reference rect, and every draw at all when
// RemixViewportFix is off.
struct ViewportFold
{
  float x_scale = 1.0f;
  float y_scale = 1.0f;
  float x_offset = 0.0f;
  float y_offset = 0.0f;
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
                                           const ViewportFold& viewport,
                                           ProjectionCorrection& out)
{
  // A reference with no x or y scale is not a frustum at all; there is nothing
  // meaningful to fold onto.
  if (std::abs(reference[0]) < 1e-6f || std::abs(reference[2]) < 1e-6f)
    return CorrectionResult::Unrepresentable;

  // reference[1] / reference[3] deliberately do not appear: the camera never
  // applied them, so the draw's own off-centre terms fold in whole.
  //
  // With an identity viewport fold - scale 1, offset 0 - these are the
  // projection-only expressions term for term: multiplying by 1.0f is exact,
  // and subtracting 0.0f / reference[0] subtracts a zero. That is the same-rect
  // and knob-off guarantee, and it is a property of these four lines.
  out.x_scale = viewport.x_scale * (draw[0] / reference[0]);
  out.x_shear = viewport.x_scale * (draw[1] / reference[0]) - viewport.x_offset / reference[0];
  out.y_scale = viewport.y_scale * (draw[2] / reference[2]);
  out.y_shear = viewport.y_scale * (draw[3] / reference[2]) - viewport.y_offset / reference[2];

  if (!std::isfinite(out.x_scale) || !std::isfinite(out.x_shear) ||
      !std::isfinite(out.y_scale) || !std::isfinite(out.y_shear))
  {
    return CorrectionResult::Unrepresentable;
  }

  // Magnitude only, deliberately. A NEGATIVE combined scale means the draw's
  // viewport has the opposite sign to the reference's - a mirrored placement -
  // and the algebra above places it correctly, so refusing it would put that
  // draw back in the middle of the screen, which is the bug this exists to
  // fix. It is applied and COUNTED instead (FrameStats::viewport_mirrored),
  // because no game is known to mix viewport signs mid-frame and the shading of
  // a mirrored instance transform is unvalidated here. If one turns up, the
  // counter says so and flipping this case to refuse is a two-line change.
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

// Where a draw lands on the EFB, as the overlay path needs it: the viewport as
// (x, y, width, height, zRange, farZ) and the scissor as (left, top, right,
// bottom), both in EFB units. The z pair is the draw's own viewport depth
// mapping - what turns ndc z into the console's 24-bit screen z - carried so
// the overlay's depth plane compares the same values the console compared.
struct UiPlacement
{
  std::array<float, 6> viewport = {};
  std::array<float, 4> clip = {};
};

// Viewport AND scissor, derived exactly the way BPFunctions does it for every
// other backend - same ComputeScissorRects, same Best() rectangle, so the
// offsets can never disagree with what the game intended.
//
// The scissor is not optional decoration here. GX games clip UI with it
// constantly - sliding panels, wipes, text windows, and banks of elements that
// are all drawn but scissored down to whichever one is showing. Without it every
// one of them appears at once.
//
// Projection-agnostic on purpose, and that is a property of GX rather than an
// assumption: this reads only viewport and scissor state, and the console
// applies both identically whichever projection produced the NDC.
// Clipper.cpp:547-556 maps perspective NDC through the very same
// xfmem.viewport wd/ht/xOrig/yOrig fields an ortho draw goes through, so one
// derivation serves the ortho overlay block and the perspective divert both.
UiPlacement ComputeUiPlacement()
{
  const BPFunctions::ScissorResult scissor = BPFunctions::ComputeScissorRects(
      bpmem.scissorTL, bpmem.scissorBR, bpmem.scissorOffset, xfmem.viewport);
  const BPFunctions::ScissorRect native_rc = scissor.Best();

  UiPlacement placement;
  placement.viewport = {
      (xfmem.viewport.xOrig - static_cast<float>(native_rc.x_off)) - xfmem.viewport.wd,
      (xfmem.viewport.yOrig - static_cast<float>(native_rc.y_off)) + xfmem.viewport.ht,
      2.0f * xfmem.viewport.wd, -2.0f * xfmem.viewport.ht,
      xfmem.viewport.zRange, xfmem.viewport.farZ};
  placement.clip = {
      static_cast<float>(native_rc.rect.left), static_cast<float>(native_rc.rect.top),
      static_cast<float>(native_rc.rect.right), static_cast<float>(native_rc.rect.bottom)};
  return placement;
}
}  // namespace

VertexManager::VertexManager() = default;

VertexManager::~VertexManager() = default;

bool VertexManager::ScissorIsEmpty()
{
  if (m_scissor_key_valid && m_scissor_key_tl == bpmem.scissorTL.hex &&
      m_scissor_key_br == bpmem.scissorBR.hex && m_scissor_key_off == bpmem.scissorOffset.hex)
  {
    return m_scissor_empty;
  }

  // The viewport argument only decides which of several rectangles is "best"
  // (ScissorResult::IsWorse); it cannot make the list empty, so it is safe to
  // pass the live one while keying the cache on the scissor registers alone.
  const BPFunctions::ScissorResult scissor = BPFunctions::ComputeScissorRects(
      bpmem.scissorTL, bpmem.scissorBR, bpmem.scissorOffset, xfmem.viewport);
  // An empty rectangle list is the console saying "this draw covers no pixels":
  // the constructor bails immediately on left > right or top > bottom, and
  // otherwise produces nothing when every wrapped range clamps outside the EFB
  // (BPFunctions.cpp:100-150). Best() would hand back a fabricated out-of-bounds
  // rect in that case, so testing the returned rect instead of the list would
  // never see it.
  m_scissor_empty = scissor.rectangles.empty();
  m_scissor_key_tl = bpmem.scissorTL.hex;
  m_scissor_key_br = bpmem.scissorBR.hex;
  m_scissor_key_off = bpmem.scissorOffset.hex;
  m_scissor_key_valid = true;
  return m_scissor_empty;
}

const DrawViewport& VertexManager::CurrentDrawViewport()
{
  const std::array<float, 4> rect = {xfmem.viewport.xOrig, xfmem.viewport.yOrig, xfmem.viewport.wd,
                                     xfmem.viewport.ht};
  if (m_viewport_key_valid && m_viewport_key_rect == rect &&
      m_viewport_key_tl == bpmem.scissorTL.hex && m_viewport_key_br == bpmem.scissorBR.hex &&
      m_viewport_key_off == bpmem.scissorOffset.hex)
  {
    return m_viewport_cached;
  }

  // The same derivation the UI path uses (see the screen-overlay block below),
  // which is in turn the one BPFunctions hands every other backend: same
  // ComputeScissorRects, same Best() rectangle, same xOrig - x_off. One
  // derivation in the tree, so the world and UI placements cannot disagree
  // about where the game asked for the draw.
  const BPFunctions::ScissorResult scissor = BPFunctions::ComputeScissorRects(
      bpmem.scissorTL, bpmem.scissorBR, bpmem.scissorOffset, xfmem.viewport);
  const BPFunctions::ScissorRect native_rc = scissor.Best();

  m_viewport_cached.rect = rect;
  m_viewport_cached.cx = xfmem.viewport.xOrig - static_cast<float>(native_rc.x_off);
  m_viewport_cached.cy = xfmem.viewport.yOrig - static_cast<float>(native_rc.y_off);
  m_viewport_cached.zrange = xfmem.viewport.zRange;
  m_viewport_cached.farz = xfmem.viewport.farZ;

  m_viewport_key_rect = rect;
  m_viewport_key_tl = bpmem.scissorTL.hex;
  m_viewport_key_br = bpmem.scissorBR.hex;
  m_viewport_key_off = bpmem.scissorOffset.hex;
  m_viewport_key_valid = true;
  return m_viewport_cached;
}

void VertexManager::DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex)
{
  if (!g_remix_api || !g_remix_api->IsValid())
    return;

  FrameStats& stats = g_remix_api->Stats();
  ++stats.draws_seen;

  // ---- Classify -----------------------------------------------------------

  // Orthographic projection is HUD/UI/menus. RemixUiMode decides what happens to
  // it: dropped (0, what v1 did), software-rasterized into the screen overlay
  // (1, the default and the only one that looks like a HUD), or carried through
  // this function like any other draw and placed on a plane in front of the
  // camera at flush time (2).
  //
  // Everything below that reads the PERSPECTIVE projection is skipped for these:
  // an ortho draw must not latch the reference camera, must not be folded onto
  // it, must not be tested for sky, and above all must not reach the modelview
  // histogram - its modelview is not a view matrix, and one in the histogram
  // would corrupt the very camera the frame is rendered from.
  const bool is_ortho = xfmem.projection.type != ProjectionType::Perspective;
  const int ui_mode = g_remix_api->UiMode();
  if (is_ortho && ui_mode == 0)
  {
    ++stats.skipped_ortho;
    return;
  }

  // First perspective draw of the frame defines the camera AND the screen rect
  // that camera is implicitly rendering into; every distinct projection and
  // viewport after it is tracked so the frame log can say whether the reference
  // is the one actually carrying the scene. Ortho draws take neither: their
  // projection is not a frustum and their viewport is the UI's, and letting
  // either latch a reference would aim every world fold at the HUD.
  const DrawViewport* draw_viewport = is_ortho ? nullptr : &CurrentDrawViewport();
  const int projection_slot =
      is_ortho ? -1 :
                 g_remix_api->ObserveProjection(xfmem.projection.rawProjection, *draw_viewport);

  // A discarded helper pass: this viewport's rect was EFB-copied to a non-XFB
  // destination with clear and the copy discarded (learned last frame). On
  // console the clear erased these pixels and the main pass overdrew the
  // region; as world geometry nothing would, so they would linger as a ghost
  // mini-scene in a corner of the view - or, before the reference gate, steal
  // the reference outright (Sonic Unleashed's quarter-screen). After
  // ObserveProjection on purpose, so the variant tables still show the pass
  // that was dropped.
  if (!is_ortho && g_remix_api->ShouldDropAuxPass(*draw_viewport))
  {
    g_remix_api->NoteAuxPassDropped();
    return;
  }

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
  //
  // The alpha_update half has to be qualified the way BlendingState::Generate
  // qualifies it (RenderState.cpp:115-119): GX can only write destination alpha
  // when the EFB format HAS one, and on RGB8_Z24 / Z24 / RGB565_Z16 an
  // alpha-only write is literally a no-op (SWEfbInterface.cpp:40-48). Testing
  // the raw bit let the classic "write an EFB alpha mask" draw through, and it
  // then rendered as opaque geometry.
  const bool target_has_alpha = bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24;
  const bool writes_colour = bpmem.blendmode.color_update;
  const bool writes_alpha = bpmem.blendmode.alpha_update && target_has_alpha;

  // Three configurations that are algebraic no-ops on console - the game is
  // using them to say "draw nothing" - and that this backend would otherwise
  // render as solid geometry:
  //   - a NoOp logic op (SWEfbInterface.cpp:353-355; RenderState.cpp:162-169
  //     turns it straight into color_update = false)
  //   - Zero * src + One * dst, which leaves the destination bit-exact
  //     (SWEfbInterface.cpp:312-332)
  const bool logic_noop = !bpmem.blendmode.blend_enable && bpmem.blendmode.logic_op_enable &&
                          bpmem.blendmode.logic_mode == LogicOp::NoOp;
  const bool blend_noop = bpmem.blendmode.blend_enable && !bpmem.blendmode.subtract &&
                          bpmem.blendmode.src_factor == SrcBlendFactor::Zero &&
                          bpmem.blendmode.dst_factor == DstBlendFactor::One;

  if ((!writes_colour && !writes_alpha) || logic_noop || blend_noop ||
      bpmem.alpha_test.TestResult() == AlphaTestResult::Fail)
  {
    ++stats.skipped_invisible;
    return;
  }

  // Scissored away entirely. Every reference clips every draw - the software
  // rasterizer rejects pixels outside the scissor rect (Rasterizer.cpp:364-365)
  // and the hardware backends set the scissor per draw (BPFunctions.cpp:103-104)
  // - while this path read scissor state nowhere, so a draw the game hid by
  // scissoring to an empty rect was fully visible AND cast shadows.
  //
  // Only the all-or-nothing case is expressible here: a path tracer has no
  // screen-space clip, so a draw the scissor merely trims is submitted whole,
  // deliberately. The UI path, which rasterizes into a 2D buffer, does honour
  // partial scissors.
  if (!is_ortho && g_remix_api->WorldScissorSkipEnabled() && ScissorIsEmpty())
  {
    ++stats.skipped_scissor;
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

  // v1 materials were "whatever TEV stage 0 samples". Every other stage - and
  // therefore every multi-stage combiner effect - was ignored, which drops the
  // texture outright whenever stage 0 does not sample one.
  //
  // Wind Waker's sea is exactly that shape: stage 0 is a pure register lerp with
  // no texture at all and stage 1 samples the water texture. So the sea arrived
  // untextured AND unregistered - a flat grey card. The first ENABLED stage is
  // the right one to hand Remix, and taking it only when stage 0 is untextured
  // makes this a strict extension: a draw that samples on stage 0 resolves
  // exactly as before.
  //
  // Two things deliberately keep reading STAGE 0 regardless, so this cannot
  // disturb behaviour that is already confirmed in-game: the sky texture-hash
  // match, and DrawDiagnostics::texture_hash, which the sky auto-detector's
  // untextured->IGNORE rule keys on. Both are decisions about identity rather
  // than about shading.
  const bool stage0_textured = bpmem.tevorders[0].getEnable(0) != 0;
  const u32 stage0_texmap = bpmem.tevorders[0].getTexMap(0);
  u32 albedo_texmap = stage0_texmap;
  bool albedo_textured = stage0_textured;
  // Which TEV stage the albedo came from. Load-bearing beyond bookkeeping: the
  // texture COORDINATE has to come from the same stage, or the right image is
  // sampled through the wrong mapping.
  u32 albedo_stage = 0;
  if (!stage0_textured && g_remix_api->GxTextureStageEnabled())
  {
    const u32 tev_stages = std::min<u32>(bpmem.genMode.numtevstages + 1, 16);
    for (u32 stage = 1; stage < tev_stages; ++stage)
    {
      if (bpmem.tevorders[stage >> 1].getEnable(stage & 1) == 0)
        continue;
      albedo_texmap = bpmem.tevorders[stage >> 1].getTexMap(stage & 1);
      albedo_textured = true;
      albedo_stage = stage;
      ++stats.texture_later_stage;
      break;
    }
  }
  const RemixTexture* albedo = nullptr;
  u8 filter_mode = 1;   // MDL Filter::Linear
  u8 wrap_mode_u = 1;   // MDL WrapMode::Repeat
  u8 wrap_mode_v = 1;
  // Provenance of stage 0's texture, taken from the cache entry rather than from
  // the texture, because the texture cannot tell: on this backend an EFB copy
  // writes nothing, so an entry over that memory decodes stale bytes perfectly
  // happily and is indistinguishable from a real texture by content alone.
  u32 texture_addr = 0;
  bool texture_is_efb_copy = false;
  bool texture_is_xfb_copy = false;

  if (albedo_textured)
  {
    albedo = g_remix_api->GetBoundTexture(albedo_texmap);
    const RcTcacheEntry& entry = g_texture_cache->GetBoundEntry(albedo_texmap);
    if (entry)
    {
      texture_addr = entry->addr;
      texture_is_efb_copy = entry->is_efb_copy;
      texture_is_xfb_copy = entry->is_xfb_copy;
    }
    // Our texture cache does not execute EFB/XFB copies, so an entry produced
    // by one never receives pixels through Load(). That is exactly the set of
    // draws we cannot translate (dynamic shadow maps, heat haze, water
    // reflections) - and also the set of custom-texture formats we do not
    // decode in v1.
    if (albedo == nullptr || !albedo->HasData())
    {
      // Only a STAGE 0 texture is load-bearing enough to drop the draw over.
      // A draw that reached here through a later stage rendered untextured
      // before this change, so falling back to that is strictly no worse -
      // whereas skipping it would make geometry disappear that used to be
      // visible, which is a regression dressed up as a fix.
      if (stage0_textured)
      {
        ++stats.skipped_efb_texture;
        return;
      }
      albedo = nullptr;
      albedo_textured = false;
      albedo_stage = 0;
      texture_addr = 0;
      texture_is_efb_copy = false;
      texture_is_xfb_copy = false;
      --stats.texture_later_stage;
    }
    // The destination was copied to, but this backend deliberately DISCARDED
    // that copy - it is a shadow map, a reflection, or any other capture of
    // world content we cannot reproduce (see EfbCopyClass). The memory holds
    // zeroes, and the test above cannot catch it: zero bytes decode into a
    // perfectly valid texture, so HasData() is true and the draw sails through
    // to be rendered as a blank rectangle over the traced scene. Confirmed on
    // SpongeBob: Battle for Bikini Bottom, whose two 256x256 top-left copies
    // per frame produced exactly that as a white box across a quarter of the
    // screen.
    //
    // Dropping the draw instead is what the discard decision MEANT: the game's
    // screen-space fake goes away and the path-traced result behind it shows,
    // which is the same bargain the discard itself makes.
    //
    // Matched on the ADDRESS alone - deliberately NOT on entry->is_efb_copy,
    // which is never set on this path and would make the whole test dead. This
    // backend's CopyEFB creates no copy cache entry, so a draw sampling the
    // destination gets an ordinary entry decoded from that RAM and arrives here
    // with is_efb_copy false. Measured on BFBB: 122 draws sample the discarded
    // destination 0x009f3220, every one of them reporting `efbcopy 0`, while
    // `efbcopy 1` appears nowhere in the entire log. The address list is the
    // authority here because we populated it ourselves, at the copy.
    //
    // Gated on stage 0 for the reason given above - a draw that only reached
    // here through a later stage rendered untextured before, and making
    // geometry disappear that used to be visible would be a regression dressed
    // up as a fix.
    else if (stage0_textured && g_remix_api->EfbDestinationDiscarded(texture_addr))
    {
      ++stats.skipped_efb_discarded;
      return;
    }
  }
  if (albedo_textured)
  {
    const TexMode0& mode = bpmem.tex.GetUnit(albedo_texmap).texMode0;
    // GX and Remix (MDL) agree numerically: Near/Linear == 0/1 and
    // Clamp/Repeat/Mirror == 0/1/2. The invalid GX wrap value 3 behaves as
    // clamp on hardware.
    filter_mode = static_cast<u8>(mode.mag_filter.Value());
    wrap_mode_u = static_cast<u8>(std::min<u32>(static_cast<u32>(mode.wrap_s.Value()), 2));
    wrap_mode_v = static_cast<u8>(std::min<u32>(static_cast<u32>(mode.wrap_t.Value()), 2));
  }

  // Stage 0's texture hash specifically, which is what every IDENTITY decision
  // keys on - the sky texture list, the sky auto-detector's untextured test - as
  // opposed to `albedo`, which is now whichever stage actually shades the draw.
  // Keeping the two apart is what lets the later-stage lookup above be a pure
  // shading change.
  const u64 stage0_texture_hash =
      stage0_textured && albedo != nullptr ? albedo->GetContentHash() : 0;

  u8 alpha_test_type = 7;
  u8 alpha_reference = 0;
  ResolveAlphaTest(bpmem.alpha_test, alpha_test_type, alpha_reference);

  // NOTE: the material itself is resolved at the BOTTOM of this function, once
  // the geometry exists - a classified sky mesh needs an emissive material, and
  // that decision is keyed on the geometry hash. Only the alpha-test state is
  // computed here, where the BP state it reads is in scope.

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
  // The same pass builds the COMPACT bone table a genuinely multi-matrix draw
  // needs, so both readings come out of one walk of the attribute (4 bytes a
  // vertex). Compact means "0, 1, 2... in order of first appearance", not the
  // physical posMatrices row: which of the 64 rows a game hands a character is
  // an allocation detail it is free to change between frames, and these indices
  // fold into the mesh hash, so naming rows directly would re-hash the mesh for
  // no reason at all. Renumbering the palette is then invisible; only genuinely
  // re-partitioning which vertices belong to which bone is not.
  u32 uniform_matrix_index = 0;
  bool uniform_matrix = per_vertex_matrix;
  if (per_vertex_matrix)
  {
    for (const u32 slot : m_palette_slots)
      m_palette_named[slot] = false;
    m_palette_slots.clear();
    m_skinning.blend_indices.clear();
    m_skinning.blend_indices.reserve(vertex_count);

    for (u32 i = 0; i < vertex_count; ++i)
    {
      u32 index = 0;
      std::memcpy(&index,
                  m_base_buffer_pointer + static_cast<size_t>(i) * stride + decl.posmtx.offset,
                  sizeof(u32));
      index &= 0x3f;
      if (!m_palette_named[index])
      {
        m_palette_named[index] = true;
        m_palette_compact[index] = static_cast<u8>(m_palette_slots.size());
        m_palette_slots.push_back(index);
      }
      m_skinning.blend_indices.push_back(m_palette_compact[index]);
    }
    uniform_matrix_index = m_palette_slots.front();
    uniform_matrix = m_palette_slots.size() == 1;
  }
  // Genuinely multi-matrix geometry either goes over with real skinning data -
  // stable object-space bytes, the palette on the instance - or, with that off,
  // has every vertex transformed here as it always did.
  //
  // Orthographic draws are excluded on purpose. Neither UI path carries bones:
  // mode 1 rasterizes vertices on the CPU, mode 2 composes its own placement
  // onto the instance transform at flush time, and a palette UI draw is rare
  // enough that growing bones plumbing into either would be all risk and no
  // payoff. They keep the bake, which works.
  const bool skin_draw = per_vertex_matrix && !uniform_matrix && !is_ortho &&
                         g_remix_api->GpuSkinningEnabled();
  const bool bake_vertices = per_vertex_matrix && !uniform_matrix && !skin_draw;

  const int position_components = std::min(decl.position.components, 3);
  const bool has_normals = decl.normals[0].enable;
  // Which coordinate ATTRIBUTE to fall back on when texgen is off or unresolved.
  // Attribute 0 was hardcoded, which is only right when the sampling stage happens
  // to name texgen slot 0 - not a safe assumption now that the albedo can come
  // from a later stage.
  u32 texcoord_slot = 0;
  {
    const u32 named = bpmem.tevorders[albedo_stage >> 1].getTexCoord(albedo_stage & 1);
    if (named < 8 && decl.texcoords[named].enable)
      texcoord_slot = named;
  }
  const bool has_texcoord = decl.texcoords[texcoord_slot].enable;
  const int texcoord_components =
      has_texcoord ? std::min(decl.texcoords[texcoord_slot].components, 2) : 0;

  // Texture coordinates the way the console generates them, rather than "vertex
  // attribute 0, verbatim". Both halves of that shortcut were wrong: stage 0
  // names which texgen slot it samples, and the xfmem texture matrix on that
  // slot is how every scrolling or scaled texture on the machine is animated.
  const TexGenState texgen =
      ResolveTexGen(decl, g_remix_api->GxTexGenEnabled(), albedo_stage);
  if (texgen.enabled)
  {
    ++stats.texgen_generated;
    if (!IsTrivialTexGen(texgen))
      ++stats.texgen_nontrivial;
  }

  // Which XF lights this draw switches on, and what each one means to it. Both
  // live on the referencing CHANNEL rather than on the light, so both are
  // per-draw state and have to be accumulated here: read once at frame end they
  // reflect only the last draw, which is how Wind Waker managed to enable light
  // 0 all frame and still have every light dropped.
  DrawLightState light_state;
  const auto claim_lights = [&](const LitChannel& lit_channel, u32 channel, bool color_channel) {
    const u32 channel_mask = lit_channel.GetFullLightMask();
    for (u32 i = 0; i < light_state.attenuation.size(); ++i)
    {
      if ((channel_mask & (1u << i)) != 0 && (light_state.mask & (1u << i)) == 0)
      {
        light_state.attenuation[i] = static_cast<u8>(lit_channel.attnfunc.Value());
        light_state.diffuse[i] = static_cast<u8>(lit_channel.diffusefunc.Value());
      }
    }
    light_state.mask |= channel_mask;
    if (color_channel)
      light_state.color_mask |= channel_mask;

    // GX ambient has no Remix analogue - the path tracer's GI is what has to
    // stand in for it - so record whether the frame had a meaningful amount of
    // it before anyone concludes the translated lights are too dim. Register
    // bytes are abgr, same packing as a light's colour (TransformUnit.cpp:342).
    if (channel_mask != 0 && lit_channel.ambsource == AmbSource::AmbColorRegister)
    {
      const u8* ambient = reinterpret_cast<const u8*>(&xfmem.ambColor[channel]);
      if (std::max({ambient[1], ambient[2], ambient[3]}) > AMBIENT_BRIGHT_THRESHOLD)
        light_state.ambient_bright = true;
    }
  };
  // BOTH colour channels before either alpha channel. First claim decides the
  // light's kind, and an alpha channel only ever reads the light's color[0]
  // (TransformUnit.cpp:290-314) - so when the two disagree about the attenuation
  // function, the colour channel's reading is the one describing what the light
  // does to the picture.
  for (u32 channel = 0; channel < xfmem.numChan.numColorChans && channel < 2; ++channel)
    claim_lights(xfmem.color[channel], channel, true);
  for (u32 channel = 0; channel < xfmem.numChan.numColorChans && channel < 2; ++channel)
    claim_lights(xfmem.alpha[channel], channel, false);
  stats.draw_light_mask |= light_state.mask;
  g_remix_api->NoteDrawLights(light_state);

  // What TEV stage 0 rasterizes, by GX's rules rather than "colours[0], always".
  const bool gx_blend = g_remix_api->GxBlendEnabled();
  //
  // The two paths take the rule from separate knobs, because they fail in
  // opposite directions and have to be A/B-able apart.
  //
  // Ortho draws are where this MATTERS: the overlay's software rasterizer reads
  // its `r` input out of the submitted vertex colour, so promoting a draw from
  // "no channel, write opaque white" to "channel 0, write the vertex colour"
  // hands it Wind Waker's actual UI vertex alpha of 0 and the element resolves
  // away. That is what removes the leaked gameplay HUD from the title screen -
  // observed by eye, and the only change known to do it.
  //
  // The same promotion also emptied frame 600 completely (HasContent false), and
  // the screenshot that showed the HUD gone was also missing PRESS START. So it
  // over-suppresses somewhere. On by default anyway - a leaked HUD is the worse
  // failure - but see RemixUiRasChannel's comment for what is still unsettled.
  const bool ras_channel =
      is_ortho ? g_remix_api->UiRasChannelEnabled() : g_remix_api->GxRasChannelEnabled();
  const RasterColor raster_color =
      ResolveRasterColor(decl, g_remix_api->GxColorEnabled(), gx_blend, ras_channel);
  const int color_slot = raster_color.vertex_slot;
  if (raster_color.channel_split)
    ++stats.ras_channel_split;
  switch (raster_color.color_arg2)
  {
  case REMIX_TEX_ARG_VERTEX_COLOR0:
    ++stats.color_vertex;
    break;
  case REMIX_TEX_ARG_TFACTOR:
    ++stats.color_register;
    break;
  default:
    ++stats.color_none;
    break;
  }

  // Resolve the TEV COLOUR chain, and fold it into the vertex colours further
  // down. Scoped to draws whose colour was already going to come from the vertex
  // stream: those are the ones where the rasterized colour is the chain's free
  // variable, and leaving every other route untouched keeps this a clean A/B.
  //
  // Ortho draws are excluded. The overlay rasterizer reads its own colour and
  // rasterized-alpha inputs straight out of the submitted vertex colour, so
  // rewriting those bytes would change UI compositing rather than material
  // albedo - a separate question, on a path that is already delicately balanced.
  TevColorFold tev_color;
  const bool tev_color_eligible = !is_ortho && g_remix_api->GxTevColorEnabled();
  const bool tev_color_valid = tev_color_eligible && ResolveTevColor(albedo_textured, tev_color);

  // Route A - the chain's free variable is the rasterized colour, and the draw
  // takes its colour from the vertex stream. Folded per vertex further down.
  const bool fold_tev_color = tev_color_valid && !tev_color.identity &&
                              raster_color.color_arg2 == REMIX_TEX_ARG_VERTEX_COLOR0 &&
                              color_slot >= 0;

  // Route B - the chain does NOT reference the rasterized colour at all, so it
  // resolves to a per-draw CONSTANT. That constant is still a colour the console
  // applies and this backend was dropping: the dominant such chain in Wind Waker
  // is `TexColor * Reg[Color0]`, a TEV register used as a multiplier, and a draw
  // taking it resolved to "no tint" here and rendered as the bare texture.
  //
  // A constant multiplier is exactly what tFactor is for, so it costs no mesh
  // identity - two draws differing only in tint keep one mesh handle. Only taken
  // when there is no vertex-colour route to fold and the constant is not white,
  // because white is the identity for MODULATE and claiming a tint that does
  // nothing would only make the counters lie.
  bool tint_tev_color = false;
  u32 tev_constant = 0xFFFFFFFFu;
  if (tev_color_valid && !fold_tev_color && tev_color.at_zero == tev_color.at_one)
  {
    tev_constant = (static_cast<u32>(tev_color.at_one[0]) << 16) |
                   (static_cast<u32>(tev_color.at_one[1]) << 8) |
                   static_cast<u32>(tev_color.at_one[2]);
    tint_tev_color = tev_constant != 0x00FFFFFFu;
  }

  if (tev_color_eligible)
  {
    if (fold_tev_color)
      ++stats.tev_color_folded;
    else if (tint_tev_color)
      ++stats.tev_color_tinted;
    else if (tev_color_valid)
      ++stats.tev_color_identity;
    else
      ++stats.tev_color_bailed;
  }

  // Route-colouring diagnostic. Packed 0xAARRGGBB, matching the vertex colour
  // Remix reads, and non-zero only while the knob is on:
  //   red     - vertex colour, TEV chain folded
  //   magenta - vertex colour, fold refused or identity
  //   blue    - register tint through tFactor
  //   green   - no rasterized colour at all
  u32 debug_route_color = 0;
  if (!is_ortho && g_remix_api->DebugColorRoutesEnabled())
  {
    if (raster_color.color_arg2 == REMIX_TEX_ARG_VERTEX_COLOR0)
      debug_route_color = fold_tev_color ? 0xFFFF0000u : 0xFFFF00FFu;
    else if (raster_color.color_arg2 == REMIX_TEX_ARG_TFACTOR)
      debug_route_color = 0xFF0000FFu;
    else
      debug_route_color = 0xFF00FF00u;
  }

  DrawBlendState blend;
  blend.color_arg1 = REMIX_TEX_ARG_TEXTURE;
  blend.color_arg2 = raster_color.color_arg2;
  blend.color_operation = REMIX_TEX_OP_MODULATE;
  if (debug_route_color != 0)
  {
    // Select the vertex colour outright so the flat route colour is the albedo,
    // with neither the texture nor the register able to disguise it.
    blend.color_arg2 = REMIX_TEX_ARG_VERTEX_COLOR0;
    blend.color_operation = REMIX_TEX_OP_SELECT_ARG2;
  }
  blend.tfactor = raster_color.tfactor;
  if (tint_tev_color)
  {
    // RGB from the resolved chain constant; the alpha half is left to whatever the
    // channel resolution decided, because alpha is resolved by ResolveTevAlpha and
    // the two must not fight over the same field.
    blend.color_arg2 = REMIX_TEX_ARG_TFACTOR;
    blend.tfactor = (raster_color.tfactor & 0xFF000000u) | tev_constant;
  }
  // A folded colour is the console's own rasterized albedo - a lerp between two
  // authored register colours - not a lighting term multiplied onto a material.
  // Letting the runtime "remove the brightness contribution" from it would divide
  // out the very thing the fold recovered, so the flag comes off whenever the
  // fold applied.
  blend.vertex_color_is_baked_lighting = raster_color.baked_lighting && !fold_tev_color;
  blend.texture_addr = texture_addr;
  blend.texture_is_efb_copy = texture_is_efb_copy;
  blend.texture_is_xfb_copy = texture_is_xfb_copy;

  if (gx_blend)
  {
    // GX's stage-0 alpha is the texture's modulated by the rasterized alpha, so
    // the opacity a blend or a cutout works from is that product - not the
    // texture alpha alone. Safe to apply unconditionally: the runtime marks a
    // surface fully opaque, and ignores opacity entirely, whenever neither
    // blending nor an alpha test is live (rtx_instance_manager.cpp:861).
    blend.alpha_arg1 = REMIX_TEX_ARG_TEXTURE;
    blend.alpha_arg2 = raster_color.alpha_arg2;
    blend.alpha_operation = raster_color.alpha_arg2 == REMIX_TEX_ARG_NONE ?
                                REMIX_TEX_OP_SELECT_ARG1 :
                                REMIX_TEX_OP_MODULATE;

    blend.alpha_test_compare = alpha_test_type;
    blend.alpha_test_reference = alpha_reference;
    blend.alpha_test_enabled = alpha_test_type != 7;

    // The raw test and the resolved TEV alpha ride alongside, for consumers that
    // can run the real thing rather than the single-comparator reduction.
    blend.raw_alpha_compare0 = static_cast<u8>(bpmem.alpha_test.comp0.Value());
    blend.raw_alpha_compare1 = static_cast<u8>(bpmem.alpha_test.comp1.Value());
    blend.raw_alpha_logic = static_cast<u8>(bpmem.alpha_test.logic.Value());
    blend.raw_alpha_reference0 = static_cast<u8>(bpmem.alpha_test.ref0.Value());
    blend.raw_alpha_reference1 = static_cast<u8>(bpmem.alpha_test.ref1.Value());
    blend.tev_alpha_known = ResolveTevAlpha(blend.tev_alpha_corners);
    if (!blend.tev_alpha_known)
    {
      switch (g_tev_alpha_bail)
      {
      case TevAlphaBail::TooManyStages: ++stats.tev_bail_stages; break;
      case TevAlphaBail::Konst: ++stats.tev_bail_konst; break;
      case TevAlphaBail::CompareMode: ++stats.tev_bail_compare; break;
      default: ++stats.tev_bail_rasterized; break;
      }
    }
    if (blend.alpha_test_enabled)
      ++stats.alpha_tested;

    ResolveBlend(blend, stats);
  }

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
      // Genuinely multi-matrix geometry with skinning turned off: each vertex
      // names its own modelview, and without real skinning data there is no
      // per-vertex transform on the Remix side, so bake the transform in and
      // submit with an identity instance transform. The mesh hash then covers
      // the transformed bytes, so these re-create every frame and lean on the
      // idle-mesh LRU - an accepted cost, now paid only by the draws that
      // actually need it.
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
      // Raw, in whatever space the game authored the model. A skinned draw takes
      // this branch too, deliberately: the whole point is that these bytes never
      // change, so the mesh hash holds still while the pose moves. The runtime's
      // kernel applies the bone matrix to the NORMAL as well as the position
      // (skinning.h), which is why the normal goes over untouched here rather
      // than being pre-transformed the way the bake does it.
      dst.position[0] = position[0];
      dst.position[1] = position[1];
      dst.position[2] = position[2];
      dst.normal[0] = normal[0];
      dst.normal[1] = normal[1];
      dst.normal[2] = normal[2];
    }

    if (texgen.enabled)
      GenerateTexCoord(texgen, src, decl, position, normal, dst.texcoord);
    else if (texcoord_components > 0)
      ReadFloats(src + decl.texcoords[texcoord_slot].offset, dst.texcoord, texcoord_components);

    if (debug_route_color != 0)
    {
      // Diagnostic mode: every draw is painted a flat colour naming the colour
      // ROUTE it took, with the texture selected out of the material below. The
      // frame counters say how many draws took each route but not WHICH pixels
      // they own, and that is the question when a surface comes out the wrong
      // colour and the arithmetic says it should not.
      dst.color = debug_route_color;
    }
    else if (color_slot >= 0)
    {
      u32 color = 0;
      std::memcpy(&color, src + decl.colors[color_slot].offset, sizeof(u32));
      // Run the vertex's rasterized colour through the resolved TEV chain, so
      // what Remix receives is the colour the console would have rasterized
      // rather than the chain's input weight. See ResolveTevColor.
      if (fold_tev_color)
        color = ApplyTevColorFold(tev_color, color);
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
    // Instances are submitted double-sided, so the sign costs shading rather
    // than visibility - but a normal on the wrong side is the side that receives
    // light and GI, so it is not cosmetic either.
    //
    // On the baked path the vertices are already in view space and the instance
    // transform is identity, so there is no modelview to un-mirror. A skinned
    // draw passes null for the same reason in a different arrangement: there is
    // no single matrix to read a determinant from, and none is needed, because
    // the kernel transforms the generated normal by the same bone matrix as the
    // positions - so a mirroring bone flips both together, exactly as the bake
    // gets it for free by computing the cross product post-transform.
    const float* const winding_matrix =
        (bake_vertices || skin_draw) ?
            nullptr :
            &xfmem.posMatrices[(uniform_matrix ?
                                    uniform_matrix_index :
                                    static_cast<u32>(
                                        g_main_cp_state.matrix_index_a.PosNormalMtxIdx)) *
                               4];
    const bool flip = ShouldFlipGeneratedNormals(winding_matrix);
    const float normal_sign = flip ? -1.0f : 1.0f;
    ++stats.normals_generated;
    if (flip)
      ++stats.normals_flipped;

    const size_t triangle_count = m_indices.size() / 3;
    m_flat_vertices.clear();
    m_flat_vertices.reserve(triangle_count * 3);
    m_flat_indices.clear();
    m_flat_indices.reserve(triangle_count * 3);
    // The blend array is indexed by SUBMITTED vertex, so a split that forks the
    // vertex array has to fork this one in lockstep or every vertex past the
    // first shared one is skinned by the wrong bone.
    if (skin_draw)
    {
      m_flat_blend_indices.clear();
      m_flat_blend_indices.reserve(triangle_count * 3);
    }

    for (size_t tri = 0; tri < triangle_count; ++tri)
    {
      const remixapi_HardcodedVertex& v0 = m_vertices[m_indices[tri * 3 + 0]];
      const remixapi_HardcodedVertex& v1 = m_vertices[m_indices[tri * 3 + 1]];
      const remixapi_HardcodedVertex& v2 = m_vertices[m_indices[tri * 3 + 2]];

      const float e0[3] = {v1.position[0] - v0.position[0], v1.position[1] - v0.position[1],
                           v1.position[2] - v0.position[2]};
      const float e1[3] = {v2.position[0] - v0.position[0], v2.position[1] - v0.position[1],
                           v2.position[2] - v0.position[2]};
      float normal[3] = {normal_sign * (e0[1] * e1[2] - e0[2] * e1[1]),
                         normal_sign * (e0[2] * e1[0] - e0[0] * e1[2]),
                         normal_sign * (e0[0] * e1[1] - e0[1] * e1[0])};
      Normalize(normal);

      for (size_t corner = 0; corner < 3; ++corner)
      {
        const u32 source_index = m_indices[tri * 3 + corner];
        remixapi_HardcodedVertex vertex = m_vertices[source_index];
        vertex.normal[0] = normal[0];
        vertex.normal[1] = normal[1];
        vertex.normal[2] = normal[2];
        m_flat_indices.push_back(static_cast<u32>(m_flat_vertices.size()));
        m_flat_vertices.push_back(vertex);
        if (skin_draw)
          m_flat_blend_indices.push_back(m_skinning.blend_indices[source_index]);
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
  // Which slot that matrix came out of, for the modelview histogram. Hoisted out
  // of the branch below because the diagnostics are filled in further down.
  u32 position_matrix_slot = DrawDiagnostics::NO_POSITION_MATRIX;
  // Non-null marks this draw GPU-skinned, and is what carries the palette down
  // into RemixApi::SubmitMesh.
  const DrawSkinning* draw_skinning = nullptr;
  if (bake_vertices || skin_draw)
  {
    // Identity, for two different reasons that arrive at the same matrix. The
    // bake has already put the vertices in view space. The skin path has the
    // modelviews themselves in the bone palette, and the runtime applies
    // skinning FIRST - into the BLAS - with the instance transform on top, so
    // the palette must be the ONLY place a modelview appears. What the flush
    // then computes is V^-1 * C * palette * v, which is term for term what the
    // bake produces; the projection correction folded in below rides C either
    // way. Pre-multiplying V^-1 into each bone would be the same algebra with
    // one more place to get the convention wrong.
    transform.matrix[0][0] = 1.0f;
    transform.matrix[1][1] = 1.0f;
    transform.matrix[2][2] = 1.0f;

    if (skin_draw)
    {
      // The flat-normal split forked the vertex array, so the blend array it
      // forked alongside is the one that parallels what is actually submitted.
      // Traded rather than copied: both are per-draw scratch, both are cleared
      // before their next use, so the swap just hands each the other's buffer.
      if (!has_normals)
        m_skinning.blend_indices.swap(m_flat_blend_indices);

      // Read xfmem HERE, in the draw, and not at frame end. The palette
      // registers are rewritten between draws - XFStructs.cpp flushes the vertex
      // manager on every XF write precisely so that draw-time reads are well
      // defined - so a deferred read would give every skinned draw in the frame
      // the last character's pose.
      //
      // GX modelview matrices are 3 rows of 4 floats, row-major, which is
      // byte-identical to remixapi_Transform::matrix[3][4] and to what the
      // non-palette instance transform memcpys below. Bone transforms and
      // instance transforms go through the same conversion inside the runtime,
      // so sharing the copy is what guarantees the two cannot drift apart.
      m_skinning.palette.clear();
      m_skinning.palette.reserve(m_palette_slots.size());
      for (const u32 slot : m_palette_slots)
      {
        remixapi_Transform bone = {};
        std::memcpy(&bone.matrix[0][0], &xfmem.posMatrices[slot * 4], sizeof(float) * 12);
        m_skinning.palette.push_back(bone);
      }
      draw_skinning = &m_skinning;
    }
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
    position_matrix_slot = matrix_index;
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
  if (!is_ortho &&
      (xfmem.projection.rawProjection[1] != 0.0f || xfmem.projection.rawProjection[3] != 0.0f))
  {
    ++stats.projection_oblique;
  }

  if (!is_ortho && g_remix_api->ProjectionFixEnabled() && g_remix_api->HasReferenceProjection())
  {
    // And this draw's viewport onto the frame's reference viewport, in the same
    // correction. A draw the game gave its own screen rect - a
    // picture-in-picture panel, a position ladder - is otherwise rendered
    // through the reference rect and lands in the middle of the world.
    //
    // The placement this produces survives camera recovery untouched: the
    // correction rides C in V^-1*(C*MV), and the camera re-applies V
    // (RemixApi.cpp:2040-2049), so the rendered position is P*C*MV whichever
    // slot the camera came from.
    ViewportFold viewport_fold;
    bool viewport_folded = false;
    if (draw_viewport != nullptr && g_remix_api->ViewportFixEnabled() &&
        !draw_viewport->SameRect(g_remix_api->ReferenceViewport()))
    {
      if (g_remix_api->HasReferenceViewport())
      {
        const DrawViewport& reference_viewport = g_remix_api->ReferenceViewport();
        viewport_fold.x_scale = draw_viewport->wd() / reference_viewport.wd();
        viewport_fold.y_scale = draw_viewport->ht() / reference_viewport.ht();
        viewport_fold.x_offset =
            (draw_viewport->cx - reference_viewport.cx) / reference_viewport.wd();
        viewport_fold.y_offset =
            (draw_viewport->cy - reference_viewport.cy) / reference_viewport.ht();
        viewport_folded = true;
      }
      else
      {
        // The rect moved and there is no reference rect to move it against, so
        // this draw is knowingly rendered where the reference was.
        ++stats.viewport_uncorrectable;
      }
    }

    ProjectionCorrection correction;
    switch (BuildProjectionCorrection(xfmem.projection.rawProjection,
                                      g_remix_api->ReferenceProjection(), viewport_fold,
                                      correction))
    {
    case CorrectionResult::Apply:
      ApplyProjectionCorrection(correction, transform);
      ++stats.projection_corrected;
      if (viewport_folded)
      {
        ++stats.viewport_corrected;
        // A sign flip between the two rects. Placed per the algebra; counted
        // because nothing has validated how a mirrored instance transform
        // shades. See BuildProjectionCorrection's scale band.
        if (correction.x_scale < 0.0f || correction.y_scale < 0.0f)
          ++stats.viewport_mirrored;
      }
      break;
    case CorrectionResult::Unrepresentable:
      ++stats.projection_uncorrectable;
      if (viewport_folded)
        ++stats.viewport_uncorrectable;
      break;
    case CorrectionResult::NotNeeded:
      break;
    }
  }

  // ---- The sky coexistence contract ---------------------------------------
  //
  // How a game's sky and Remix's replacement atmosphere share one frame, end to
  // end. All of this is already implemented; it is written down here because it
  // spans the client, the API and the runtime, and none of the three states it.
  //
  //   1. A draw reaches the runtime carrying REMIXAPI_INSTANCE_CATEGORY_BIT_SKY.
  //      There are three ways to earn that bit, in precedence order:
  //        veto   - RemixSkyVetoHashes names a texture or mesh hash: never sky.
  //        manual - RemixSkyTextures names the stage-0 texture hash.
  //        auto   - RemixSkyAutoDetect = 2 and the transform-signature
  //                 classifier has classified this draw's MESH hash.
  //   2. The runtime turns that bit into CameraType::Sky
  //      (rtx_remix_api.cpp:730-738). Its own texture-grid sky heuristics never
  //      run on API draws - cameraType is frozen from the instance's category
  //      flags before the grid lookup - so passing the bit ourselves is the only
  //      route that works.
  //   3. Under rtx.skyMode = 1 (Numos) a sky-categorized draw is then SKIPPED
  //      ENTIRELY (rtx_sky.h:149-154). So the correct configuration is "tag the
  //      game's sky and let Numos have the volume". No rtx.ignoreTextures entry
  //      is needed for it, and adding one is redundant at best.
  //   4. Numos requires RemixCameraRecovery = True. The replacement sky is
  //      generated from the submitted camera's world basis, so with recovery off
  //      the camera is the identity and the sky welds itself to the view. Auto
  //      detection needs recovery on too, by construction: it classifies on the
  //      recovered world transform.
  //
  // Two routes to sky at this point in the draw path. The explicit texture list
  // is the reliable one - it names the exact skybox texture, so it cannot
  // mistake clouds or overlays for the horizon dome the way the depth heuristic
  // did. That heuristic remains behind RemixSkyMode for games where no hash is
  // known yet, defaulted off. The third route, auto-detection, keys on the MESH
  // hash and so is applied at flush time instead of here - which is also what
  // lets it reach Wind Waker's untextured dome, something no texture-hash list
  // can ever match.
  const int sky_mode = g_remix_api->SkyMode();
  const bool is_sky = !is_ortho &&
                      ((stage0_texture_hash != 0 && g_remix_api->IsSkyTexture(stage0_texture_hash)) ||
                       (sky_mode != 0 && IsSkyDraw(bpmem.zmode)));
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
    const float* const tex_matrix = &xfmem.posMatrices[texgen.default_matrix * 4];
    INFO_LOG_FMT(VIDEO,
                 "Remix {} draw: ztest {} zfunc {} zwrite {} | blend {} | verts {} tris {} | "
                 "tex {:#018x} | texgen coord {} type {} row {} (slot {}) proj {} form {} mtx {}{} "
                 "[{} {} {} {} / {} {} {} {}] dual {} | blend {}->{} op {} alpha {}->{} op {} "
                 "mask {:#x} | atest {} ref {}",
                 is_sky ? "SKY" : "world", bpmem.zmode.test_enable ? 1 : 0,
                 static_cast<u32>(bpmem.zmode.func.Value()), bpmem.zmode.update_enable ? 1 : 0,
                 bpmem.blendmode.blend_enable ? 1 : 0, out_vertices->size(),
                 out_indices->size() / 3, albedo != nullptr ? albedo->GetContentHash() : 0,
                 texgen.coord, static_cast<u32>(texgen.type), static_cast<u32>(texgen.source_row),
                 texgen.source_slot, static_cast<u32>(texgen.projection),
                 static_cast<u32>(texgen.input_form), texgen.default_matrix,
                 texgen.matrix_index_slot >= 0 ? " (per-vertex)" : "", tex_matrix[0], tex_matrix[1],
                 tex_matrix[2], tex_matrix[3], tex_matrix[4], tex_matrix[5], tex_matrix[6],
                 tex_matrix[7],
                 texgen.dual_transform ?
                     (IsIdentityTexMatrix(texgen.post_matrix) ? "identity" : "ACTIVE") :
                     "off",
                 blend.src_color_factor, blend.dst_color_factor, blend.color_blend_op,
                 blend.src_alpha_factor, blend.dst_alpha_factor, blend.alpha_blend_op,
                 blend.write_mask, blend.alpha_test_compare, blend.alpha_test_reference);
  }

  // Where a world draw's colour comes from, per draw, with the resolution's own
  // working shown. The frame counters say how many draws took each route; they
  // cannot say whether the ONE draw that matters - an ocean, a vertex-coloured
  // dome - is among them, or which of the four independent things that can wash
  // a colour out did it. This is the only instrument that can.
  //
  // Deliberately covers a large slice of the frame rather than the first few
  // draws: the geometry in question is typically hundreds of draws in, and the
  // existing trace above caps at three.
  const bool has_vertex_color = decl.colors[0].enable || decl.colors[1].enable;
  if (!is_ortho && g_remix_api->TraceColorsEnabled() && g_remix_api->ShouldTraceDraws() &&
      stats.draws_seen < COLOR_TRACE_DRAW_LIMIT &&
      (has_vertex_color || xfmem.numChan.numColorChans != 0))
  {
    // Raw vertex colour, straight out of the attribute, per enabled slot: the
    // range over the draw plus vertex 0. A washed-out result whose SOURCE bytes
    // are already pale is a decode bug; one whose source is saturated is a
    // downstream bug, and those are the two halves this line separates.
    std::string raw_colors;
    for (u32 slot = 0; slot < 2; ++slot)
    {
      if (!decl.colors[slot].enable)
        continue;
      u32 low[4] = {255, 255, 255, 255};
      u32 high[4] = {0, 0, 0, 0};
      for (u32 i = 0; i < vertex_count; ++i)
      {
        u32 packed = 0;
        std::memcpy(&packed,
                    m_base_buffer_pointer + static_cast<size_t>(i) * stride +
                        decl.colors[slot].offset,
                    sizeof(u32));
        for (u32 c = 0; c < 4; ++c)
        {
          const u32 byte = (packed >> (c * 8)) & 0xFFu;
          low[c] = std::min(low[c], byte);
          high[c] = std::max(high[c], byte);
        }
      }
      u32 first = 0;
      std::memcpy(&first, m_base_buffer_pointer + decl.colors[slot].offset, sizeof(u32));
      // Dolphin writes the attribute R,G,B,A in memory order, so the low byte is
      // red; `v0` is printed as the u32 actually handed to ToRemixVertexColor.
      raw_colors += fmt::format("{}s{}[r {}-{} g {}-{} b {}-{} a {}-{} v0 {:#010x}]",
                                slot == 0 ? "" : " ", slot, low[0], high[0], low[1], high[1],
                                low[2], high[2], low[3], high[3], first);
    }
    if (raw_colors.empty())
      raw_colors = "none";

    // The two LitChannels the resolution reads, printed for both channels rather
    // than only the referenced one: "which channel did it pick" is exactly what
    // this line exists to check, so printing only the pick would beg the
    // question. `lit` is enablelighting - the bit that decides baked-lighting
    // normalization, and therefore the bit most likely to be wrong here.
    std::string channels;
    for (u32 channel = 0; channel < 2; ++channel)
    {
      const LitChannel& c = xfmem.color[channel];
      const LitChannel& a = xfmem.alpha[channel];
      channels += fmt::format(
          "{}ch{}[c mat {} lit {} amb {} mask {:#x} / a mat {} lit {} amb {} | matReg {:#010x} "
          "ambReg {:#010x}]",
          channel == 0 ? "" : " ", channel, static_cast<u32>(c.matsource.Value()),
          c.enablelighting ? 1 : 0, static_cast<u32>(c.ambsource.Value()), c.GetFullLightMask(),
          static_cast<u32>(a.matsource.Value()), a.enablelighting ? 1 : 0,
          static_cast<u32>(a.ambsource.Value()), xfmem.matColor[channel], xfmem.ambColor[channel]);
    }

    // The COLOUR combiner chain, and the TEV colour registers it can name. This
    // is the half that decides whether a draw's colour is reachable at all:
    // TevColorArg 2/4/6 are registers c0/c1/c2 (per-draw constants written by
    // GXSetTevColor), 14 is konst, 10 is the rasterized colour. A chain that
    // tints through a REGISTER carries its colour in state this backend reads
    // nowhere - and would present exactly as "the surface is white".
    const u32 stages = std::min<u32>(bpmem.genMode.numtevstages + 1, 16);
    std::string color_chain;
    for (u32 stage = 0; stage < stages; ++stage)
    {
      const auto& cc = bpmem.combiners[stage].colorC;
      color_chain += fmt::format(
          "{}[a{} b{} c{} d{} k{} r{} bias{} op{} ->{}]", stage == 0 ? "" : " ",
          static_cast<u32>(cc.a.Value()), static_cast<u32>(cc.b.Value()),
          static_cast<u32>(cc.c.Value()), static_cast<u32>(cc.d.Value()),
          static_cast<u32>(bpmem.tevksel.GetKonstColor(stage)),
          static_cast<u32>(bpmem.tevorders[stage >> 1].getColorChan(stage & 1)),
          static_cast<u32>(cc.bias.Value()), static_cast<u32>(cc.op.Value()),
          static_cast<u32>(cc.dest.Value()));
    }

    // creg/konst RGB. Printed unconditionally because "the colour is in a
    // register the resolution never reads" is a hypothesis that can only be
    // checked against the register's actual value.
    const auto& psm = Core::System::GetInstance().GetPixelShaderManager().constants;
    std::string registers;
    for (u32 i = 0; i < 4; ++i)
    {
      registers += fmt::format("{}c{}[{} {} {}]k{}[{} {} {}]", i == 0 ? "" : " ", i,
                               psm.colors[i][0], psm.colors[i][1], psm.colors[i][2], i,
                               psm.kcolors[i][0], psm.kcolors[i][1], psm.kcolors[i][2]);
    }

    // The albedo texture's MEAN colour, and which stage it came from. This is the
    // other half of "where does the surface's colour come from": a chain whose
    // resolved vertex term is near-white is only correct if the texture it
    // multiplies carries the hue, and nothing else in the log can say whether it
    // does. Cheap enough - it runs on one frame in 120, behind a default-off knob.
    std::string tex_mean = "none";
    if (albedo != nullptr && albedo->HasData())
    {
      const std::vector<u8>& pixels = albedo->GetPixels();
      const size_t texels = pixels.size() / 4;
      if (texels != 0)
      {
        // Stride the samples: a 1024x1024 texture is a million texels and the mean
        // does not need all of them.
        const size_t step = std::max<size_t>(1, texels / 4096);
        u64 sum[4] = {};
        size_t taken = 0;
        for (size_t t = 0; t < texels; t += step, ++taken)
          for (u32 c = 0; c < 4; ++c)
            sum[c] += pixels[t * 4 + c];
        tex_mean = fmt::format("[{} {} {} {}] {} texels", sum[0] / taken, sum[1] / taken,
                               sum[2] / taken, sum[3] / taken, texels);
      }
    }

    INFO_LOG_FMT(VIDEO, "Remix colour f{} draw {}: albedo stage {} mean {}", g_remix_api->FrameIndex(),
                 stats.draws_seen, albedo_stage, tex_mean);

    INFO_LOG_FMT(VIDEO,
                 "Remix colour f{} draw {}: tex {:#018x} verts {} | nchan {} | ras stage c{} a{} -> "
                 "chan c{} a{} | {} | resolved arg1 {} arg2 {} op {} | alpha arg1 {} arg2 {} op {} "
                 "| tfactor {:#010x} | baked {} | vslot {} | raw {} | fold {} [{} {} {}]->[{} {} "
                 "{}] bail {} | tev {} | reg {}",
                 g_remix_api->FrameIndex(), stats.draws_seen,
                 albedo != nullptr ? albedo->GetContentHash() : 0, vertex_count,
                 static_cast<u32>(xfmem.numChan.numColorChans), raster_color.ras_color_stage,
                 raster_color.ras_alpha_stage, raster_color.color_channel,
                 raster_color.alpha_channel, channels, blend.color_arg1, blend.color_arg2,
                 blend.color_operation, blend.alpha_arg1, blend.alpha_arg2, blend.alpha_operation,
                 blend.tfactor, raster_color.baked_lighting ? 1 : 0, color_slot, raw_colors,
                 fold_tev_color ? "vertex" :
                     (tint_tev_color ? "tfactor" :
                          (tev_color_valid ? (tev_color.identity ? "identity" : "white") : "BAILED")),
                 tev_color.at_zero[0], tev_color.at_zero[1], tev_color.at_zero[2],
                 tev_color.at_one[0], tev_color.at_one[1], tev_color.at_one[2],
                 static_cast<u32>(g_tev_color_bail), color_chain, registers);
  }

  // Recorded, never classified on: the sky auto-detector keys on the transform
  // signature alone, and these are what let its verdicts be checked against the
  // weaker signals - and what makes a classification hand-transcribable into
  // rtx.skyBoxGeometries or RemixSkyTextures.
  DrawDiagnostics diagnostics;
  diagnostics.texture_hash = stage0_texture_hash;
  diagnostics.depth_test = bpmem.zmode.test_enable;
  diagnostics.depth_func = static_cast<u8>(bpmem.zmode.func.Value());
  diagnostics.depth_write = bpmem.zmode.update_enable;
  diagnostics.draw_index = stats.draws_seen;
  diagnostics.position_matrix = position_matrix_slot;

  // The UI path submits a DrawBlendState rather than a DrawDiagnostics, so the
  // depth state rides along there too. Once audit-only; now what the overlay's
  // depth plane honours (RemixUiDepth).
  blend.depth_test = bpmem.zmode.test_enable;
  blend.depth_func = static_cast<u8>(bpmem.zmode.func.Value());
  blend.depth_write = bpmem.zmode.update_enable;

  if (!is_ortho)
    g_remix_api->NoteProjectionUse(projection_slot, static_cast<u32>(out_vertices->size()));

  // ---- Tag-driven 2D routing -----------------------------------------------
  //
  // The Remix dev menu's texture grid can categorize any texture the runtime
  // knows about, but a category set on a 2D draw had no effect here: this
  // backend's overlay path never produces a runtime draw call at all - it hands
  // over finished pixels - so the runtime never gets to apply its own routing.
  // This block applies it from our side instead, before the overlay path runs.
  //
  // Mode 1 only. Mode 0 drops everything by definition, and mode 2 already
  // sends every ortho draw world-side, where the runtime applies Ignore and
  // World Space UI itself.
  //
  // ORTHO draws only, and that is the first of three routing sites in this
  // function. The second is the mode-2 / world-route fall-through below. The
  // third is the PERSPECTIVE divert near the end (after EnsureMaterial), which
  // handles a HUD the game draws through the 3D frustum; it acts on the UI tag
  // alone, because for a perspective draw the runtime does see a real API draw
  // and applies Ignore and World Space UI to it itself.
  bool tag_bypass = false;
  bool route_world = false;
  if (is_ortho && ui_mode == 1 && g_remix_api->UiTagRoutingEnabled())
  {
    // The key the runtime records for THIS draw. A textured draw is identified
    // by its albedo's content hash - which is the hash we hand CreateTexture,
    // and the one the grid is keyed on. An untextured draw has no texture to
    // identify, so the runtime falls back to the mesh handle; ours is a fold of
    // geometry and material, reproduced exactly by UntexturedOrthoKey (see the
    // contract on that function - the two must never be allowed to drift).
    const bool textured = albedo != nullptr && albedo->HasData();
    const u64 tag_key =
        textured ? albedo->GetContentHash() :
                   g_remix_api->UntexturedOrthoKey(*out_vertices, *out_indices, filter_mode,
                                                   wrap_mode_u, wrap_mode_v, alpha_test_type,
                                                   alpha_reference);

    // Before any drop decision, so that even a draw about to be discarded still
    // earns a thumbnail in the grid. Otherwise the elements most in need of an
    // explicit tag - the ones a heuristic is eating - would be exactly the ones
    // with no way to tag them.
    if (textured)
      g_remix_api->RegisterOverlayTexture(albedo);

    const RemixApi::UiTagClass tag = g_remix_api->ClassifyUiDraw(tag_key);

    if (g_remix_api->UiTaggingModeActive())
    {
      // The tagging view is on (RemixUiWorldView - a user toggle, no longer
      // coupled to the dev menu being open). Send EVERY 2D draw through the
      // world path: an overlay draw is finished pixels by the time the runtime
      // sees it, so it has no object for the click-to-tag picker to hit. World
      // draws do, including untextured white boxes - which are precisely the
      // ones with no grid thumbnail to reach them by.
      //
      // All ortho draws rather than only the untagged ones, so what is on screen
      // while tagging is predictable: the whole 2D layer, in one place, in one
      // form.
      route_world = true;
      ++stats.ui_tagmode_world;
    }
    else
    {
      switch (tag)
      {
      case RemixApi::UiTagClass::Ui:
        // Explicitly kept. Beats every drop rule below, including Ignore - this
        // tag is the user's rescue lever and an automated rule must not be able
        // to overrule it.
        tag_bypass = true;
        ++stats.ui_tagged_ui;
        break;
      case RemixApi::UiTagClass::Ignore:
        ++stats.ui_tagged_ignore;
        return;
      case RemixApi::UiTagClass::WorldUi:
        route_world = true;
        ++stats.ui_tagged_world;
        break;
      case RemixApi::UiTagClass::None:
        // Strict mode is a policy over UNTAGGED draws only, which is why it is
        // tested here and not above: an explicit tag of any kind has already
        // been honoured by this point.
        if (g_remix_api->UiStrictEnabled())
        {
          ++stats.ui_dropped_strict;
          return;
        }
        break;
      }
    }
  }

  if (is_ortho && ui_mode == 1 && !route_world)
  {
    // Screen overlay. This draw never becomes a mesh or an instance - it is
    // rasterized to pixels and composited after the frame is traced, which is
    // the only way UI comes out looking like UI.
    //
    // Where it lands on the EFB, derived the way every other backend derives it.
    // Shared with the perspective divert further down, which needs the identical
    // placement: see ComputeUiPlacement for why one derivation serves both.
    const UiPlacement placement = ComputeUiPlacement();
    const std::array<float, 6>& viewport = placement.viewport;
    const std::array<float, 4>& clip = placement.clip;

    // One frame's worth of the state that decides whether a UI draw is visible
    // on console. The backend reads final alpha from stage 0's texture only,
    // while GX resolves it through the whole TEV chain - so a HUD element faded
    // out by a TEV register or konst alpha is invisible on hardware and fully
    // opaque here. This log is what says whether that is actually happening,
    // rather than assuming it.
    // Cap raised from 12 to 160 because the symptom lives past the old cap: Wind
    // Waker's title screen submits 104 UI draws and the unexplained HUD sprites
    // are draws 2-103, so a 12-draw window recorded the title art and nothing
    // else. 160 covers every draw seen on any screen measured so far, and the
    // whole block is behind ShouldTraceDraws() (one frame in 120) so the cost off
    // a trace frame is unchanged.
    if (g_remix_api->ShouldTraceDraws() && stats.ui_placed < UI_DRAW_DUMP_LIMIT)
    {
      const u32 stages = bpmem.genMode.numtevstages + 1;
      std::string alpha_chain;
      for (u32 stage = 0; stage < stages && stage < 16; ++stage)
      {
        // r{} is the stage's OWN rasterized colour channel. It is per stage -
        // bpmem.tevorders[stage >> 1].getColorChan(stage & 1), same indexing as
        // PixelShaderGen.cpp:257 - and the stage that reads RASA is very often
        // not stage 0, so reading stage 0's channel and calling it "the draw's
        // channel" answers a different question than the one being asked.
        //
        // rs/ts are the stage's swap-table SELECTORS (BPMemory.h:466-467, the
        // low four bits of the alpha combiner). They are what turns "texture
        // alpha" into texel[swap[Alpha]] and "rasterized alpha" into
        // channel[swap[Alpha]] on console (Tev.cpp:473-477 and 34-56). Nothing
        // in this backend reads them yet, and they are the only per-stage alpha
        // state the previous measurement did not print - which is exactly why
        // two draws could come out bit-identical in every printed field and
        // still differ on hardware.
        const auto& ac = bpmem.combiners[stage].alphaC;
        alpha_chain += fmt::format(
            "{}[a{} b{} c{} d{} k{} r{} rs{} ts{} ->{}]", stage == 0 ? "" : " ",
            static_cast<u32>(ac.a.Value()), static_cast<u32>(ac.b.Value()),
            static_cast<u32>(ac.c.Value()), static_cast<u32>(ac.d.Value()),
            static_cast<u32>(bpmem.tevksel.GetKonstAlpha(stage)),
            static_cast<u32>(bpmem.tevorders[stage >> 1].getColorChan(stage & 1)),
            static_cast<u32>(ac.rswap.Value()), static_cast<u32>(ac.tswap.Value()),
            static_cast<u32>(ac.dest.Value()));
      }

      // The four swap tables themselves, as the channel index each of R, G, B, A
      // is taken from (BPMemory.h:2006). A selector is meaningless without them:
      // rs1 says "table 1", and only the table says whether that maps Alpha to
      // Alpha or to Red. All four are printed rather than the referenced ones
      // because they are eight BP registers in total and a wrong guess about
      // which is referenced is the mistake this line exists to rule out.
      std::string swap_tables;
      for (u32 table = 0; table < 4; ++table)
      {
        const auto& swap = bpmem.tevksel.GetSwapTable(table);
        swap_tables += fmt::format("{}t{}[{} {} {} {}]", table == 0 ? "" : " ", table,
                                   static_cast<u32>(swap[ColorChannel::Red]),
                                   static_cast<u32>(swap[ColorChannel::Green]),
                                   static_cast<u32>(swap[ColorChannel::Blue]),
                                   static_cast<u32>(swap[ColorChannel::Alpha]));
      }

      INFO_LOG_FMT(VIDEO,
                   "Remix UI draw {}: WHERE clip [{:.0f} {:.0f} {:.0f} {:.0f}] vp [{:.0f} {:.0f} "
                   "{:.0f} {:.0f}] COL v0 {:#010x} v1 {:#010x} | tex {:#018x} src {:#010x} "
                   "efbcopy {} xfbcopy {} | blend en {} "
                   "src {} dst {} sub {} logic {} op "
                   "{} | colorupd {} alphaupd {} pixfmt {} | atest {:#010x} | dstalpha {:#x} | "
                   "verts {} | ind stages {} ind0 {:#07x} | swap {} | tev stages {} alpha {}",
                   stats.ui_placed, clip[0], clip[1], clip[2], clip[3], viewport[0], viewport[1],
                   viewport[2], viewport[3],
                   // The colour the rasterizer will actually fill an UNTEXTURED
                   // draw with - it has nothing else to go on. If this reads
                   // white on a draw the console shaded from a TEV register,
                   // that is the bug, not the symptom.
                   out_vertices->empty() ? 0 : (*out_vertices)[0].color,
                   out_vertices->size() < 2 ? 0 : (*out_vertices)[1].color,
                   albedo != nullptr ? albedo->GetContentHash() : 0, texture_addr,
                   texture_is_efb_copy ? 1 : 0, texture_is_xfb_copy ? 1 : 0,
                   bpmem.blendmode.blend_enable ? 1 : 0,
                   static_cast<u32>(bpmem.blendmode.src_factor.Value()),
                   static_cast<u32>(bpmem.blendmode.dst_factor.Value()),
                   bpmem.blendmode.subtract ? 1 : 0, bpmem.blendmode.logic_op_enable ? 1 : 0,
                   static_cast<u32>(bpmem.blendmode.logic_mode.Value()),
                   bpmem.blendmode.color_update ? 1 : 0, bpmem.blendmode.alpha_update ? 1 : 0,
                   static_cast<u32>(bpmem.zcontrol.pixel_format.Value()), bpmem.alpha_test.hex,
                   bpmem.dstalpha.hex, out_vertices->size(),
                   static_cast<u32>(bpmem.genMode.numindstages), bpmem.tevind[0].hex, swap_tables,
                   stages, alpha_chain);

      // Everything that decides the draw's FINAL alpha, in one line, so a HUD
      // sprite that should be invisible can be told apart from title art that
      // should not. Three groups, each discriminating a different mechanism:
      //
      //   resolved/corners - what the rasterizer will actually apply. If these
      //     are ~0 and the sprite is still on screen, the bug is downstream in
      //     the rasterizer, not in the state read.
      //   creg/konst       - the TEV register and konst alphas as read AT THIS
      //     DRAW. GXSetTevColor writes land in PixelShaderManager::constants via
      //     BPWritten, which calls FlushPipeline() before it updates bpmem, so a
      //     value read here belongs to this draw and not to a later one. Printing
      //     them is what turns that argument into a measurement.
      //   chN                - the alpha each XF colour channel would rasterize.
      //     BOTH channels, because the stage that reads RASA names its own
      //     channel and it is routinely not the one stage 0 names. The UI path
      //     writes opaque white into the vertex colour whenever
      //     ResolveRasterColor cannot name a vertex slot, so a channel whose
      //     alpha comes from the material register (or from a channel index the
      //     draw does not rasterize at all) is silently promoted to 255.
      //   vtxA / rawA        - what was WRITTEN into the submitted vertices
      //     versus what the vertex stream actually carried. They disagree exactly
      //     when a real per-vertex alpha was dropped.
      const auto& psm_constants = Core::System::GetInstance().GetPixelShaderManager().constants;
      std::string channels;
      for (u32 ch = 0; ch < 2; ++ch)
      {
        // MatColorToRemix's note applies: matColor's bytes are R,G,B,A from the
        // high end down, so alpha is the LOW byte.
        channels += fmt::format("{}ch{}[msC{} msA{} litA{} matA{} ambA{} vslot{}]",
                                ch == 0 ? "" : " ", ch,
                                static_cast<u32>(xfmem.color[ch].matsource.Value()),
                                static_cast<u32>(xfmem.alpha[ch].matsource.Value()),
                                xfmem.alpha[ch].enablelighting ? 1 : 0,
                                xfmem.matColor[ch] & 0xFFu, xfmem.ambColor[ch] & 0xFFu,
                                VertexSlotForChannel(decl, ch));
      }
      u32 vertex_alpha_min = 256;
      u32 vertex_alpha_max = 0;
      for (const remixapi_HardcodedVertex& vertex : *out_vertices)
      {
        const u32 alpha = (vertex.color >> 24) & 0xFFu;
        vertex_alpha_min = std::min(vertex_alpha_min, alpha);
        vertex_alpha_max = std::max(vertex_alpha_max, alpha);
      }
      // Straight off the decoded stream, bypassing ResolveRasterColor's choice,
      // so "the game supplied no alpha" and "we threw the alpha away" are
      // distinguishable. Dolphin's vertex loader writes colours R,G,B,A in memory
      // order, so alpha is the HIGH byte of the little-endian u32 - the same byte
      // ToRemixVertexColor leaves in place.
      std::string raw_alpha;
      for (u32 slot = 0; slot < 2; ++slot)
      {
        if (!decl.colors[slot].enable)
        {
          raw_alpha += fmt::format("{}s{}[-]", slot == 0 ? "" : " ", slot);
          continue;
        }
        u32 low = 256;
        u32 high = 0;
        for (u32 i = 0; i < vertex_count; ++i)
        {
          u32 packed = 0;
          std::memcpy(&packed, m_base_buffer_pointer + static_cast<size_t>(i) * stride +
                                   decl.colors[slot].offset,
                      sizeof(u32));
          const u32 alpha = (packed >> 24) & 0xFFu;
          low = std::min(low, alpha);
          high = std::max(high, alpha);
        }
        u32 first = 0;
        std::memcpy(&first, m_base_buffer_pointer + decl.colors[slot].offset, sizeof(u32));
        raw_alpha += fmt::format("{}s{}[a {} {} v0 {:#010x} off {} comp {} type {}]",
                                 slot == 0 ? "" : " ", slot, low > 255 ? 0 : low, high, first,
                                 decl.colors[slot].offset, decl.colors[slot].components,
                                 static_cast<u32>(decl.colors[slot].type));
      }
      INFO_LOG_FMT(
          VIDEO,
          "Remix UI alpha {}: resolved {} corners [{:.3f} {:.3f} {:.3f} {:.3f}] | creg a [{} {} {} "
          "{}] konst a [{} {} {} {}] | nchan {} | {} | vslot {} vtxA [{} {}] rawA {}",
          stats.ui_placed, blend.tev_alpha_known ? 1 : 0, blend.tev_alpha_corners[0],
          blend.tev_alpha_corners[1], blend.tev_alpha_corners[2], blend.tev_alpha_corners[3],
          psm_constants.colors[0][3], psm_constants.colors[1][3], psm_constants.colors[2][3],
          psm_constants.colors[3][3], psm_constants.kcolors[0][3], psm_constants.kcolors[1][3],
          psm_constants.kcolors[2][3], psm_constants.kcolors[3][3],
          static_cast<u32>(xfmem.numChan.numColorChans), channels, color_slot,
          vertex_alpha_min > 255 ? 0 : vertex_alpha_min, vertex_alpha_max, raw_alpha);
    }

    std::array<float, 6> ortho_raw = xfmem.projection.rawProjection;
    g_remix_api->SubmitUiDraw(*out_vertices, *out_indices, raw_modelview, ortho_raw, viewport, clip,
                              albedo, filter_mode, wrap_mode_u, wrap_mode_v, blend, tag_bypass);
    return;
  }

  // Mode 2 - and, since tag routing landed, mode 1's world-routed draws: a
  // "World Space UI" tag, or every 2D draw while the dev menu is open. Nothing
  // below needs to distinguish them; the mode-2 path already does exactly what
  // a world-routed ortho draw needs (raw projection handed over, null modelview,
  // no reference-projection latch, no histogram entry, no sky test), and it is
  // the path the runtime's picker can see.
  //
  // Perspective draws also arrive here, as they always have. The third routing
  // site - the UI-tag divert for a HUD drawn in 3D - sits further down, after
  // EnsureMaterial, because the key it looks up is built out of the material.
  //
  // An ortho draw hands over its raw projection and a NULL modelview:
  // the first is what maps its screen-space vertices onto the UI plane, the
  // second keeps it out of the camera histogram and the estimator both.
  const std::array<float, 6>* world_ui_projection = nullptr;
  std::array<float, 6> ortho_raw = {};
  if (is_ortho)
  {
    ortho_raw = xfmem.projection.rawProjection;
    world_ui_projection = &ortho_raw;
    raw_modelview = nullptr;
  }
  // Material resolution happens HERE, not at the top of the function, because it
  // depends on the decoded geometry. A classified sky mesh takes an unlit
  // (emissive) material, the classifier is keyed on the geometry hash, and the
  // geometry hash needs the final vertex and index buffers - which only exist at
  // this point. The material then folds into the mesh hash as it always has.
  //
  // The untextured sky's colour is the folded TEV constant, which is already
  // sitting in blend.tfactor as 0x00RRGGBB; a textured sky ignores it and uses
  // its own albedo as the emissive texture instead.
  // The vertex-to-bone partition is part of what this mesh IS, so it belongs in
  // the identity the mesh cache and every sky decision key on. See
  // RemixApi::FoldSkinningHash.
  const u64 geometry_hash =
      draw_skinning != nullptr ?
          RemixApi::FoldSkinningHash(RemixApi::GeometryHash(*out_vertices, *out_indices),
                                     draw_skinning->blend_indices) :
          RemixApi::GeometryHash(*out_vertices, *out_indices);
  const bool sky_emissive =
      !is_ortho && g_remix_api->SkyEmissiveEnabled() && g_remix_api->IsSkyGeometry(geometry_hash);
  const MaterialRef material =
      g_remix_api->EnsureMaterial(albedo, filter_mode, wrap_mode_u, wrap_mode_v, alpha_test_type,
                                  alpha_reference, sky_emissive, blend.tfactor & 0x00FFFFFFu);
  if (material.handle == nullptr)
    return;

  // ---- Tag-driven routing for PERSPECTIVE draws ----------------------------
  //
  // Not every HUD is 2D. A game is free to park its HUD quads a short distance
  // in front of the camera and draw them through the ordinary 3D frustum, and
  // Resident Evil 4 does exactly that - so a "UI Texture" tag on its health ring
  // moved nothing, because the block above only ever looks at ortho draws.
  // This is the third routing site (ortho overlay, ortho world-route, and now
  // this one), and the only one that can act on a perspective draw.
  //
  // It sits HERE, after EnsureMaterial, and that placement is the whole reason
  // the untextured key below is trustworthy rather than hopeful: geometry_hash
  // (skinning fold included), material.hash (upload outcome and sky fold
  // included) are at this point the exact two values SubmitMesh would fold into
  // the mesh handle, which is the value the dev menu records when the user
  // clicks an untextured object. Computing the key earlier, from re-derived
  // inputs, is how the two drift apart.
  //
  // ONLY the UI tag acts. Ignore and World Space UI on a perspective draw are
  // applied by the runtime's own category hook - it receives a real API draw and
  // handles those two itself - so doing it here as well would double-apply.
  // RemixUiStrict is likewise ortho-only: a policy over UNTAGGED draws, applied
  // to perspective geometry, would delete the world.
  if (!is_ortho && ui_mode == 1 && g_remix_api->UiTagRoutingEnabled() &&
      g_remix_api->UiTagPerspectiveEnabled() && !g_remix_api->UiTaggingModeActive())
  {
    // Mode 1 is required because the overlay only exists in mode 1; diverting in
    // mode 0 or 2 would vanish the draw rather than move it. The tagging-view
    // check is the mirror of the ortho block's: while RemixUiWorldView is on
    // the element stays a world draw, because overlay pixels have no object for
    // the click-to-tag picker to hit and a tag that cannot be removed is a
    // trap. Neither needs a counter - nothing about the draw changed.
    const bool textured = albedo != nullptr && albedo->HasData();
    const u64 tag_key = textured ? albedo->GetContentHash() :
                                   RemixApi::ComputeMeshHash(geometry_hash, material.hash);

    if (g_remix_api->ClassifyUiDraw(tag_key) == RemixApi::UiTagClass::Ui)
    {
      if (draw_skinning != nullptr)
      {
        // GPU-skinned: the vertices are bone-local and the pose lives entirely
        // in the palette the runtime applies, so there is no single modelview
        // the overlay could place them with. Kept world-side and counted.
        // (A CPU-baked matrix-palette draw is fine and does not land here - it
        // passes a null raw_modelview with vertices already in view space, which
        // is exactly SubmitUiDraw's null-modelview contract.)
        ++stats.ui_persp_skinned;
      }
      else
      {
        // This draw's OWN projection and the RAW modelview, not the reference
        // projection and not the folded `transform`. The projection-fold
        // correction exists to render a foreign-frustum draw through the frame's
        // reference camera; the overlay wants the game's own screen mapping,
        // which is the raw pair. The fold only ever touches `transform`, so
        // there is nothing to undo here.
        const UiPlacement placement = ComputeUiPlacement();
        const std::array<float, 6> persp_raw = xfmem.projection.rawProjection;
        if (g_remix_api->SubmitUiDraw(*out_vertices, *out_indices, raw_modelview, persp_raw,
                                      placement.viewport, placement.clip, albedo, filter_mode,
                                      wrap_mode_u, wrap_mode_v, blend, /*tag_bypass=*/true,
                                      /*perspective=*/true))
        {
          ++stats.ui_tagged_persp;
          return;
        }
        // False means the overlay did not take it: either it is structurally
        // unavailable, or a vertex sat at or behind the eye plane. Fall through
        // and submit it as world geometry, which is what it was a moment ago -
        // a non-destructive failure either way.
      }
    }
  }

  if (sky_emissive)
    ++stats.sky_emissive;

  g_remix_api->SubmitMesh(material, geometry_hash, *out_vertices, *out_indices, transform,
                          category_flags, blend, raw_modelview, diagnostics, world_ui_projection,
                          draw_skinning);
}

}  // namespace Remix
