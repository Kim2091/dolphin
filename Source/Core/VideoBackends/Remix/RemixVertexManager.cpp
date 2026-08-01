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

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/XFMemory.h"

namespace Remix
{
namespace
{
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
};

RasterColor ResolveRasterColor(const PortableVertexDeclaration& decl, bool gx_semantics,
                               bool resolve_alpha)
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

  u32 channel;
  switch (bpmem.tevorders[0].getColorChan(0))
  {
  case RasColorChan::Color0:
    channel = 0;
    break;
  case RasColorChan::Color1:
    channel = 1;
    break;
  default:
    // Alpha bump or a hardwired zero: not a lit colour channel at all.
    return out;
  }

  if (channel >= xfmem.numChan.numColorChans)
    return out;

  out.tfactor = MatColorToRemix(xfmem.matColor[channel]);
  const int vertex_slot = VertexSlotForChannel(decl, channel);

  const LitChannel& color_channel = xfmem.color[channel];
  if (color_channel.matsource == MatSource::MatColorRegister)
  {
    out.color_arg2 = REMIX_TEX_ARG_TFACTOR;
  }
  else if (vertex_slot >= 0)
  {
    out.color_arg2 = REMIX_TEX_ARG_VERTEX_COLOR0;
    out.vertex_slot = vertex_slot;
    // With lighting off the channel colour IS the vertex colour, which on GC is
    // overwhelmingly baked lighting - that is why the games use it. With
    // lighting on, the vertex colour is the material term the hardware
    // multiplies the lights into, so it is a real material colour and must not
    // be normalized.
    out.baked_lighting = !color_channel.enablelighting;
  }

  if (!resolve_alpha)
    return out;

