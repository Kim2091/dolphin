// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Remix/RemixUiRaster.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Remix
{
namespace
{
// Everything below works in 0-255 integers rather than floats. This is a
// full-screen-per-draw inner loop - a game that fades the screen covers every
// pixel in the overlay several times a frame - and the first version, which
// unpacked to float, blended, and packed back, cost enough to drop the emulated
// game to a third of full speed.

// (a * b) / 255, exact for all 0-255 inputs.
constexpr u32 MulDiv255(u32 a, u32 b)
{
  const u32 t = a * b + 128;
  return (t + (t >> 8)) >> 8;
}

// GX CompareMode, which is already Remix's and Vulkan's numbering. Alpha and
// reference are both 0-255 here, so the test is exact rather than depending on
// float equality for the == and != cases.
bool CompareAlpha(u8 compare, u32 alpha, u32 reference)
{
  switch (compare)
  {
  case 0:
    return false;  // Never
  case 1:
    return alpha < reference;
  case 2:
    return alpha == reference;
  case 3:
    return alpha <= reference;
  case 4:
    return alpha > reference;
  case 5:
    return alpha != reference;
  case 6:
    return alpha >= reference;
  default:
    return true;  // Always
  }
}

// The full GX alpha test: two comparators joined by a logic op (Tev.cpp
// TevAlphaTest). Costs nothing extra in a software rasterizer, and reducing it
// to comparator 0 alone turns every Or/Xor/Xnor configuration into "always
// pass", which draws things the console rejects.
bool AlphaPasses(u8 compare0, u32 reference0, u8 compare1, u32 reference1, u8 logic, u32 alpha)
{
  const bool first = CompareAlpha(compare0, alpha, reference0);
  const bool second = CompareAlpha(compare1, alpha, reference1);
  switch (logic)
  {
  case 0:
    return first && second;
  case 1:
    return first || second;
  case 2:
    return first != second;
  default:
    return first == second;
  }
}

// The GX z compare, on 24-bit screen z, in the same CompareMode numbering as
// the alpha test above - the exact switch SWEfbInterface::ZCompare runs.
bool DepthPasses(u8 func, u32 z, u32 stored)
{
  switch (func)
  {
  case 0:
    return false;  // Never
  case 1:
    return z < stored;
  case 2:
    return z == stored;
  case 3:
    return z <= stored;
  case 4:
    return z > stored;
  case 5:
    return z != stored;
  case 6:
    return z >= stored;
  default:
    return true;  // Always
  }
}

// The console's z clear value: the far plane of a 24-bit depth buffer, which is
// what games overwhelmingly clear to. The overlay has no game-controlled clear,
// so this is the fixed background every depth-tested frame starts from.
constexpr u32 kDepthClear = 0x00FFFFFFu;
}  // namespace

UiRasterizer::UiRasterizer() = default;

UiRasterizer::~UiRasterizer()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_shutdown = true;
  }
  m_work_ready.notify_all();
  for (std::thread& worker : m_workers)
  {
    if (worker.joinable())
      worker.join();
  }
}

void UiRasterizer::Begin(u32 width, u32 height)
{
  m_width = width;
  m_height = height;
  m_touched.store(false, std::memory_order_relaxed);
  m_draw_count = 0;
  // Per-frame like everything else here: the caller re-decides it before each
  // Flush, from that frame's own world-draw count.
  m_pre_world_filter = false;
  // Re-decided per frame from the draws actually recorded; the plane itself is
  // only touched in Flush, and only when this comes up true.
  m_depth_used = false;
  const size_t needed = static_cast<size_t>(width) * height;
  // The buffer has to come back fully transparent every frame or last frame's
  // HUD ghosts under this one's. resize + fill rather than assign so the
  // allocation is reused once the size has settled.
  m_pixels.resize(needed);
  std::fill(m_pixels.begin(), m_pixels.end(), 0u);
}

