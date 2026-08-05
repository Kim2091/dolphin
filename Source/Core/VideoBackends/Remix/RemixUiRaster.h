// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"

namespace Remix
{
// A small software rasterizer, used for one thing: turning the game's
// orthographic draws - HUD, menus, every 2D screen - into a finished RGBA image
// that remixapi_DrawScreenOverlay composites over the path-traced frame.
//
// Why software, and why here. This backend has no rasterizer at all; it decodes
// geometry and hands it to a path tracer. UI cannot go through that path and
// come out looking like UI: submitting it as world geometry (which is what
// REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI does) makes it a lit, denoised,
// camera-welded object that swims as the view moves. The runtime's overlay entry
// point wants finished pixels instead, and nothing else in the pipeline can
// produce them - so they get produced here.
//
// The inputs this needs are both already in CPU memory, which is what makes it
// cheap: VertexManagerBase hands out decoded vertices in plain memory, and
// bSupportsGPUTextureDecoding = false means TextureCacheBase has already decoded
// every GC texture format to RGBA8 before Load() (see RemixTexture::GetPixels).
//
// Deliberately not a general rasterizer. Orthographic draws have w = 1, so there
// is no perspective divide and affine interpolation is exact; UI is drawn in
// submission order, so there is no depth buffer. Both assumptions are checked
// where they are relied on rather than merely assumed.
class UiRasterizer
{
public:
  // How a source pixel combines with what is already in the overlay. Translated
  // from the draw's GX blend state by the caller, which already has it in
  // Vulkan's numbering.
  enum class BlendMode
  {
    // Blending off: the draw replaces, and is fully opaque.
    Opaque,
    // The overwhelmingly common UI case, SrcAlpha / InvSrcAlpha.
    Over,
    // One / One and friends. Cannot be expressed exactly in a single overlay
    // layer - the real operation needs the framebuffer underneath, which we do
    // not have - so it accumulates colour and coverage and is approximate by
    // construction.
    Additive,
  };

  struct Texture
  {
    // Decoded RGBA8, row-major, `width * height * 4` bytes. Null for an
    // untextured draw, which then takes its colour from the vertices alone.
    const u8* pixels = nullptr;
    u32 width = 0;
    u32 height = 0;
    bool bilinear = true;
    bool clamp_u = false;
    bool clamp_v = false;
  };

  struct Vertex
  {
    // Normalized device coordinates: x and y in [-1, 1], y pointing UP, which is
    // GX's own convention (Dolphin's vertex shader negates it on the way out,
    // for APIs whose y points down - VertexShaderGen.cpp).
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    // Straight (non-premultiplied) RGBA, 0-1.
    std::array<float, 4> color = {1.0f, 1.0f, 1.0f, 1.0f};
  };

  struct DrawCall
  {
    Texture texture;
    BlendMode blend = BlendMode::Over;
    // GX alpha test - BOTH comparators and the logic op joining them, in the
    // CompareMode numbering the rest of the backend already uses
    // (Never = 0 .. Always = 7), against 0-1 references. Collapsing this to one
    // comparator surrenders every Or/Xor/Xnor configuration to "always pass".
    u8 alpha_compare = 7;
    u8 alpha_compare1 = 7;
    // 0 = And, 1 = Or, 2 = Xor, 3 = Xnor (BPMemory.h AlphaTest::Op).
    u8 alpha_logic = 0;
    float alpha_reference = 0.0f;
    float alpha_reference1 = 0.0f;

    // Final alpha as GX's TEV chain resolves it, at the four corners of
    // (texture alpha, rasterized alpha): (0,0) (1,0) (0,1) (1,1). Bilinearly
    // interpolated, this REPLACES the texture-alpha-times-vertex-alpha guess -
    // including the vertex-alpha modulation, which the chain already accounts
    // for. False means it could not be resolved and the old behaviour stands.
    bool tev_alpha_known = false;
    std::array<float, 4> tev_alpha_corners = {1.0f, 1.0f, 1.0f, 1.0f};
    // Where on screen NDC [-1,1] lands, in pixels. Normally the whole surface,
    // but a game is free to draw its UI through a sub-viewport.
    float viewport_x = 0.0f;
    float viewport_y = 0.0f;
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;
    // The draw's scissor rect in overlay pixels, inclusive on all four sides.
    // GX games clip UI with the scissor constantly - sliding panels, wipes, text
    // windows, banks of elements all drawn but scissored down to the one showing
    // - so ignoring it makes every element appear at once.
    int clip_left = 0;
    int clip_top = 0;
    int clip_right = 0;
    int clip_bottom = 0;