  const LitChannel& alpha_channel = xfmem.alpha[channel];
  if (alpha_channel.matsource == MatSource::MatColorRegister)
  {
    out.alpha_arg2 = REMIX_TEX_ARG_TFACTOR;
  }
  else if (vertex_slot >= 0)
  {
    out.alpha_arg2 = REMIX_TEX_ARG_VERTEX_COLOR0;
    out.vertex_slot = vertex_slot;
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

TexGenState ResolveTexGen(const PortableVertexDeclaration& decl, bool gx_texgen)
{
  TexGenState out;
  if (!gx_texgen)
    return out;

  // Stage 0 names the texgen slot it samples; it is not always slot 0.
  const u32 coord = bpmem.tevorders[0].getTexCoord(0);
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
struct ProjectionCorrection
{
  float x_scale = 1.0f;
  float x_shear = 0.0f;
  float y_scale = 1.0f;
  float y_shear = 0.0f;
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
                                           ProjectionCorrection& out)
{
  // A reference with no x or y scale is not a frustum at all; there is nothing
  // meaningful to fold onto.
  if (std::abs(reference[0]) < 1e-6f || std::abs(reference[2]) < 1e-6f)
    return CorrectionResult::Unrepresentable;

  // reference[1] / reference[3] deliberately do not appear: the camera never
  // applied them, so the draw's own off-centre terms fold in whole.
  out.x_scale = draw[0] / reference[0];
  out.x_shear = draw[1] / reference[0];
  out.y_scale = draw[2] / reference[2];
  out.y_shear = draw[3] / reference[2];

  if (!std::isfinite(out.x_scale) || !std::isfinite(out.x_shear) ||
      !std::isfinite(out.y_scale) || !std::isfinite(out.y_shear))
  {
    return CorrectionResult::Unrepresentable;
  }

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
}  // namespace

VertexManager::VertexManager() = default;

VertexManager::~VertexManager() = default;

void VertexManager::DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex)
{
  if (!g_remix_api || !g_remix_api->IsValid())
    return;

  FrameStats& stats = g_remix_api->Stats();
  ++stats.draws_seen;

  // ---- Classify -----------------------------------------------------------

  // Orthographic projection is HUD/UI/menus. It is never submitted; v1 has no
  // 2D compositing path at all, so those draws are simply absent from the
  // path-traced output.
  if (xfmem.projection.type != ProjectionType::Perspective)
  {
    ++stats.skipped_ortho;
    return;
  }

  // First perspective draw of the frame defines the camera; every distinct
  // projection after it is tracked so the frame log can say whether the
  // reference is the one actually carrying the scene.
  const int projection_slot = g_remix_api->ObserveProjection(xfmem.projection.rawProjection);

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
  if ((!bpmem.blendmode.color_update && !bpmem.blendmode.alpha_update) ||
      bpmem.alpha_test.TestResult() == AlphaTestResult::Fail)
  {
    ++stats.skipped_invisible;
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

  // v1 materials are "whatever TEV stage 0 samples". Every other stage - and
  // therefore every multi-stage combiner effect - is ignored.
  const bool stage0_textured = bpmem.tevorders[0].getEnable(0) != 0;
  const u32 stage0_texmap = bpmem.tevorders[0].getTexMap(0);
  const RemixTexture* albedo = nullptr;
  u8 filter_mode = 1;   // MDL Filter::Linear
  u8 wrap_mode_u = 1;   // MDL WrapMode::Repeat
  u8 wrap_mode_v = 1;

  if (stage0_textured)
  {
    albedo = g_remix_api->GetBoundTexture(stage0_texmap);
    // Our texture cache does not execute EFB/XFB copies, so an entry produced
    // by one never receives pixels through Load(). That is exactly the set of
    // draws we cannot translate (dynamic shadow maps, heat haze, water
    // reflections) - and also the set of custom-texture formats we do not
    // decode in v1.
    if (albedo == nullptr || !albedo->HasData())
    {
      ++stats.skipped_efb_texture;
      return;
    }

    const TexMode0& mode = bpmem.tex.GetUnit(stage0_texmap).texMode0;
    // GX and Remix (MDL) agree numerically: Near/Linear == 0/1 and
    // Clamp/Repeat/Mirror == 0/1/2. The invalid GX wrap value 3 behaves as
    // clamp on hardware.
    filter_mode = static_cast<u8>(mode.mag_filter.Value());
    wrap_mode_u = static_cast<u8>(std::min<u32>(static_cast<u32>(mode.wrap_s.Value()), 2));
    wrap_mode_v = static_cast<u8>(std::min<u32>(static_cast<u32>(mode.wrap_t.Value()), 2));
  }

  u8 alpha_test_type = 7;
  u8 alpha_reference = 0;
  ResolveAlphaTest(bpmem.alpha_test, alpha_test_type, alpha_reference);

  const MaterialRef material = g_remix_api->EnsureMaterial(albedo, filter_mode, wrap_mode_u,
                                                           wrap_mode_v, alpha_test_type,
                                                           alpha_reference);
  if (material.handle == nullptr)
    return;

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
  // Costs one extra pass over the posmtx attribute, which is 4 bytes a vertex.
  u32 uniform_matrix_index = 0;
  bool uniform_matrix = per_vertex_matrix;
  if (per_vertex_matrix)
  {
    std::memcpy(&uniform_matrix_index, m_base_buffer_pointer + decl.posmtx.offset, sizeof(u32));
    uniform_matrix_index &= 0x3f;
    for (u32 i = 1; i < vertex_count; ++i)
    {
      u32 index = 0;
      std::memcpy(&index, m_base_buffer_pointer + static_cast<size_t>(i) * stride + decl.posmtx.offset,
                  sizeof(u32));
      if ((index & 0x3f) != uniform_matrix_index)
      {
        uniform_matrix = false;
        break;
      }
    }
  }
  // Only genuinely multi-matrix geometry gets its vertices transformed.
  const bool bake_vertices = per_vertex_matrix && !uniform_matrix;

  const int position_components = std::min(decl.position.components, 3);
  const bool has_normals = decl.normals[0].enable;
  const bool has_texcoord = decl.texcoords[0].enable;
  const int texcoord_components = has_texcoord ? std::min(decl.texcoords[0].components, 2) : 0;

  // Texture coordinates the way the console generates them, rather than "vertex
  // attribute 0, verbatim". Both halves of that shortcut were wrong: stage 0
  // names which texgen slot it samples, and the xfmem texture matrix on that
  // slot is how every scrolling or scaled texture on the machine is animated.
  const TexGenState texgen = ResolveTexGen(decl, g_remix_api->GxTexGenEnabled());
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
  u32 draw_light_mask = 0;
  std::array<u8, 8> draw_attenuation = {};
  const auto claim_lights = [&](const LitChannel& lit_channel) {
    const u32 channel_mask = lit_channel.GetFullLightMask();
    for (u32 i = 0; i < draw_attenuation.size(); ++i)
    {
      if ((channel_mask & (1u << i)) != 0 && (draw_light_mask & (1u << i)) == 0)
        draw_attenuation[i] = static_cast<u8>(lit_channel.attnfunc.Value());
    }
    draw_light_mask |= channel_mask;
  };
  for (u32 channel = 0; channel < xfmem.numChan.numColorChans && channel < 2; ++channel)
  {
    claim_lights(xfmem.color[channel]);
    claim_lights(xfmem.alpha[channel]);
  }
  stats.draw_light_mask |= draw_light_mask;
  g_remix_api->NoteDrawLights(draw_light_mask, draw_attenuation);

  // What TEV stage 0 rasterizes, by GX's rules rather than "colours[0], always".
  const bool gx_blend = g_remix_api->GxBlendEnabled();
  const RasterColor raster_color =
      ResolveRasterColor(decl, g_remix_api->GxColorEnabled(), gx_blend);
  const int color_slot = raster_color.vertex_slot;
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

  DrawBlendState blend;
  blend.color_arg1 = REMIX_TEX_ARG_TEXTURE;
  blend.color_arg2 = raster_color.color_arg2;
  blend.color_operation = REMIX_TEX_OP_MODULATE;
  blend.tfactor = raster_color.tfactor;
  blend.vertex_color_is_baked_lighting = raster_color.baked_lighting;

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
      // Genuinely multi-matrix geometry: each vertex names its own modelview.
      // There is no per-vertex transform on the Remix side without real skinning
      // data, so bake the transform in and submit with an identity instance
      // transform. The mesh hash then covers the transformed bytes, so these
      // re-create every frame and lean on the idle-mesh LRU - an accepted cost,
      // now paid only by the draws that actually need it.
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
      ReadFloats(src + decl.texcoords[0].offset, dst.texcoord, texcoord_components);

    if (color_slot >= 0)
    {
      u32 color = 0;
      std::memcpy(&color, src + decl.colors[color_slot].offset, sizeof(u32));
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
    // transform is identity, so there is no modelview to un-mirror.
    const float* const winding_matrix =
        bake_vertices ? nullptr :
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

      for (const remixapi_HardcodedVertex* source : {&v0, &v1, &v2})
      {
        remixapi_HardcodedVertex vertex = *source;
        vertex.normal[0] = normal[0];
        vertex.normal[1] = normal[1];
        vertex.normal[2] = normal[2];
        m_flat_indices.push_back(static_cast<u32>(m_flat_vertices.size()));
        m_flat_vertices.push_back(vertex);
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
  if (bake_vertices)
  {
    transform.matrix[0][0] = 1.0f;
    transform.matrix[1][1] = 1.0f;
    transform.matrix[2][2] = 1.0f;
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
  if (xfmem.projection.rawProjection[1] != 0.0f || xfmem.projection.rawProjection[3] != 0.0f)
    ++stats.projection_oblique;

  if (g_remix_api->ProjectionFixEnabled() && g_remix_api->HasReferenceProjection())
  {
    ProjectionCorrection correction;
    switch (BuildProjectionCorrection(xfmem.projection.rawProjection,
                                      g_remix_api->ReferenceProjection(), correction))
    {
    case CorrectionResult::Apply:
      ApplyProjectionCorrection(correction, transform);
      ++stats.projection_corrected;
      break;
    case CorrectionResult::Unrepresentable:
      ++stats.projection_uncorrectable;
      break;
    case CorrectionResult::NotNeeded:
      break;
    }
  }

  // Two routes to sky. The explicit texture list is the reliable one - it names
  // the exact skybox texture, so it cannot mistake clouds or overlays for the
  // horizon dome the way the depth heuristic did. The heuristic remains behind
  // RemixSkyMode for games where the hash is not known yet, defaulted off.
  const int sky_mode = g_remix_api->SkyMode();
  const bool is_sky = (albedo != nullptr && g_remix_api->IsSkyTexture(albedo->GetContentHash())) ||
                      (sky_mode != 0 && IsSkyDraw(bpmem.zmode));
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

  g_remix_api->NoteProjectionUse(projection_slot, static_cast<u32>(out_vertices->size()));
  g_remix_api->SubmitMesh(material, *out_vertices, *out_indices, transform, category_flags, blend,
                          raw_modelview);
}

}  // namespace Remix