void UiRasterizer::WorkerLoop()
{
  u64 seen_generation = 0;
  while (true)
  {
    u32 band = 0;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_work_ready.wait(lock, [&] { return m_shutdown || m_generation != seen_generation; });
      if (m_shutdown)
        return;
      if (m_next_band >= m_bands_total)
      {
        // This generation is fully claimed; wait for the next one.
        seen_generation = m_generation;
        continue;
      }
      band = m_next_band++;
    }

    const int min_y = static_cast<int>(band * m_band_height);
    const int max_y =
        std::min(static_cast<int>((band + 1) * m_band_height) - 1, static_cast<int>(m_height) - 1);
    RasterizeBand(min_y, max_y);

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_bands_finished;
    }
    m_band_done.notify_one();
  }
}

void UiRasterizer::RasterizeBand(int min_y, int max_y)
{
  for (size_t i = 0; i < m_draw_count; ++i)
  {
    const RecordedDraw& draw = m_draws[i];
    if (draw.max_y < min_y || draw.min_y > max_y)
      continue;
    // Pre-world wash, on a frame that turned out to have world content, with no
    // UI tag rescuing it. Skipped rather than removed, so the recorded draw
    // keeps its storage - see SetPreWorldFilter.
    if (m_pre_world_filter && draw.call.world_draws_at_submit == 0 && !draw.call.tag_protected)
      continue;
    const size_t triangles = draw.indices.size() / 3;
    for (size_t t = 0; t < triangles; ++t)
    {
      const u32 i0 = draw.indices[t * 3 + 0];
      const u32 i1 = draw.indices[t * 3 + 1];
      const u32 i2 = draw.indices[t * 3 + 2];
      if (i0 >= draw.vertices.size() || i1 >= draw.vertices.size() || i2 >= draw.vertices.size())
        continue;
      DrawTriangle(draw.call, draw.vertices[i0], draw.vertices[i1], draw.vertices[i2], min_y,
                   max_y);
    }
  }
}

void UiRasterizer::Flush()
{
  if (m_draw_count == 0 || m_width == 0 || m_height == 0)
    return;

  // The depth plane exists only on frames that need it. Prepared here, before
  // any band starts, because the bands write disjoint rows of it but all of
  // them read the clear value.
  if (m_depth_used)
  {
    m_depth.resize(static_cast<size_t>(m_width) * m_height);
    std::fill(m_depth.begin(), m_depth.end(), kDepthClear);
  }

  // Bands are sized so that every worker gets one and the calling thread takes
  // the last. Below this much work the handoff costs more than it saves.
  const u32 hardware = std::max(1u, std::thread::hardware_concurrency());
  const u32 wanted = std::clamp(hardware, 1u, 16u);
  if (wanted <= 1 || m_height < 64)
  {
    RasterizeBand(0, static_cast<int>(m_height) - 1);
    return;
  }

  while (m_workers.size() < wanted - 1)
    m_workers.emplace_back([this] { WorkerLoop(); });

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_band_height = (m_height + wanted - 1) / wanted;
    m_bands_total = (m_height + m_band_height - 1) / m_band_height;
    m_next_band = 0;
    m_bands_finished = 0;
    ++m_generation;
  }
  m_work_ready.notify_all();

  // The calling thread is a worker too - it would otherwise sit blocked while
  // one fewer core did the work.
  while (true)
  {
    u32 band = 0;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_next_band >= m_bands_total)
        break;
      band = m_next_band++;
    }
    const int min_y = static_cast<int>(band * m_band_height);
    const int max_y =
        std::min(static_cast<int>((band + 1) * m_band_height) - 1, static_cast<int>(m_height) - 1);
    RasterizeBand(min_y, max_y);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_bands_finished;
    }
  }

  std::unique_lock<std::mutex> lock(m_mutex);
  m_band_done.wait(lock, [&] { return m_bands_finished >= m_bands_total; });
}