    // How many world draws the frame had already submitted when this draw was
    // recorded. Zero means the draw arrived before any 3D geometry, which on
    // console means the scene was drawn over it - and here means it would be
    // composited over the scene instead. Stamped rather than decided at record
    // time because the verdict ("does this frame have world content at all?")
    // is not known until the frame is over: a frame that turns out to be all-2D
    // is a menu, where these draws ARE the content.
    u32 world_draws_at_submit = 0;
    // Set when the user explicitly tagged this draw's texture "UI Texture".
    // Exempts it from the pre-world filter below, for the same reason it is
    // exempt from every drop rule at submit time: the tag is the rescue lever.
    bool tag_protected = false;
  };

  UiRasterizer();
  ~UiRasterizer();
  UiRasterizer(const UiRasterizer&) = delete;
  UiRasterizer& operator=(const UiRasterizer&) = delete;

  // Resizes and clears to fully transparent. Everything recorded afterwards
  // accumulates until the next Begin.
  void Begin(u32 width, u32 height);

  // Records a triangle list. `indices` are into `vertices`; a count that is not
  // a multiple of three is truncated rather than refused.
  //
  // Recorded, not drawn: the draws are replayed by Flush across several threads,
  // each owning a horizontal band of the output. Rasterizing them here instead
  // costs 9-19 ms a frame on a busy screen, which is the entire frame budget.
  void Draw(const DrawCall& call, const std::vector<Vertex>& vertices,
            const std::vector<u32>& indices);

  // Skip, at replay time, every recorded draw that arrived before the frame's
  // first world draw and was not tagged "UI Texture". Set just before Flush, by
  // which point the frame's world-draw count is final - which is the point of
  // deciding here rather than at record time.
  //
  // A filter rather than a compaction: the recorded draws keep their slots and
  // their reused vertex/index storage, so enabling this costs one comparison per
  // draw per band and allocates nothing. If it filters everything, no pixel is
  // ever touched and HasContent() stays false, which correctly makes the frame
  // send no overlay at all instead of a transparent one.
  //
  // Reset to off by Begin, so it is per-frame state like everything else here.
  void SetPreWorldFilter(bool enabled) { m_pre_world_filter = enabled; }

  // Replays everything recorded since Begin. Must be called before reading the
  // buffer; safe to call with nothing recorded.
  void Flush();

  // True once any pixel has been touched. A frame that drew no UI must send no
  // overlay at all rather than a transparent one, so that the runtime can drop
  // its pending overlay instead of compositing a full-screen no-op every frame.
  bool HasContent() const { return m_touched.load(std::memory_order_relaxed); }

  u32 Width() const { return m_width; }
  u32 Height() const { return m_height; }
  // Straight-alpha RGBA8, packed 0xAABBGGRR so the bytes read R,G,B,A in memory
  // on a little-endian host - i.e. REMIXAPI_FORMAT_R8G8B8A8_UNORM.
  const std::vector<u32>& Buffer() const { return m_pixels; }

private:
  // One recorded draw. The vertex and index storage is reused across frames -
  // a HUD records well over a hundred of these every frame and none of them
  // should allocate.
  struct RecordedDraw
  {
    DrawCall call;
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    // Screen-space row range this draw can possibly touch, computed once when it
    // is recorded. Every band replays every draw, so without this a HUD element
    // covering twenty rows still pays triangle setup in all sixteen bands.
    int min_y = 0;
    int max_y = 0;
  };

  // Rasterizes `count` recorded draws, in order, clipped to scanlines
  // [min_y, max_y]. Threads own disjoint bands, so they write disjoint rows of
  // m_pixels and need no synchronisation between them; ordering within a band is
  // preserved, which is what keeps the painter's-algorithm result identical to
  // the single-threaded one.
  void RasterizeBand(int min_y, int max_y);
  void DrawTriangle(const DrawCall& call, const Vertex& a, const Vertex& b, const Vertex& c,
                    int clip_min_y, int clip_max_y);
  // Packed 0xAABBGGRR, the same layout as the overlay buffer, so that the hot
  // paths never unpack a colour.
  u32 Sample(const Texture& texture, float u, float v) const;
  void WorkerLoop();

  std::vector<u32> m_pixels;
  u32 m_width = 0;
  u32 m_height = 0;
  std::atomic<bool> m_touched{false};

  std::vector<RecordedDraw> m_draws;
  size_t m_draw_count = 0;
  // See SetPreWorldFilter. Read by every band worker during a Flush and written
  // only between flushes, so it needs no synchronisation of its own.
  bool m_pre_world_filter = false;

  // Worker pool. Started on the first Flush that has enough work to be worth it,
  // and joined in the destructor.
  std::vector<std::thread> m_workers;
  std::mutex m_mutex;
  std::condition_variable m_work_ready;
  std::condition_variable m_band_done;
  u64 m_generation = 0;
  u32 m_next_band = 0;
  u32 m_bands_total = 0;
  u32 m_bands_finished = 0;
  u32 m_band_height = 0;
  bool m_shutdown = false;
};
}  // namespace Remix