u32 UiRasterizer::Sample(const Texture& texture, float u, float v) const
{
  // Returns RGBA packed 0xAABBGGRR, matching the overlay buffer's own layout so
  // that the common paths never have to unpack a colour at all.
  const int width = static_cast<int>(texture.width);
  const int height = static_cast<int>(texture.height);

  const auto wrap = [](int value, int limit, bool clamp) {
    if (clamp)
      return std::clamp(value, 0, limit - 1);
    // Almost every fetch is already in range; the modulo is the slow path.
    if (static_cast<u32>(value) < static_cast<u32>(limit))
      return value;
    value %= limit;
    return value < 0 ? value + limit : value;
  };
  const auto fetch = [&](int x, int y) {
    const size_t offset =
        (static_cast<size_t>(wrap(y, height, texture.clamp_v)) * texture.width +
         wrap(x, width, texture.clamp_u)) *
        4;
    u32 texel = 0;
    std::memcpy(&texel, texture.pixels + offset, sizeof(u32));
    return texel;
  };

  const float tx = u * static_cast<float>(width);
  const float ty = v * static_cast<float>(height);

  if (!texture.bilinear)
    return fetch(static_cast<int>(std::floor(tx)), static_cast<int>(std::floor(ty)));

  // Half-texel offset: sampling at the centre of a texel must return that texel
  // exactly, which is what keeps 1:1 UI from being softened for no reason.
  const float fx = tx - 0.5f;
  const float fy = ty - 0.5f;
  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const u32 wx = static_cast<u32>((fx - static_cast<float>(x0)) * 256.0f);
  const u32 wy = static_cast<u32>((fy - static_cast<float>(y0)) * 256.0f);

  // Exactly on a texel centre - which is the whole of a 1:1 UI element - so skip
  // three fetches and the interpolation entirely.
  if (wx == 0 && wy == 0)
    return fetch(x0, y0);

  const u32 c00 = fetch(x0, y0);
  const u32 c10 = fetch(x0 + 1, y0);
  const u32 c01 = fetch(x0, y0 + 1);
  const u32 c11 = fetch(x0 + 1, y0 + 1);

  // The differences below are computed in unsigned and can "go negative". That
  // is fine and deliberate: every intermediate's true mathematical value is in
  // [0, 2^24), so wrapping arithmetic mod 2^32 reproduces it exactly.
  u32 out = 0;
  for (int shift = 0; shift < 32; shift += 8)
  {
    const u32 a = (c00 >> shift) & 0xFF;
    const u32 b = (c10 >> shift) & 0xFF;
    const u32 c = (c01 >> shift) & 0xFF;
    const u32 d = (c11 >> shift) & 0xFF;
    const u32 top = (a << 8) + (b - a) * wx;
    const u32 bottom = (c << 8) + (d - c) * wx;
    out |= (((top << 8) + (bottom - top) * wy) >> 16) << shift;
  }
  return out;
}

void UiRasterizer::DrawTriangle(const DrawCall& call, const Vertex& a, const Vertex& b,
                                const Vertex& c, int clip_min_y, int clip_max_y)
{
  // NDC -> pixels. y is flipped because GX ndc y points up and the buffer's
  // first row is the top of the screen.
  const auto to_screen_x = [&](float ndc_x) {
    return call.viewport_x + (ndc_x * 0.5f + 0.5f) * call.viewport_width;
  };
  const auto to_screen_y = [&](float ndc_y) {
    return call.viewport_y + (0.5f - ndc_y * 0.5f) * call.viewport_height;
  };

  const float ax = to_screen_x(a.x), ay = to_screen_y(a.y);
  const float bx = to_screen_x(b.x), by = to_screen_y(b.y);
  const float cx = to_screen_x(c.x), cy = to_screen_y(c.y);

  const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  // Degenerate. Not an error - UI meshes carry plenty of zero-area triangles -
  // and a zero area would divide by zero in the barycentrics below.
  if (!(std::abs(area) > 1e-9f))
    return;
  const float inv_area = 1.0f / area;

  // Both windings are drawn. There is no culling to speak of in 2D, and a UI
  // quad's winding depends on how the game happened to emit it.
  int min_x = static_cast<int>(std::floor(std::min({ax, bx, cx})));
  int max_x = static_cast<int>(std::ceil(std::max({ax, bx, cx})));
  int min_y = static_cast<int>(std::floor(std::min({ay, by, cy})));
  int max_y = static_cast<int>(std::ceil(std::max({ay, by, cy})));
  // Clipped to the surface, to the draw's own scissor rect, and to this thread's
  // band. Every thread sees every triangle; the band is what makes their writes
  // disjoint.
  min_x = std::max({min_x, 0, call.clip_left});
  min_y = std::max({min_y, clip_min_y, call.clip_top});
  max_x = std::min({max_x, static_cast<int>(m_width) - 1, call.clip_right});
  max_y = std::min({max_y, clip_max_y, static_cast<int>(m_height) - 1, call.clip_bottom});
  if (min_x > max_x || min_y > max_y)
    return;

  // Edge functions stepped incrementally. Recomputing the barycentrics from
  // scratch per pixel was six multiplies a pixel for no reason; across a
  // full-screen quad that alone is millions of wasted multiplies a frame.
  // Only the x steps: each row re-seeds from the edge equations, which costs six
  // multiplies per SCANLINE instead of per pixel and avoids accumulating drift
  // down the triangle.
  const float d0_dx = -(cy - by) * inv_area;
  const float d1_dx = -(ay - cy) * inv_area;

  // A single flat colour for the whole triangle is by far the common case - a
  // fade, a letterbox bar, a solid panel - so the per-pixel colour interpolation
  // is skipped entirely when the three vertices agree.
  const bool flat_color = a.color == b.color && b.color == c.color;
  const u32 flat_r = static_cast<u32>(std::clamp(a.color[0], 0.0f, 1.0f) * 255.0f + 0.5f);
  const u32 flat_g = static_cast<u32>(std::clamp(a.color[1], 0.0f, 1.0f) * 255.0f + 0.5f);
  const u32 flat_b = static_cast<u32>(std::clamp(a.color[2], 0.0f, 1.0f) * 255.0f + 0.5f);
  const u32 flat_a = static_cast<u32>(std::clamp(a.color[3], 0.0f, 1.0f) * 255.0f + 0.5f);
  const bool white = flat_color && flat_r == 255 && flat_g == 255 && flat_b == 255 && flat_a == 255;
  const bool textured = call.texture.pixels != nullptr && call.texture.width != 0 &&
                        call.texture.height != 0;
  // Nothing to contribute at all: an untextured, fully transparent draw still
  // covers every pixel it spans, so rejecting it here is worth the check.
  if (!textured && flat_color && flat_a == 0 && call.blend != BlendMode::Opaque)
    return;

  const u32 alpha_reference =
      static_cast<u32>(std::clamp(call.alpha_reference, 0.0f, 1.0f) * 255.0f + 0.5f);
  const u32 alpha_reference1 =
      static_cast<u32>(std::clamp(call.alpha_reference1, 0.0f, 1.0f) * 255.0f + 0.5f);

  // Depth participation. GX only updates z while the test is enabled, so one
  // flag decides both; the plane is guaranteed allocated by Flush whenever any
  // recorded draw set it.
  const bool depth_active = call.depth_test;

  // A flat, opaque, untextured span is a fade, a letterbox bar or a solid panel,
  // and every pixel in it is the same value - so it becomes a fill rather than a
  // per-pixel blend. This is the single biggest saving on the draws that hurt,
  // because those are exactly the ones covering the whole screen. A depth-tested
  // draw cannot take it - each pixel's verdict is its own - but no ordinary 2D
  // fade tests depth, so the fast path keeps its whole audience.
  const bool flat_fill = !textured && flat_color && !depth_active &&
                         (call.blend == BlendMode::Opaque ||
                          (call.blend == BlendMode::Over && flat_a == 255));
  const u32 flat_texel = flat_r | (flat_g << 8) | (flat_b << 16) | 0xFF000000u;

  const float d2_dx = -d0_dx - d1_dx;
  const float start_px = static_cast<float>(min_x) + 0.5f;
  for (int y = min_y; y <= max_y; ++y)
  {
    const float py = static_cast<float>(y) + 0.5f;
    const float row_w0 = ((bx - start_px) * (cy - py) - (by - py) * (cx - start_px)) * inv_area;
    const float row_w1 = ((cx - start_px) * (ay - py) - (cy - py) * (ax - start_px)) * inv_area;
    const float row_w2 = 1.0f - row_w0 - row_w1;

    // Solve the row's covered x-range from the three edge equations instead of
    // walking the bounding box and rejecting. Each barycentric is linear in x, so
    // "all three >= 0" is an interval; a full-screen quad's two triangles each
    // cover half the box, and testing every pixel in it threw away half the work.
    int span_start = min_x;
    int span_end = max_x;
    bool empty = false;
    const auto clip_edge = [&](float value, float slope) {
      if (std::abs(slope) < 1e-12f)
      {
        if (value < 0.0f)
          empty = true;
        return;
      }
      // x where the edge hits zero, relative to min_x.
      const float crossing = -value / slope;
      if (slope > 0.0f)
        span_start = std::max(span_start, min_x + static_cast<int>(std::ceil(crossing)));
      else
        span_end = std::min(span_end, min_x + static_cast<int>(std::floor(crossing)));
    };
    clip_edge(row_w0, d0_dx);
    clip_edge(row_w1, d1_dx);
    clip_edge(row_w2, d2_dx);
    if (empty || span_start > span_end)
      continue;

    u32* const scanline = m_pixels.data() + static_cast<size_t>(y) * m_width;
    u32* const depth_scanline =
        depth_active ? m_depth.data() + static_cast<size_t>(y) * m_width : nullptr;

    if (flat_fill)
    {
      std::fill(scanline + span_start, scanline + span_end + 1, flat_texel);
      m_touched.store(true, std::memory_order_relaxed);
      continue;
    }

    float w0 = row_w0 + d0_dx * static_cast<float>(span_start - min_x);
    float w1 = row_w1 + d1_dx * static_cast<float>(span_start - min_x);
    for (int x = span_start; x <= span_end; ++x, w0 += d0_dx, w1 += d1_dx)
    {
      const float w2 = 1.0f - w0 - w1;
      // The span is already the covered range; this only catches the boundary
      // pixel that rounding put a hair outside.
      if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
        continue;

      // Orthographic draws have w = 1, so affine interpolation of the attributes
      // is exact - no perspective correction is being skipped here, there is
      // none to do.
      u32 source = 0xFFFFFFFFu;
      if (textured)
      {
        source = Sample(call.texture, w0 * a.u + w1 * b.u + w2 * c.u,
                        w0 * a.v + w1 * b.v + w2 * c.v);
      }

      u32 mr = flat_r, mg = flat_g, mb = flat_b, ma = flat_a;
      if (!flat_color)
      {
        const auto lerp = [&](int i) {
          return static_cast<u32>(
              std::clamp(w0 * a.color[i] + w1 * b.color[i] + w2 * c.color[i], 0.0f, 1.0f) * 255.0f +
              0.5f);
        };
        mr = lerp(0);
        mg = lerp(1);
        mb = lerp(2);
        ma = lerp(3);
      }

      if (call.tev_alpha_known)
      {
        // GX's own answer for this draw's alpha, bilinear in (texture alpha,
        // rasterized alpha). It REPLACES the product of the two rather than
        // scaling it, because the chain already consumed both - modulating by
        // the vertex alpha afterwards would apply it twice.
        const float t = static_cast<float>(source >> 24) / 255.0f;
        const float r = static_cast<float>(ma) / 255.0f;
        const float low = call.tev_alpha_corners[0] +
                          (call.tev_alpha_corners[1] - call.tev_alpha_corners[0]) * t;
        const float high = call.tev_alpha_corners[2] +
                           (call.tev_alpha_corners[3] - call.tev_alpha_corners[2]) * t;
        const float resolved = std::clamp(low + (high - low) * r, 0.0f, 1.0f);
        source = (source & 0x00FFFFFFu) | (static_cast<u32>(resolved * 255.0f + 0.5f) << 24);
        // Colour still modulates; alpha is already final.
        if (!white)
        {
          source = MulDiv255(source & 0xFF, mr) | (MulDiv255((source >> 8) & 0xFF, mg) << 8) |
                   (MulDiv255((source >> 16) & 0xFF, mb) << 16) | (source & 0xFF000000u);
        }
      }
      else if (!white)
      {
        source = MulDiv255(source & 0xFF, mr) | (MulDiv255((source >> 8) & 0xFF, mg) << 8) |
                 (MulDiv255((source >> 16) & 0xFF, mb) << 16) |
                 (MulDiv255((source >> 24) & 0xFF, ma) << 24);
      }

      const u32 source_alpha = source >> 24;
      if (!AlphaPasses(call.alpha_compare, alpha_reference, call.alpha_compare1, alpha_reference1,
                       call.alpha_logic, source_alpha))
        continue;

      // Late z, the hardware order: the alpha test has already killed the
      // pixel or not, and only a surviving pixel compares and writes depth
      // (Software/Tev.cpp does exactly this). The write happens even when the
      // blend then contributes nothing visible - that too is what the console
      // does with an alpha-passing, fully transparent pixel.
      if (depth_active)
      {
        const float zf = w0 * a.z + w1 * b.z + w2 * c.z;
        const u32 z = static_cast<u32>(std::clamp(zf, 0.0f, 16777215.0f));
        u32& stored = depth_scanline[x];
        if (!DepthPasses(call.depth_func, z, stored))
          continue;
        if (call.depth_write)
          stored = z;
      }

      u32& target = scanline[x];
      switch (call.blend)
      {
      case BlendMode::Opaque:
        // Keep the source alpha rather than stamping 255. On console "opaque"
        // writes into the framebuffer that IS the scene, so alpha is free to be
        // meaningless; here the result is COMPOSITED over the traced frame, so
        // alpha is coverage. Forcing it opaque printed every transparent texel
        // of a UI texture as whatever garbage RGB sat in it - the speckled boxes
        // behind Wind Waker's HUD icons were exactly this.
        target = source;
        break;
      case BlendMode::Additive:
      {
        // Approximate, and unavoidably so: real additive blending needs the
        // framebuffer underneath, and the overlay is composited over a frame
        // this code never sees. Accumulating colour and coverage is the closest
        // a single layer gets, and it reads correctly for the glows and flashes
        // that actually use it.
        const u32 dest = target;
        u32 result = 0;
        for (int shift = 0; shift < 24; shift += 8)
        {
          const u32 sum = ((dest >> shift) & 0xFF) +
                          MulDiv255((source >> shift) & 0xFF, source_alpha);
          result |= std::min(sum, 255u) << shift;
        }
        target = result | (std::min((dest >> 24) + source_alpha, 255u) << 24);
        break;
      }
      case BlendMode::Over:
      default:
      {
        // Fully transparent contributes nothing, and UI textures are mostly
        // transparent - this is the single most valuable early-out here.
        if (source_alpha == 0)
          continue;
        // Fully opaque needs no read of what is underneath.
        if (source_alpha == 255)
        {
          target = source;
          break;
        }
        // Straight-alpha source-over. The alpha channel accumulates real
        // coverage, which is what the runtime composites the overlay with.
        const u32 dest = target;
        const u32 inv = 255 - source_alpha;
        u32 result = 0;
        for (int shift = 0; shift < 24; shift += 8)
        {
          result |= (MulDiv255((source >> shift) & 0xFF, source_alpha) +
                     MulDiv255((dest >> shift) & 0xFF, inv))
                    << shift;
        }
        target = result | ((source_alpha + MulDiv255(dest >> 24, inv)) << 24);
        break;
      }
      }
      m_touched.store(true, std::memory_order_relaxed);
    }
  }
}

void UiRasterizer::Draw(const DrawCall& call, const std::vector<Vertex>& vertices,
                        const std::vector<u32>& indices)
{
  if (m_width == 0 || m_height == 0)
    return;
  if (!(call.viewport_width > 0.0f) || !(call.viewport_height > 0.0f))
    return;
  if (indices.size() < 3 || vertices.empty())
    return;

  if (m_draw_count == m_draws.size())
    m_draws.emplace_back();
  RecordedDraw& draw = m_draws[m_draw_count++];
  draw.call = call;
  if (call.depth_test)
    m_depth_used = true;
  // assign onto the existing storage: these vectors are recycled frame to frame
  // precisely so that recording a HUD does not allocate a hundred times.
  draw.vertices.assign(vertices.begin(), vertices.end());
  draw.indices.assign(indices.begin(), indices.end());

  float lowest = std::numeric_limits<float>::max();
  float highest = std::numeric_limits<float>::lowest();
  for (const Vertex& vertex : vertices)
  {
    const float screen_y = call.viewport_y + (0.5f - vertex.y * 0.5f) * call.viewport_height;
    lowest = std::min(lowest, screen_y);
    highest = std::max(highest, screen_y);
  }
  draw.min_y = static_cast<int>(std::floor(lowest));
  draw.max_y = static_cast<int>(std::ceil(highest));
}
}  // namespace Remix
