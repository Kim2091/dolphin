// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Remix/RemixApi.h"

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <exception>
#include <iterator>
#include <chrono>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <xxhash.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/WindowSystemInfo.h"

#include "Core/Config/RemixSettings.h"
// For the running game's ID and the per-game folder layout it selects.
#include "Core/ConfigManager.h"
#include "Core/RemixPaths.h"

#include "VideoBackends/Remix/RemixTexture.h"
// For ResetPendingAlphaMask: the EFB alpha mask a priming pass leaves pending is
// file-scope state in the vertex manager's translation unit, and Shutdown is
// what has to drop it.
#include "VideoBackends/Remix/RemixVertexManager.h"
// For RemixEFBInterface::DrainAccessCounters and the shared store's colour
// packer, both of which live with the EFB interface itself.
#include "VideoBackends/Remix/RemixGfx.h"

// The Software backend's EFB store, which the UI fold blends into. Linked in
// rather than duplicated; see CMakeLists.txt.
#include "VideoBackends/Software/SWEfbInterface.h"

#include "Common/FileUtil.h"

// For EFB_WIDTH / EFB_HEIGHT: the UI viewport arrives in EFB space and the
// screen overlay is rasterized at the swapchain's, so the two need relating.
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/XFMemory.h"

namespace Remix
{
std::unique_ptr<RemixApi> g_remix_api;

namespace
{
// Number of frames a mesh handle may go undrawn before it is destroyed. Batch
// composition legitimately changes when a game's own culling changes what it
// submits, so stale handles are reaped rather than treated as an error: churn
// costs VRAM temporarily, never correctness.
constexpr u64 MESH_IDLE_FRAMES_BEFORE_DESTROY = 300;

// Seed for a promoted (regenerated) mesh's synthetic m_meshes key, derived from
// its topology key. Its full mesh hash churns every frame, so the lineage's one
// MeshEntry needs a key that does not; deriving it deterministically also lets
// a re-promotion after an LOD-switch demotion find and destroy its own zombie.
// Distinct from every seed the identity hashes use.
constexpr u64 DYNAMIC_MESH_KEY_SEED = 0x44796e4d65736821ull;  // "DynMesh!"

// Fixed hashes for the handles we own that are not derived from game content.
constexpr u64 FALLBACK_MATERIAL_HASH = 0x8B1D0F17'52D9A3C1ULL;
constexpr u64 FALLBACK_MESH_HASH = 0x8B1D0F17'52D9A3C2ULL;
constexpr u64 FALLBACK_LIGHT_HASH = 0x8B1D0F17'52D9A3C3ULL;
constexpr u64 XF_LIGHT_HASH_BASE = 0x8B1D0F17'52D9A400ULL;

// ---------------------------------------------------------------------------
// 3x4 affine helpers, used by the camera recovery path.
// ---------------------------------------------------------------------------
constexpr Affine IDENTITY_AFFINE = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                    0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

// How far apart two transforms may be and still count as the same one. Rotation
// and translation get separate budgets because they live on different scales: a
// rotation is unit-magnitude by construction and takes an absolute threshold,
// while a translation is in game world units that vary by orders of magnitude
// between titles and takes a relative one.
//
// Keeping them in one combined norm is what made the stable-W diagnostic read
// 100% on a frame where a quarter of the objects had visibly moved - an
// object's distance from the origin inflated the tolerance for its own motion.
constexpr float VIEW_ROTATION_EPSILON = 2e-3f;
constexpr float VIEW_TRANSLATION_EPSILON = 5e-3f;

// The world frame is defined by frame 0's camera: m_view starts as the identity,
// so world space IS that first camera's space and these are its basis vectors by
// construction. Reporting the recovered camera against them turns slow
// integrated drift - many individually plausible per-frame deltas, each too
// small to trip the spike log, accumulating into a large ABSOLUTE rotation -
// into a number that can be read straight off the log. That failure produces
// the same "sky swings while geometry holds still" symptom as a per-frame spike
// and is otherwise invisible.
constexpr float VIEW_REFERENCE_FORWARD[3] = {0.0f, 0.0f, -1.0f};
constexpr float VIEW_REFERENCE_UP[3] = {0.0f, 1.0f, 0.0f};

// A single frame's accepted camera rotation above this gets its own log line.
// A real camera turns a fraction of a degree per frame; half a degree at 60 Hz
// is a 30 deg/s slew, which the title screen of a game has no business doing.
constexpr float VIEW_SPIKE_ROTATION_DEGREES = 0.5f;
constexpr int VIEW_SPIKE_LOG_CAP = 96;
int s_view_spike_log_count = 0;

// How close the runner-up cluster has to be, as a percentage of the winner's
// inlier count, before the vote is treated as too close to call on size alone.
constexpr u32 VIEW_TIE_BREAK_PERCENT = 75;

// --- Sky auto-detection tolerances --------------------------------------
// How straight the sky ratio's rotation part has to be. Same class as
// VIEW_ROTATION_EPSILON, a little looser because the ratio is built from an
// ESTIMATED view and inherits its error.
constexpr float SKY_ROTATION_EPSILON = 5e-3f;
// How closely the ratio's translation has to match the camera's own position
// delta, relative to the size of that delta.
constexpr float SKY_TRANSLATION_RELATIVE_EPSILON = 0.1f;
// How far the camera must travel in a frame for that frame to say anything. Below
// this the sky and the static world are genuinely indistinguishable by this test
// - a parked or rotation-only camera - and the frame is simply skipped.
constexpr float SKY_INFORMATIVE_FRACTION = 1e-4f;
// Ceiling on how many meshes may be tracked as candidates at once, so a game
// churning geometry cannot grow the map without bound.
constexpr size_t MAX_SKY_CANDIDATES = 4096;
constexpr int SKY_CLASSIFY_LOG_CAP = 32;
int s_sky_classify_log_count = 0;

// Rotation angle of an affine's 3x3, in degrees: trace = 1 + 2cos(theta).
float AffineRotationDegrees(const Affine& m)
{
  const float trace = m[0] + m[5] + m[10];
  return std::acos(std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f)) * (180.0f / 3.14159265358979323846f);
}

// Length of an affine's translation column. On a frame-to-frame delta this is a
// distance moved rather than a position, which is what makes it comparable
// between candidates.
float AffineTranslationLength(const Affine& m)
{
  return std::sqrt(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
}

// Which of two candidate deltas is closer to "the camera did not move". The two
// terms live on different scales - a rotation is unit-magnitude by construction,
// a translation is in game world units - so they are compared in order rather
// than summed: rotation first, because rotation is what visibly swings the sky,
// and translation only to separate rotations that are effectively equal.
bool DeltaIsCalmer(const Affine& a, const Affine& b)
{
  const float rotation_a = AffineRotationDegrees(a);
  const float rotation_b = AffineRotationDegrees(b);
  if (std::abs(rotation_a - rotation_b) > 0.01f)
    return rotation_a < rotation_b;
  return AffineTranslationLength(a) < AffineTranslationLength(b);
}

// Angle between two unit-ish vectors, in degrees.
float AngleBetweenDegrees(const float a[3], const float b[3])
{
  const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  return std::acos(std::clamp(dot, -1.0f, 1.0f)) * (180.0f / 3.14159265358979323846f);
}

// out = a * b, with the implicit [0 0 0 1] fourth row on both. b's translation
// column participates through a's 3x3, then a's own translation is added.
Affine AffineMultiply(const Affine& a, const Affine& b)
{
  Affine out = {};
  for (int i = 0; i < 3; ++i)
  {
    for (int j = 0; j < 4; ++j)
    {
      out[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                       a[i * 4 + 2] * b[2 * 4 + j] + (j == 3 ? a[i * 4 + 3] : 0.0f);
    }
  }
  return out;
}

// [R|t]^-1 = [R^-1 | -R^-1 t]. A real 3x3 inverse rather than a transpose,
// because a GX modelview legitimately carries scale - only the accumulated view
// matrix is guaranteed rigid, and only because we re-orthonormalise it.
bool AffineInvert(const Affine& m, Affine& out)
{
  const float a = m[0], b = m[1], c = m[2];
  const float d = m[4], e = m[5], f = m[6];
  const float g = m[8], h = m[9], i = m[10];

  const float c00 = e * i - f * h;
  const float c01 = -(d * i - f * g);
  const float c02 = d * h - e * g;
  const float det = a * c00 + b * c01 + c * c02;
  if (!std::isfinite(det) || std::abs(det) < 1e-20f)
    return false;
  const float s = 1.0f / det;

  const float r00 = c00 * s, r01 = -(b * i - c * h) * s, r02 = (b * f - c * e) * s;
  const float r10 = c01 * s, r11 = (a * i - c * g) * s, r12 = -(a * f - c * d) * s;
  const float r20 = c02 * s, r21 = -(a * h - b * g) * s, r22 = (a * e - b * d) * s;

  const float tx = m[3], ty = m[7], tz = m[11];
  out = {r00, r01, r02, -(r00 * tx + r01 * ty + r02 * tz),
         r10, r11, r12, -(r10 * tx + r11 * ty + r12 * tz),
         r20, r21, r22, -(r20 * tx + r21 * ty + r22 * tz)};
  return std::isfinite(out[3]) && std::isfinite(out[7]) && std::isfinite(out[11]);
}

void Normalize3(float* v)
{
  const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (length > 1e-20f)
  {
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
  }
}

// Gram-Schmidt the rotation part back to orthonormal. Integrating a delta every
// frame accumulates error, and the camera extraction in SetupCamera assumes
// R^-1 == R^T - so left alone the basis would slowly shear and the recovered
// camera would stop matching the geometry it is meant to frame.
void AffineOrthonormalize(Affine& m, bool preserve_handedness)
{
  const float z0[3] = {m[8], m[9], m[10]};
  float x[3] = {m[0], m[1], m[2]};
  float y[3] = {m[4], m[5], m[6]};
  Normalize3(x);
  const float xy = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
  for (int k = 0; k < 3; ++k)
    y[k] -= xy * x[k];
  Normalize3(y);
  float z[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2],
                x[0] * y[1] - x[1] * y[0]};
  // A cross product is right-handed BY CONSTRUCTION, so rebuilding row 2 from
  // one silently converts a MIRRORED basis into a right-handed one and throws
  // the reflection away. Some games hand us exactly that: The Force Unleashed's
  // modelview measures det -1 on every frame.
  //
  // Geometry hides the damage, because the image is invariant to V - world and
  // camera move together and the picture is unchanged. LIGHTS DO NOT, because a
  // light's world direction is computed from XF state through V^-1 and never
  // touches MV, so it does not participate in that cancellation. A reflected V
  // turns a rotation of +theta into -theta, a 2*theta error against the truth,
  // which is why TFU's lights measured world/camera 1.79-1.90 where an anchored
  // light reads 0, and why its shadows swing while its geometry looks right.
  // Preserving the sign drops the median light drift from 89/123 deg to 5.0/0.8.
  //
  // No-op for a right-handed input: the cross product already agrees with row 2.
  if (preserve_handedness && z[0] * z0[0] + z[1] * z0[1] + z[2] * z0[2] < 0.0f)
  {
    for (int k = 0; k < 3; ++k)
      z[k] = -z[k];
  }
  m[0] = x[0];  m[1] = x[1];  m[2] = x[2];
  m[4] = y[0];  m[5] = y[1];  m[6] = y[2];
  m[8] = z[0];  m[9] = z[1];  m[10] = z[2];
}

// A camera delta is rigid. A candidate that is not is a moving or scaling
// object that happened to win the vote, and folding it into the accumulated
// view would corrupt the basis permanently rather than for one frame.
bool AffineIsRigid(const Affine& m)
{
  for (int i = 0; i < 3; ++i)
  {
    const float* row = &m[i * 4];
    const float length_sq = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
    if (!std::isfinite(length_sq) || std::abs(length_sq - 1.0f) > 0.02f)
      return false;
  }
  for (int i = 0; i < 3; ++i)
  {
    const float* a = &m[i * 4];
    const float* b = &m[((i + 1) % 3) * 4];
    if (std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) > 0.02f)
      return false;
  }
  return true;
}

// Determinant of an affine's 3x3. A view matrix is rigid, so a candidate whose
// determinant is anything but +/-1 carries a model scale and cannot be one.
float AffineDeterminant(const Affine& m)
{
  return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
         m[2] * (m[4] * m[9] - m[5] * m[8]);
}

// Which of two histogram buckets is the better claim to "the frame's shared
// space". Distinct meshes decide it: a matrix used four hundred times by one
// mesh is an instanced prop, while one used by four hundred DIFFERENT meshes is
// a space they were all authored in.
bool BucketOutranks(const ModelviewBucket& a, const ModelviewBucket& b)
{
  if (a.meshes.size() != b.meshes.size())
    return a.meshes.size() > b.meshes.size();
  if (a.draws != b.draws)
    return a.draws > b.draws;
  return a.vertices > b.vertices;
}

// How often the histogram prints. Tighter than the 60-frame stats cadence
// because the window in which a game is actually rendering 3D can be short -
// Wind Waker's title screen is over in a few hundred frames.
// Deliberately odd (as are the other report intervals): an even interval
// samples only one parity of frame, and a game that alternates its content
// between even and odd frames (Skylanders submits the world pass and the 2D
// pass on alternating frames) is then invisible to every periodic report at
// once.
constexpr u64 MODELVIEW_LOG_INTERVAL = 31;

// A frame is "minor" (a 2D-only present between full world frames) when the
// recent peak proves a real scene exists here and this frame holds under
// 1/MINOR_FRAME_RATIO of it. The floor keeps all-2D stretches presenting
// normally - in a menu the few dozen quads ARE the scene.
//
// The ratio is "under half", not something stricter, because the 2D pass is
// not always small: Skylanders' HUD pass is 34 instances against a ~900-
// instance world, but its Wiimote tutorial popup is 98 instances (panels plus
// a LIT 3D remote model - the pass even carries the scene's 3 XF lights)
// against a 715-instance world, which a 1/8 ratio missed by a hair and the
// strobing returned for exactly those popups. Nothing lights-based can
// discriminate that pass; relative size is the signal that survives. The cost
// of the loose ratio is bounded by the ring: a genuine world frame under half
// its own recent peak (a hard cutscene cut into a sparse shot) is dropped for
// at most the ring length (~4 frames) before the peak adapts.
constexpr u32 MINOR_FRAME_FLOOR = 200;
constexpr u32 MINOR_FRAME_RATIO = 2;

// Do two transforms agree? Rotation and translation are tested separately - see
// the epsilon comments above for why one combined norm silently trades one
// against the other.
bool AffineSimilar(const Affine& a, const Affine& b)
{
  float rotation_diff_sq = 0.0f;
  for (int i = 0; i < 3; ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      const float d = a[i * 4 + j] - b[i * 4 + j];
      rotation_diff_sq += d * d;
    }
  }
  if (!(std::sqrt(rotation_diff_sq) <= VIEW_ROTATION_EPSILON))
    return false;

  float translation_diff_sq = 0.0f;
  float translation_scale_sq = 0.0f;
  for (int i = 0; i < 3; ++i)
  {
    const float d = a[i * 4 + 3] - b[i * 4 + 3];
    translation_diff_sq += d * d;
    translation_scale_sq +=
        std::max(a[i * 4 + 3] * a[i * 4 + 3], b[i * 4 + 3] * b[i * 4 + 3]);
  }
  return std::sqrt(translation_diff_sq) <=
         VIEW_TRANSLATION_EPSILON * (1.0f + std::sqrt(translation_scale_sq));
}

constexpr float PI_F = 3.14159265358979323846f;
constexpr float RAD_TO_DEG = 180.0f / PI_F;
constexpr float DEG_TO_RAD = PI_F / 180.0f;

// A GX spot cone is not an angle - it is a quadratic in cos(theta),
//   cosAtt = cosatt[0] + cosatt[1]*a + cosatt[2]*a*a,   a = dot(toLight, spotDir)
// clamped at zero (TransformUnit.cpp CalculateLightAttn, Spot case). The OUTER
// cone edge is wherever that first reaches zero coming in from the axis; the
// INNER edge - where the falloff starts rather than finishes - is where it first
// drops below most of its on-axis value. Remix wants both, as an outer angle
// plus coneSoftness = cos(inner) - cos(outer), which is exactly the shape the
// runtime's own D3D9 spot conversion builds (rtx_lights_data.cpp:417-420).
//
// Walked rather than solved: this runs at most eight times a frame, the
// polynomial is arbitrary (games do set degenerate ones), and a root-finder
// here would need more edge-case handling than the whole search costs.
constexpr float SPOT_INNER_FRACTION = 0.9f;

void SolveSpotCone(const float cosatt[3], float& outer_degrees, float& inner_degrees)
{
  constexpr int STEPS = 180;
  const auto evaluate = [&](float a) { return cosatt[0] + cosatt[1] * a + cosatt[2] * a * a; };

  outer_degrees = 180.0f;
  inner_degrees = 180.0f;

  // Not lit on-axis, or nothing sensible to measure - treat as unshaped.
  const float on_axis = evaluate(1.0f);
  if (!(on_axis > 0.0f))
    return;

  const float inner_threshold = SPOT_INNER_FRACTION * on_axis;
  bool inner_found = false;
  for (int step = 1; step <= STEPS; ++step)
  {
    const float a = 1.0f - 2.0f * (static_cast<float>(step) / static_cast<float>(STEPS));
    const float value = evaluate(a);
    const float degrees = std::acos(std::clamp(a, -1.0f, 1.0f)) * RAD_TO_DEG;
    if (!inner_found && value < inner_threshold)
    {
      inner_degrees = degrees;
      inner_found = true;
    }
    if (!(value > 0.0f))
    {
      outer_degrees = degrees;
      break;
    }
  }
  // A polynomial that is flat out to its own edge has no transition region, and
  // cos(inner) - cos(outer) must not come out negative.
  if (!inner_found || inner_degrees > outer_degrees)
    inner_degrees = outer_degrees;
}

// ---------------------------------------------------------------------------
// Radiance calibration, ported from the runtime's own legacy-light conversion
// (LightUtils::calculateIntensity, rtx_light_utils.cpp:122-182).
//
// GX's distance attenuation is 1 / (distatt[0] + distatt[1]*d + distatt[2]*d^2)
// - term for term the polynomial D3D9 spells Attenuation0/1/2 - so the same
// conversion applies, and it is the one already look-validated across Remix's
// whole D3D9 catalogue. Handing the raw 0-1 colour straight over as radiance,
// which is what this replaces, is roughly two orders of magnitude too dim.
// ---------------------------------------------------------------------------

// The brightness the original light has to fall to in order to have "ended".
// Deliberately in the game's own gamma space (rtx_lights.h:41).
constexpr float LEGACY_LIGHT_END_VALUE = 1.0f / 255.0f;
// The radiance the converted light will have at that same distance
// (rtx_lights.h:46).
constexpr float NEW_LIGHT_END_VALUE = 0.01f;
// The runtime's fixed sphere radius for converted point/spot lights
// (rtx_light_manager.h:243, lightConversionSphereLightFixedRadius). The formula
// below is derived against it, so the two have to agree.
constexpr float SPHERE_LIGHT_RADIUS = 4.0f;
// Ceiling on the result, standing in for the runtime's lightConversionMaxIntensity.
// A degenerate attenuation polynomial can otherwise solve to an absurd distance
// and hand the tracer a light bright enough to swamp the frame.
constexpr float MAX_LIGHT_RADIANCE = 100000.0f;

// Distance at which `brightness` divided by the attenuation polynomial falls to
// LEGACY_LIGHT_END_VALUE. `fallback_range` stands in for D3D9's Light.Range and
// is what a polynomial with no falloff at all resolves to.
float SolveLightEndDistance(const float distatt[3], float brightness, float fallback_range)
{
  constexpr float EPSILON = 0.000001f;
  const float a = distatt[2], b = distatt[1], c = distatt[0];

  if (c > 0.0f && brightness / c < LEGACY_LIGHT_END_VALUE)
  {
    // Already below the threshold right next to the light.
    return 0.0f;
  }
  if (a < EPSILON)
  {
    if (b > EPSILON)
    {
      // Linear falloff: 1/(b*d + c) = LEGACY_LIGHT_END_VALUE.
      return std::max(0.0f, ((brightness / LEGACY_LIGHT_END_VALUE) - c) / b);
    }
    // No falloff - GX_DA_OFF leaves distatt = (1,0,0), and the light is at full
    // power until something else stops it. Nothing in GX says where that is.
    return fallback_range;
  }

  // Quadratic: a*d^2 + b*d + (c - brightness/END) = 0, smaller positive root.
  const float new_c = c - brightness / LEGACY_LIGHT_END_VALUE;
  const float discriminant = b * b - 4.0f * a * new_c;
  if (!(discriminant >= 0.0f))
    return fallback_range;  // never reaches the threshold
  const float root = std::sqrt(discriminant);
  const float root1 = (-b + root) / (2.0f * a);
  const float root2 = (-b - root) / (2.0f * a);
  float end_distance = 0.0f;
  if (root1 > 0.0f)
    end_distance = root1;
  if (root2 > 0.0f)
    end_distance = root2;
  return end_distance;
}

// Radiance a sphere light of `radius` needs so that it reaches
// NEW_LIGHT_END_VALUE at the distance the original light ended. Derivation is in
// rtx_light_utils.cpp:167-179; the result is (d^2 * t) / (pi * r^2).
float LightEndDistanceToRadiance(float end_distance, float radius)
{
  const float distance_sq_to_radiance = NEW_LIGHT_END_VALUE / (PI_F * radius * radius);
  const float radiance = distance_sq_to_radiance * end_distance * end_distance;
  if (!std::isfinite(radiance))
    return 0.0f;
  return std::min(radiance, MAX_LIGHT_RADIANCE);
}

// remixapi_Transform::matrix and Affine are the same 3x4 row-major floats.
Affine FromRemixTransform(const remixapi_Transform& transform)
{
  Affine out = {};
  std::memcpy(out.data(), &transform.matrix[0][0], sizeof(float) * 12);
  return out;
}

remixapi_Transform ToRemixTransform(const Affine& affine)
{
  remixapi_Transform out = {};
  std::memcpy(&out.matrix[0][0], affine.data(), sizeof(float) * 12);
  return out;
}

// ---------------------------------------------------------------------------
// Guarded remixapi calls.
//
// dxvk-remix is built /MD and throws std::out_of_range out of internal resource
// lookups when a stale handle reaches the API; it can also fault outright. An
// exception crossing back into Dolphin from a runtime call would take down the
// emulator, and an unwind through the runtime's own locks leaves it wedged. So
// every remixapi call on the frame path goes through here.
//
// The inner/outer split is mandatory, not style: MSVC rejects __try in the same
// function as C++ EH (C2713) and in any function that owns unwindable objects
// (C2712), so the __except frame must stay free of both.
// ---------------------------------------------------------------------------
constexpr int GUARD_LOG_CAP = 32;
int s_guard_log_count = 0;

// Same idea for the light trace: one line whenever a submitted light's
// translation materially changes, and never more than this many in a session.
constexpr int LIGHT_TRACE_LOG_CAP = 48;
int s_light_trace_count = 0;

void LogGuardHit(const char* site, const char* kind, unsigned long code)
{
  if (s_guard_log_count >= GUARD_LOG_CAP)
    return;
  ++s_guard_log_count;
  ERROR_LOG_FMT(VIDEO, "Remix: {} escaped the runtime at {} (code {:#010x}); call skipped", kind,
                site, code);
}

template <typename F>
int CallCxxGuarded(const char* site, F& fn)
{
  try
  {
    fn();
    return 0;
  }
  catch (const std::exception&)
  {
    LogGuardHit(site, "a C++ exception", 0);
    return 2;
  }
  catch (...)
  {
    LogGuardHit(site, "an unknown C++ exception", 0);
    return 2;
  }
}

template <typename F>
int CallGuarded(const char* site, F&& fn)
{
  __try
  {
    return CallCxxGuarded(site, fn);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    LogGuardHit(site, "an SEH exception", GetExceptionCode());
    return 1;
  }
}

// Remix Plus resolves a material texture path of the form "0x<hex>" to a
// memory-uploaded texture with that hash, which is how we avoid dumping decoded
// GC textures to disk.
std::wstring HashToPath(u64 hash)
{
  wchar_t buf[32];
  std::swprintf(buf, std::size(buf), L"0x%llX", static_cast<unsigned long long>(hash));
  return std::wstring(buf);
}

u64 FoldHash(u64 seed, u64 value)
{
  return XXH64(&value, sizeof(value), seed);
}

// A zero handle value is rejected by the runtime; keep the fixup deterministic.
u64 NonZeroHash(u64 hash)
{
  return hash != 0 ? hash : 0xD6E8FEB86659FD93ULL;
}

// "0xabc,0xdef" -> a set of hashes. Separators are anything that is not a hex
// digit or an 'x', so commas, spaces and newlines all work.
void ParseHashList(const std::string& text, std::unordered_set<u64>& out)
{
  out.clear();
  for (size_t i = 0; i < text.size();)
  {
    if (std::isxdigit(static_cast<unsigned char>(text[i])) == 0)
    {
      ++i;
      continue;
    }
    size_t consumed = 0;
    try
    {
      const u64 hash = std::stoull(text.substr(i), &consumed, 16);
      if (hash != 0)
        out.insert(hash);
    }
    catch (const std::exception&)
    {
      // Malformed or out-of-range entry: skip this token rather than take the
      // whole backend down over a typo in a config string.
      consumed = 0;
    }
    i += std::max<size_t>(consumed, 1);
  }
}

// ---------------------------------------------------------------------------
// Per-game runtime files.
//
// The Remix runtime reads its whole file-system layout out of four environment
// variables, once, from inside Direct3DCreate9. Setting them just before the
// runtime DLL is loaded is the entire mechanism behind giving each game its own
// config, mods, captures and log - the runtime itself needed no change.
//
// The two config variables are the interesting ones: each takes a
// COMMA-SEPARATED list of files, lowest priority first, and the runtime
// designates the LAST entry as the layer its dev menu saves into. Passing
// "<global>,<per-game>" therefore buys both halves at once - the shared settings
// apply underneath, and anything edited in-game is written to the game's own
// file rather than to the file every other game reads.
//
// user.conf matters more than rtx.conf here, even though it looks like the
// afterthought: every edit made through the Remix dev menu targets the USER
// layer, so that is where a texture tagged in-game actually lands.
// DXVK_USER_CONFIG_FILE is a fork addition (rtx_option_layer.cpp step 6) - it
// was the one layer with no environment variable, which left the whole
// separation defeated at the exact moment someone used it.
// ---------------------------------------------------------------------------
constexpr const wchar_t* ENV_RTX_CONFIG_FILE = L"DXVK_RTX_CONFIG_FILE";
constexpr const wchar_t* ENV_USER_CONFIG_FILE = L"DXVK_USER_CONFIG_FILE";
constexpr const wchar_t* ENV_MODS_DIR = L"DEFAULT_MODS_DIR";
constexpr const wchar_t* ENV_CAPTURE_PATH = L"DXVK_CAPTURE_PATH";
constexpr const wchar_t* ENV_LOG_PATH = L"DXVK_LOG_PATH";

constexpr const wchar_t* REMIX_ENV_VARS[] = {ENV_RTX_CONFIG_FILE, ENV_USER_CONFIG_FILE,
                                             ENV_MODS_DIR, ENV_CAPTURE_PATH, ENV_LOG_PATH};
constexpr size_t REMIX_ENV_VAR_COUNT = std::size(REMIX_ENV_VARS);

// Indices into REMIX_ENV_VARS, so the calls below read as names.
enum RemixEnvVar : size_t
{
  ENV_INDEX_RTX_CONFIG = 0,
  ENV_INDEX_USER_CONFIG = 1,
  ENV_INDEX_MODS = 2,
  ENV_INDEX_CAPTURES = 3,
  ENV_INDEX_LOGS = 4,
};

// Every one of these is read into a MAX_PATH buffer on the runtime side
// (util_env.cpp's getEnvVar, util_filesys.cpp's own copy of it). An over-long
// value does not arrive truncated: the read fails and yields an EMPTY string, so
// the runtime silently falls back to its defaults. Refusing to set one is the
// same outcome with a log line attached.
constexpr size_t MAX_ENV_VALUE_LENGTH = MAX_PATH - 1;

// Whether we have set any of these variables in this process. There is
// deliberately NO "respect a value that was already set" courtesy here, and
// there must never be one again: the per-game restart
// (MainWindow::MaybeRestartForRemix) relaunches Dolphin after every game, and
// a child process inherits its parent's environment - so a value found at
// startup is indistinguishable from the previous game's leftovers. When such
// values were honoured, a freshly restarted Dolphin booted Super Mario Galaxy
// against SpongeBob's rtx.conf. These five variables are Dolphin's per-game
// contract with the runtime; Dolphin owns them outright, every boot.
bool s_env_applied = false;

std::string EnvVarName(size_t index)
{
  return WStringToUTF8(REMIX_ENV_VARS[index]);
}

bool EnvValueFits(const std::string& value)
{
  return value.size() <= MAX_ENV_VALUE_LENGTH &&
         UTF8ToWString(value).size() <= MAX_ENV_VALUE_LENGTH;
}

// Sets one of the five. Returns false without setting anything if the value
// cannot survive the runtime's MAX_PATH read, since a silent fallback to the
// shared files is exactly what this whole feature exists to prevent.
bool SetRuntimeEnvVar(size_t index, const std::string& value)
{
  if (!EnvValueFits(value))
  {
    ERROR_LOG_FMT(VIDEO,
                  "Remix: '{}' is {} characters, over the {}-character limit the runtime reads "
                  "{} with; leaving that path at its default",
                  value, value.size(), MAX_ENV_VALUE_LENGTH, EnvVarName(index));
    return false;
  }

  if (SetEnvironmentVariableW(REMIX_ENV_VARS[index], UTF8ToWString(value).c_str()) == 0)
  {
    ERROR_LOG_FMT(VIDEO, "Remix: could not set {} (error {})", EnvVarName(index), GetLastError());
    return false;
  }

  INFO_LOG_FMT(VIDEO, "Remix: {} = {}", EnvVarName(index), value);
  s_env_applied = true;
  return true;
}

// Sets one of the two config variables to the game's own file - and only that
// file.
//
// The globals are TEMPLATES that were copied into the game's folder when it was
// set up, not layers that keep applying underneath, so nothing else belongs in
// this list. That is what makes a game's config genuinely its own: deleting
// something from it actually deletes it, rather than being re-supplied from a
// layer below on the next boot. It also means the file the runtime saves into -
// the last entry, here the only one - is the game's.
void SetConfigEnvVar(size_t index, const std::string& path)
{
  if (path.find(',') != std::string::npos)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Remix: '{}' contains a comma, which is the separator in {}; this game falls "
                  "back to the shared file",
                  path, EnvVarName(index));
    return;
  }

  SetRuntimeEnvVar(index, path);
}

// Counts texture/mesh hash entries in a config file.
//
// Used on the TEMPLATE, and only when a game has just been seeded from it. A
// hash means something only in the game it was tagged in, so hashes sitting in
// the template get stamped into every new game folder from then on - which is
// worth saying out loud at the moment it happens, and only then. Nothing can fix
// it automatically: only a human knows which game each one came from.
size_t CountHashListEntries(const std::string& conf_path, size_t& lists_out)
{
  // The runtime's cross-game category lists, as written by the dev menu's
  // texture tagging. Keys are matched at the start of a line, so a commented-out
  // entry does not count.
  static constexpr const char* HASH_LIST_KEYS[] = {
      "rtx.skyBoxTextures",   "rtx.ignoreTextures",       "rtx.hideInstanceTextures",
      "rtx.lightmapTextures", "rtx.ignoreLights",         "rtx.particleTextures",
      "rtx.decalTextures",    "rtx.terrainTextures",      "rtx.worldSpaceUiTextures",
  };

  lists_out = 0;

  std::ifstream file(conf_path);
  if (!file)
    return 0;

  size_t entries = 0;
  size_t keys = 0;
  std::string line;
  while (std::getline(file, line))
  {
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos)
      continue;

    for (const char* const key : HASH_LIST_KEYS)
    {
      if (line.compare(first, std::strlen(key), key) != 0)
        continue;

      ++keys;
      // One more entry than separators, and an empty list has neither.
      const size_t equals = line.find('=');
      if (equals != std::string::npos && line.find_first_not_of(" \t", equals + 1) !=
                                             std::string::npos)
      {
        entries += 1 + static_cast<size_t>(std::count(line.begin() + equals, line.end(), ','));
      }
      break;
    }
  }

  lists_out = keys;
  return entries;
}

void ClearRuntimeEnvVars()
{
  for (size_t i = 0; i < REMIX_ENV_VAR_COUNT; ++i)
    SetEnvironmentVariableW(REMIX_ENV_VARS[i], nullptr);
  s_env_applied = false;
}

// Which runtime file to load.
//
// The default name is deliberately NOT d3d9.dll - see the comment on
// GFX_REMIX_DLL_PATH; a runtime under that name is dragged into the process by
// Qt's platform plugin at startup and has already fixed its config and file
// paths by the time any of this runs. Installs predating the rename still have
// it as d3d9.dll though, so fall back to that rather than failing to start, and
// name the consequence.
std::string ResolveRuntimeDllPath()
{
  const std::string configured = Config::Get(Config::GFX_REMIX_DLL_PATH);

  // Only for the untouched default, and only for a bare filename: an explicit
  // setting is an instruction, not a guess to second-guess.
  if (configured != Config::GFX_REMIX_DLL_PATH.GetDefaultValue() ||
      configured.find_first_of("/\\") != std::string::npos)
  {
    return configured;
  }

  const std::string next_to_exe = File::GetExeDirectory() + DIR_SEP + configured;
  if (File::Exists(next_to_exe))
    return configured;

  const std::string legacy_name = "d3d9.dll";
  if (!File::Exists(File::GetExeDirectory() + DIR_SEP + legacy_name))
    return configured;  // Neither is there; let the load fail and report normally.

  WARN_LOG_FMT(VIDEO,
               "Remix: using the runtime at '{}' because '{}' is not there. Rename it to '{}' when "
               "convenient: Qt's platform plugin imports the name 'd3d9.dll', so Windows loads "
               "that file while Dolphin is still starting up, which fixes the runtime's config and "
               "folder paths before a game is even chosen - and that makes per-game Remix files "
               "(RemixPerGamePaths) impossible. Renaming also keeps a 242 MB path tracer out of "
               "every Dolphin process that is not using this backend.",
               legacy_name, configured, configured);
  return legacy_name;
}

// Points the runtime at this game's own files. MUST run before the runtime DLL
// is loaded.
void ConfigureRuntimePaths()
{
  if (!Config::Get(Config::GFX_REMIX_PER_GAME_PATHS))
  {
    // Booting one game with separation on and then another with it off would
    // otherwise leave the second game reading the first game's folders: these
    // variables are process-wide and outlive a game - and they even outlive
    // the process, riding into a relaunched Dolphin as inherited environment.
    // So clear unconditionally; s_env_applied only knows what THIS process set.
    if (s_env_applied)
    {
      INFO_LOG_FMT(VIDEO, "Remix: per-game files are off; runtime paths reset to the shared ones "
                          "next to Dolphin.exe");
    }
    ClearRuntimeEnvVars();
    return;
  }

  // Drop whatever is currently set, BEFORE working this game's paths out -
  // whether this process set it for a previous game, or the Dolphin that
  // relaunched us set it and we inherited it. Any of the early returns below
  // would otherwise leave the previous game's folders in place for this one -
  // which is the exact failure this feature exists to prevent, arrived at from
  // the other direction.
  ClearRuntimeEnvVars();

  const std::string game_id = SConfig::GetInstance().GetGameID();
  const RemixPaths::GamePaths paths = RemixPaths::ForGame(game_id);
  if (paths.game_dir.empty())
  {
    WARN_LOG_FMT(VIDEO,
                 "Remix: the running title has no usable game ID ('{}'), so it uses the shared "
                 "Remix files this boot",
                 game_id);
    return;
  }

  // Eagerly, and before anything is pointed at them: the Remix Toolkit's project
  // wizard has to be able to SELECT this folder, which means it has to exist
  // before the game has ever been played.
  RemixPaths::CreateDirectories(paths);
  if (!File::IsDirectory(paths.game_dir))
  {
    ERROR_LOG_FMT(VIDEO,
                  "Remix: '{}' could not be created, so this game uses the shared Remix files",
                  paths.game_dir);
    return;
  }

  // Give a brand-new game folder a starting point, copied from the templates
  // next to Dolphin.exe. Only files the game does not already have are written,
  // so this is safe on every boot - re-copying would wipe out everything tagged
  // in-game since.
  if (RemixPaths::SeedFromGlobals(paths) != 0)
  {
    // Said only at the moment it propagates. A hash means something only in the
    // game it was tagged in, so any sitting in the template have just been
    // stamped into a game they have nothing to do with.
    size_t lists = 0;
    const size_t entries = CountHashListEntries(RemixPaths::GlobalRtxConf(), lists);
    if (entries != 0)
    {
      WARN_LOG_FMT(VIDEO,
                   "Remix: the template '{}' carries {} texture/mesh hash entr(ies) across {} "
                   "list(s), and they have just been copied into '{}'. A hash tagged for one game "
                   "means nothing in another, or matches the wrong thing - worth clearing out of "
                   "the template so later games start clean, and out of this game's copy now.",
                   RemixPaths::GlobalRtxConf(), entries, lists, paths.rtx_conf);
    }
  }

  // The game's own files, and only those: the templates were copied in above,
  // they are not layered underneath.
  SetConfigEnvVar(ENV_INDEX_RTX_CONFIG, paths.rtx_conf);
  SetConfigEnvVar(ENV_INDEX_USER_CONFIG, paths.user_conf);

  // One mods directory is all the runtime takes, so this is a choice between the
  // per-game folder and the shared one rather than a search order. Set either
  // way, so that turning the option off does not leave the previous game's
  // folder behind.
  const bool per_game_mods = Config::Get(Config::GFX_REMIX_PER_GAME_MODS);
  SetRuntimeEnvVar(ENV_INDEX_MODS, per_game_mods ? paths.mods : RemixPaths::GlobalModsDir());
  SetRuntimeEnvVar(ENV_INDEX_CAPTURES, paths.captures);
  SetRuntimeEnvVar(ENV_INDEX_LOGS, paths.logs);

  INFO_LOG_FMT(VIDEO,
               "Remix: game '{}' uses '{}'. The runtime writes its own log to "
               "'{}\\remix-dxvk.log' - that is where [RTX-API-Cat] and the rest of the runtime's "
               "output goes, not dolphin.log.",
               game_id, paths.game_dir, paths.logs);
}
}  // namespace

RemixApi::RemixApi() = default;

RemixApi::~RemixApi()
{
  Shutdown();
}

bool RemixApi::Initialize(const WindowSystemInfo& wsi)
{
  if (m_valid)
    return true;

  m_log_stats = Config::Get(Config::GFX_REMIX_LOG_STATS);
  m_projection_fix = Config::Get(Config::GFX_REMIX_PROJECTION_FIX);
  m_viewport_fix = Config::Get(Config::GFX_REMIX_VIEWPORT_FIX);
  m_viewport_ref_xfb = Config::Get(Config::GFX_REMIX_VIEWPORT_REF_XFB);
  m_efb_drop_aux_pass = Config::Get(Config::GFX_REMIX_EFB_DROP_AUX_PASS);
  m_trace_projections = Config::Get(Config::GFX_REMIX_TRACE_PROJECTIONS);
  m_trace_modelviews = Config::Get(Config::GFX_REMIX_TRACE_MODELVIEWS);
  m_camera_from_modelview = Config::Get(Config::GFX_REMIX_CAMERA_FROM_MODELVIEW);
  m_ui_mode = Config::Get(Config::GFX_REMIX_UI_MODE);
  m_ui_dump_frame = Config::Get(Config::GFX_REMIX_UI_DUMP_FRAME);
  m_world_ui_distance = Config::Get(Config::GFX_REMIX_WORLD_UI_DISTANCE);
  m_world_ui_flip_y = Config::Get(Config::GFX_REMIX_WORLD_UI_FLIP_Y);
  m_skip_minor_frames = Config::Get(Config::GFX_REMIX_SKIP_MINOR_FRAMES);
  // The histogram is what the camera is read out of, so the mode cannot run
  // without it. Forcing it on beats silently falling back to the estimator.
  if (m_camera_from_modelview)
    m_trace_modelviews = true;
  m_camera_recovery = Config::Get(Config::GFX_REMIX_CAMERA_RECOVERY);
  m_view_electorate_fix = Config::Get(Config::GFX_REMIX_VIEW_ELECTORATE_FIX);
  m_view_hold_on_miss = Config::Get(Config::GFX_REMIX_VIEW_HOLD_ON_MISS);
  m_view_tie_break = Config::Get(Config::GFX_REMIX_VIEW_TIE_BREAK);
  m_gx_color = Config::Get(Config::GFX_REMIX_GX_COLOR);
  m_gx_texgen = Config::Get(Config::GFX_REMIX_GX_TEXGEN);
  m_gx_blend = Config::Get(Config::GFX_REMIX_GX_BLEND);
  m_gx_alpha_mask = Config::Get(Config::GFX_REMIX_GX_ALPHA_MASK);
  m_gx_light_fix = Config::Get(Config::GFX_REMIX_GX_LIGHT_FIX);
  m_gx_ras_channel = Config::Get(Config::GFX_REMIX_GX_RAS_CHANNEL);
  m_gx_ramp_albedo_skip = Config::Get(Config::GFX_REMIX_GX_RAMP_ALBEDO_SKIP);
  m_gx_lit_channel_texgen = Config::Get(Config::GFX_REMIX_GX_LIT_CHANNEL_TEXGEN);
  m_gx_efb_alpha_passes = Config::Get(Config::GFX_REMIX_GX_EFB_ALPHA_PASSES);
  m_gx_preserve_handedness = Config::Get(Config::GFX_REMIX_GX_PRESERVE_HANDEDNESS);
  m_gx_envmap_albedo_skip = Config::Get(Config::GFX_REMIX_GX_ENVMAP_ALBEDO_SKIP);
  m_gx_unused_stage_albedo_skip =
      Config::Get(Config::GFX_REMIX_GX_UNUSED_STAGE_ALBEDO_SKIP);
  m_trace_colors = Config::Get(Config::GFX_REMIX_TRACE_COLORS);
  m_gx_tev_color = Config::Get(Config::GFX_REMIX_GX_TEV_COLOR);
  m_gx_texture_stage = Config::Get(Config::GFX_REMIX_GX_TEXTURE_STAGE);
  m_debug_color_routes = Config::Get(Config::GFX_REMIX_DEBUG_COLOR_ROUTES);
  m_ui_ras_channel = Config::Get(Config::GFX_REMIX_UI_RAS_CHANNEL);
  m_world_scissor_skip = Config::Get(Config::GFX_REMIX_WORLD_SCISSOR_SKIP);
  // Read once, never refreshed: it decides how a matrix-palette draw's vertices
  // are built, so it is baked into every mesh hash the cache holds. See
  // GpuSkinningEnabled.
  m_gpu_skinning = Config::Get(Config::GFX_REMIX_GPU_SKINNING);
  // Read once for the same reason: promoted meshes live under synthetic cache
  // keys, so the two modes cannot share a session. Degraded below, after the
  // runtime is loaded, if the deployed d3d9 predates UpdateMeshBatched.
  m_dynamic_mesh_identity = Config::Get(Config::GFX_REMIX_DYNAMIC_MESH_IDENTITY);
  m_light_range = std::max(1.0f, Config::Get(Config::GFX_REMIX_LIGHT_RANGE));
  // World space starts as view space and drifts away from it as the estimator
  // integrates. Frame 0 is therefore exactly the identity-view behaviour.
  m_view = IDENTITY_AFFINE;
  m_view_inverse = IDENTITY_AFFINE;
  m_view_inverse_previous = IDENTITY_AFFINE;
  m_sky_mode = Config::Get(Config::GFX_REMIX_SKY_MODE);
  m_sky_auto_detect = std::clamp(Config::Get(Config::GFX_REMIX_SKY_AUTO_DETECT), 0, 2);
  m_sky_auto_frames =
      static_cast<u32>(std::max(1, Config::Get(Config::GFX_REMIX_SKY_AUTO_FRAMES)));
  m_sky_auto_min_extent = std::max(0.0f, Config::Get(Config::GFX_REMIX_SKY_AUTO_MIN_EXTENT));
  m_sky_auto_untextured_ignore = Config::Get(Config::GFX_REMIX_SKY_AUTO_UNTEXTURED_IGNORE);
  m_sky_at_infinity = Config::Get(Config::GFX_REMIX_SKY_AT_INFINITY);
  m_sky_infinity_scale = std::max(1.0f, Config::Get(Config::GFX_REMIX_SKY_INFINITY_SCALE));
  m_gx_light_no_falloff_distant = Config::Get(Config::GFX_REMIX_GX_LIGHT_NO_FALLOFF_DISTANT);
  m_sky_emissive = Config::Get(Config::GFX_REMIX_SKY_EMISSIVE);
  m_sky_emissive_intensity = std::max(0.0f, Config::Get(Config::GFX_REMIX_SKY_EMISSIVE_INTENSITY));
  m_trace_efb_copies = Config::Get(Config::GFX_REMIX_TRACE_EFB_COPIES);
  m_efb_emulation = Config::Get(Config::GFX_REMIX_EFB_EMULATION);
  m_efb_copy_2d = Config::Get(Config::GFX_REMIX_EFB_COPY_2D);
  m_efb_copy_scene = Config::Get(Config::GFX_REMIX_EFB_COPY_SCENE);
  m_efb_copy_depth = Config::Get(Config::GFX_REMIX_EFB_COPY_DEPTH);
  m_efb_copy_intensity = Config::Get(Config::GFX_REMIX_EFB_COPY_INTENSITY);
  m_efb_xfb_encode = Config::Get(Config::GFX_REMIX_EFB_XFB_ENCODE);
  m_efb_ui_compose = Config::Get(Config::GFX_REMIX_EFB_UI_COMPOSE);
  m_efb_skip_discarded_tex = Config::Get(Config::GFX_REMIX_EFB_SKIP_DISCARDED_TEX);
  m_ui_drop_pre_world_blank = Config::Get(Config::GFX_REMIX_UI_DROP_PRE_WORLD_BLANK);
  m_gx_light_drop_distant = Config::Get(Config::GFX_REMIX_GX_LIGHT_DROP_DISTANT);
  m_fallback_light_enabled = Config::Get(Config::GFX_REMIX_FALLBACK_LIGHT);
  m_ui_drop_dst_alpha = Config::Get(Config::GFX_REMIX_UI_DROP_DST_ALPHA);
  m_ui_drop_efb_copy_textures = Config::Get(Config::GFX_REMIX_UI_DROP_EFB_COPY_TEXTURES);
  m_ui_scale_to_xfb = Config::Get(Config::GFX_REMIX_UI_SCALE_TO_XFB);
  m_sky_candidates.clear();
  m_sky_classified.clear();

  // The stage-0 texture hashes to treat as skybox, and the hashes never to treat
  // as skybox whatever anything else says.
  ParseHashList(Config::Get(Config::GFX_REMIX_SKY_TEXTURES), m_sky_textures);
  ParseHashList(Config::Get(Config::GFX_REMIX_SKY_VETO_HASHES), m_sky_veto_hashes);
  if (!m_sky_textures.empty() || !m_sky_veto_hashes.empty())
  {
    INFO_LOG_FMT(VIDEO, "Remix: {} sky texture hash(es), {} veto hash(es) configured",
                 m_sky_textures.size(), m_sky_veto_hashes.size());
  }
  m_light_scale = Config::Get(Config::GFX_REMIX_LIGHT_SCALE);

  const std::string dll_path_utf8 = ResolveRuntimeDllPath();
  const std::wstring dll_path = UTF8ToWString(dll_path_utf8);

  // Before the load, and only before it: the runtime resolves its file system
  // from the environment inside Direct3DCreate9. Older runtimes did that once,
  // under a run-once guard, and refused to redo it; runtimes with the
  // resident-refresh fix (2026-08-08) re-resolve whenever these env values
  // change - which is why they must be set before EVERY game, not just the
  // first.
  ConfigureRuntimePaths();

  // A runtime that is ALREADY resident historically ignored everything set
  // above: it resolved its config layers and file paths once, on load, under a
  // run-once guard. Runtimes built 2026-08-08 or later re-resolve both when
  // the env vars change between games (the resident-refresh fix), so for them
  // this is informational; for older runtimes it is the per-game-paths bug.
  // Dolphin cannot query which kind is loaded - there is no API for it - so
  // the log states the version dependence and the one tell that settles it:
  // whether the runtime's own log shows it re-reading this game's rtx.conf.
  //
  // The d3d9.dll case is still a real misconfiguration on EVERY runtime: Qt's
  // platform plugin imports that name at startup, before any per-game env is
  // set, and the first resolve then happens against no environment at all.
  if (s_env_applied)
  {
    const HMODULE resident = GetModuleHandleW(dll_path.c_str());
    if (resident != nullptr)
    {
      wchar_t resident_path[MAX_PATH] = {};
      GetModuleFileNameW(resident, resident_path, static_cast<DWORD>(std::size(resident_path)));

      WARN_LOG_FMT(VIDEO,
                   "Remix: '{}' was already loaded before this game started. A runtime built "
                   "2026-08-08 or later re-resolves the per-game folders set up above on its own "
                   "(its log will show it rebuilding config layers for this game); an older "
                   "runtime keeps whatever it resolved at load time, and restarting Dolphin is "
                   "the fix. If that file is named d3d9.dll, Qt's platform plugin loaded it "
                   "during startup and renaming it is the fix on every runtime version.",
                   WStringToUTF8(resident_path));
    }
  }

  remixapi_ErrorCode status =
      remixapi_lib_loadRemixDllAndInitialize(dll_path.c_str(), &m_interface, &m_dll);
  if (status != REMIXAPI_ERROR_CODE_SUCCESS)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Remix: failed to load and initialize '{}' (error {}). "
                  "Error 8 means the runtime rejected our API version - Externals/remix/remix_c.h "
                  "and the deployed d3d9.dll are from different Remix builds.",
                  dll_path_utf8, static_cast<int>(status));
    m_dll = nullptr;
    return false;
  }
  INFO_LOG_FMT(VIDEO, "Remix: runtime '{}' loaded", dll_path_utf8);

  // Everything on the per-frame path must exist; a runtime missing any of these
  // is not one we can drive, and finding out mid-frame means a null call.
  // Shutdown is in this list because it is not just a teardown nicety: it is the
  // only thing that destroys a device handed over by dxvk_RegisterD3D9Device,
  // which by then is no longer ours to release (see Shutdown).
  if (m_interface.Shutdown == nullptr || m_interface.CreateMaterial == nullptr ||
      m_interface.DestroyMaterial == nullptr ||
      (m_interface.CreateMesh == nullptr && m_interface.CreateMeshBatched == nullptr) ||
      m_interface.DestroyMesh == nullptr || m_interface.SetupCamera == nullptr ||
      m_interface.DrawInstance == nullptr || m_interface.CreateLight == nullptr ||
      m_interface.DestroyLight == nullptr || m_interface.DrawLightInstance == nullptr ||
      m_interface.CreateTexture == nullptr || m_interface.DestroyTexture == nullptr ||
      m_interface.Present == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Remix: the loaded runtime is missing required API entry points");
    remixapi_lib_shutdownAndUnloadRemixDll(&m_interface, m_dll);
    m_dll = nullptr;
    return false;
  }

  // Degrade the dynamic-mesh-identity feature, once and audibly, when the
  // loaded runtime predates the UpdateMeshBatched vtable slot. The knob's
  // promise is "off is the old behaviour exactly"; a silent nullptr here would
  // make ON the old behaviour too, which would poison any A/B run.
  if (m_dynamic_mesh_identity && m_interface.UpdateMeshBatched == nullptr)
  {
    WARN_LOG_FMT(VIDEO,
                 "Remix: RemixDynamicMeshIdentity is on, but the loaded runtime is too old to "
                 "support it (no UpdateMeshBatched entry point). Regenerated meshes keep the old "
                 "one-handle-per-frame behaviour this session; deploy a newer d3d9-remix.dll to "
                 "get stable identity.");
    m_dynamic_mesh_identity = false;
  }

  HWND hwnd = static_cast<HWND>(wsi.render_surface);
  if (hwnd == nullptr)
    hwnd = static_cast<HWND>(wsi.render_window);
  if (hwnd == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Remix: no render surface to present into (headless?); backend disabled");
    remixapi_lib_shutdownAndUnloadRemixDll(&m_interface, m_dll);
    m_dll = nullptr;
    return false;
  }

  u32 width = 1280;
  u32 height = 720;
  RECT client_rect = {};
  if (GetClientRect(hwnd, &client_rect) && client_rect.right > client_rect.left &&
      client_rect.bottom > client_rect.top)
  {
    width = static_cast<u32>(client_rect.right - client_rect.left);
    height = static_cast<u32>(client_rect.bottom - client_rect.top);
  }

  // The screen overlay is rasterized at a resolution derived from the window,
  // so it has to be known whether or not the explicit-device path below is
  // taken. Kept current at every frame boundary from here on (RefreshLiveConfig
  // calls the same function), so window resizes and the scale knob apply
  // without a restart.
  m_hwnd = hwnd;
  UpdateOverlaySurface();

  // Preferred path: create and register the D3D9 device explicitly through the
  // dxvk extension. Startup()'s default-init flow spawns the dev-menu overlay
  // window whose RegisterRawInputDevices() call would replace Dolphin's own raw
  // input registrations (Win32 is last-call-wins) and kill controller input.
  bool device_registered = false;
  if (m_interface.dxvk_CreateD3D9 != nullptr && m_interface.dxvk_RegisterD3D9Device != nullptr)
  {
    status = m_interface.dxvk_CreateD3D9(FALSE, &m_d3d9);
    if (status == REMIXAPI_ERROR_CODE_SUCCESS && m_d3d9 != nullptr)
    {
      D3DPRESENT_PARAMETERS pp = {};
      pp.BackBufferWidth = width;
      pp.BackBufferHeight = height;
      pp.BackBufferFormat = D3DFMT_A8R8G8B8;
      pp.BackBufferCount = 1;
      pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
      pp.hDeviceWindow = hwnd;
      pp.Windowed = TRUE;
      pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

      // D3DCREATE_MULTITHREADED is load-bearing even though we only ever call
      // in from the GPU thread: dxvk's D3D9DeviceLock - which remixapi_Present
      // holds across its flush - compiles to a no-op without this flag, and the
      // runtime has its own internal threads behind that lock.
      const HRESULT hr = m_d3d9->CreateDeviceEx(
          D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
          D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, nullptr,
          &m_d3d9_device);
      if (SUCCEEDED(hr) && m_d3d9_device != nullptr)
      {
        status = m_interface.dxvk_RegisterD3D9Device(m_d3d9_device);
        if (status == REMIXAPI_ERROR_CODE_SUCCESS)
        {
          device_registered = true;
          // The runtime now owns both objects; Shutdown must not release them.
          m_device_registered = true;
          INFO_LOG_FMT(VIDEO, "Remix: D3D9 device created and registered ({}x{}, hwnd {})", width,
                       height, fmt::ptr(hwnd));
        }
        else
        {
          WARN_LOG_FMT(VIDEO, "Remix: dxvk_RegisterD3D9Device failed (error {}), trying Startup()",
                       static_cast<int>(status));
          m_d3d9_device->Release();
          m_d3d9_device = nullptr;
          m_d3d9->Release();
          m_d3d9 = nullptr;
        }
      }
      else
      {
        WARN_LOG_FMT(VIDEO, "Remix: CreateDeviceEx failed (hr {:#010x}), trying Startup()",
                     static_cast<u32>(hr));
        m_d3d9->Release();
        m_d3d9 = nullptr;
      }
    }
    else
    {
      WARN_LOG_FMT(VIDEO, "Remix: dxvk_CreateD3D9 failed (error {}), trying Startup()",
                   static_cast<int>(status));
      m_d3d9 = nullptr;
    }
  }

  if (!device_registered)
  {
    if (m_interface.Startup == nullptr)
    {
      ERROR_LOG_FMT(VIDEO, "Remix: runtime exposes neither the dxvk interop path nor Startup()");
      remixapi_lib_shutdownAndUnloadRemixDll(&m_interface, m_dll);
      m_dll = nullptr;
      return false;
    }

    remixapi_StartupInfo startup_info = {};
    startup_info.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
    startup_info.pNext = nullptr;
    startup_info.hwnd = hwnd;
    startup_info.disableSrgbConversionForOutput = 0;
    startup_info.forceNoVkSwapchain = 0;
    startup_info.editorModeEnabled = 0;
    startup_info.combineGuiInFinalColor = 1;

    status = m_interface.Startup(&startup_info);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS)
    {
      ERROR_LOG_FMT(VIDEO, "Remix: Startup() failed (error {})", static_cast<int>(status));
      remixapi_lib_shutdownAndUnloadRemixDll(&m_interface, m_dll);
      m_dll = nullptr;
      return false;
    }
    INFO_LOG_FMT(VIDEO, "Remix: started via Startup() fallback");
  }

  // GC world units differ wildly per title; rtx.sceneScale is the runtime's
  // "how many centimetres is one game unit" knob and drives every distance the
  // path tracer cares about (light falloff, volumetrics, displacement).
  if (m_interface.SetConfigVariable != nullptr)
  {
    const std::string scene_scale = fmt::format("{}", Config::Get(Config::GFX_REMIX_SCENE_SCALE));
    CallGuarded("SetConfigVariable(rtx.sceneScale)", [&] {
      m_interface.SetConfigVariable("rtx.sceneScale", scene_scale.c_str());
    });
  }

  m_after_frame_event =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) { OnAfterFrame(); });

  m_valid = true;
  return true;
}

void RemixApi::Shutdown()
{
  if (m_dll == nullptr)
    return;

  m_after_frame_event.reset();
  DestroyAllHandles();
  // Synthesized masked albedos are ours, not the texture cache's, so nothing
  // else will ever free them.
  m_masked_textures.clear();
  // The other half of the same idiom: a mask a priming pass left pending lives
  // in file-scope state in the vertex manager, which nothing else tears down.
  ResetPendingAlphaMask();

  // dxvk_RegisterD3D9Device TRANSFERS OWNERSHIP of the device and its
  // IDirect3D9Ex to the runtime. It stores the raw pointers without taking a
  // reference of its own, and remixapi_Shutdown then releases each one in a
  // loop until its refcount reaches zero. Releasing our reference here first
  // would take that count to zero and destroy both objects, so the loop inside
  // Shutdown would then run on freed memory - a use-after-free that killed the
  // process every single time emulation stopped.
  //
  // So once registration succeeded, drop the pointers without releasing and let
  // Shutdown destroy them; our reference is exactly the one its loop consumes.
  // The unregistered paths are the opposite and must still release: the
  // IDirect3D9Ex handed back by dxvk_CreateD3D9 is not stored by the runtime,
  // and dxvk_RegisterD3D9Device stores nothing on any of its failure paths, so
  // whatever we hold there is ours alone. Those failure paths already release
  // and null both pointers before m_device_registered is ever set, which is why
  // this branch only has to cover a shutdown that never got that far.
  if (m_device_registered)
  {
    m_d3d9_device = nullptr;
    m_d3d9 = nullptr;
    m_device_registered = false;
  }
  else
  {
    if (m_d3d9_device != nullptr)
    {
      m_d3d9_device->Release();
      m_d3d9_device = nullptr;
    }
    if (m_d3d9 != nullptr)
    {
      m_d3d9->Release();
      m_d3d9 = nullptr;
    }
  }

  // Shut the runtime down, but DELIBERATELY DO NOT FreeLibrary it. This is the
  // fix for the crash-on-stop-emulation, and it is proven by a crash dump
  // (Dolphin.exe.33288.dmp), whose faulting stack is this, repeated until the
  // stack overflows:
  //
  //     <Unloaded_d3d9-remix.dll>+0x680bd0
  //     ntdll!RtlpCallVectoredHandlers+0xd6
  //     ntdll!RtlDispatchException+0x206
  //     ntdll!KiUserExceptionDispatch+0x2e
  //
  // The runtime registers a VECTORED EXCEPTION HANDLER and never unregisters
  // it - remixapi_Shutdown does not, and the API exposes nothing that would.
  // The registration is process-wide and outlives the module, so once the code
  // it points at is unmapped the next exception of ANY kind calls into dead
  // memory; that access violation dispatches exceptions again, hits the same
  // dead handler, and recurses until the stack is gone. Exceptions here are
  // routine, not exotic - an ordinary C++ throw during teardown is enough - so
  // this fired essentially every time.
  //
  // Note this is also why the crash could never be guarded against locally:
  // vectored handlers run BEFORE any SEH frame handler, so CallGuarded's
  // __except below could not have caught it.
  //
  // Keeping the module mapped costs nothing real, because the runtime is
  // already a once-per-process thing by construction: it resolves its config
  // layers and file paths under a ONCE() inside Direct3DCreate9 and refuses to
  // redo them. Initialize already knows this and warns about an
  // already-resident runtime (see the s_env_applied block) - unloading was
  // never buying a clean second init, it was only making every pointer left
  // behind (this handler, the swapchain's window-proc subclass, thread
  // trampolines) point at unmapped memory instead of valid code.
  //
  // m_dll is cleared rather than kept: a later Initialize calls LoadLibraryW
  // again, which on an already-mapped module just bumps its refcount and
  // re-runs remixapi_InitializeLibrary, which is exactly what we want.
  CallGuarded("Shutdown", [&] { m_interface.Shutdown(); });
  m_interface = {};
  m_dll = nullptr;
  m_valid = false;
}

void RemixApi::DestroyAllHandles()
{
  for (const auto& mesh : m_meshes)
  {
    remixapi_MeshHandle handle = mesh.second.handle;
    if (handle != nullptr)
      CallGuarded("DestroyMesh", [&] { m_interface.DestroyMesh(handle); });
  }
  m_meshes_destroyed_total += m_meshes.size();
  m_mesh_bytes_live = 0;
  m_meshes.clear();
  m_topology.clear();
  m_poisoned_meshes.clear();

  if (m_fallback_mesh != nullptr)
  {
    remixapi_MeshHandle handle = m_fallback_mesh;
    CallGuarded("DestroyMesh(fallback)", [&] { m_interface.DestroyMesh(handle); });
    m_fallback_mesh = nullptr;
  }

  for (const auto& material : m_materials)
  {
    remixapi_MaterialHandle handle = material.second;
    if (handle != nullptr)
      CallGuarded("DestroyMaterial", [&] { m_interface.DestroyMaterial(handle); });
  }
  m_materials.clear();

  for (const auto& texture : m_textures)
  {
    remixapi_TextureHandle handle = texture.second;
    if (handle != nullptr)
      CallGuarded("DestroyTexture", [&] { m_interface.DestroyTexture(handle); });
  }
  m_textures.clear();

  for (LightEntry& light : m_lights)
  {
    if (light.handle != nullptr)
    {
      remixapi_LightHandle handle = light.handle;
      CallGuarded("DestroyLight", [&] { m_interface.DestroyLight(handle); });
      light.handle = nullptr;
    }
  }
  if (m_fallback_light != nullptr)
  {
    remixapi_LightHandle handle = m_fallback_light;
    CallGuarded("DestroyLight(fallback)", [&] { m_interface.DestroyLight(handle); });
    m_fallback_light = nullptr;
  }

  // Queued instances reference mesh handles that were just destroyed, and the
  // sample tables key off hashes that will not mean the same thing after a
  // backend restart - a stale pair would hand the estimator one bogus delta on
  // the first frame back.
  m_pending_instances.clear();
  m_view_samples.clear();
  m_view_samples_previous.clear();
  m_view_duplicate_hashes.clear();
  // Sky classifications key off mesh hashes whose meshes have just been
  // destroyed, and the signature that produced them was measured against a view
  // that is about to restart from the identity.
  m_sky_candidates.clear();
  m_sky_classified.clear();
  m_view = IDENTITY_AFFINE;
  m_view_inverse = IDENTITY_AFFINE;
  m_view_inverse_previous = IDENTITY_AFFINE;
  m_view_miss_streak = 0;

  m_bound_textures = {};
}

void RemixApi::SetBoundTexture(u32 index, const RemixTexture* texture)
{
  if (index < m_bound_textures.size())
    m_bound_textures[index] = texture;
}

void RemixApi::UnbindTexture(const RemixTexture* texture)
{
  for (const RemixTexture*& bound : m_bound_textures)
  {
    if (bound == texture)
      bound = nullptr;
  }
}

const RemixTexture* RemixApi::GetBoundTexture(u32 index) const
{
  return index < m_bound_textures.size() ? m_bound_textures[index] : nullptr;
}

namespace
{
// Two projections are the same one if every raw term matches to a relative
// epsilon. All six participate, not just the four the correction uses: a pair
// differing only in near/far is still worth seeing in the log, and it costs
// nothing because such a pair yields a no-op correction anyway.
bool SameProjection(const std::array<float, 6>& a, const std::array<float, 6>& b)
{
  for (size_t i = 0; i < a.size(); ++i)
  {
    const float scale = std::max({1.0f, std::abs(a[i]), std::abs(b[i])});
    if (std::abs(a[i] - b[i]) > 1e-6f * scale)
      return false;
  }
  return true;
}

// Recover the field of view and aspect the parameterized camera needs from a
// GX perspective projection:
//   raw[0] = 1 / (tan(fovY/2) * aspect),  raw[2] = 1 / tan(fovY/2)
// Returns false when the result is not a frustum the camera can represent, in
// which case SetupCamera keeps a fabricated default - and, crucially, draws
// must NOT be folded onto it, because it encodes no raw term at all.
//
// Shared by SetupCamera and ObserveProjection deliberately: the projection
// correction is defined relative to the camera's effective frustum, so the two
// cannot be allowed to disagree about what that frustum is.
bool RecoverFovAspect(const std::array<float, 6>& raw, float& fov_y_deg, float& aspect)
{
  if (!(raw[2] > 0.0001f) || !(raw[0] > 0.0001f))
    return false;
  fov_y_deg = 2.0f * std::atan(1.0f / raw[2]) * (180.0f / 3.14159265358979323846f);
  aspect = raw[2] / raw[0];
  return fov_y_deg >= 5.0f && fov_y_deg <= 170.0f && aspect >= 0.25f && aspect <= 8.0f;
}

// What fraction of `rect` (EFB pixels) this viewport's screen rect covers.
// Coverage rather than rect equality on purpose, twice over: a 640x480
// viewport must still qualify against a 640x448 presented rect (overscan
// crop), and field-rendering games move their viewport by half a line every
// frame, so any bit-exact comparison against last frame's value would never
// match again. The viewport edges come from the scissor-adjusted centre and
// the half extents - the same screen mapping every hardware backend positions
// by - with abs() because a mirrored viewport encodes winding, not size.
float ViewportCoverageOfRect(const DrawViewport& viewport, const MathUtil::Rectangle<int>& rect)
{
  const float half_w = std::abs(viewport.wd());
  const float half_h = std::abs(viewport.ht());
  const float left = std::max(viewport.cx - half_w, static_cast<float>(rect.left));
  const float right = std::min(viewport.cx + half_w, static_cast<float>(rect.right));
  const float top = std::max(viewport.cy - half_h, static_cast<float>(rect.top));
  const float bottom = std::min(viewport.cy + half_h, static_cast<float>(rect.bottom));
  const float rect_area =
      static_cast<float>(rect.GetWidth()) * static_cast<float>(rect.GetHeight());
  if (right <= left || bottom <= top || rect_area <= 0.0f)
    return 0.0f;
  return ((right - left) * (bottom - top)) / rect_area;
}
}  // namespace

int RemixApi::ObserveProjection(const std::array<float, 6>& raw_projection,
                                const DrawViewport& viewport)
{
  if (!m_projection_latched)
  {
    // The reference gate. First-draw-wins breaks any game that renders an
    // off-screen helper pass before its main scene (Sonic Unleashed: a
    // quarter-rect pass latched as "the screen" and the real scene was folded
    // out of the frustum). So when last presented frame had a viewport
    // covering most of its XFB rect, only a draw covering most of that rect
    // may latch; helper draws wait, and render unfolded if nothing drops them
    // first. When no viewport covered the rect (split screen, menus, boot) the
    // gate stands down and this is bit-for-bit the old first-draw latch.
    const bool gated = m_viewport_ref_xfb && m_ref_gate_valid && m_presented_rect_valid;
    if (!gated || ViewportCoverageOfRect(viewport, m_presented_rect) > 0.5f)
    {
      m_raw_projection = raw_projection;
      m_projection_latched = true;
      float fov_y_deg = 0.0f;
      float aspect = 0.0f;
      m_reference_usable = RecoverFovAspect(raw_projection, fov_y_deg, aspect);
      // Latched together with the projection, by the same draw, for the reason
      // spelled out on ObserveProjection's declaration.
      m_reference_viewport = viewport;
      // A rect with no extent cannot be divided by, so it is not a reference
      // even though it was latched. 1e-3 rather than 1e-6: viewport extents are
      // HALF widths in EFB pixels, where anything under a thousandth of a pixel
      // is a degenerate write and not a small screen.
      m_reference_viewport_usable =
          std::abs(viewport.wd()) > 1e-3f && std::abs(viewport.ht()) > 1e-3f;
    }
    else
    {
      ++m_stats.viewport_ref_deferred;
    }
  }

  // Against the REFERENCE, not against the previously seen viewport: the fold
  // is defined relative to the reference, so this counter has to count the same
  // population the fold acts on. Skipped while the gate is still holding the
  // latch open - there is no reference yet, and comparing against the empty
  // one would count every deferred draw as a change.
  if (m_projection_latched)
  {
    if (!viewport.SameRect(m_reference_viewport))
      ++m_stats.viewport_changed;
    if (!viewport.SameDepth(m_reference_viewport))
      ++m_stats.viewport_depth_changed;
  }

  bool viewport_known = false;
  for (u32 i = 0; i < m_viewport_variant_count; ++i)
  {
    if (m_viewport_variants[i].viewport.SameRect(viewport))
    {
      ++m_viewport_variants[i].draws;
      viewport_known = true;
      break;
    }
  }
  if (!viewport_known)
  {
    if (m_viewport_variant_count >= MAX_VIEWPORT_VARIANTS)
    {
      ++m_stats.viewport_overflow;
    }
    else
    {
      const u32 slot = m_viewport_variant_count++;
      m_viewport_variants[slot].viewport = viewport;
      m_viewport_variants[slot].draws = 1;
    }
  }

  for (u32 i = 0; i < m_projection_variant_count; ++i)
  {
    if (SameProjection(m_projection_variants[i].raw, raw_projection))
      return static_cast<int>(i);
  }

  if (m_projection_variant_count >= MAX_PROJECTION_VARIANTS)
  {
    ++m_stats.projection_overflow;
    return -1;
  }

  const u32 slot = m_projection_variant_count++;
  m_projection_variants[slot].raw = raw_projection;
  return static_cast<int>(slot);
}

void RemixApi::NoteProjectionUse(int slot, u32 vertex_count)
{
  if (slot < 0 || static_cast<u32>(slot) >= m_projection_variant_count)
    return;
  ++m_projection_variants[slot].draws;
  m_projection_variants[slot].vertices += vertex_count;
}

void RemixApi::LogProjectionVariants()
{
  if (!m_log_stats)
    return;

  // Self-silencing by default: a game that keeps one centred projection all
  // frame has nothing to report, and this runs at 60 Hz. Anything else - a
  // mid-frame switch, or an off-centre frustum - is exactly the condition the
  // single-camera design cannot represent, so say so.
  //
  // A frame that moved its viewport is interesting for the same reason and on
  // the same terms: one camera cannot express two screen rects either, and the
  // table below is the only place the rect geometry is readable.
  const bool viewport_interesting =
      m_viewport_variant_count > 1 || m_stats.viewport_overflow > 0;
  const bool interesting = m_projection_variant_count > 1 || m_stats.projection_oblique > 0 ||
                           m_stats.projection_overflow > 0 ||
                           m_stats.projection_uncorrectable > 0 || viewport_interesting;
  if (!m_trace_projections && (!interesting || (m_frame_index % 61) != 0))
    return;

  INFO_LOG_FMT(VIDEO,
               "Remix frame {} projections: {} variant(s) | corrected {} draw(s), "
               "off-centre {}, UNCORRECTABLE {} | ref-deferred {} | table overflow {} | fix {}",
               m_frame_index, m_projection_variant_count, m_stats.projection_corrected,
               m_stats.projection_oblique, m_stats.projection_uncorrectable,
               m_stats.viewport_ref_deferred, m_stats.projection_overflow,
               m_projection_fix ? "on" : "off");

  for (u32 i = 0; i < m_projection_variant_count; ++i)
  {
    const ProjectionVariant& variant = m_projection_variants[i];
    // raw[1] and raw[3] are the off-centre shear terms. They are the reason a
    // parameterized camera alone cannot reproduce the frame: it has no field
    // for them, so on the pre-fix path they were silently dropped.
    //
    // By VALUE, not "#0 is the reference": the reference gate can defer the
    // latch past the first-observed variant, and a frame whose gate never
    // opened has no reference at all - both must read honestly here.
    const bool is_reference = m_projection_latched && SameProjection(variant.raw, m_raw_projection);
    INFO_LOG_FMT(VIDEO,
                 "  #{}{} raw {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} | draws {} verts {}{}", i,
                 is_reference ? " (ref)" : "     ", variant.raw[0], variant.raw[1], variant.raw[2],
                 variant.raw[3], variant.raw[4], variant.raw[5], variant.draws, variant.vertices,
                 (variant.raw[1] != 0.0f || variant.raw[3] != 0.0f) ? "  <-- off-centre" : "");
  }

  // Only when the frame actually used more than one rect. A game that keeps one
  // viewport all frame - the overwhelmingly common case - has nothing to say
  // here, and saying it anyway would bury the projection table it sits under.
  if (!viewport_interesting)
    return;

  INFO_LOG_FMT(VIDEO,
               "Remix frame {} viewports: {} variant(s) | changes {} (corrected {}, "
               "REFUSED {}, mirrored {}, depth-only {}, ref-deferred {}, aux-dropped {}) | "
               "table overflow {} | fix {}",
               m_frame_index, m_viewport_variant_count, m_stats.viewport_changed,
               m_stats.viewport_corrected, m_stats.viewport_uncorrectable,
               m_stats.viewport_mirrored, m_stats.viewport_depth_changed,
               m_stats.viewport_ref_deferred, m_stats.skipped_aux_pass, m_stats.viewport_overflow,
               m_viewport_fix ? "on" : "off");

  for (u32 i = 0; i < m_viewport_variant_count; ++i)
  {
    const ViewportVariant& variant = m_viewport_variants[i];
    // Raw rect AND scissor-adjusted centre, because they differ by exactly the
    // scissor offset and a disagreement between them is the one derivation this
    // change could get wrong. zRange/farZ ride along as the viewmodel evidence:
    // a full-screen rect with a compressed zRange is the shape to look for.
    const bool is_reference = m_projection_latched && variant.viewport.SameRect(m_reference_viewport);
    INFO_LOG_FMT(VIDEO,
                 "  #{}{} rect xOrig {:.1f} yOrig {:.1f} wd {:.1f} ht {:.1f} | centre {:.1f} {:.1f} "
                 "| zRange {:.1f} farZ {:.1f} | draws {}",
                 i, is_reference ? " (ref)" : "     ", variant.viewport.rect[0],
                 variant.viewport.rect[1], variant.viewport.rect[2], variant.viewport.rect[3],
                 variant.viewport.cx, variant.viewport.cy, variant.viewport.zrange,
                 variant.viewport.farz, variant.draws);
  }
}

void RemixApi::LogResourceRegistry()
{
  if (!m_log_stats)
    return;

  // Reading the line: MESH is the only registry that is reaped, so its live
  // figure is expected to sit well above the per-frame draw count - it holds
  // MESH_IDLE_FRAMES_BEFORE_DESTROY frames of history on purpose - and what
  // matters there is whether `made` and `reaped` climb together. TEX and MAT
  // are never reaped at all, so anything they gain they keep: their totals
  // should go flat within a few seconds of a scene settling, and a figure still
  // climbing after that is the leak.
  //
  // `composed` and `masked` are the two texture kinds this backend derives
  // rather than receives. Both are keyed on content, so they are supposed to
  // converge on a small number; either one tracking the frame counter means its
  // key is unstable and it is minting a texture per frame.
  const auto mib = [](u64 bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
  INFO_LOG_FMT(VIDEO,
               "Remix frame {} registry: tex {} live ({:.1f} MiB, {} composed, {} masked albedos) "
               "| mat {} live | mesh {} live ({:.1f} MiB, {} made, {} reaped) | topo {} | "
               "overlay-only tex {}",
               m_frame_index, m_textures.size(), mib(m_texture_bytes),
               m_composed_textures_created, m_masked_albedos_created, m_materials.size(),
               m_meshes.size(), mib(m_mesh_bytes_live), m_meshes_created_total,
               m_meshes_destroyed_total, m_topology.size(), m_overlay_texture_frames.size());
}

bool RemixApi::UploadTexture(const RemixTexture& texture)
{
  const u64 hash = texture.GetContentHash();
  if (m_textures.count(hash) != 0)
    return true;
  if (m_interface.CreateTexture == nullptr)
    return false;

  remixapi_TextureInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.pNext = nullptr;
  info.hash = hash;
  info.width = texture.GetWidth();
  info.height = texture.GetHeight();
  info.depth = 1;
  info.mipLevels = 1;
  // Dolphin hands us the decoded texels in the console's own (sRGB-ish)
  // encoding, and Remix's albedo path expects sRGB content. If albedo comes out
  // uniformly too dark, the fork is linearizing a second time and this is the
  // one line to flip to REMIXAPI_FORMAT_R8G8B8A8_UNORM.
  info.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  info.data = texture.GetPixels().data();
  info.dataSize = texture.GetPixels().size();

  remixapi_TextureHandle handle = nullptr;
  remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  const int guard =
      CallGuarded("CreateTexture", [&] { status = m_interface.CreateTexture(&info, &handle); });
  if (guard != 0 || status != REMIXAPI_ERROR_CODE_SUCCESS || handle == nullptr)
  {
    WARN_LOG_FMT(VIDEO, "Remix: CreateTexture({:#018x}) failed (guard {}, error {})", hash, guard,
                 static_cast<int>(status));
    return false;
  }

  m_textures.emplace(hash, handle);
  ++m_textures_created;
  m_texture_bytes += info.dataSize;

  // One line per unique texture: the alpha range of the uploaded pixels. A
  // cutout that stays a solid card while this reports [255, 255] means the
  // coverage mask is not in the albedo texture at all - it lives in another
  // TEV stage's texture - and no amount of alpha-state plumbing can fix that.
  {
    const std::vector<u8>& pixels = texture.GetPixels();
    u8 alpha_min = 255;
    u8 alpha_max = 0;
    for (size_t i = 3; i < pixels.size(); i += 4)
    {
      alpha_min = std::min(alpha_min, pixels[i]);
      alpha_max = std::max(alpha_max, pixels[i]);
    }
    INFO_LOG_FMT(VIDEO, "Remix: texture {:#018x} {}x{} alpha range [{}, {}]", hash,
                 texture.GetWidth(), texture.GetHeight(), alpha_min, alpha_max);
  }
  return true;
}

u64 RemixApi::EnsureComposedAlphaTexture(const RemixTexture& albedo, const RemixTexture& mask)
{
  // Derived identity: both source hashes folded under a seed of their own, so a
  // composed texture can never collide with a raw upload of either source, and
  // the same (albedo, mask) pair always resolves to the same handle.
  constexpr u64 ALPHA_MASK_SEED = 0x414C50484D41534Bull;  // "ALPHMASK"
  const u64 hash =
      NonZeroHash(FoldHash(FoldHash(ALPHA_MASK_SEED, albedo.GetContentHash()),
                           mask.GetContentHash()));
  if (m_textures.count(hash) != 0)
    return hash;
  if (m_interface.CreateTexture == nullptr)
    return 0;

  const u32 width = albedo.GetWidth();
  const u32 height = albedo.GetHeight();
  const u32 mask_width = mask.GetWidth();
  const u32 mask_height = mask.GetHeight();
  const std::vector<u8>& albedo_pixels = albedo.GetPixels();
  const std::vector<u8>& mask_pixels = mask.GetPixels();
  if (width == 0 || height == 0 || mask_width == 0 || mask_height == 0 ||
      albedo_pixels.size() < static_cast<size_t>(width) * height * 4 ||
      mask_pixels.size() < static_cast<size_t>(mask_width) * mask_height * 4)
  {
    return 0;
  }

  // Albedo RGB, mask ALPHA. The mask is nearest-sampled to the albedo's grid;
  // an intensity texture decodes with its value in every channel, so reading
  // .a covers I4/I8/IA8 and real RGBA masks alike. Both textures are sampled
  // through the SAME coordinates on the Remix side (the mesh carries one
  // texcoord set - the albedo stage's), so texel-space composition is not an
  // approximation of anything: it is the only mapping the submitted mesh can
  // express.
  std::vector<u8> pixels(static_cast<size_t>(width) * height * 4);
  u8 alpha_min = 255;
  u8 alpha_max = 0;
  for (u32 y = 0; y < height; ++y)
  {
    const u32 mask_y = mask_height == height ? y : (y * mask_height) / height;
    const u8* src_row = albedo_pixels.data() + static_cast<size_t>(y) * width * 4;
    const u8* mask_row = mask_pixels.data() + static_cast<size_t>(mask_y) * mask_width * 4;
    u8* dst_row = pixels.data() + static_cast<size_t>(y) * width * 4;
    for (u32 x = 0; x < width; ++x)
    {
      const u32 mask_x = mask_width == width ? x : (x * mask_width) / width;
      dst_row[x * 4 + 0] = src_row[x * 4 + 0];
      dst_row[x * 4 + 1] = src_row[x * 4 + 1];
      dst_row[x * 4 + 2] = src_row[x * 4 + 2];
      const u8 a = mask_row[mask_x * 4 + 3];
      dst_row[x * 4 + 3] = a;
      alpha_min = std::min(alpha_min, a);
      alpha_max = std::max(alpha_max, a);
    }
  }

  remixapi_TextureInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.pNext = nullptr;
  info.hash = hash;
  info.width = width;
  info.height = height;
  info.depth = 1;
  info.mipLevels = 1;
  info.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  info.data = pixels.data();
  info.dataSize = pixels.size();

  remixapi_TextureHandle handle = nullptr;
  remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  const int guard =
      CallGuarded("CreateTexture", [&] { status = m_interface.CreateTexture(&info, &handle); });
  if (guard != 0 || status != REMIXAPI_ERROR_CODE_SUCCESS || handle == nullptr)
  {
    WARN_LOG_FMT(VIDEO, "Remix: CreateTexture(composed {:#018x}) failed (guard {}, error {})",
                 hash, guard, static_cast<int>(status));
    return 0;
  }

  m_textures.emplace(hash, handle);
  ++m_textures_created;
  ++m_composed_textures_created;
  m_texture_bytes += info.dataSize;
  INFO_LOG_FMT(VIDEO,
               "Remix: composed alpha-mask texture {:#018x} = albedo {:#018x} ({}x{}) + mask "
               "{:#018x} ({}x{}) alpha [{}, {}]",
               hash, albedo.GetContentHash(), width, height, mask.GetContentHash(), mask_width,
               mask_height, alpha_min, alpha_max);
  return hash;
}

void RemixApi::RegisterOverlayTexture(const RemixTexture* texture)
{
  if (!m_valid || texture == nullptr || !texture->HasData())
    return;

  const u64 hash = texture->GetContentHash();

  // Already a material's texture. Leave it alone entirely: it is in the grid
  // already, and adding it to the overlay-tracking map would make the idle
  // sweep a candidate for destroying a texture the scene still samples.
  if (m_textures.count(hash) != 0 && m_overlay_texture_frames.count(hash) == 0)
    return;

  // UploadTexture is a no-op past the first sighting (it deduplicates on the
  // content hash), so this is a map lookup on every frame after the first.
  if (!UploadTexture(*texture))
    return;

  const auto [it, inserted] = m_overlay_texture_frames.insert_or_assign(hash, m_frame_index);
  (void)it;
  if (inserted)
    ++m_stats.ui_overlay_tex_registered;
}

RemixApi::UiTagClass RemixApi::ClassifyUiDraw(u64 key) const
{
  // UI first, deliberately. A hash carrying two tags is a mistake the user made
  // in the grid, and of the possible readings the one that keeps the element on
  // screen is the recoverable one - an Ignore that wins silently looks exactly
  // like the feature being broken.
  if (m_tag_ui.count(key) != 0)
    return UiTagClass::Ui;
  if (m_tag_ignore.count(key) != 0)
    return UiTagClass::Ignore;
  if (m_tag_world_ui.count(key) != 0)
    return UiTagClass::WorldUi;
  return UiTagClass::None;
}

bool RemixApi::PollTagSet(const char* option_name, std::unordered_set<u64>& out)
{
  // 256 covers any plausible hand-tagged category; the retry below is what makes
  // that a starting size rather than a limit.
  constexpr u32 INITIAL_CAPACITY = 256;
  if (m_tag_poll_buffer.size() < INITIAL_CAPACITY)
    m_tag_poll_buffer.resize(INITIAL_CAPACITY);

  u32 count = 0;
  remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  const int guard = CallGuarded("GetTextureHashList", [&] {
    status = m_interface.GetTextureHashList(
        option_name, m_tag_poll_buffer.data(), static_cast<u32>(m_tag_poll_buffer.size()), &count);
  });
  if (guard != 0 || status != REMIXAPI_ERROR_CODE_SUCCESS)
    return false;

  // Truncated: the runtime wrote the true size but no data. Grow to it and ask
  // again. Once - a set that grew between two calls one microsecond apart will
  // be caught by next frame's poll, and looping here would hand a misbehaving
  // runtime the frame.
  if (count > m_tag_poll_buffer.size())
  {
    m_tag_poll_buffer.resize(count);
    const int retry_guard = CallGuarded("GetTextureHashList(retry)", [&] {
      status = m_interface.GetTextureHashList(option_name, m_tag_poll_buffer.data(),
                                              static_cast<u32>(m_tag_poll_buffer.size()), &count);
    });
    if (retry_guard != 0 || status != REMIXAPI_ERROR_CODE_SUCCESS ||
        count > m_tag_poll_buffer.size())
    {
      return false;
    }
  }

  out.clear();
  for (u32 i = 0; i < count; ++i)
    out.insert(m_tag_poll_buffer[i]);
  return true;
}

// Must match dxvk::fork_game_state::kTaggingWorldViewKey in the runtime
// (src/dxvk/rtx_render/rtx_fork_game_state.h). Convention keys are a documented
// string contract rather than part of the ABI, so there is nothing to include
// here - this is a deliberate duplicate and the two spellings must stay equal.
static constexpr const char* kTaggingWorldViewKey = "__remix.tagging.worldView";
static constexpr const char* kTaggingWorldViewFollowsMenuKey =
    "__remix.tagging.worldViewFollowsMenu";

bool RemixApi::ReadGameFlag(const char* key, bool& out)
{
  char buffer[8] = {};
  u32 actual = 0;
  remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  const int guard = CallGuarded("GetGameValue", [&] {
    status = m_interface.GetGameValue(key, buffer, static_cast<u32>(sizeof(buffer)), &actual);
  });
  // A missing key is SUCCESS with a zero size, so "no answer" and "answered
  // false" have to stay distinguishable - the caller keeps its own value when
  // this returns false rather than reading the absence as an off.
  if (guard != 0 || status != REMIXAPI_ERROR_CODE_SUCCESS || actual == 0 ||
      actual > sizeof(buffer))
  {
    return false;
  }
  out = (buffer[0] == '1');
  return true;
}

void RemixApi::SyncTaggingWorldView()
{
  // Polled before the bridge check below, and ungated, because it is also the
  // frame line's `uistate` diagnostic - an older runtime without the game-value
  // slots should still report it. Non-zero = the dev menu is open.
  int ui_state = 0;
  if (m_valid && m_interface.GetUIState != nullptr)
    CallGuarded("GetUIState", [&] { ui_state = static_cast<int>(m_interface.GetUIState()); });
  m_runtime_ui_state = ui_state;

  // Both halves are needed: we publish so the dev menu knows the mode exists,
  // and read so its checkboxes can drive us. An older runtime with neither just
  // leaves the Dolphin-side setting as the only control, which is the previous
  // behaviour exactly.
  if (!m_valid || m_interface.SetGameValue == nullptr || m_interface.GetGameValue == nullptr)
    return;

  const auto publish = [this](const char* key, bool value) {
    CallGuarded("SetGameValue(tagging)",
                [&] { m_interface.SetGameValue(key, value ? "1" : "0"); });
  };

  const bool config_now = Config::Get(Config::GFX_REMIX_UI_WORLD_VIEW);

  // Seed once. The key's PRESENCE is what makes the dev-menu checkboxes appear
  // at all - the runtime cannot know whether a given host is able to route its
  // 2D layer into the world, so publishing is how we say we can. The auto-switch
  // preference is seeded off: it moves the HUD on every menu open, which has to
  // be asked for rather than inherited.
  if (!m_tagging_view_seeded)
  {
    publish(kTaggingWorldViewKey, config_now);
    publish(kTaggingWorldViewFollowsMenuKey, false);
    m_tagging_view_config = config_now;
    m_tagging_view_manual = config_now;
    m_ui_world_view = config_now;
    m_tagging_view_seeded = true;
    return;
  }

  // Track the MANUAL position separately from what is in effect, so that the
  // auto-switch below can override it without destroying it: turning the
  // preference back off has to return to whatever was last chosen by hand, not
  // to wherever the menu happened to leave things.
  if (config_now != m_tagging_view_config)
  {
    // Dolphin's own setting moved, so it wins and gets pushed out. Comparing
    // the config against its previous value rather than against the live flag
    // is what keeps the two controls from fighting: without it, a dev-menu
    // toggle would look like a config edit on the very next frame and get
    // stomped.
    m_tagging_view_config = config_now;
    m_tagging_view_manual = config_now;
    publish(kTaggingWorldViewKey, config_now);
  }
  else
  {
    bool manual = m_tagging_view_manual;
    if (ReadGameFlag(kTaggingWorldViewKey, manual))
      m_tagging_view_manual = manual;
  }

  bool follows_menu = false;
  ReadGameFlag(kTaggingWorldViewFollowsMenuKey, follows_menu);

  // Runs after RefreshLiveConfig, which has just reset m_ui_world_view from the
  // Dolphin setting, so this is the override that makes either dev-menu control
  // stick.
  m_ui_world_view = follows_menu ? (m_runtime_ui_state != 0) : m_tagging_view_manual;
}

void RemixApi::PollRuntimeTagState()
{
  if (!m_valid || !m_ui_tag_routing)
    return;

  // A runtime older than the GetTextureHashList slot cannot answer, so the
  // feature degrades to "nothing is ever tagged" - which is exactly its off
  // position - rather than failing. Said once, because a silent degrade here
  // looks identical to the user never having tagged anything.
  if (m_interface.GetTextureHashList == nullptr)
  {
    if (!m_tag_list_warned)
    {
      WARN_LOG_FMT(VIDEO,
                   "Remix: RemixUiTagRouting is on, but the loaded runtime is too old to support "
                   "it (no GetTextureHashList entry point). Texture tags set in the dev menu will "
                   "not route 2D draws this session; deploy a newer d3d9-remix.dll to get them.");
      m_tag_list_warned = true;
    }
    return;
  }

  PollTagSet("rtx.uiTextures", m_tag_ui);
  PollTagSet("rtx.ignoreTextures", m_tag_ignore);
  PollTagSet("rtx.worldSpaceUiTextures", m_tag_world_ui);

  // The dev-menu open state used to be read here. It moved to
  // SyncTaggingWorldView, which is not gated on RemixUiTagRouting: the
  // auto-switch preference drives the tagging view from it, and that has to
  // keep working with routing off so a tag can be placed before the routing
  // that consumes it is switched on.
}

const RemixTexture* RemixApi::MaskedAlbedo(const RemixTexture& colour,
                                           const std::vector<u8>& mask_pixels, u32 mask_width,
                                           u32 mask_height, u64 mask_hash)
{
  const u32 width = colour.GetWidth();
  const u32 height = colour.GetHeight();
  if (width != mask_width || height != mask_height)
    return nullptr;

  const std::vector<u8>& colour_pixels = colour.GetPixels();
  if (colour_pixels.empty() || colour_pixels.size() != mask_pixels.size())
    return nullptr;

  // Keyed on the PAIR: the same colour image masked two different ways is two
  // different images, and the same pair must resolve to one texture every frame
  // or the mesh would re-materialise continuously.
  const u64 key = colour.GetContentHash() ^ (mask_hash * 0x9E3779B97F4A7C15ULL);
  if (const auto it = m_masked_textures.find(key); it != m_masked_textures.end())
    return it->second.get();

  std::vector<u8> combined = colour_pixels;
  for (size_t i = 3; i < combined.size(); i += 4)
    combined[i] = mask_pixels[i];

  // Load() repacks and hashes exactly as it does for a decoded game texture, so
  // the result is indistinguishable from one downstream - same upload path, same
  // content-hash identity, same LRU.
  TextureConfig config = colour.GetConfig();
  auto texture = std::make_unique<RemixTexture>(config);
  texture->Load(0, width, height, width, combined.data(), combined.size(), 0);
  if (!texture->HasData())
    return nullptr;

  // Bounded, because nothing else bounds it. Every distinct (colour, mask) pair
  // adds a full decoded image that only Shutdown frees, while the game's own
  // texture cache is busy evicting the sources - so a long session in a title
  // that masks many characters would grow this without limit.
  //
  // Dropped wholesale rather than by LRU: the map exists to keep a mesh's
  // material stable frame to frame, and everything still on screen is re-made
  // on the next draw that needs it. A cap this size is not reached by any title
  // measured here, so the cliff is a backstop rather than a working behaviour.
  constexpr size_t MAX_MASKED_TEXTURES = 512;
  if (m_masked_textures.size() >= MAX_MASKED_TEXTURES)
  {
    WARN_LOG_FMT(VIDEO,
                 "Remix: synthesized masked-albedo cache hit {} entries and was dropped. If this "
                 "repeats, the EFB alpha mask idiom is matching far more pairs than expected.",
                 m_masked_textures.size());
    m_masked_textures.clear();
  }

  const RemixTexture* result = texture.get();
  ++m_masked_albedos_created;
  m_masked_textures.emplace(key, std::move(texture));
  return result;
}

u64 RemixApi::GeometryHash(const std::vector<remixapi_HardcodedVertex>& vertices,
                           const std::vector<u32>& indices)
{
  u64 hash = XXH64(vertices.data(), vertices.size() * sizeof(remixapi_HardcodedVertex), 0);
  hash ^= XXH64(indices.data(), indices.size() * sizeof(u32),
                static_cast<u64>(sizeof(remixapi_HardcodedVertex)));
  return hash;
}

u64 RemixApi::FoldSkinningHash(u64 geometry_hash, const std::vector<u32>& blend_indices)
{
  // A third distinct seed, in the same XXH64-with-a-distinct-seed style the two
  // folds above use, so the blend indices cannot alias either of them.
  constexpr u64 SKINNING_SEED = 0x536b696e6e696e67ull;  // "Skinning"
  return geometry_hash ^ XXH64(blend_indices.data(), blend_indices.size() * sizeof(u32),
                               SKINNING_SEED);
}

u64 RemixApi::TopologyHash(const std::vector<remixapi_HardcodedVertex>& vertices,
                           const std::vector<u32>& indices, u64 material_hash)
{
  // A fourth distinct seed, same style as the three above, so a topology key
  // cannot alias a geometry or skinning hash.
  constexpr u64 TOPOLOGY_SEED = 0x546f706f6c6f6779ull;  // "Topology"

  // Gather the pose-invariant vertex bytes: texcoord (8 B) + colour (4 B) per
  // vertex. Positions and normals are excluded on purpose - they are exactly
  // what a re-pose changes - and the vertex layout is one hardcoded struct, so
  // the format needs no fold of its own.
  m_topology_scratch.clear();
  m_topology_scratch.reserve(vertices.size() * 12);
  for (const remixapi_HardcodedVertex& vertex : vertices)
  {
    const u8* texcoord = reinterpret_cast<const u8*>(&vertex.texcoord[0]);
    m_topology_scratch.insert(m_topology_scratch.end(), texcoord,
                              texcoord + sizeof(vertex.texcoord));
    const u8* color = reinterpret_cast<const u8*>(&vertex.color);
    m_topology_scratch.insert(m_topology_scratch.end(), color, color + sizeof(vertex.color));
  }

  // Chained through the seed rather than XORed together, so two components
  // that happen to hash equal cannot cancel each other out.
  u64 key = XXH64(indices.data(), indices.size() * sizeof(u32), TOPOLOGY_SEED);
  key = XXH64(m_topology_scratch.data(), m_topology_scratch.size(), key);
  key = FoldHash(key, static_cast<u64>(vertices.size()));
  key = FoldHash(key, material_hash);
  // Zero is MeshEntry.topology_key's "untracked" sentinel, so it must never be
  // a real key.
  return NonZeroHash(key);
}

u64 RemixApi::ComputeMaterialHash(u64 texture_hash, u8 filter_mode, u8 wrap_mode_u, u8 wrap_mode_v,
                                  u8 alpha_test_type, u8 alpha_reference, bool emissive,
                                  u32 emissive_rgb)
{
  // Sampler AND alpha-test state participate in material identity because Remix
  // bakes both into the material, and the same texture is legitimately sampled
  // with different wrap modes - or cut out at a different alpha threshold - by
  // different draws. Leaving the alpha test out of the key would let whichever
  // variant registered first decide the cutout for every other draw.
  const u64 state_key = (static_cast<u64>(alpha_test_type) << 32) |
                        (static_cast<u64>(alpha_reference) << 24) |
                        (static_cast<u64>(filter_mode) << 16) |
                        (static_cast<u64>(wrap_mode_u) << 8) | static_cast<u64>(wrap_mode_v);
  u64 material_hash;
  if (texture_hash != 0)
  {
    material_hash = FoldHash(texture_hash, state_key);
  }
  else
  {
    material_hash = FoldHash(FALLBACK_MATERIAL_HASH, state_key);
  }
  // Folded only when emissive, so every existing material keeps the hash it had
  // and the knob's off position is byte-identical to the pre-feature build.
  if (emissive)
    material_hash = FoldHash(material_hash, 0x5B10C0DEull ^ (emissive_rgb & 0x00FFFFFFu));
  return NonZeroHash(material_hash);
}

u64 RemixApi::ComputeMeshHash(u64 geometry_hash, u64 material_hash)
{
  // The material has to participate: Remix handles ARE hashes and a mesh bakes
  // its surface material at create time, so two material variants of the same
  // geometry must not collapse onto one handle (whichever registered first would
  // decide the other's appearance).
  //
  // De-zeroed because the runtime rejects a zero handle value, and because a
  // key of zero would alias the "untracked" sentinels elsewhere in this file.
  return NonZeroHash(FoldHash(geometry_hash, material_hash));
}

u64 RemixApi::UntexturedOrthoKey(const std::vector<remixapi_HardcodedVertex>& vertices,
                                 const std::vector<u32>& indices, u8 filter_mode, u8 wrap_mode_u,
                                 u8 wrap_mode_v, u8 alpha_test_type, u8 alpha_reference)
{
  // The exact fold SubmitMesh performs, from the exact arguments the world path
  // would have supplied for this draw. See the contract in the header: an ortho
  // draw is never skinned and never sky-emissive, so there is no skinning fold
  // and emissive is false - which leaves the geometry hash and the material
  // hash, handed to the one function that defines how the two combine.
  const u64 material_hash = ComputeMaterialHash(0, filter_mode, wrap_mode_u, wrap_mode_v,
                                                alpha_test_type, alpha_reference, false, 0);
  return ComputeMeshHash(GeometryHash(vertices, indices), material_hash);
}

MaterialRef RemixApi::EnsureMaterial(const RemixTexture* texture, u8 filter_mode, u8 wrap_mode_u,
                                     u8 wrap_mode_v, u8 alpha_test_type, u8 alpha_reference,
                                     bool emissive, u32 emissive_rgb,
                                     const RemixTexture* alpha_mask)
{
  MaterialRef result;
  if (!m_valid)
    return result;

  u64 texture_hash = 0;
  if (texture != nullptr && texture->HasData() && UploadTexture(*texture))
  {
    texture_hash = texture->GetContentHash();
    // This texture is now referenced by a material, so it must never be reaped
    // as an idle overlay thumbnail: the overlay sweep would destroy a texture
    // the scene is still sampling. Cheap - the map is empty unless tag routing
    // has actually registered something.
    m_overlay_texture_frames.erase(texture_hash);
  }

  // The draw's alpha comes from a DIFFERENT texture than its albedo: reference
  // the composed "albedo RGB + mask alpha" texture instead, so the opacity the
  // runtime samples is the mask the console actually tested. The derived hash
  // replaces the albedo's for EVERY downstream identity - material hash, mesh
  // handle, dev-menu tagging key - which keeps all of them consistent with what
  // the runtime is really rendering.
  if (texture_hash != 0 && alpha_mask != nullptr && alpha_mask->HasData())
  {
    const u64 composed_hash = EnsureComposedAlphaTexture(*texture, *alpha_mask);
    if (composed_hash != 0)
    {
      texture_hash = composed_hash;
      m_overlay_texture_frames.erase(texture_hash);
    }
  }

  const u64 material_hash = ComputeMaterialHash(texture_hash, filter_mode, wrap_mode_u, wrap_mode_v,
                                                alpha_test_type, alpha_reference, emissive,
                                                emissive_rgb);

  if (const auto it = m_materials.find(material_hash); it != m_materials.end())
  {
    result.handle = it->second;
    result.hash = material_hash;
    return result;
  }

  if (m_interface.CreateMaterial == nullptr)
    return result;

  const std::wstring albedo_path = texture_hash != 0 ? HashToPath(texture_hash) : std::wstring();

  remixapi_MaterialInfoOpaqueEXT opaque_ext = {};
  opaque_ext.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque_ext.pNext = nullptr;
  opaque_ext.roughnessTexture = nullptr;
  opaque_ext.metallicTexture = nullptr;
  opaque_ext.heightTexture = nullptr;
  // Untextured draws got a neutral mid-grey so they were visibly present rather
  // than black; textured draws leave albedo entirely to the texture.
  //
  // With the TEV colour fold on, that placeholder is wrong: an untextured draw's
  // colour is now fully described by its submitted vertex colour, and argument 1 is
  // the texture, so anything but white halves it. Wind Waker's sea would arrive
  // correctly coloured and then be rendered at half brightness. White is the
  // identity for MODULATE, and a draw with no colour at all already gets opaque
  // white vertices, so it stays visible either way.
  const float untextured_albedo = m_gx_tev_color ? 1.0f : 0.5f;
  opaque_ext.albedoConstant =
      texture_hash != 0 ?
          remixapi_Float3D{1.0f, 1.0f, 1.0f} :
          remixapi_Float3D{untextured_albedo, untextured_albedo, untextured_albedo};
  // An emissive sky must have NO diffuse albedo, or it is emissive AND lit -
  // which is neither. On console the sky carries no lighting term at all: the
  // TEV constant IS the pixel. Leaving the albedo in place means the sun and the
  // sky still shade it, so its brightness keeps tracking the scene's lighting
  // and the emissive intensity only shifts the mix instead of controlling it.
  // Zeroing albedo makes emission the surface's entire output, which is both the
  // faithful translation and what makes emissiveIntensity a real exposure
  // control over the sky rather than a blend slider.
  //
  // ⚠ This only fully applies to UNTEXTURED sky. An albedo texture REPLACES the
  // constant rather than modulating it (opaque_surface_material_interaction
  // .slangh:656-658 `if (albedoOpacityLoaded) albedo = albedoOpacitySample.rgb`),
  // so a textured sky keeps its diffuse response and stays emissive-plus-lit.
  // Dropping its albedo texture would fix that and is tempting, but the same
  // texture carries the OPACITY a cutout cloud sheet needs, so it would turn
  // alpha-tested clouds into solid rectangles. Left alone deliberately: Wind
  // Waker's sky is four untextured domes (draws 1, 2, 8, 9) and three small
  // 32-vertex textured pieces, so this covers the part that actually matters.
  if (emissive)
    opaque_ext.albedoConstant = {0.0f, 0.0f, 0.0f};
  opaque_ext.opacityConstant = 1.0f;
  opaque_ext.roughnessConstant = 0.8f;
  opaque_ext.metallicConstant = 0.0f;
  opaque_ext.anisotropy = 0.0f;
  // Which alpha state the runtime believes: the material's, or the draw call's.
  // The draw call's is the only one that can express blending, because the
  // runtime derives translucent/emissive/multiplicative from the draw's blend
  // FACTORS (rtx_instance_manager.cpp:716-820) and a material can only say
  // "blend on/off". Alpha test then also comes from the instance, which is why
  // both are set there.
  opaque_ext.useDrawCallAlphaState = m_gx_blend ? 1 : 0;
  // GX CompareMode (Never=0 .. Always=7) is already the numbering Remix wants,
  // so the draw's own alpha test carries straight across. 7 still means "no
  // test", which is what a draw without one resolves to.
  opaque_ext.alphaTestType = alpha_test_type;
  opaque_ext.alphaReferenceValue = alpha_reference;

  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &opaque_ext;
  info.hash = material_hash;
  info.albedoTexture = texture_hash != 0 ? albedo_path.c_str() : nullptr;
  info.normalTexture = nullptr;
  info.tangentTexture = nullptr;
  // Unlit sky. The runtime derives `enableEmission` from `emissiveIntensity > 0`
  // (rtx_remix_api.cpp:504), so a positive intensity is the whole switch.
  //
  // Textured sky takes its own albedo as the emissive texture - the same patch
  // the runtime applies to WorldUI - so a cloud sheet keeps its art. Untextured
  // sky has no texture to point at and its colour lives in the folded TEV
  // constant, which arrives here as emissive_rgb (0x00RRGGBB, matching the
  // tFactor byte order: 0xff5078ff traced alongside `fold tfactor [80 120 255]`).
  info.emissiveTexture = nullptr;
  info.emissiveIntensity = 0.0f;
  info.emissiveColorConstant = {0.0f, 0.0f, 0.0f};
  if (emissive)
  {
    info.emissiveIntensity = m_sky_emissive_intensity;
    if (texture_hash != 0)
    {
      info.emissiveTexture = albedo_path.c_str();
      info.emissiveColorConstant = {1.0f, 1.0f, 1.0f};
    }
    else
    {
      info.emissiveColorConstant = {static_cast<float>((emissive_rgb >> 16) & 0xFFu) / 255.0f,
                                    static_cast<float>((emissive_rgb >> 8) & 0xFFu) / 255.0f,
                                    static_cast<float>(emissive_rgb & 0xFFu) / 255.0f};
    }
  }
  info.spriteSheetRow = 1;
  info.spriteSheetCol = 1;
  info.spriteSheetFps = 0;
  info.filterMode = filter_mode;
  info.wrapModeU = wrap_mode_u;
  info.wrapModeV = wrap_mode_v;

  remixapi_MaterialHandle handle = nullptr;
  remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  const int guard =
      CallGuarded("CreateMaterial", [&] { status = m_interface.CreateMaterial(&info, &handle); });
  if (guard != 0 || status != REMIXAPI_ERROR_CODE_SUCCESS || handle == nullptr)
  {
    WARN_LOG_FMT(VIDEO, "Remix: CreateMaterial({:#018x}) failed (guard {}, error {})",
                 material_hash, guard, static_cast<int>(status));
    return result;
  }

  m_materials.emplace(material_hash, handle);
  ++m_materials_created;
  result.handle = handle;
  result.hash = material_hash;
  return result;
}

void RemixApi::NoteDrawLights(const DrawLightState& state)
{
  for (u32 i = 0; i < m_frame_light_attenuation.size(); ++i)
  {
    const u32 bit = 1u << i;
    if ((state.mask & bit) == 0)
      continue;

    // The registers are per-DRAW state, exactly like the enable mask and the
    // attenuation function turned out to be. Reading them at frame end sees only
    // whatever the last draw of the frame left in xfmem; snapshotting them here,
    // at the draw that first switched the light on, is what the geometry lit by
    // that light actually saw. At most eight copies a frame.
    const Light& src = xfmem.lights[i];
    if ((m_frame_light_mask & bit) == 0)
    {
      m_frame_light_attenuation[i] = state.attenuation[i];
      m_frame_light_diffuse[i] = state.diffuse[i];
      m_frame_lights[i] = src;
      continue;
    }

    // Already claimed. First claim still wins - splitting one GX slot into
    // several Remix lights is not worth it until a game shows these counters
    // non-zero - but the disagreement stops being silent.
    if (std::memcmp(&m_frame_lights[i], &src, sizeof(Light)) != 0)
      ++m_stats.lights_rewritten;
    if (m_frame_light_attenuation[i] != state.attenuation[i])
      ++m_stats.lights_conflicted;
  }
  m_frame_light_mask |= state.mask;
  m_frame_light_color_mask |= state.color_mask;
  m_stats.ambient_bright = m_stats.ambient_bright || state.ambient_bright;
}

void RemixApi::SubmitMesh(const MaterialRef& material, u64 geometry_hash,
                          const std::vector<remixapi_HardcodedVertex>& vertices,
                          const std::vector<u32>& indices, const remixapi_Transform& transform,
                          remixapi_InstanceCategoryFlags category_flags,
                          const DrawBlendState& blend, const float* raw_modelview,
                          const DrawDiagnostics& diagnostics,
                          const std::array<float, 6>* world_ui_projection,
                          const DrawSkinning* skinning)
{
  if (!m_valid || material.handle == nullptr || vertices.empty() || indices.empty())
    return;

  // "A perspective draw reached submission." This is the entire Scene signal the
  // EFB copy classifier reads, and it is counted HERE rather than at the
  // vertex-manager's classification point so that it means what it says: a draw
  // that was classified world but then refused still contributed no world pixels
  // to the console's EFB either. A non-null world_ui_projection marks the
  // orthographic path (RemixApi.h:735-738), which must not count.
  //
  // Counted before the poisoned-mesh check on purpose: the game DREW that
  // geometry, whether or not the runtime would take it.
  if (world_ui_projection == nullptr)
    ++m_frame_world_draws;

  // Mesh identity = geometry bytes folded with the material hash, through the
  // one function that defines that fold (see ComputeMeshHash's contract in the
  // header). This value becomes the mesh handle, and the dev menu records it as
  // the tag key for an untextured draw, so every other site that has to
  // reproduce it calls the same function rather than repeating the expression.
  const u64 mesh_hash = ComputeMeshHash(geometry_hash, material.hash);

  // Decided once, before anything acts on it, so that the mesh created on the
  // first submission and the bones attached on every later one cannot disagree
  // about whether this draw is skinned. The two size tests are what makes it a
  // decision rather than an assumption: the runtime indexes the blend array by
  // vertex, and a palette this instance cannot fill would skin the mesh by
  // whatever a zeroed bone array means.
  const bool skinned = skinning != nullptr && !skinning->palette.empty() &&
                       skinning->blend_indices.size() == vertices.size();

  if (m_poisoned_meshes.count(mesh_hash) != 0)
    return;

  remixapi_MeshHandle mesh_handle = nullptr;
  // Recorded rather than inferred from meshes_created: that counter is global to
  // the frame, and we need to know whether THIS draw reused a handle.
  bool minted_mesh = false;
  // Set when this draw was served by rewriting an existing handle's vertex
  // bytes in place. Such a draw must stay out of the camera electorate below:
  // a stable correspondence key over per-frame-changing object-space bytes
  // would poison the estimator's "same hash means same geometry" premise.
  bool updated_in_place = false;
  // The pose-invariant key of this draw, or 0 when the dynamic-mesh-identity
  // feature is not tracking it (feature off, skinned, world-UI, or a plain
  // full-hash hit that carries its create-time key instead).
  u64 topology_key = 0;
  TopoEntry* topo = nullptr;
  if (const auto it = m_meshes.find(mesh_hash); it != m_meshes.end())
  {
    mesh_handle = it->second.handle;
    it->second.last_used_frame = m_frame_index;
    it->second.geometry_hash = geometry_hash;
    it->second.diagnostics = diagnostics;
    // A repeating full hash is the ordinary cache working; make sure its
    // lineage cannot creep toward promotion. Refreshed in O(1) through the
    // key recorded at create time - no regather, no rehash.
    if (it->second.topology_key != 0)
    {
      if (const auto topo_it = m_topology.find(it->second.topology_key);
          topo_it != m_topology.end())
      {
        topo_it->second.last_full_hash = mesh_hash;
        topo_it->second.last_seen_frame = m_frame_index;
        topo_it->second.change_streak = 0;
      }
    }
  }
  else if (m_dynamic_mesh_identity && !skinned && world_ui_projection == nullptr)
  {
    // Dynamic mesh identity. A full-hash MISS is either genuinely new
    // geometry or a known mesh in a new pose; the two are told apart by the
    // topology key - everything a re-pose does NOT change (index bytes,
    // per-vertex UVs and colours, vertex count, material). A key whose full
    // hash is observed changing across two distinct frames is geometry the
    // game regenerates per frame (BFBB's CPU-animated characters and that
    // whole class), and it is served by rewriting its existing handle's
    // vertex bytes through UpdateMeshBatched - BLAS refit, real per-vertex
    // motion vectors - instead of minting a fresh handle, and a fresh ghost,
    // every frame. Skinned and world-UI draws never reach this block: the
    // palette-skinning path is stable and validated, and this must not touch
    // its gate.
    topology_key = TopologyHash(vertices, indices, material.hash);
    if (const auto topo_it = m_topology.find(topology_key); topo_it != m_topology.end())
      topo = &topo_it->second;

    MeshEntry* update_target = nullptr;
    if (topo != nullptr)
    {
      const bool counts_match = topo->vertex_count == static_cast<u32>(vertices.size()) &&
                                topo->index_count == static_cast<u32>(indices.size());

      // Demote before anything acts on the promotion, so the promoted branch
      // below only ever sees a servable lineage: a count change under a
      // stable key is an LOD switch (the runtime would drop the update
      // anyway), and a missing dynamic entry means the reaper destroyed the
      // handle while the lineage record outlived it.
      if (topo->dynamic_mesh_key != 0 &&
          (!counts_match || m_meshes.find(topo->dynamic_mesh_key) == m_meshes.end()))
      {
        topo->dynamic_mesh_key = 0;
        topo->change_streak = 0;
      }

      if (topo->dynamic_mesh_key != 0)
      {
        MeshEntry& entry = m_meshes.find(topo->dynamic_mesh_key)->second;
        if (mesh_hash == topo->last_full_hash)
        {
          // Paused animation: the bytes did not change, so the handle is
          // reused as-is and the pause costs zero API traffic. The draw may
          // also vote in the camera electorate below - its bytes really are
          // frame-stable for as long as the hash repeats, which is exactly
          // the behaviour these draws had before this feature existed.
          mesh_handle = entry.handle;
          entry.last_used_frame = m_frame_index;
          entry.geometry_hash = geometry_hash;
          entry.diagnostics = diagnostics;
          topo->last_seen_frame = m_frame_index;
        }
        else
        {
          update_target = &entry;
        }
      }
      else if (topo->last_seen_frame == m_frame_index)
      {
        // Two different objects landed on one topology key in the SAME frame
        // (mirrored props, particle quads). Two objects are not one object
        // regenerating, so this creates normally and leaves the lineage
        // record alone - the collision must not be able to walk it toward
        // promotion.
      }
      else
      {
        if (mesh_hash != topo->last_full_hash)
        {
          ++topo->change_streak;
          if (topo->change_streak >= 2 && counts_match)
          {
            // The full hash has now changed across two distinct frames with
            // stable counts: regenerated geometry. Move last frame's
            // MeshEntry from its already-stale full-hash key to the
            // lineage's synthetic key, and serve THIS frame's bytes through
            // the update path below.
            if (const auto prev_it = m_meshes.find(topo->last_full_hash);
                prev_it != m_meshes.end())
            {
              const u64 dynamic_key = NonZeroHash(FoldHash(topology_key, DYNAMIC_MESH_KEY_SEED));
              // An LOD-switch demotion can leave a zombie under the
              // deterministic synthetic key. Nothing looks it up after the
              // demote, so destroy it rather than collide with it.
              if (const auto stale_it = m_meshes.find(dynamic_key); stale_it != m_meshes.end())
              {
                if (stale_it->second.handle != nullptr)
                {
                  remixapi_MeshHandle stale_handle = stale_it->second.handle;
                  CallGuarded("DestroyMesh(stale dynamic)",
                              [&] { m_interface.DestroyMesh(stale_handle); });
                }
                m_mesh_bytes_live -= stale_it->second.gpu_bytes;
                ++m_meshes_destroyed_total;
                m_meshes.erase(stale_it);
              }
              MeshEntry moved = prev_it->second;
              m_meshes.erase(prev_it);
              moved.topology_key = topology_key;
              update_target = &m_meshes.emplace(dynamic_key, moved).first->second;
              topo->dynamic_mesh_key = dynamic_key;
            }
          }
        }
        // Record this frame's observation whichever way the miss resolves:
        // the lineage last showed THIS hash, on THIS frame, at these counts.
        topo->last_full_hash = mesh_hash;
        topo->last_seen_frame = m_frame_index;
        topo->vertex_count = static_cast<u32>(vertices.size());
        topo->index_count = static_cast<u32>(indices.size());
      }
    }

    if (update_target != nullptr)
    {
      // The same surface the create path would build, minus skinning (draws
      // with a palette never enter this block). info.hash names the EXISTING
      // handle - the runtime's update convention - rather than minting a new
      // identity.
      remixapi_MeshInfoSurfaceTriangles surface = {};
      surface.vertices_values = vertices.data();
      surface.vertices_count = vertices.size();
      surface.indices_values = indices.data();
      surface.indices_count = indices.size();
      surface.skinning_hasvalue = 0;
      surface.material = material.handle;

      remixapi_MeshInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      info.pNext = nullptr;
      info.hash = reinterpret_cast<uint64_t>(update_target->handle);
      info.surfaces_values = &surface;
      info.surfaces_count = 1;

      remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      const int guard = CallGuarded("UpdateMeshBatched",
                                    [&] { status = m_interface.UpdateMeshBatched(&info); });
      if (guard == 0 && status == REMIXAPI_ERROR_CODE_SUCCESS)
      {
        mesh_handle = update_target->handle;
        update_target->last_used_frame = m_frame_index;
        update_target->geometry_hash = geometry_hash;
        update_target->diagnostics = diagnostics;
        topo->last_full_hash = mesh_hash;
        topo->last_seen_frame = m_frame_index;
        ++m_stats.meshes_updated;
        updated_in_place = true;
      }
      else
      {
        // Not poisoned: the full hash is per-frame anyway, so poisoning would
        // blacklist one pose of the mesh and nothing else. Demote and fall
        // back to a plain create this frame; the lineage can re-promote once
        // the hash is seen changing again.
        WARN_LOG_FMT(VIDEO,
                     "Remix: UpdateMeshBatched({:#018x}) failed (guard {}, error {}); falling "
                     "back to a fresh mesh",
                     info.hash, guard, static_cast<int>(status));
        topo->dynamic_mesh_key = 0;
        topo->change_streak = 0;
        topo->last_full_hash = mesh_hash;
        topo->last_seen_frame = m_frame_index;
        topo->vertex_count = static_cast<u32>(vertices.size());
        topo->index_count = static_cast<u32>(indices.size());
      }
    }
  }

  if (mesh_handle == nullptr)
  {
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = vertices.data();
    surface.vertices_count = vertices.size();
    surface.indices_values = indices.data();
    surface.indices_count = indices.size();
    surface.skinning_hasvalue = 0;
    surface.material = material.handle;

    // GX has no bones, so its palette does not decompose into a rig: a vertex
    // names ONE of 64 complete object-to-view modelviews and that matrix is the
    // whole of its transform. That is exactly one bone per vertex at full
    // weight, which is the degenerate case the runtime's skinning kernel already
    // handles (positionOut = sum of w_i * bone[idx_i] * v, over one term).
    //
    // Both weights and indices are consumed before this call returns - the
    // batched path deep-copies them into its own pending record, the plain path
    // memcpys them straight into DXVK buffers - so a local weight array and the
    // caller's scratch indices are safe to hand over.
    std::vector<float> blend_weights;
    if (skinned)
    {
      blend_weights.assign(vertices.size(), 1.0f);
      surface.skinning_hasvalue = 1;
      surface.skinning_value.bonesPerVertex = 1;
      surface.skinning_value.blendWeights_values = blend_weights.data();
      surface.skinning_value.blendWeights_count = static_cast<u32>(blend_weights.size());
      surface.skinning_value.blendIndices_values = skinning->blend_indices.data();
      surface.skinning_value.blendIndices_count =
          static_cast<u32>(skinning->blend_indices.size());
    }

    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.pNext = nullptr;
    info.hash = mesh_hash;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;

    // CreateMeshBatched, not CreateMesh: the batched variant deep-copies the
    // surfaces and lets the runtime's own render thread materialize them at its
    // next flush point, instead of emitting onto the device command stream from
    // whatever thread called in.
    const bool have_batched = m_interface.CreateMeshBatched != nullptr;
    remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    const int guard = CallGuarded(have_batched ? "CreateMeshBatched" : "CreateMesh", [&] {
      status = have_batched ? m_interface.CreateMeshBatched(&info, &mesh_handle) :
                              m_interface.CreateMesh(&info, &mesh_handle);
    });
    if (guard != 0 || status != REMIXAPI_ERROR_CODE_SUCCESS || mesh_handle == nullptr)
    {
      // A handle that made the runtime fault once will do it every frame; drop
      // this geometry permanently rather than re-faulting at 60 Hz.
      m_poisoned_meshes.insert(mesh_hash);
      WARN_LOG_FMT(VIDEO, "Remix: mesh create {:#018x} failed (guard {}, error {}); skipping it",
                   mesh_hash, guard, static_cast<int>(status));
      return;
    }

    // Object-space bounding radius, measured once here where the decoded
    // vertices are already in hand. The sky classifier's size gate scales this
    // by the modelview and compares it against the far plane; without it,
    // camera-welded geometry is indistinguishable from a dome during a
    // translation-only window.
    float radius_sq = 0.0f;
    for (const remixapi_HardcodedVertex& vertex : vertices)
    {
      const float length_sq = vertex.position[0] * vertex.position[0] +
                              vertex.position[1] * vertex.position[1] +
                              vertex.position[2] * vertex.position[2];
      radius_sq = std::max(radius_sq, length_sq);
    }

    MeshEntry entry;
    entry.handle = mesh_handle;
    entry.last_used_frame = m_frame_index;
    entry.object_radius = std::sqrt(radius_sq);
    entry.geometry_hash = geometry_hash;
    entry.diagnostics = diagnostics;
    entry.topology_key = topology_key;
    // What this mesh costs on the GPU, charged here and refunded by whichever
    // destroy eventually takes it. Vertex and index bytes only - the runtime's
    // BLAS is its own business and we have no figure for it - so read the total
    // as a floor on the geometry footprint, not as the whole of it.
    entry.gpu_bytes = vertices.size() * sizeof(remixapi_HardcodedVertex) +
                      indices.size() * sizeof(u32);
    m_mesh_bytes_live += entry.gpu_bytes;
    ++m_meshes_created_total;
    m_meshes.emplace(mesh_hash, entry);
    ++m_stats.meshes_created;
    minted_mesh = true;

    // First sighting of a tracked topology: open its lineage record, so a
    // hash change on a later frame is observable at all. Existing lineages
    // were already brought up to date (or deliberately left alone, for the
    // same-frame collision) before the create was chosen.
    if (topology_key != 0 && topo == nullptr)
    {
      TopoEntry fresh;
      fresh.last_full_hash = mesh_hash;
      fresh.last_seen_frame = m_frame_index;
      fresh.vertex_count = static_cast<u32>(vertices.size());
      fresh.index_count = static_cast<u32>(indices.size());
      m_topology.emplace(topology_key, fresh);
    }
  }

  // Sample this draw for the camera estimator before queueing it. The mesh hash
  // is the correspondence key across frames, and it works precisely because the
  // non-palette path hashes OBJECT-space vertices: a static object keeps the
  // same hash however the camera moves. Palette draws have no single modelview
  // and their baked view-space vertices re-hash every frame, so they cannot
  // take part either way.
  const size_t sample_cap = m_view_electorate_fix ? MAX_VIEW_SAMPLES : LEGACY_MAX_VIEW_SAMPLES;
  // Update-path draws are excluded: the electorate pairs by mesh hash on the
  // premise that a repeated hash means repeated geometry, and an in-place
  // update is precisely a stable identity over CHANGING object-space bytes.
  // Before this feature such a draw entered once under a never-repeating hash
  // and could not pair - harmless; with a stable key it WOULD pair, and the
  // false correspondence would feed the camera estimator.
  if (!updated_in_place && raw_modelview != nullptr && m_view_samples.size() < sample_cap &&
      vertices.size() >= MIN_VIEW_SAMPLE_VERTICES)
  {
    Affine modelview = {};
    std::memcpy(modelview.data(), raw_modelview, sizeof(float) * 12);
    // emplace keeps the FIRST instance of a repeated mesh, so a hash submitted
    // twice pairs an arbitrary instance across frames. Record which hashes those
    // are; what to do about it is a separate question.
    if (!m_view_samples.emplace(mesh_hash, modelview).second)
      m_view_duplicate_hashes.insert(mesh_hash);
  }

  // Modelview histogram, deliberately outside every cap the electorate above
  // applies. That is the entire point of it: the estimator votes with the few
  // dozen meshes that both persist across the frame boundary and clear the vertex
  // floor, while this sees every draw in the frame. If the view matrix is sitting
  // in xfmem.posMatrices as a literal value - which it is whenever a game draws
  // world-authored geometry with an identity model transform - then one bucket
  // here holds most of the scene and no estimate is needed at all.
  if (m_trace_modelviews)
  {
    if (raw_modelview == nullptr)
    {
      ++m_stats.modelview_palette;
    }
    else
    {
      ++m_stats.modelview_samples;
      // Fold -0.0 onto +0.0 before hashing. The two compare equal and mean the
      // same transform, so a game that writes either into the same slot must not
      // end up with its scene split across two buckets over a sign bit.
      Affine matrix = {};
      for (size_t i = 0; i < matrix.size(); ++i)
        matrix[i] = raw_modelview[i] == 0.0f ? 0.0f : raw_modelview[i];

      const u64 key = XXH64(matrix.data(), matrix.size() * sizeof(float), 0);
      auto bucket = m_modelviews.find(key);
      if (bucket == m_modelviews.end() && m_modelviews.size() < MAX_MODELVIEW_BUCKETS)
      {
        ModelviewBucket fresh;
        fresh.matrix = matrix;
        bucket = m_modelviews.emplace(key, std::move(fresh)).first;
      }
      if (bucket == m_modelviews.end())
      {
        ++m_modelview_overflow;
      }
      else
      {
        bucket->second.meshes.insert(mesh_hash);
        ++bucket->second.draws;
        bucket->second.vertices += static_cast<u32>(vertices.size());
        if (diagnostics.position_matrix < 64)
          bucket->second.slots |= 1ull << diagnostics.position_matrix;
      }
    }
  }

  PendingInstance pending{mesh_handle, transform, category_flags, blend, mesh_hash, geometry_hash};
  if (world_ui_projection != nullptr)
  {
    pending.world_ui = true;
    pending.ortho_projection = *world_ui_projection;
  }
  // On every submission, hit or miss. The mesh carries the vertex-to-bone
  // partition, which is fixed the moment it is created; the pose is what changes
  // per draw, and it lives here.
  if (skinned)
  {
    pending.bones = skinning->palette;
    ++m_stats.draws_skinned;
    if (minted_mesh)
      ++m_stats.draws_skinned_new_mesh;
  }
  m_pending_instances.push_back(std::move(pending));
}

const char* EfbCopyClassName(EfbCopyClass copy_class)
{
  switch (copy_class)
  {
  case EfbCopyClass::Xfb:
    return "xfb";
  case EfbCopyClass::Depth:
    return "depth";
  case EfbCopyClass::Intensity:
    return "intensity";
  case EfbCopyClass::Scene:
    return "scene";
  case EfbCopyClass::Composed2D:
    return "composed2d";
  }
  return "unknown";
}

bool RemixApi::NoteEfbCopy(const MathUtil::Rectangle<int>& src_rect, u32 dst_addr, u8 copy_format,
                           bool xfb, bool clear, bool half_scale, bool is_depth, bool is_intensity,
                           float y_scale)
{
  ++m_stats.efb_copies;
  if (!xfb && clear)
    ++m_stats.efb_copies_scratch;

  // The XFB copy's SOURCE rect is the part of the EFB that reaches the screen,
  // and therefore the region the UI overlay has to be mapped onto - see
  // GFX_REMIX_UI_SCALE_TO_XFB. Unioned rather than latched from one copy: a game
  // that presents two fields, or splits the frame into bands, contributes each
  // of them and the presented region is their extent.
  //
  // Recorded unconditionally, like the destination set below and for the same
  // reason: the mapping must not quietly change when the trace log is turned off.
  if (xfb)
  {
    if (!m_xfb_frame_valid)
    {
      m_xfb_frame_rect = src_rect;
      m_xfb_frame_valid = true;
    }
    else
    {
      m_xfb_frame_rect.left = std::min(m_xfb_frame_rect.left, src_rect.left);
      m_xfb_frame_rect.top = std::min(m_xfb_frame_rect.top, src_rect.top);
      m_xfb_frame_rect.right = std::max(m_xfb_frame_rect.right, src_rect.right);
      m_xfb_frame_rect.bottom = std::max(m_xfb_frame_rect.bottom, src_rect.bottom);
    }
  }

  // The set of GC addresses this session has ever aimed an EFB copy at. Kept
  // ACROSS frames, not per frame, because the property it records is permanent:
  // this backend writes no EFB copy, ever, so that memory holds whatever was
  // there before for the whole run. A per-frame set would only catch a game that
  // copies and samples in the same frame, and would miss one that composes once
  // at load and samples the result for the next ten minutes.
  //
  // The XFB copy is excluded. It is the frame's presentation copy rather than an
  // off-screen surface a UI element samples, and its destination is a buffer
  // VideoCommon manages on its own terms.
  //
  // Address 0 is never recorded: DrawBlendState::texture_addr is 0 for an
  // untextured draw, so admitting it would refuse every untextured UI draw in
  // the game.
  if (!xfb && dst_addr != 0 && m_efb_copy_destinations.size() < MAX_EFB_COPY_DESTINATIONS &&
      std::find(m_efb_copy_destinations.begin(), m_efb_copy_destinations.end(), dst_addr) ==
          m_efb_copy_destinations.end())
  {
    m_efb_copy_destinations.push_back(dst_addr);
  }

  // --- Classification ---
  //
  // Tested in the priority order of the enum, because the signals overlap: a
  // depth copy is also a copy with world draws behind it, an intensity tap is
  // also a colour copy. First match wins.
  //
  // `execute` starts false and is only ever assigned from a per-class knob, so
  // the fall-through of every arm is DISCARD - which is bit-for-bit what this
  // backend did before any of this existed (the staging buffers were never
  // written, so destinations received zeroes). That is the whole safety
  // argument: an unrecognised or misclassified copy cannot look worse than the
  // build before this one.
  //
  // Skipped entirely when the emulation is off, so that every counter below
  // reads zero exactly as it did when these counters did not exist.
  EfbCopyClass copy_class = EfbCopyClass::Scene;
  bool execute = false;
  if (m_efb_emulation)
  {
    if (xfb)
    {
      // The presentation copy. Nothing on this backend consumes the XFB image -
      // the Remix runtime produces and presents the picture - so this is off by
      // default, and it is also the one recurring per-frame full-width encode.
      copy_class = EfbCopyClass::Xfb;
      execute = m_efb_xfb_encode;
      ++m_stats.efb_copies_xfb;
    }
    else if (is_depth)
    {
      // Source pixel format Z24 (BPStructs.cpp:308). The signal is exact; the
      // purpose - a shadow map - is inferred, and both point at discard: the
      // tracer casts real shadows, and this EFB's depth plane holds only clear-Z
      // plus pokes, so executing would hand the game a uniform depth map.
      copy_class = EfbCopyClass::Depth;
      execute = m_efb_copy_depth;
      ++m_stats.efb_copies_depth;
    }
    else if (is_intensity)
    {
      // Luminance destination format - how a bloom or glow chain opens. The
      // runtime does its own bloom; feeding the chain flat clear-luminance only
      // blends a uniform wash back over the screen.
      copy_class = EfbCopyClass::Intensity;
      execute = m_efb_copy_intensity;
      ++m_stats.efb_copies_intensity;
    }
    else if (m_frame_world_draws > 0)
    {
      // THE heuristic. World geometry has been submitted this frame, so the
      // copied rect very likely holds world pixels - which this backend never
      // rasterizes, so an encode would produce a flat clear-coloured rectangle
      // where the console had the scene. Discarding keeps the invisible zeroes
      // and lets the traced effect show through instead.
      copy_class = EfbCopyClass::Scene;
      execute = m_efb_copy_scene;
      ++m_stats.efb_copies_scene;
    }
    else
    {
      // Not a fall-through: this arm is positively identified by
      // m_frame_world_draws == 0. With no world draw yet, everything the
      // console's EFB contained is clear colour plus ortho draws plus pokes -
      // exactly what this EFB holds - so the content is complete by
      // construction rather than by luck. The only class that executes by
      // default.
      copy_class = EfbCopyClass::Composed2D;
      execute = m_efb_copy_2d;
      ++m_stats.efb_copies_composed2d;
    }

    if (execute)
      ++m_stats.efb_copies_executed;
    else
      ++m_stats.efb_copies_discarded;

    // Maintain the discarded-destination set in both directions, so the draw
    // path can tell "we left this memory empty on purpose" from "this memory
    // holds a real encode". XFB is excluded for the same reason it is excluded
    // from m_efb_copy_destinations above: it is not an off-screen surface a
    // draw samples.
    //
    // Note this runs on the CLASSIFICATION, not on what the encode produced -
    // a destination is discarded the moment we decide not to write it, which is
    // before any draw this frame could sample it.
    if (!xfb && dst_addr != 0)
    {
      const auto it = std::find(m_efb_discarded_destinations.begin(),
                                m_efb_discarded_destinations.end(), dst_addr);
      if (!execute)
      {
        if (it == m_efb_discarded_destinations.end() &&
            m_efb_discarded_destinations.size() < MAX_EFB_COPY_DESTINATIONS)
        {
          m_efb_discarded_destinations.push_back(dst_addr);
        }
      }
      else if (it != m_efb_discarded_destinations.end())
      {
        // It holds real pixels now. Anything sampling it must be drawn.
        m_efb_discarded_destinations.erase(it);
      }
    }

    // The scratch REGION, for the UI path. Recorded only for a discarded copy of
    // a strict subregion: a full-EFB rect is a whole-frame capture, and the
    // pre-world rule already handles the clear that precedes one.
    if (!xfb && !execute && src_rect.GetWidth() > 0 && src_rect.GetHeight() > 0 &&
        (src_rect.GetWidth() < static_cast<int>(EFB_WIDTH) ||
         src_rect.GetHeight() < static_cast<int>(EFB_HEIGHT)) &&
        m_efb_discarded_rects.size() < MAX_EFB_COPY_DESTINATIONS &&
        std::find(m_efb_discarded_rects.begin(), m_efb_discarded_rects.end(), src_rect) ==
            m_efb_discarded_rects.end())
    {
      m_efb_discarded_rects.push_back(src_rect);
    }

    // The aux-pass signature, for the WORLD path: the same discarded strict
    // subregion, but additionally copied WITH the clear flag - the game erased
    // those pixels the moment it had copied them, which is what says they were
    // never meant to reach the screen directly. Collected per frame and
    // consumed one presented frame later by ShouldDropAuxPass; the clear
    // requirement is the difference from m_efb_discarded_rects above, whose
    // UI consumer has no reason to care.
    if (!xfb && !execute && clear && src_rect.GetWidth() > 0 && src_rect.GetHeight() > 0 &&
        (src_rect.GetWidth() < static_cast<int>(EFB_WIDTH) ||
         src_rect.GetHeight() < static_cast<int>(EFB_HEIGHT)) &&
        m_scratch_clear_rects.size() < MAX_EFB_COPY_DESTINATIONS &&
        std::find(m_scratch_clear_rects.begin(), m_scratch_clear_rects.end(), src_rect) ==
            m_scratch_clear_rects.end())
    {
      m_scratch_clear_rects.push_back(src_rect);
    }
  }

  // The misclassification instrument. It names every input the decision used,
  // because "why did this copy get that class" is the only question anyone will
  // ask of it: a depth copy says `depth 1`, a bloom tap says `int 1 half 1`, and
  // a mid-gameplay colour copy says `world-draws` greater than zero.
  if (ShouldTraceDraws())
  {
    INFO_LOG_FMT(VIDEO,
                 "Remix EFB copy: class {} action {} | rect [{} {} {} {}] dst {:#010x} fmt {} "
                 "depth {} int {} half {} yscale {:.3f} clear {} | world-draws {} ui-draws {}",
                 EfbCopyClassName(copy_class), execute ? "exec" : "discard", src_rect.left,
                 src_rect.top, src_rect.right, src_rect.bottom, dst_addr, copy_format,
                 is_depth ? 1 : 0, is_intensity ? 1 : 0, half_scale ? 1 : 0, y_scale, clear ? 1 : 0,
                 m_frame_world_draws, m_stats.ui_placed);
  }

  if (!m_trace_efb_copies)
    return execute;

  // A frame cannot legitimately need more than a handful of these; the cap only
  // exists so a runaway game cannot grow the vector without bound. Overflowing
  // copies are still counted above, so the summary stays honest about them.
  if (m_efb_copies.size() >= MAX_EFB_COPY_EVENTS)
    return execute;

  EfbCopyEvent event;
  event.seq = m_stats.draws_seen;
  event.rect = src_rect;
  event.dst_addr = dst_addr;
  event.copy_format = copy_format;
  event.xfb = xfb;
  event.clear = clear;
  event.half_scale = half_scale;
  event.depth = is_depth;
  event.intensity = is_intensity;
  event.y_scale = y_scale;
  event.copy_class = copy_class;
  event.executed = execute;
  // Latched here, at the copy, rather than read at frame end: frame-end values
  // are the LAST copy's view of the frame, which would make every
  // misclassification look retroactively justified.
  event.world_draws = m_frame_world_draws;
  event.ui_draws = m_stats.ui_placed;
  m_efb_copies.push_back(event);
  return execute;
}

bool RemixApi::ShouldDropAuxPass(const DrawViewport& viewport) const
{
  if (!m_efb_drop_aux_pass || m_scratch_clear_rects_previous.empty())
    return false;
  // Both stand-down rules ride the same learned state as the reference gate:
  // without a presented rect, or on a frame layout where no viewport covered
  // it (split screen), there is no safe notion of "not the scene" and nothing
  // is dropped.
  if (!m_presented_rect_valid || !m_ref_gate_valid)
    return false;
  // Never the scene. A viewport covering most of the presented region IS the
  // frame, whatever the copy pattern around it looked like - this is what
  // protects the very common render-whole-scene-then-copy-it-out game.
  if (ViewportCoverageOfRect(viewport, m_presented_rect) > 0.5f)
    return false;

  const float half_w = std::abs(viewport.wd());
  const float half_h = std::abs(viewport.ht());
  const float left = viewport.cx - half_w;
  const float top = viewport.cy - half_h;
  const float right = viewport.cx + half_w;
  const float bottom = viewport.cy + half_h;
  for (const MathUtil::Rectangle<int>& rect : m_scratch_clear_rects_previous)
  {
    // Whole-rect match with a one-pixel slop per edge, not containment: a draw
    // must claim exactly the region the copy-and-clear consumed before it is
    // called part of that helper pass. Anything looser starts eating
    // picture-in-picture panels that merely overlap a scratch region.
    if (std::abs(left - static_cast<float>(rect.left)) <= 1.0f &&
        std::abs(top - static_cast<float>(rect.top)) <= 1.0f &&
        std::abs(right - static_cast<float>(rect.right)) <= 1.0f &&
        std::abs(bottom - static_cast<float>(rect.bottom)) <= 1.0f)
    {
      return true;
    }
  }
  return false;
}

void RemixApi::NoteAuxPassDropped()
{
  ++m_stats.skipped_aux_pass;
  // The Scene signal must keep seeing this draw: the console's EFB held world
  // pixels here, and without this the helper pass's own copy would arrive with
  // world-draws still at zero, reclassify as Composed2D and execute - handing
  // the game a blank encode AND flipping the discarded-destination bookkeeping
  // the drop itself depends on next frame.
  ++m_frame_world_draws;
}

bool RemixApi::EfbDestinationDiscarded(u32 addr) const
{
  // Both knobs restore the draw-it-anyway behaviour, so either one is a full
  // escape hatch if skipping ever removes something the game genuinely needed.
  if (!m_efb_emulation || !m_efb_skip_discarded_tex || addr == 0)
    return false;

  return std::find(m_efb_discarded_destinations.begin(), m_efb_discarded_destinations.end(),
                   addr) != m_efb_discarded_destinations.end();
}

bool RemixApi::SubmitUiDraw(const std::vector<remixapi_HardcodedVertex>& vertices,
                            const std::vector<u32>& indices, const float* modelview,
                            const std::array<float, 6>& raw_projection,
                            const std::array<float, 6>& viewport, const std::array<float, 4>& clip,
                            const RemixTexture* texture, u8 filter_mode, u8 wrap_mode_u,
                            u8 wrap_mode_v, const DrawBlendState& blend, bool tag_bypass,
                            bool perspective)
{
  // The only exits that report "NOT consumed". Nothing about this draw was
  // decided here - the overlay simply does not exist to take it - so a caller
  // with a world path available must be free to use it rather than lose the
  // draw. Every exit below this point returns true: those are decisions.
  if (!m_valid || m_ui_mode != 1 || vertices.empty() || indices.empty())
    return false;
  if (m_interface.DrawScreenOverlay == nullptr)
    return false;

  // Draws this compositing model cannot represent, refused before anything else
  // happens - ahead of Begin(), so a frame whose ONLY UI draws are these still
  // counts as a frame with no UI and sends no overlay at all.
  //
  // Destination-alpha blending. The factors arrive already translated into
  // Vulkan's numbering by ToVkSrcFactor / ToVkDstFactor
  // (RemixVertexManager.cpp:343-355), where DST_ALPHA = 8 and
  // ONE_MINUS_DST_ALPHA = 9 for both the source and the destination table - and
  // where RemoveDstAlphaUsage has ALREADY rewritten them to One/Zero if the EFB
  // format has no alpha channel, so anything still reading 8 or 9 here is a draw
  // whose result genuinely depends on EFB alpha. The overlay has no EFB alpha to
  // offer and its own accumulated alpha is not a stand-in: these draws are
  // post-process passes over the 3D scene, so the value they want is what the 3D
  // pass wrote.
  //
  // Colour factors only. The alpha equation's factors decide the destination
  // ALPHA, which on the overlay is coverage rather than a colour anyone sees, and
  // widening the rule to them would refuse draws whose visible output is fine.
  //
  // Every drop rule from here down is skipped for a draw the user explicitly
  // tagged "UI Texture". The tag is the escape hatch these rules are documented
  // as having, so an automated rule that could still overrule it would make the
  // escape hatch a lie.
  if (!tag_bypass && m_ui_drop_dst_alpha && blend.blend_enabled &&
      (blend.src_color_factor == 8 || blend.src_color_factor == 9 ||
       blend.dst_color_factor == 8 || blend.dst_color_factor == 9))
  {
    ++m_stats.ui_skipped_dst_alpha;
    return true;
  }

  // Textured from an EFB copy's destination. Nothing writes that memory on this
  // backend, so the entry decodes stale bytes; RemixTexture::HasData() is true
  // for it and the skipped_efb_texture guard, which tests exactly that, does not
  // fire.
  //
  // Matched on the ADDRESS, not on TCacheEntry::is_efb_copy, and that is a
  // measured choice rather than a preference. The flag is false on every one of
  // these draws: bSupportsCopyToVram is false on this backend, so
  // CopyRenderTargetToTexture takes the copy-to-RAM arm and never creates an
  // EFB-copy cache entry at all. What the game binds afterwards is an ORDINARY
  // texture entry that happens to be decoded out of the copy's destination, and
  // the entry's `addr` is the only thing that still says so. Wind Waker's speckle
  // quad reads src 0x0065ff20, which is exactly the destination the copy
  // recorder saw one draw earlier, and no other draw in the frame is anywhere
  // near it.
  if (!tag_bypass && m_ui_drop_efb_copy_textures && texture != nullptr &&
      blend.texture_addr != 0 &&
      std::find(m_efb_copy_destinations.begin(), m_efb_copy_destinations.end(),
                blend.texture_addr) != m_efb_copy_destinations.end())
  {
    ++m_stats.ui_skipped_efb_copy_tex;
    return true;
  }

  // The overlay is sized to the swapchain. Cleared here, at the frame's first UI
  // draw, rather than in OnAfterFrame: a frame that draws no UI must leave the
  // buffer untouched so SubmitScreenOverlay can tell the runtime to drop its
  // pending overlay instead of compositing a transparent full-screen quad.
  if (!m_ui_frame_begun)
  {
    m_ui_raster.Begin(m_surface_width, m_surface_height);
    m_ui_frame_begun = true;
  }

  UiRasterizer::DrawCall call;
  if (texture != nullptr && !texture->GetPixels().empty())
  {
    call.texture.pixels = texture->GetPixels().data();
    // Identity for the unchanged-frame cache; the pointer above is not one.
    call.texture.content_hash = texture->GetContentHash();
    // Carried explicitly so the rasterizer can check the buffer against the
    // dimensions rather than trusting width*height*4 to be readable.
    call.texture.pixels_size = texture->GetPixels().size();
    call.texture.width = texture->GetWidth();
    call.texture.height = texture->GetHeight();
    // GX TexMode0 filter: 0 = near, anything else is some flavour of linear.
    call.texture.bilinear = filter_mode != 0;
    // GX wrap mode 0 = clamp, 1 = repeat, 2 = mirror. Mirror falls back to
    // repeat in the sampler; see the note there.
    call.texture.clamp_u = wrap_mode_u == 0;
    call.texture.clamp_v = wrap_mode_v == 0;
  }

  // A BACKGROUND draw, not a UI draw, and the overlay cannot express it.
  //
  // An untextured ortho quad submitted before any world geometry this frame is
  // the game clearing the EFB - a full-screen wash, or a scratch region it is
  // about to render into and copy out. On console the scene is drawn over it.
  // Here the ortho layer is composited ON TOP of the traced image, so the draw
  // that belongs underneath everything lands over everything and the order is
  // inverted.
  //
  // Measured on BFBB: draw 0 is a full-screen white quad and draw 1 clips to
  // [0 0 256 256] - the exact rect of the EFB copy it feeds - and both carry
  // vertex colour 0xffffffff. They are the white screen and the white box.
  //
  // Untextured was believed load-bearing here - "real 2D content is textured,
  // so this cannot swallow a menu or a HUD element". That holds for BFBB and
  // Wind Waker and is FALSE for Mario Kart: Double Dash, whose in-race clear is
  // a full-screen quad carrying a 4x4 texture. It sailed past this test and
  // painted the whole overlay white over the traced frame.
  //
  // So take a second shape as well: blending disabled AND depth write enabled.
  // Neither bit can be faked by 2D content. Blending disabled means the draw
  // OVERWRITES, and in a layer composited over an already-finished image an
  // opaque overwrite can only destroy what is under it - there is nothing in
  // the overlay for it to legitimately cover. Writing depth is something no 2D
  // overlay draw does at all. Measured on MK:DD frame 5003: of the 76 UI draws
  // taken, this quad is the ONLY one with either bit set - every HUD element is
  // blend-enabled with depth write off.
  //
  // ...and require the texture to be a PLACEHOLDER, not artwork. Those two bits
  // alone would take a legitimate full-screen background that a game happens to
  // draw opaque with z-write left on, which is a real shape - and on a frame
  // with no world content that background IS the screen, so dropping it leaves
  // the HUD floating over nothing. What a clear actually carries is a stand-in:
  // MK:DD's is 4x4. No background art is 16x16 or smaller, so that bound
  // separates the two without needing to know either game.
  //
  // The pre-world test below is still what separates a clear from a legitimate
  // 2D layer drawn after the scene, and it does the real work in all shapes.
  constexpr u32 MAX_PLACEHOLDER_TEXELS = 16 * 16;
  const bool placeholder_texture =
      static_cast<u32>(call.texture.width) * call.texture.height <= MAX_PLACEHOLDER_TEXELS;
  const bool clear_shaped =
      call.texture.pixels == nullptr ||
      (!blend.blend_enabled && blend.depth_write && placeholder_texture);
  if (!tag_bypass && m_ui_drop_pre_world_blank && clear_shaped)
  {
    if (m_frame_world_draws == 0)
    {
      ++m_stats.ui_dropped_pre_world;
      return true;
    }

    // The mid-frame scratch case is NOT tested here. It has to be decided on
    // what the draw paints, which is not known until the vertices have been
    // transformed - see the `painted` test further down.
  }

  // Blend state arrives already translated into Vulkan's numbering, so the cases
  // are read off that rather than re-derived from GX.
  //   VK_BLEND_FACTOR_ZERO = 0, ONE = 1, SRC_ALPHA = 6, ONE_MINUS_SRC_ALPHA = 7
  //
  // Anything unrecognised falls to Over rather than Opaque: on a composited
  // overlay, guessing "opaque" paints a solid rectangle over the scene, while
  // guessing "over" at worst gets the compositing weight wrong on pixels the
  // element actually covers. The failure modes are not remotely symmetric.
  if (!blend.blend_enabled)
    call.blend = UiRasterizer::BlendMode::Opaque;
  else if (blend.src_color_factor == 1 && blend.dst_color_factor == 1)
    call.blend = UiRasterizer::BlendMode::Additive;
  else if (blend.src_color_factor == 6 && blend.dst_color_factor == 1)
  {
    // SrcAlpha / One: additive with the source scaled by its own alpha, which
    // is EXACTLY what the rasterizer's Additive mode computes - so this is the
    // one blend the fallthrough got actively wrong rather than approximately
    // right. Falling to Over multiplies the destination by (1 - alpha): a glow
    // layer that on console merely brightens what is under it instead ERASES
    // it. Measured on RE4's HUD - the ammo glow (src 6 dst 1), drawn after the
    // digits, blanked them out; every other element is ordinary src 6 dst 7.
    call.blend = UiRasterizer::BlendMode::Additive;
  }
  else if (blend.src_color_factor == 1 && blend.dst_color_factor == 0)
    call.blend = UiRasterizer::BlendMode::Opaque;
  else
    call.blend = UiRasterizer::BlendMode::Over;

  // The write mask is computed by ResolveBlend and was then ignored here. A draw
  // that writes no colour contributes nothing on console; one that writes no
  // alpha must not be allowed to reduce the overlay's coverage.
  //
  // Consumed, not refused: this draw paints nothing anywhere, so handing it back
  // to a caller that would then render it as world geometry would resurrect
  // pixels the console never showed.
  if ((blend.write_mask & 0x7) == 0)
    return true;
  // The raw GX alpha test, both comparators - not the And-only decomposition the
  // material path uses, which has to surrender to Always on any other logic op.
  call.alpha_compare = blend.raw_alpha_compare0;
  call.alpha_compare1 = blend.raw_alpha_compare1;
  call.alpha_logic = blend.raw_alpha_logic;
  call.alpha_reference = static_cast<float>(blend.raw_alpha_reference0) / 255.0f;
  call.alpha_reference1 = static_cast<float>(blend.raw_alpha_reference1) / 255.0f;

  call.tev_alpha_known = blend.tev_alpha_known;
  call.tev_alpha_corners = blend.tev_alpha_corners;
  if (call.tev_alpha_known)
    ++m_stats.ui_tev_alpha;

  // EFB space -> presentation rectangle -> overlay pixels. The overlay is the
  // swapchain, while the viewport is in EFB units. The presenter contributes
  // the graphics-panel aspect rectangle so 2D UI follows the same fit/crop as
  // the normal backend. The source region is the XFB copy's rect, NOT the
  // EFB's own 640x528 - Wind Waker copies 480 rows, so the EFB constants
  // stretched every UI element over a region 10% taller than the one on screen
  // and left the bottom of the window dead. It falls back to the constants
  // until an XFB copy has been seen, which is also what the knob's off position
  // restores.
  const float region_width = (m_ui_scale_to_xfb && m_presented_width != 0) ?
                                 static_cast<float>(m_presented_width) :
                                 static_cast<float>(EFB_WIDTH);
  const float region_height = (m_ui_scale_to_xfb && m_presented_height != 0) ?
                                  static_cast<float>(m_presented_height) :
                                  static_cast<float>(EFB_HEIGHT);
  float presentation_left = 0.0f;
  float presentation_top = 0.0f;
  float presentation_width = static_cast<float>(m_surface_width);
  float presentation_height = static_cast<float>(m_surface_height);
  if (g_presenter && g_presenter->GetBackbufferWidth() > 0 &&
      g_presenter->GetBackbufferHeight() > 0)
  {
    const MathUtil::Rectangle<int>& target = g_presenter->GetTargetRectangle();
    if (target.GetWidth() > 0 && target.GetHeight() > 0)
    {
      const float surface_per_window_x =
          static_cast<float>(m_surface_width) / g_presenter->GetBackbufferWidth();
      const float surface_per_window_y =
          static_cast<float>(m_surface_height) / g_presenter->GetBackbufferHeight();
      presentation_left = target.left * surface_per_window_x;
      presentation_top = target.top * surface_per_window_y;
      presentation_width = target.GetWidth() * surface_per_window_x;
      presentation_height = target.GetHeight() * surface_per_window_y;
    }
  }
  const float scale_x = presentation_width / region_width;
  const float scale_y = presentation_height / region_height;
  call.viewport_x = presentation_left + viewport[0] * scale_x;
  call.viewport_y = presentation_top + viewport[1] * scale_y;
  call.viewport_width = viewport[2] * scale_x;
  call.viewport_height = viewport[3] * scale_y;
  call.clip_left = static_cast<int>(std::floor(presentation_left + clip[0] * scale_x));
  call.clip_top = static_cast<int>(std::floor(presentation_top + clip[1] * scale_y));
  // The EFB rect's right/bottom are exclusive, so the last included pixel is one
  // short of them.
  call.clip_right = static_cast<int>(std::ceil(presentation_left + clip[2] * scale_x)) - 1;
  call.clip_bottom = static_cast<int>(std::ceil(presentation_top + clip[3] * scale_y)) - 1;

  // The draw's footprint back in EFB space, for the copy audit. The rasterizer
  // works in overlay pixels; EFB copy rects are in EFB units, so the comparison
  // has to happen in the latter - which is the space the incoming viewport and
  // clip are already in, before the scale above is applied.
  const bool record_footprint = m_trace_efb_copies;
  float footprint_left = std::numeric_limits<float>::max();
  float footprint_top = std::numeric_limits<float>::max();
  float footprint_right = std::numeric_limits<float>::lowest();
  float footprint_bottom = std::numeric_limits<float>::lowest();

  // Set when a perspective draw has a vertex at or behind the eye plane, where
  // the perspective divide has no answer. Decided across the WHOLE draw before
  // anything acts on it: refusing only the offending vertices would tear the
  // primitive, which is a worse outcome than handing the draw back.
  bool behind_eye = false;
  // Small enough that no plausible screen-parked HUD is near it, large enough
  // that the reciprocal below stays finite. The comparison is written negated so
  // that a NaN w - which compares false against everything - is refused too.
  constexpr float kMinW = 1e-6f;
  // The draw's depth spread, for the guard after the loop. Two failure modes
  // hide in a wide spread and the ratio catches both: a vertex inside the
  // eye-to-near zone blows 1/w up into a screen-filling smear (the console
  // would have CLIPPED it at the near plane; this rasterizer has no clipper),
  // and the affine u/v interpolation the rasterizer runs is only exact while w
  // is near-constant across the primitive. A screen-parked HUD quad sits at one
  // depth and passes trivially; anything genuinely 3D fails fast and visibly in
  // the counter instead of failing as garbage on screen.
  float min_w = std::numeric_limits<float>::max();
  float max_w = 0.0f;
  constexpr float kMaxWSpread = 1.2f;

  m_ui_vertices.clear();
  m_ui_vertices.reserve(vertices.size());
  for (const remixapi_HardcodedVertex& source : vertices)
  {
    // Object -> view. A baked draw (matrix palette) is already in view space and
    // passes a null modelview.
    float view[3] = {source.position[0], source.position[1], source.position[2]};
    if (modelview != nullptr)
    {
      for (int i = 0; i < 3; ++i)
      {
        view[i] = modelview[i * 4 + 0] * source.position[0] +
                  modelview[i * 4 + 1] * source.position[1] +
                  modelview[i * 4 + 2] * source.position[2] + modelview[i * 4 + 3];
      }
    }

    UiRasterizer::Vertex vertex;
    if (perspective)
    {
      // View -> clip -> NDC, the GX perspective convention verbatim:
      //   clip.x = raw[0]*x + raw[1]*z, clip.y = raw[2]*y + raw[3]*z, clip.w = -z
      // (Software/TransformUnit.cpp:64-71), divided the way Clipper.cpp:552-554
      // divides it. raw[1] and raw[3] are the off-centre terms and are carried
      // rather than dropped, so a game rendering its HUD through an off-axis
      // frustum still lands where it drew it.
      //
      // The interpolation caveat lives here: u/v/colour are then interpolated
      // affinely in screen space, which is exact only while w barely varies
      // across the primitive. That is the defining property of a screen-parked
      // HUD quad, and this path exists for nothing else - see the header.
      const float w = -view[2];
      if (!(w > kMinW))
      {
        // At or behind the eye, or NaN. Recorded and dealt with after the loop,
        // because the verdict is a property of the whole primitive.
        behind_eye = true;
        // Keep the vertex finite so nothing downstream sees an infinity if this
        // draw is somehow still read; it is discarded either way.
        vertex.x = 0.0f;
        vertex.y = 0.0f;
      }
      else
      {
        min_w = std::min(min_w, w);
        max_w = std::max(max_w, w);
        const float inv_w = 1.0f / w;
        vertex.x = (raw_projection[0] * view[0] + raw_projection[1] * view[2]) * inv_w;
        vertex.y = (raw_projection[2] * view[1] + raw_projection[3] * view[2]) * inv_w;
        // Clip z, with TransformUnit.cpp:69's own epsilon nudge, divided and
        // mapped through the draw's viewport exactly as Clipper.cpp:555 does.
        // This IS the value the console's z test compared for this vertex.
        const float clip_z =
            (raw_projection[4] * view[2] + raw_projection[5]) * (1.0f - 1e-7f);
        vertex.z = clip_z * inv_w * viewport[4] + viewport[5];
      }
    }
    else
    {
      // View -> NDC. w is 1 for an orthographic projection, which is what lets
      // the rasterizer interpolate affinely and still be exact.
      vertex.x = raw_projection[0] * view[0] + raw_projection[1];
      vertex.y = raw_projection[2] * view[1] + raw_projection[3];
      // Same mapping at w = 1 (MultipleVec3Ortho carries no epsilon nudge).
      vertex.z = (raw_projection[4] * view[2] + raw_projection[5]) * viewport[4] + viewport[5];
    }
    vertex.u = source.texcoord[0];
    vertex.v = source.texcoord[1];
    // remixapi_HardcodedVertex::color is packed B,G,R,A in memory - see
    // ToRemixVertexColor, which does that swap on the way in.
    vertex.color = {static_cast<float>((source.color >> 16) & 0xFF) / 255.0f,
                    static_cast<float>((source.color >> 8) & 0xFF) / 255.0f,
                    static_cast<float>(source.color & 0xFF) / 255.0f,
                    static_cast<float>((source.color >> 24) & 0xFF) / 255.0f};
    m_ui_vertices.push_back(vertex);

    // Accumulated unconditionally, not just for the audit: the scratch-region
    // drop below needs the real painted extent, and a few min/max per vertex is
    // nothing next to rasterizing the quad.
    //
    // NDC -> EFB, the pre-scale analogue of UiRasterizer's to_screen_x/y. y is
    // flipped for the same reason: GX ndc y points up and EFB row 0 is the top.
    const float efb_x = viewport[0] + (vertex.x * 0.5f + 0.5f) * viewport[2];
    const float efb_y = viewport[1] + (0.5f - vertex.y * 0.5f) * viewport[3];
    footprint_left = std::min(footprint_left, efb_x);
    footprint_right = std::max(footprint_right, efb_x);
    footprint_top = std::min(footprint_top, efb_y);
    footprint_bottom = std::max(footprint_bottom, efb_y);
  }

  // Handed back to the caller rather than dropped, and before either rasterizer
  // records anything, so the draw is untouched by the time the caller sees the
  // false: a HUD element that straddles the eye plane is not a HUD element this
  // overlay can place, but it is still geometry the game drew, and rendering it
  // as world geometry is a far better failure than deleting it.
  //
  // Expected to read zero forever - a quad parked in front of the camera does
  // not reach the eye and sits at effectively one depth - so a non-zero `wref`
  // means the tagged element is not actually screen-parked and the tag is on
  // the wrong texture. The spread arm is what keeps a mis-tag from failing as
  // garbage: a vertex inside the eye-to-near zone (which the console would
  // have clipped, and this rasterizer cannot) blows 1/w up into a
  // screen-filling smear, and a genuinely tilted element breaks the affine
  // interpolation this path is built on. Both read as depth spread.
  if (perspective && (behind_eye || max_w > min_w * kMaxWSpread))
  {
    ++m_stats.ui_persp_refused_w;
    return false;
  }

  // What the draw actually covers: its geometry, cut down by its own scissor.
  const MathUtil::Rectangle<int> painted{
      std::max(static_cast<int>(std::floor(footprint_left)), static_cast<int>(clip[0])),
      std::max(static_cast<int>(std::floor(footprint_top)), static_cast<int>(clip[1])),
      std::min(static_cast<int>(std::ceil(footprint_right)), static_cast<int>(clip[2])),
      std::min(static_cast<int>(std::ceil(footprint_bottom)), static_cast<int>(clip[3]))};

  // The mid-frame scratch fill, tested on what the draw PAINTS rather than on
  // its scissor. BFBB's is scissored to the whole screen but covers exactly
  // [0 0 256 256] - the rect of the EFB copy it feeds - and arrives at
  // world-draw 204, so neither the clip rect nor the pre-world test above can
  // see it. Measured: `Remix UI paints: efb [0 0 256 256] | clip [0 0 640 528]`.
  //
  // Exact match against a rect we discard a copy of, and untextured, for the
  // same reason as before: it is the tightest test that catches the case.
  if (!tag_bypass && m_ui_drop_pre_world_blank && call.texture.pixels == nullptr &&
      std::find(m_efb_discarded_rects.begin(), m_efb_discarded_rects.end(), painted) !=
          m_efb_discarded_rects.end())
  {
    ++m_stats.ui_dropped_pre_world;
    return true;
  }

  // Everything that decides a DIVERTED perspective draw's compositing, one line
  // per draw, in submission order. This exists because RE4's HUD panels draw
  // AFTER its text with the z test off, so on console the text survives purely
  // through the panels' per-pixel ALPHA - and if that alpha resolves too opaque
  // here, the panel "overtakes" the text with every counter reading clean.
  // The ortho path has an equivalent dump; the divert path skipped it, and the
  // first RE4 report cost a build to even name the mechanism.
  if (ShouldTraceDraws())
  {
    u32 vtx_alpha_min = 256;
    u32 vtx_alpha_max = 0;
    float z_min = std::numeric_limits<float>::max();
    float z_max = std::numeric_limits<float>::lowest();
    for (const UiRasterizer::Vertex& v : m_ui_vertices)
    {
      const u32 alpha = static_cast<u32>(std::clamp(v.color[3], 0.0f, 1.0f) * 255.0f + 0.5f);
      vtx_alpha_min = std::min(vtx_alpha_min, alpha);
      vtx_alpha_max = std::max(vtx_alpha_max, alpha);
      z_min = std::min(z_min, v.z);
      z_max = std::max(z_max, v.z);
    }
    // Both projection kinds, not just perspective: the depth plane serves ortho
    // draws too, and Super Monkey Ball's title UI - the plane's first live
    // customer - z-tests all 155 of its overlay draws, most of them ortho. The
    // z range is printed at full precision because the suspected failure mode
    // is draws ONE integer apart after float truncation, which a rounded
    // display would hide.
    INFO_LOG_FMT(VIDEO,
                 "Remix {} UI {}: tex {:#018x} | blend en {} src {} dst {} -> mode {} | "
                 "corners {} [{:.2f} {:.2f} {:.2f} {:.2f}] | vtxA [{} {}] | z [{:.3f} {:.3f}] "
                 "ztest {} func {} zwrite {} | atest c{}/{} r{}/{} logic {} | painted [{} {} {} {}]",
                 perspective ? "persp" : "ortho", m_stats.ui_placed,
                 texture != nullptr ? texture->GetContentHash() : 0, blend.blend_enabled ? 1 : 0,
                 blend.src_color_factor, blend.dst_color_factor, static_cast<int>(call.blend),
                 call.tev_alpha_known ? 1 : 0, call.tev_alpha_corners[0],
                 call.tev_alpha_corners[1], call.tev_alpha_corners[2], call.tev_alpha_corners[3],
                 vtx_alpha_min > 255 ? 0 : vtx_alpha_min, vtx_alpha_max, z_min, z_max,
                 blend.depth_test ? 1 : 0, blend.depth_func, blend.depth_write ? 1 : 0,
                 blend.raw_alpha_compare0, blend.raw_alpha_compare1, blend.raw_alpha_reference0,
                 blend.raw_alpha_reference1, blend.raw_alpha_logic, painted.left, painted.top,
                 painted.right, painted.bottom);

    // The INPUTS behind a diverted perspective draw, because Super Monkey Ball
    // proved the outputs alone cannot name a placement failure: its title
    // banner diverts to ndc in the thousands and z = -11 million while the
    // very same texture composites fine through the ortho path in the same
    // frame. Whether that is a wrong modelview, a stale projection, or
    // vertices arriving in a space the null-modelview contract does not
    // cover is only readable from the raw matrix, the raw projection, the
    // viewport, and one vertex followed through the chain.
    if (perspective && !vertices.empty())
    {
      const remixapi_HardcodedVertex& v0 = vertices[0];
      float view0[3] = {v0.position[0], v0.position[1], v0.position[2]};
      if (modelview != nullptr)
      {
        for (int i = 0; i < 3; ++i)
        {
          view0[i] = modelview[i * 4 + 0] * v0.position[0] +
                     modelview[i * 4 + 1] * v0.position[1] +
                     modelview[i * 4 + 2] * v0.position[2] + modelview[i * 4 + 3];
        }
      }
      const std::string mv =
          modelview == nullptr ?
              std::string("null") :
              fmt::format("[{:.6g} {:.6g} {:.6g} {:.6g} | {:.6g} {:.6g} {:.6g} {:.6g} | "
                          "{:.6g} {:.6g} {:.6g} {:.6g}]",
                          modelview[0], modelview[1], modelview[2], modelview[3], modelview[4],
                          modelview[5], modelview[6], modelview[7], modelview[8], modelview[9],
                          modelview[10], modelview[11]);
      INFO_LOG_FMT(VIDEO,
                   "Remix persp UI inputs: mv {} | proj [{:.6g} {:.6g} {:.6g} {:.6g} {:.6g} "
                   "{:.6g}] | vp [{:.6g} {:.6g} {:.6g} {:.6g} {:.6g} {:.6g}] | v0 obj "
                   "({:.6g} {:.6g} {:.6g}) view ({:.6g} {:.6g} {:.6g})",
                   mv, raw_projection[0], raw_projection[1], raw_projection[2], raw_projection[3],
                   raw_projection[4], raw_projection[5], viewport[0], viewport[1], viewport[2],
                   viewport[3], viewport[4], viewport[5], v0.position[0], v0.position[1],
                   v0.position[2], view0[0], view0[1], view0[2]);
    }
  }

  // A full-screen opaque 2D draw arriving AFTER world geometry.
  //
  // Composited over the traced image, such a draw hides the scene outright -
  // there is nothing left of the frame to see. On console it is almost always
  // either a screen-space fake drawn underneath everything, or a fade this
  // compositing model has no way to express. Three conditions, each doing real
  // work:
  //
  //   world draws > 0 - before any world geometry this is a clear, which the
  //     pre-world rules already own; after it, the scene exists and is being
  //     covered.
  //   opaque         - a blended full-screen quad is an ordinary fade and stays.
  //     The two accepted forms are exactly the mapping to BlendMode::Opaque
  //     above: blending off, or One/Zero.
  //   >= 95% of the presented region - measured on `painted`, i.e. geometry cut
  //     down by the draw's own scissor, not on the scissor alone. A quad
  //     scissored to the whole screen that covers one corner is not this.
  //
  // Placed here, after `painted` and before either rasterizer records, so the
  // EFB compose sees the same 2D layer the overlay does - the same rule every
  // other drop above follows.
  if (!tag_bypass && m_ui_drop_fullscreen_opaque && m_frame_world_draws > 0)
  {
    const bool opaque = !blend.blend_enabled ||
                        (blend.src_color_factor == 1 && blend.dst_color_factor == 0);
    // The presented region in EFB units, which is the space `painted` is in.
    const float screen_w = m_presented_width != 0 ? static_cast<float>(m_presented_width) :
                                                    static_cast<float>(EFB_WIDTH);
    const float screen_h = m_presented_height != 0 ? static_cast<float>(m_presented_height) :
                                                     static_cast<float>(EFB_HEIGHT);
    const float painted_area = static_cast<float>(std::max(0, painted.GetWidth())) *
                               static_cast<float>(std::max(0, painted.GetHeight()));
    if (opaque && painted_area >= 0.95f * screen_w * screen_h)
    {
      ++m_stats.ui_dropped_fullscreen;
      return true;
    }
  }

  // Both stamps are decided here, at record time, because both facts are only
  // true here: how much world geometry the frame had so far, and whether the
  // user tagged this draw. The verdict that consumes them is taken at flush
  // time, when the frame's total world-draw count is finally known.
  call.world_draws_at_submit = m_frame_world_draws;
  call.tag_protected = tag_bypass;
  if (m_frame_world_draws == 0 && !tag_bypass)
    ++m_frame_preworld_unprotected;

  // The draw's real z mode, honoured by the rasterizer's depth plane. Knob off
  // leaves the fields at their zero defaults, which IS the painter's algorithm:
  // the rasterizer never learns depth existed. GX only updates z while the
  // test is enabled, so there is no test-off-write-on case to carry.
  //
  // ORTHO DRAWS ONLY, and that is a measurement, not a simplification. The 2D
  // layer's own layering is what the plane exists for, and it reproduces it
  // faithfully - Super Monkey Ball graduates its title UI across distinct
  // integer depths (21..336, func Less) and the values come out exact. A
  // DIVERTED perspective draw's z lives in a different regime entirely: its
  // geometry sits nearer than the near plane (fine for x and y, meaningless
  // through the 2D depth mapping - SMB's banners read z = -11,184,865 against
  // a legal range of 0..16,777,215), the per-pixel clamp pins that to 0 = the
  // nearest possible value, and with zwrite on it POISONS the plane: every 2D
  // draw at depth 21+ fails `Less` against 0 for the rest of the frame. That
  // was the white-boxes-eating-their-text screenshot. Diverted draws therefore
  // composite in submission order, exactly as they did before the plane
  // existed - no tagged HUD measured so far z-tests meaningfully (RE4: ztest 0
  // on all nine).
  // Additive coverage rule, stamped per draw for the same reason the depth
  // fields are: the rasterizer stays a pure function of the DrawCall and never
  // learns a knob exists.
  call.additive_light_coverage = m_ui_additive_light_coverage;

  if (m_ui_depth && blend.depth_test && !perspective)
  {
    call.depth_test = true;
    call.depth_func = blend.depth_func;
    call.depth_write = blend.depth_write;
    ++m_stats.ui_depth_tested;
  }

  m_ui_raster.Draw(call, m_ui_vertices, indices);
  ++m_stats.ui_placed;

  // The same draw, recorded a second time at the EFB's own resolution, so that
  // an EXECUTED EFB copy encodes the 2D layer the console would have had there
  // instead of bare clear colour.
  //
  // The incoming viewport and clip go in UNSCALED because they are already in
  // EFB units (RemixVertexManager.cpp:2608-2617); the scale applied above exists
  // only to map them onto the swapchain, and applying it here would be undoing
  // the coordinate system this rasterizer works in. The vertices need no
  // adjustment at all - they are NDC.
  //
  // Placed after every drop rule above, so the EFB copy sees the same 2D layer
  // the overlay does, minus nothing and plus nothing.
  if (m_efb_emulation && m_efb_ui_compose)
  {
    if (!m_efb_ui_frame_begun)
    {
      m_efb_ui_raster.Begin(EFB_WIDTH, EFB_HEIGHT);
      m_efb_ui_frame_begun = true;
    }

    UiRasterizer::DrawCall efb_call = call;
    efb_call.viewport_x = viewport[0];
    efb_call.viewport_y = viewport[1];
    efb_call.viewport_width = viewport[2];
    efb_call.viewport_height = viewport[3];
    efb_call.clip_left = static_cast<int>(std::floor(clip[0]));
    efb_call.clip_top = static_cast<int>(std::floor(clip[1]));
    // Right/bottom of an EFB rect are exclusive; the rasterizer's clip is
    // inclusive on all four sides, same conversion as above.
    efb_call.clip_right = static_cast<int>(std::ceil(clip[2])) - 1;
    efb_call.clip_bottom = static_cast<int>(std::ceil(clip[3])) - 1;
    m_efb_ui_raster.Draw(efb_call, m_ui_vertices, indices);
  }

  // Recorded after the rasterizer has taken the draw and keyed by the same
  // counter the per-draw GX dump uses (RemixVertexManager.cpp:1670 keys on
  // stats.ui_placed), so audit line i, dump line i and overlay draw i are the
  // same draw.
  if (record_footprint && m_ui_footprints.size() < MAX_UI_FOOTPRINTS)
  {
    UiDrawFootprint footprint;
    footprint.seq = m_stats.draws_seen;
    // Right/bottom exclusive, matching the EFB copy rect convention
    // (BPStructs.cpp:255-256) that this is compared against.
    footprint.left = static_cast<int>(std::floor(footprint_left));
    footprint.top = static_cast<int>(std::floor(footprint_top));
    footprint.right = static_cast<int>(std::ceil(footprint_right));
    footprint.bottom = static_cast<int>(std::ceil(footprint_bottom));
    // Clipped by the draw's own scissor: a quad the scissor cuts down only ever
    // touched the intersection, and the copy that contains THAT is the copy that
    // could have hidden it.
    footprint.left = std::max(footprint.left, static_cast<int>(clip[0]));
    footprint.top = std::max(footprint.top, static_cast<int>(clip[1]));
    footprint.right = std::min(footprint.right, static_cast<int>(clip[2]));
    footprint.bottom = std::min(footprint.bottom, static_cast<int>(clip[3]));
    footprint.texture_hash = texture != nullptr ? texture->GetContentHash() : 0;
    footprint.tev_alpha_known = blend.tev_alpha_known;
    footprint.depth_test = blend.depth_test;
    footprint.depth_write = blend.depth_write;
    m_ui_footprints.push_back(footprint);

    // What this draw ACTUALLY paints, after geometry and scissor are both
    // applied - as opposed to `clip`, which is only the scissor. A quad can be
    // scissored to the whole screen and still cover one corner, so identifying
    // draws by their clip rect (as an earlier pass here did) misattributes them.
    //
    // Everything reaching this point has already survived the drop rules above,
    // so a rectangle that appears here is a rectangle that ends up on screen.
    if (ShouldTraceDraws())
    {
      INFO_LOG_FMT(VIDEO,
                   "Remix UI paints: efb [{} {} {} {}] | clip [{:.0f} {:.0f} {:.0f} {:.0f}] | tex "
                   "{:#018x} untextured {} | blend {} | world-draws {}",
                   footprint.left, footprint.top, footprint.right, footprint.bottom, clip[0],
                   clip[1], clip[2], clip[3], footprint.texture_hash,
                   call.texture.pixels == nullptr ? 1 : 0, static_cast<int>(call.blend),
                   m_frame_world_draws);
    }
  }

  return true;
}

void RemixApi::FoldUiIntoEfb()
{
  if (!m_efb_emulation || !m_efb_ui_compose || !m_efb_ui_frame_begun)
    return;

  // Replays everything recorded since the last Begin. Cheap when nothing was
  // recorded - Flush returns immediately on an empty draw list
  // (RemixUiRaster.cpp:162-163).
  m_efb_ui_raster.Flush();

  if (m_efb_ui_raster.HasContent())
  {
    const std::vector<u32>& pixels = m_efb_ui_raster.Buffer();
    const u32 width = std::min(m_efb_ui_raster.Width(), static_cast<u32>(EFB_WIDTH));
    const u32 height = std::min(m_efb_ui_raster.Height(), static_cast<u32>(EFB_HEIGHT));

    for (u32 y = 0; y < height; ++y)
    {
      const u32* row = pixels.data() + static_cast<size_t>(y) * m_efb_ui_raster.Width();
      for (u32 x = 0; x < width; ++x)
      {
        // Packed 0xAABBGGRR, straight (non-premultiplied) alpha
        // (RemixUiRaster.h:149-151).
        const u32 src = row[x];
        const u32 src_a = src >> 24;
        // The overwhelming majority of the buffer on any real screen. Skipping
        // it is what keeps this a scan rather than a full read-modify-write of
        // the EFB.
        if (src_a == 0)
          continue;

        const u32 src_r = src & 0xFF;
        const u32 src_g = (src >> 8) & 0xFF;
        const u32 src_b = (src >> 16) & 0xFF;

        // GetColor unpacks whatever the current pixel format is into 0xRRGGBBAA
        // (SWEfbInterface.cpp:147-172 through :474-478), which is the same byte
        // order EfbWriteStoreColor packs back down from - so the round trip is
        // format-agnostic and neither end needs to know the format.
        const u32 dst = EfbInterface::GetColor(static_cast<u16>(x), static_cast<u16>(y));
        const u32 dst_r = dst >> 24;
        const u32 dst_g = (dst >> 16) & 0xFF;
        const u32 dst_b = (dst >> 8) & 0xFF;
        const u32 dst_a = dst & 0xFF;

        // Over. Fully opaque is the common case and skips the arithmetic
        // entirely; the rounding is +127 so that a = 255 is exactly src.
        u32 out_r = src_r;
        u32 out_g = src_g;
        u32 out_b = src_b;
        u32 out_a = 255;
        if (src_a != 255)
        {
          const u32 inv = 255 - src_a;
          out_r = (src_r * src_a + dst_r * inv + 127) / 255;
          out_g = (src_g * src_a + dst_g * inv + 127) / 255;
          out_b = (src_b * src_a + dst_b * inv + 127) / 255;
          out_a = src_a + (dst_a * inv + 127) / 255;
        }

        const u32 store = (out_r << 24) | (out_g << 16) | (out_b << 8) | std::min(out_a, 255u);
        EfbWriteStoreColor(static_cast<u16>(x), static_cast<u16>(y), store);
      }
    }

    ++m_stats.efb_ui_folds;
  }

  // Flush replays EVERYTHING recorded since Begin (RemixUiRaster.cpp:160-173),
  // so a second executed copy in the same frame would fold the first copy's
  // draws a second time. Re-Begin resets the draw list and clears the buffer, so
  // each fold sees only what was submitted since the previous one. This is the
  // whole guard against double-composited UI in copied textures; the
  // efb_ui_folds counter exceeding efb_copies_executed is the log-side tell that
  // it has broken.
  m_efb_ui_raster.Begin(EFB_WIDTH, EFB_HEIGHT);
}

namespace
{
// Which recorded copy, if any, swallowed this UI draw. The rule, entirely from
// console behaviour:
//   - not the XFB copy: that one ENDS the frame, everything before it is on
//     screen by definition;
//   - the clear bit is set: without it the copy reads the region and leaves it
//     alone, so the draw is still there when the XFB copy runs
//     (BPStructs.cpp:377-393 is the clear);
//   - the copy fired at or after the draw was submitted - a copy earlier in the
//     frame cannot have taken pixels that did not exist yet;
//   - the copy's rect fully CONTAINS the draw's, not merely overlaps it. A
//     partial overlap means part of the draw survived onto the screen, and the
//     console drew it.
// Returns SIZE_MAX for a live draw.
size_t FindScratchCopy(const UiDrawFootprint& draw, const std::vector<EfbCopyEvent>& copies)
{
  // A degenerate footprint is contained by everything, which would make every
  // zero-area draw a false positive.
  if (draw.right <= draw.left || draw.bottom <= draw.top)
    return SIZE_MAX;

  for (size_t i = 0; i < copies.size(); ++i)
  {
    const EfbCopyEvent& copy = copies[i];
    if (copy.xfb || !copy.clear)
      continue;
    if (copy.seq < draw.seq)
      continue;
    if (copy.rect.left <= draw.left && copy.rect.top <= draw.top &&
        copy.rect.right >= draw.right && copy.rect.bottom >= draw.bottom)
    {
      return i;
    }
  }
  return SIZE_MAX;
}
}  // namespace

void RemixApi::AuditUiFootprints()
{
  // Log-only. Every copy of the frame is already recorded by the time this runs:
  // OnAfterFrame is triggered from inside the XFB-copy handler
  // (BPStructs.cpp:353), after CopyRenderTargetToTexture, and SubmitScreenOverlay
  // is called from OnAfterFrame.
  if (!m_trace_efb_copies || !ShouldTraceDraws())
    return;
  if (m_efb_copies.empty() && m_ui_footprints.empty())
    return;

  u32 tex_copies = 0;
  u32 tex_clear_copies = 0;
  u32 xfb_copies = 0;
  for (size_t i = 0; i < m_efb_copies.size(); ++i)
  {
    const EfbCopyEvent& copy = m_efb_copies[i];
    if (copy.xfb)
      ++xfb_copies;
    else if (copy.clear)
      ++tex_clear_copies;
    else
      ++tex_copies;
    INFO_LOG_FMT(VIDEO,
                 "Remix EFB copy {}: class {} action {} | seq {} rect [{},{} -> {},{}] dst "
                 "{:#010x} fmt {} xfb {} clear {} depth {} int {} half {} yscale {:.3f} | "
                 "world-draws {} ui-draws {}",
                 i, EfbCopyClassName(copy.copy_class), copy.executed ? "exec" : "discard", copy.seq,
                 copy.rect.left, copy.rect.top, copy.rect.right, copy.rect.bottom, copy.dst_addr,
                 copy.copy_format, copy.xfb ? 1 : 0, copy.clear ? 1 : 0, copy.depth ? 1 : 0,
                 copy.intensity ? 1 : 0, copy.half_scale ? 1 : 0, copy.y_scale, copy.world_draws,
                 copy.ui_draws);
  }

  u32 scratch = 0;
  for (size_t i = 0; i < m_ui_footprints.size(); ++i)
  {
    const UiDrawFootprint& draw = m_ui_footprints[i];
    const size_t copy_index = FindScratchCopy(draw, m_efb_copies);
    if (copy_index != SIZE_MAX)
      ++scratch;
    // Capped so a screen full of 2D cannot flood the log; the summary below
    // still counts every draw.
    if (i >= 160)
      continue;
    INFO_LOG_FMT(VIDEO,
                 "Remix UI audit {}: seq {} rect [{},{} -> {},{}] tex {:#018x} tevA {} ztest {} "
                 "zwrite {} -> {}",
                 i, draw.seq, draw.left, draw.top, draw.right, draw.bottom, draw.texture_hash,
                 draw.tev_alpha_known ? 1 : 0, draw.depth_test ? 1 : 0, draw.depth_write ? 1 : 0,
                 copy_index == SIZE_MAX ? std::string("LIVE") :
                                          fmt::format("SCRATCH(copy {})", copy_index));
  }

  INFO_LOG_FMT(VIDEO,
               "Remix UI audit: {} UI draws, {} scratch, {} live | copies {} ({} tex, {} tex+clear,"
               " {} xfb)",
               m_ui_footprints.size(), scratch, m_ui_footprints.size() - scratch,
               m_efb_copies.size(), tex_copies, tex_clear_copies, xfb_copies);
}

void RemixApi::SubmitScreenOverlay()
{
  if (m_ui_mode != 1 || m_interface.DrawScreenOverlay == nullptr)
    return;

  AuditUiFootprints();

  // The pre-world verdict, taken here because here is the first point at which
  // it CAN be: whether the frame had any world geometry at all is not known
  // until the frame is over. On an all-2D frame - a menu, a loading screen, a
  // title card - m_frame_world_draws is zero and nothing is filtered, which is
  // correct: there the 2D layer IS the frame.
  //
  // Deliberately NOT applied to m_efb_ui_raster. That one is folded into an EFB
  // copy at execute time, mid-frame, when the console's EFB genuinely did
  // contain the pre-world wash (it is drawn UNDER the copy on hardware, and
  // reproducing that is the entire reason the compose exists) - and the fold may
  // well have consumed the draws before this verdict is even available.
  const bool pre_world_filter = m_ui_drop_pre_world && m_frame_world_draws > 0;
  m_ui_raster.SetPreWorldFilter(pre_world_filter);
  if (pre_world_filter)
    m_stats.ui_dropped_preworld = m_frame_preworld_unprotected;

  // Replay the frame's recorded draws across the worker bands. This is where the
  // rasterization actually happens, so it is what the raster timing measures.
  // The cache verdict is Flush's own: an unchanged frame serves the previous
  // composite and the timing reads near zero.
  m_ui_raster.SetFrameCache(m_ui_frame_cache);
  const auto raster_start = std::chrono::steady_clock::now();
  if (m_ui_frame_begun)
    m_ui_raster.Flush();
  m_stats.ui_raster_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - raster_start)
                                              .count());
  m_stats.ui_raster_cached = m_ui_raster.LastFlushWasCached();

  // Nothing drawn this frame: clear the runtime's pending overlay rather than
  // leaving the last one up. A null pointer is the documented way to do that
  // (rtx_fork_api_entry.cpp drawScreenOverlay), and it costs no upload.
  if (!m_ui_frame_begun || !m_ui_raster.HasContent())
  {
    CallGuarded("DrawScreenOverlay(clear)", [&] {
      m_interface.DrawScreenOverlay(nullptr, 0, 0, REMIXAPI_FORMAT_R8G8B8A8_UNORM, 1.0f);
    });
    return;
  }

  const std::vector<u32>& buffer = m_ui_raster.Buffer();

  // One-shot dump of the composited overlay, for checking that this path
  // produces an IMAGE rather than merely calling the API successfully - the two
  // are not the same thing and only one of them is the feature. Written over a
  // checkerboard so transparent and black are distinguishable.
  if (m_ui_dump_frame != 0 && m_frame_index == static_cast<u64>(m_ui_dump_frame))
  {
    const std::string path = File::GetUserPath(D_LOGS_IDX) + "remix-ui-overlay.bmp";
    std::ofstream out(path, std::ios::binary);
    if (out)
    {
      const u32 width = m_ui_raster.Width();
      const u32 height = m_ui_raster.Height();
      const u32 row_bytes = (width * 3 + 3) & ~3u;
      const u32 image_bytes = row_bytes * height;
      const u32 file_bytes = 54 + image_bytes;
      const auto put16 = [&](u16 v) { out.write(reinterpret_cast<const char*>(&v), 2); };
      const auto put32 = [&](u32 v) { out.write(reinterpret_cast<const char*>(&v), 4); };
      out.write("BM", 2);
      put32(file_bytes);
      put32(0);
      put32(54);
      put32(40);
      put32(width);
      put32(height);
      put16(1);
      put16(24);
      put32(0);
      put32(image_bytes);
      put32(2835);
      put32(2835);
      put32(0);
      put32(0);
      std::vector<u8> row(row_bytes, 0);
      // BMP rows run bottom-up.
      for (u32 y = height; y-- > 0;)
      {
        for (u32 x = 0; x < width; ++x)
        {
          const u32 texel = buffer[static_cast<size_t>(y) * width + x];
          const float alpha = static_cast<float>((texel >> 24) & 0xFF) / 255.0f;
          const float checker = ((x / 16 + y / 16) & 1) != 0 ? 0.35f : 0.55f;
          // The buffer is 0xAABBGGRR, so byte 0 is RED. BMP rows are B, G, R -
          // hence the reversed store below. Getting this backwards made a dumped
          // overlay look like it had its channels swapped when the overlay was
          // fine, which is worse than not dumping at all.
          const float channel[3] = {static_cast<float>(texel & 0xFF) / 255.0f,
                                    static_cast<float>((texel >> 8) & 0xFF) / 255.0f,
                                    static_cast<float>((texel >> 16) & 0xFF) / 255.0f};
          // The buffer is straight alpha, so undo nothing - just composite.
          for (int i = 0; i < 3; ++i)
          {
            const float v = channel[i] * alpha + checker * (1.0f - alpha);
            row[x * 3 + static_cast<size_t>(2 - i)] =
                static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
          }
        }
        out.write(reinterpret_cast<const char*>(row.data()), row_bytes);
      }
      INFO_LOG_FMT(VIDEO, "Remix: dumped UI overlay ({}x{}) to {}", width, height, path);
    }
  }

  const auto start = std::chrono::steady_clock::now();
  CallGuarded("DrawScreenOverlay", [&] {
    m_interface.DrawScreenOverlay(buffer.data(), m_ui_raster.Width(), m_ui_raster.Height(),
                                  REMIXAPI_FORMAT_R8G8B8A8_UNORM, 1.0f);
  });
  m_stats.ui_upload_us = static_cast<u64>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
          .count());
}

bool RemixApi::BuildWorldUiTransform(const std::array<float, 6>& raw, Affine& out) const
{
  // Maps an orthographic draw's VIEW space onto a plane in front of the camera.
  //
  //   ndc = (raw0*x + raw1, raw2*y + raw3, raw4*z + raw5)         [w = 1]
  //   world = C + F*(d + ndc.z*depth) + R*(ndc.x*half_w) + U*(ndc.y*half_h)
  //
  // Every term is affine in (x, y, z), so this is a 3x4 and composes with the
  // draw's own modelview into one instance transform. That is the whole reason
  // to do it this way rather than transforming vertices: baked vertices would
  // re-hash on every camera move and churn a mesh handle per UI element per
  // frame, which is the same trap the matrix-palette path already falls into.
  // Read back the frustum SetupCamera actually SUBMITTED rather than recovering
  // it again here. The two must agree exactly or the UI plane does not line up
  // with the frame, and re-deriving it would also fail on a screen that has no
  // perspective draw at all - which is precisely the case this exists for. A
  // game sitting on a 2D menu still gets a camera (SetupCamera falls back to a
  // fabricated one), so there is always a frustum to fill.
  const float distance = std::max(m_world_ui_distance, 0.001f);
  const float half_h = distance * std::tan(0.5f * m_camera_fov_y_deg * DEG_TO_RAD);
  const float half_w = half_h * m_camera_aspect;
  // Enough separation for a UI element's own depth values to order its layers
  // without the plane visibly tilting away from the camera.
  const float depth = distance * 0.02f;

  // Camera basis, exactly as SetupCamera hands it to the runtime - the two must
  // agree or the UI lands somewhere the camera is not looking.
  const Affine& v = m_view;
  const float tx = v[3], ty = v[7], tz = v[11];
  const float centre[3] = {-(v[0] * tx + v[4] * ty + v[8] * tz),
                           -(v[1] * tx + v[5] * ty + v[9] * tz),
                           -(v[2] * tx + v[6] * ty + v[10] * tz)};
  const float right[3] = {v[0], v[1], v[2]};
  const float up[3] = {v[4], v[5], v[6]};
  const float forward[3] = {-v[8], -v[9], -v[10]};

  const float y_sign = m_world_ui_flip_y ? -1.0f : 1.0f;
  const float scale_x = half_w * raw[0];
  const float scale_y = half_h * raw[2] * y_sign;
  // A UI projection with no depth range at all would make this column zero and
  // the transform singular, which leaves the runtime inverse-transposing a
  // degenerate matrix for its normals. Floor it; the value is arbitrary because
  // nothing depends on the scale of a dimension the game is not using.
  float scale_z = depth * raw[4];
  if (std::abs(scale_z) < depth * 1e-3f)
    scale_z = depth * 1e-3f;

  const float offset = distance + depth * raw[5];
  const float bias_x = half_w * raw[1];
  const float bias_y = half_h * raw[3] * y_sign;

  for (int i = 0; i < 3; ++i)
  {
    out[i * 4 + 0] = right[i] * scale_x;
    out[i * 4 + 1] = up[i] * scale_y;
    out[i * 4 + 2] = forward[i] * scale_z;
    out[i * 4 + 3] =
        centre[i] + forward[i] * offset + right[i] * bias_x + up[i] * bias_y;
  }
  return std::isfinite(out[3]) && std::isfinite(out[7]) && std::isfinite(out[11]);
}

void RemixApi::FlushPendingInstances()
{
  for (const PendingInstance& pending : m_pending_instances)
  {
    // Object picking: the runtime only records a pick for a non-zero value, and
    // only resolves a click to a texture when the draw carries one. Without this
    // the dev menu highlights API geometry on hover (that path reads the picking
    // buffer directly) but clicking selects nothing.
    remixapi_InstanceInfoObjectPickingEXT picking = {};
    picking.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT;
    picking.pNext = nullptr;
    picking.objectPickingValue = m_next_picking_value++;
    if (m_next_picking_value == 0)
      m_next_picking_value = 1;

    // This draw's bone palette, which is the entire handshake that makes a
    // matrix-palette mesh move: the runtime hashes it, re-skins the BLAS on
    // every frame the hash changes, and keeps the BLAS - and therefore the
    // motion vectors - when it does not. Chained off picking, which was the end
    // of the chain, so both the with-blend and without-blend arrangements pick
    // it up; the runtime's pNext walk is a search, not an order.
    remixapi_InstanceInfoBoneTransformsEXT bones_ext = {};
    if (!pending.bones.empty())
    {
      bones_ext.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
      bones_ext.pNext = nullptr;
      bones_ext.boneTransforms_values = pending.bones.data();
      bones_ext.boneTransforms_count = static_cast<u32>(pending.bones.size());
      picking.pNext = &bones_ext;
    }

    // Fixed-function texture-stage state. Without it the runtime keeps its own
    // defaults - argument 1 is the texture, argument 2 is nothing - and the
    // vertex colour we so carefully decode reaches the geometry buffer only to
    // be read by no one. Everything GX resolves per draw rather than per
    // material rides here rather than in the material, so it cannot split a mesh
    // handle.
    remixapi_InstanceInfoBlendEXT blend = {};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.pNext = &picking;
    blend.textureColorArg1Source = pending.blend.color_arg1;
    blend.textureColorArg2Source = pending.blend.color_arg2;
    blend.textureColorOperation = pending.blend.color_operation;
    blend.textureAlphaArg1Source = pending.blend.alpha_arg1;
    blend.textureAlphaArg2Source = pending.blend.alpha_arg2;
    blend.textureAlphaOperation = pending.blend.alpha_operation;
    blend.tFactor = pending.blend.tfactor;
    blend.isVertexColorBakedLighting = pending.blend.vertex_color_is_baked_lighting ? 1 : 0;
    blend.alphaTestEnabled = pending.blend.alpha_test_enabled ? 1 : 0;
    blend.alphaTestCompareOp = pending.blend.alpha_test_compare;
    blend.alphaTestReferenceValue = pending.blend.alpha_test_reference;
    blend.alphaBlendEnabled = pending.blend.blend_enabled ? 1 : 0;
    blend.srcColorBlendFactor = pending.blend.src_color_factor;
    blend.dstColorBlendFactor = pending.blend.dst_color_factor;
    blend.colorBlendOp = pending.blend.color_blend_op;
    blend.srcAlphaBlendFactor = pending.blend.src_alpha_factor;
    blend.dstAlphaBlendFactor = pending.blend.dst_alpha_factor;
    blend.alphaBlendOp = pending.blend.alpha_blend_op;
    blend.writeMask = pending.blend.write_mask;

    // Auto-detected sky. This runs after EstimateView by design, so the sticky
    // set is up to date by the time instances are placed. The runtime then
    // treats these exactly as it treats a manually tagged draw: SKY category ->
    // CameraType::Sky -> skipped entirely under rtx.skyMode = 1, which is the
    // mechanism by which Numos replaces the game's sky rather than fighting it.
    remixapi_InstanceCategoryFlags category_flags = pending.category_flags;
    if (m_sky_auto_detect >= 2 && m_sky_classified.count(pending.geometry_hash) != 0)
    {
      // An untextured classified draw takes IGNORE instead. SKY only promises
      // that the sky PATH stops drawing the geometry; it does not promise the
      // geometry leaves the scene, and a dome that survives as a closed shell
      // around the viewpoint occludes the Numos distant sun from every
      // direction - a lighting failure that presents as a dark scene rather
      // than as a sky artefact. Four of Wind Waker's seven classified meshes
      // are untextured, so this is most of the set. A textured dome keeps SKY:
      // it has albedo the sky path is designed to consume.
      const auto mesh = m_meshes.find(pending.mesh_hash);
      const bool untextured =
          mesh != m_meshes.end() && mesh->second.diagnostics.texture_hash == 0;
      if (m_sky_auto_untextured_ignore && untextured)
      {
        category_flags |= REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE;
        ++m_stats.sky_auto_ignored;
      }
      else
      {
        category_flags |= REMIXAPI_INSTANCE_CATEGORY_BIT_SKY;
      }
      ++m_stats.sky_auto_tagged;
    }

    remixapi_InstanceInfo instance = {};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    const bool need_blend_ext = m_gx_color || m_gx_blend;
    instance.pNext = need_blend_ext ? static_cast<void*>(&blend) : static_cast<void*>(&picking);
    instance.categoryFlags = category_flags;
    instance.mesh = pending.mesh;
    if (pending.world_ui)
    {
      // Already world space: BuildWorldUiTransform maps view -> WORLD, camera
      // basis included, so applying V^-1 on top would undo the placement. Built
      // per draw rather than once per frame because a game is free to use more
      // than one orthographic projection - a full-screen fade and a corner HUD
      // element need not share a frustum.
      Affine placement = {};
      if (!BuildWorldUiTransform(pending.ortho_projection, placement))
      {
        ++m_stats.ui_unplaceable;
        continue;
      }
      instance.transform =
          ToRemixTransform(AffineMultiply(placement, FromRemixTransform(pending.transform)));
      instance.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI;
      ++m_stats.ui_placed;
    }
    else
    {
      // The submitted transform is V^-1 * (C * MV), and the camera is V, so Remix
      // computes P * V * V^-1 * C * MV = P * C * MV. The rendered image is
      // therefore INVARIANT to whatever V the estimator produces - a wrong camera
      // costs temporal quality, never correctness. The one thing that can break
      // the picture is SetupCamera's basis extraction disagreeing with this V,
      // which is why that is the part to suspect if geometry ever moves wrongly.
      Affine world = m_camera_recovery ?
                         AffineMultiply(m_view_inverse, FromRemixTransform(pending.transform)) :
                         FromRemixTransform(pending.transform);

      // Sky at infinity. On console every classified sky draw is `ztest 1
      // zwrite 0`, drawn first - a hard guarantee it can never occlude anything
      // that follows. A path tracer has no draw order, so the same dome (15k-25k
      // units out against a 160k far plane in Wind Waker) is just solid geometry
      // parked in front of the island.
      //
      // Scaling the instance about the CAMERA POSITION translates that guarantee
      // into geometry exactly: for every vertex x the map is
      //     x -> p + k*(x - p)
      // which leaves (x - p), the direction from the eye, pointing exactly where
      // it did. The rendered image is therefore unchanged angle for angle while
      // the surface moves behind all world geometry. It also kills the residual
      // parallax that makes a near dome read as fake.
      //
      // As an affine, T(p) * S(k) * T(-p) = [ k*I | (1-k)*p ]. The camera
      // position p is the translation column of V^-1 - and with camera recovery
      // off that is the identity, i.e. p = 0, which is still correct because
      // that mode genuinely does put the camera at the origin.
      if (m_sky_at_infinity && m_sky_auto_detect >= 1 &&
          m_sky_classified.count(pending.geometry_hash) != 0)
      {
        const float k = m_sky_infinity_scale;
        Affine push = {};
        push[0] = k;
        push[5] = k;
        push[10] = k;
        push[3] = (1.0f - k) * m_view_inverse[3];
        push[7] = (1.0f - k) * m_view_inverse[7];
        push[11] = (1.0f - k) * m_view_inverse[11];
        world = AffineMultiply(push, world);
        ++m_stats.sky_pushed;
      }
      instance.transform = ToRemixTransform(world);
    }
    // v1 pins double-sided: GC winding under our right-handed identity view is
    // not verified, and a wrong guess would silently cull whole scenes.
    instance.doubleSided = 1;

    const u64 mesh_hash = pending.mesh_hash;
    const int guard = CallGuarded("DrawInstance", [&] { m_interface.DrawInstance(&instance); });
    if (guard != 0)
    {
      m_poisoned_meshes.insert(mesh_hash);
      continue;
    }
    ++m_stats.instances_drawn;
  }
  m_pending_instances.clear();
}

void RemixApi::EstimateView()
{
  m_stats.view_samples = static_cast<u32>(m_view_samples.size());
  m_stats.view_duplicates = static_cast<u32>(m_view_duplicate_hashes.size());
  m_view_delta_rotation_deg = 0.0f;
  m_view_delta_translation = 0.0f;
  if (!m_camera_recovery)
    return;

  // Snapshot before the estimator integrates anything, so that a histogram gate
  // miss can hold the pose the frame started with.
  const Affine view_at_entry = m_view;

  // A mesh hash submitted more than once this frame corresponds to nothing in
  // particular: emplace kept whichever instance arrived first, so pairing it
  // against last frame's first instance compares two arbitrary members of a set
  // of identical props. The delta that falls out is neither the camera's nor any
  // one object's, and Wind Waker's ocean tiles produce a lot of them. Drop them
  // from the electorate entirely rather than letting them vote on noise.
  if (m_view_electorate_fix)
  {
    for (const u64 hash : m_view_duplicate_hashes)
      m_stats.view_dup_excluded += static_cast<u32>(m_view_samples.erase(hash));
  }

  // A classified skybox votes for the camera's ROTATION delta with the
  // translation missing - not a useless hypothesis but an actively wrong one,
  // which poisons the translation consensus every time the camera moves. Drop
  // it.
  //
  // This used to be gated at >= 2, "only in tagging mode: mode 1 has to leave
  // the estimate untouched, or its log-only promise is not worth anything".
  // That justification was false. Mode 1 is not log-only and never was: with
  // RemixSkyAtInfinity, which defaults on, classification already MOVES the
  // geometry at >= 1 - as the note on that setting says outright. So the
  // exclusion was withheld to protect a promise mode 1 does not keep, while the
  // poisoning it prevents ran at the default.
  //
  // Measured on Skyward Sword (SOUE01) at mode 1, same scene, parked vs running:
  //   w_stable   94.3% -> 0.9%      tie_breaks 0 -> 44
  //   runner-up  0     -> 3/frame   (exactly the size of the classified set)
  // The runner-up bloc appears only once the camera translates and is exactly
  // the sky's own size. Parked, a skybox's translation-free delta happens to BE
  // correct, which is why this hides in any capture taken standing still.
  //
  // m_view_samples is keyed by MESH hash while the classified set holds GEOMETRY
  // hashes, so this has to go through the mesh record to translate rather than
  // erasing by key directly.
  if (m_sky_auto_detect >= 1)
  {
    for (auto it = m_view_samples.begin(); it != m_view_samples.end();)
    {
      const auto mesh = m_meshes.find(it->first);
      if (mesh != m_meshes.end() && m_sky_classified.count(mesh->second.geometry_hash) != 0)
      {
        ++m_stats.view_sky_excluded;
        it = m_view_samples.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  // GX gives us no view matrix, only combined modelviews. But for STATIC
  // geometry the world transform is constant, so it cancels across a frame
  // boundary:
  //   MV(t) * MV(t-1)^-1  ==  V(t)*W * (V(t-1)*W)^-1  ==  V(t) * V(t-1)^-1
  // Every static draw therefore votes for the same delta, and anything that
  // moved is an outlier. Nothing here knows which draws are static - the
  // consensus below is what decides, which is why it must be a vote and not an
  // average.
  // EVERY persisting draw contributes a delta and gets to vote. The cap is on
  // how many are tried as hypotheses, which is what makes the pass O(48*n)
  // rather than O(n^2) - it is not a cap on the electorate.
  //
  // Capping the voters too was the original mistake, and it did not fail
  // gracefully: the samples live in an unordered_map, so taking the first 48 of
  // a few hundred is an arbitrary slice, and a slice that happened to hold
  // mostly moving objects lost the vote even with a static scene all around it.
  // That pinned the camera to identity and re-anchored every fourth frame.
  std::vector<Affine> deltas;
  deltas.reserve(m_view_samples.size());
  for (const auto& [mesh_hash, current] : m_view_samples)
  {
    const auto previous = m_view_samples_previous.find(mesh_hash);
    if (previous == m_view_samples_previous.end())
      continue;
    Affine previous_inverse = {};
    if (!AffineInvert(previous->second, previous_inverse))
      continue;
    deltas.push_back(AffineMultiply(current, previous_inverse));
  }
  m_stats.view_candidates = static_cast<u32>(deltas.size());

  const size_t hypotheses = std::min(deltas.size(), MAX_VIEW_CANDIDATES);
  std::vector<u32> hypothesis_inliers(hypotheses, 0);
  size_t best = 0;
  u32 best_inliers = 0;
  for (size_t i = 0; i < hypotheses; ++i)
  {
    u32 inliers = 0;
    for (size_t j = 0; j < deltas.size(); ++j)
    {
      if (AffineSimilar(deltas[i], deltas[j]))
        ++inliers;
    }
    hypothesis_inliers[i] = inliers;
    if (inliers > best_inliers)
    {
      best_inliers = inliers;
      best = i;
    }
  }

  // The biggest cluster that is NOT the winner's. Two hypotheses drawn from one
  // cluster agree with each other, so the runner-up has to be tested against the
  // winner rather than just being the second-highest count. Reported either way:
  // a runner-up nearly as large as the winner is the signature of a big rigid
  // animated object competing with the static world, which is precisely the case
  // where max-inliers has no business deciding on its own.
  size_t runner_up = hypotheses;
  u32 runner_up_inliers = 0;
  if (best_inliers != 0)
  {
    for (size_t i = 0; i < hypotheses; ++i)
    {
      if (AffineSimilar(deltas[i], deltas[best]))
        continue;
      if (hypothesis_inliers[i] > runner_up_inliers)
      {
        runner_up_inliers = hypothesis_inliers[i];
        runner_up = i;
      }
    }
  }
  m_stats.view_runnerup_inliers = runner_up_inliers;

  // A genuinely turning camera makes EVERYTHING static vote together, so it wins
  // by a mile and never reaches this. Two comparable clusters mean a large rigid
  // moving thing versus the world, and "the camera is whichever moved least" is
  // the right prior there - a title screen's camera is not the part of the scene
  // swinging around.
  if (m_view_tie_break && runner_up < hypotheses &&
      runner_up_inliers * 100 >= best_inliers * VIEW_TIE_BREAK_PERCENT &&
      DeltaIsCalmer(deltas[runner_up], deltas[best]))
  {
    best = runner_up;
    best_inliers = runner_up_inliers;
    ++m_stats.view_tie_breaks;
  }
  m_stats.view_inliers = best_inliers;

  // Consensus needed to believe the winner. A pure fraction of the sample count
  // is wrong: outliers are part of that count, so a frame full of moving objects
  // raises the bar to accept the static ones - the opposite of what should
  // happen. Capping it fixes that. A dozen objects agreeing on a rigid delta is
  // decisive evidence whether they share the frame with five movers or fifty.
  const u32 required =
      std::clamp<u32>(static_cast<u32>(deltas.size()) / 4, 3, MAX_VIEW_CONSENSUS_REQUIRED);
  if (!deltas.empty() && best_inliers >= required && AffineIsRigid(deltas[best]))
  {
    // How much camera motion this frame is claiming. The rotation comes from the
    // trace of the 3x3 (trace = 1 + 2cos(theta) for a rotation), the translation
    // from the delta's own translation column - which, the delta being a
    // frame-to-frame ratio, is the distance moved rather than a position.
    const Affine& delta = deltas[best];
    m_view_delta_rotation_deg = AffineRotationDegrees(delta);
    m_view_delta_translation = AffineTranslationLength(delta);
    m_view_max_rotation_deg = std::max(m_view_max_rotation_deg, m_view_delta_rotation_deg);
    m_view_max_translation = std::max(m_view_max_translation, m_view_delta_translation);

    if (m_log_stats && m_view_delta_rotation_deg > VIEW_SPIKE_ROTATION_DEGREES &&
        s_view_spike_log_count < VIEW_SPIKE_LOG_CAP)
    {
      ++s_view_spike_log_count;
      INFO_LOG_FMT(VIDEO,
                   "Remix frame {} camera SPIKE: accepted rotation {:.3f} deg, translation {:.2f} "
                   "| inliers {}/{} (needed {}, runner-up {}) | samples {} duplicates {}",
                   m_frame_index, m_view_delta_rotation_deg, m_view_delta_translation, best_inliers,
                   deltas.size(), required, runner_up_inliers, m_view_samples.size(),
                   m_view_duplicate_hashes.size());
    }

    m_view_miss_streak = 0;
    m_view = AffineMultiply(delta, m_view);
    AffineOrthonormalize(m_view, m_gx_preserve_handedness);
  }
  else if (deltas.size() >= 2)
  {
    // No consensus: a cut, a teleport, or a frame where the estimator simply
    // could not tell. Either way we do not integrate a delta we do not believe.
    // The streak keeps a single odd frame, or a cutscene in which genuinely
    // everything moves, from tripping it; a frame with almost nothing persisting
    // is not evidence of a cut.
    //
    // What happens then used to be a reset to the identity, and both choices are
    // equally correct for the geometry - the image is invariant to V and the
    // world origin is arbitrary. They are not equally correct for the SKY: the
    // replacement atmosphere is generated from the submitted camera's world
    // basis, so re-welding world space onto whatever pose the camera holds this
    // frame swings the whole sky in one frame, and after a pitched or rolled cut
    // it leaves the horizon tilted to that pose for good. Holding costs nothing
    // - the estimator resumes integrating valid deltas either way - so hold.
    if (++m_view_miss_streak >= VIEW_MISS_STREAK_BEFORE_REANCHOR)
    {
      m_view_miss_streak = 0;
      if (!m_view_hold_on_miss)
        m_view = IDENTITY_AFFINE;
      ++m_stats.view_reanchors;
      INFO_LOG_FMT(VIDEO, "Remix: camera estimate {} at frame {} ({} deltas, {} inliers, {} needed)",
                   m_view_hold_on_miss ? "held" : "re-anchored", m_frame_index, deltas.size(),
                   best_inliers, required);
    }
  }

  // --- Stop estimating the view matrix and use the one the game wrote -------
  //
  // For any object whose model transform is the identity, the combined modelview
  // IS the view matrix - and world-authored geometry (terrain, rooms, the sea)
  // is typically drawn exactly that way. So V is not something to infer: it is
  // sitting in xfmem.posMatrices as a literal value, and the histogram says
  // which slot from EVERY draw in the frame rather than from the few dozen
  // meshes that happen to persist across the boundary. Measured on Wind Waker
  // it is slot 0 on 340 of 340 frames, rigid, shared by 160-293 meshes against a
  // runner-up pinned at 14.
  //
  // The estimator above still runs and its numbers still reach the log, so the
  // two can be read against each other on the same frame. It just no longer
  // decides. On a gate miss the pose is HELD at what it was when the frame
  // started, rather than inheriting whatever the estimator did with it - the
  // mode has to be independent of the estimator or the A/B means nothing.
  if (m_camera_from_modelview)
  {
    if (m_modelview_camera_valid)
    {
      m_view = m_modelview_camera;
      ++m_modelview_camera_accepted;
    }
    else
    {
      m_view = view_at_entry;
      ++m_modelview_camera_held;
    }
  }

  m_view_inverse_previous = m_view_inverse;
  if (!AffineInvert(m_view, m_view_inverse))
  {
    // Unreachable while m_view stays orthonormal, but an un-invertible view
    // would put every instance at the origin, so fail back to camera-relative.
    m_view = IDENTITY_AFFINE;
    m_view_inverse = IDENTITY_AFFINE;
  }

  // The real verdict on the decomposition: an object that did not move should
  // have a world transform that does not change. W = V^-1 * MV, so comparing
  // this frame's against last frame's - each built with its own V - measures
  // exactly what recovery is for. A high stable fraction means motion vectors
  // and reservoir reuse now have something to hold on to.
  //
  // Compare W(t) * W(t-1)^-1 against identity rather than W(t) against W(t-1)
  // directly. The ratio is near-identity for a genuinely static object whatever
  // its position, so its translation term is the distance the object actually
  // moved rather than where it happens to sit - which puts this on the same
  // footing as the inlier test instead of scaling with distance from origin.
  //
  // The sky classifier rides this same loop: the ratio it needs is the one
  // already being built here, and the two tests are complementary readings of
  // it. Static world geometry has ratio == identity, which is what w_stable
  // counts; a skybox has ratio == a pure translation equal to the camera's own
  // position delta, because its modelview carries the camera's ROTATION but not
  // its translation, so W = V^-1 * MV slides with the camera while its rotation
  // holds still. A camera-welded overlay fails the rotation half whenever the
  // camera turns.
  //
  // The camera's world position is the translation column of V^-1.
  const float camera_delta[3] = {m_view_inverse[3] - m_view_inverse_previous[3],
                                 m_view_inverse[7] - m_view_inverse_previous[7],
                                 m_view_inverse[11] - m_view_inverse_previous[11]};
  float near_plane = 0.0f;
  float far_plane = 0.0f;
  if (!ComputeDepthRange(near_plane, far_plane))
    far_plane = 0.0f;

  for (const auto& [mesh_hash, current] : m_view_samples)
  {
    const auto previous = m_view_samples_previous.find(mesh_hash);
    if (previous == m_view_samples_previous.end())
      continue;
    Affine previous_world_inverse = {};
    if (!AffineInvert(AffineMultiply(m_view_inverse_previous, previous->second),
                      previous_world_inverse))
    {
      continue;
    }
    ++m_stats.w_compared;
    const Affine ratio =
        AffineMultiply(AffineMultiply(m_view_inverse, current), previous_world_inverse);
    if (AffineSimilar(ratio, IDENTITY_AFFINE))
      ++m_stats.w_stable;
    if (m_sky_auto_detect != 0)
    {
      // Classification is stored under the GEOMETRY hash, which the mesh record
      // is the only place to get it from. A sample whose mesh has already been
      // reaped simply does not classify this frame.
      const auto mesh = m_meshes.find(mesh_hash);
      if (mesh != m_meshes.end())
      {
        ClassifySky(mesh->second.geometry_hash, mesh_hash, ratio, current, camera_delta,
                    far_plane);
      }
    }
  }

  m_stats.sky_auto_classified = static_cast<u32>(m_sky_classified.size());
}

void RemixApi::ClassifySky(u64 geometry_hash, u64 mesh_hash, const Affine& world_ratio,
                           const Affine& modelview, const float camera_delta[3], float far_plane)
{
  // Keyed on GEOMETRY, not on the mesh hash. The mesh hash folds in the material,
  // and classifying a mesh CHANGES its material (sky becomes emissive) and hence
  // its mesh hash - so a mesh-hash-keyed set would lose the classification the
  // instant it made one, and oscillate. See MeshEntry::geometry_hash.
  //
  // Sky must not flicker, so a classification is sticky for the session. It also
  // means a classified mesh costs nothing to re-test.
  if (m_sky_classified.count(geometry_hash) != 0)
    return;
  if (IsSkyVetoed(mesh_hash))
    return;

  // Informative-frame gate. With the camera parked, or turning without
  // translating, a skybox and the static world produce the SAME ratio and the
  // test cannot tell them apart. Such a frame neither advances nor decays a
  // candidate: no progress is made and nothing is mis-tagged, and the manual
  // RemixSkyTextures / rtx.skyBoxGeometries list remains the documented
  // fallback for exactly this case.
  const float camera_distance = std::sqrt(camera_delta[0] * camera_delta[0] +
                                          camera_delta[1] * camera_delta[1] +
                                          camera_delta[2] * camera_delta[2]);
  if (!(far_plane > 0.0f) || !(camera_distance > SKY_INFORMATIVE_FRACTION * far_plane))
    return;

  // Rotation half: the ratio's 3x3 must be the identity. A camera-welded view
  // model fails here the moment the camera turns.
  float rotation_diff_sq = 0.0f;
  for (int i = 0; i < 3; ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      const float d = world_ratio[i * 4 + j] - IDENTITY_AFFINE[i * 4 + j];
      rotation_diff_sq += d * d;
    }
  }
  const bool rotation_holds = std::sqrt(rotation_diff_sq) <= SKY_ROTATION_EPSILON;

  // Translation half: it must be the camera's own position delta. Static world
  // geometry fails here - its ratio translation is zero, which is what makes
  // this test the exact complement of w_stable.
  const float translation_error[3] = {world_ratio[3] - camera_delta[0],
                                      world_ratio[7] - camera_delta[1],
                                      world_ratio[11] - camera_delta[2]};
  const float translation_error_length =
      std::sqrt(translation_error[0] * translation_error[0] +
                translation_error[1] * translation_error[1] +
                translation_error[2] * translation_error[2]);
  const bool follows_camera =
      translation_error_length <= SKY_TRANSLATION_RELATIVE_EPSILON * camera_distance;

  // Size half: a dome spans a large fraction of the frustum. Scale the mesh's
  // object-space radius by the modelview's largest row norm, which is the most
  // any direction can be stretched by it.
  float scaled_extent = 0.0f;
  const auto mesh = m_meshes.find(mesh_hash);
  if (mesh != m_meshes.end())
  {
    float max_row_norm = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
      const float norm = std::sqrt(modelview[i * 4 + 0] * modelview[i * 4 + 0] +
                                   modelview[i * 4 + 1] * modelview[i * 4 + 1] +
                                   modelview[i * 4 + 2] * modelview[i * 4 + 2]);
      max_row_norm = std::max(max_row_norm, norm);
    }
    scaled_extent = mesh->second.object_radius * max_row_norm;
  }
  const bool big_enough = scaled_extent >= m_sky_auto_min_extent * far_plane;

  if (!(rotation_holds && follows_camera && big_enough))
  {
    // One disagreeing INFORMATIVE frame resets an unclassified candidate. A
    // moving object can match by coincidence; it cannot match persistently.
    if (const auto it = m_sky_candidates.find(geometry_hash); it != m_sky_candidates.end())
      it->second.streak = 0;
    return;
  }

  ++m_stats.sky_auto_candidates;
  if (m_sky_candidates.size() >= MAX_SKY_CANDIDATES && m_sky_candidates.count(geometry_hash) == 0)
    return;

  // Candidates are geometry-keyed too, so that a mesh which legitimately changes
  // material mid-streak (a different alpha-test variant, say) does not silently
  // restart its 30-frame count under a fresh mesh hash.
  SkyCandidate& candidate = m_sky_candidates[geometry_hash];
  ++candidate.streak;
  ++candidate.informative_frames;
  candidate.scaled_extent = scaled_extent;
  if (candidate.streak < m_sky_auto_frames)
    return;

  // The veto list names textures as readily as meshes, and the texture hash is
  // only known once the mesh has been seen at least once.
  if (mesh != m_meshes.end() && IsSkyVetoed(mesh->second.diagnostics.texture_hash))
    return;

  m_sky_classified.insert(geometry_hash);
  if (!m_log_stats || s_sky_classify_log_count >= SKY_CLASSIFY_LOG_CAP)
    return;
  ++s_sky_classify_log_count;

  // The graduation path: everything needed to make this permanent by hand is on
  // this line. The depth state and draw index are recorded to check the weaker
  // signals against reality - they are not what classified anything.
  const DrawDiagnostics& diagnostics =
      mesh != m_meshes.end() ? mesh->second.diagnostics : DrawDiagnostics{};
  INFO_LOG_FMT(VIDEO,
               "Remix SKY AUTO frame {}: mesh {:#018x} geom {:#018x} tex {:#018x} | extent {:.1f} "
               "vs far {:.1f} "
               "({:.2f}x) | {} informative frames | ztest {} zfunc {} zwrite {} draw #{} | "
               "classified set now {}",
               m_frame_index, mesh_hash, geometry_hash, diagnostics.texture_hash, scaled_extent,
               far_plane,
               far_plane > 0.0f ? scaled_extent / far_plane : 0.0f, candidate.informative_frames,
               diagnostics.depth_test ? 1 : 0, diagnostics.depth_func,
               diagnostics.depth_write ? 1 : 0, diagnostics.draw_index, m_sky_classified.size());
}

void RemixApi::LogCameraRecovery()
{
  if (!m_log_stats || !m_camera_recovery || (m_frame_index % 61) != 0)
    return;

  const Affine& v = m_view;
  const float tx = v[3], ty = v[7], tz = v[11];
  const float position[3] = {-(v[0] * tx + v[4] * ty + v[8] * tz),
                             -(v[1] * tx + v[5] * ty + v[9] * tz),
                             -(v[2] * tx + v[6] * ty + v[10] * tz)};
  const u32 stable_pct =
      m_stats.w_compared != 0 ? (m_stats.w_stable * 100) / m_stats.w_compared : 0;

  // The ABSOLUTE recovered basis, not just the per-frame delta. The maxima below
  // catch a camera that oscillates; these catch one that creeps - a run of small,
  // individually plausible deltas integrating into a large standing rotation.
  // Both produce a sky that swings while the geometry holds still, and only one
  // of them is visible frame by frame.
  const float forward[3] = {-v[8], -v[9], -v[10]};
  const float up[3] = {v[4], v[5], v[6]};

  INFO_LOG_FMT(VIDEO,
               "Remix frame {} camera: samples {} (dup {}, excluded {}) | inliers {}/{} "
               "(runner-up {}, tie-breaks {}) | sky excluded {} | stable W {}/{} ({}%) "
               "| pos ({:.1f} {:.1f} {:.1f}) fwd ({:.3f} {:.3f} {:.3f}) up ({:.3f} {:.3f} {:.3f}) "
               "| drift fwd {:.2f} deg up {:.2f} deg | max delta rot {:.3f} deg trans {:.2f}",
               m_frame_index, m_stats.view_samples, m_stats.view_duplicates,
               m_stats.view_dup_excluded, m_stats.view_inliers, m_stats.view_candidates,
               m_stats.view_runnerup_inliers, m_stats.view_tie_breaks, m_stats.view_sky_excluded,
               m_stats.w_stable, m_stats.w_compared, stable_pct,
               position[0], position[1], position[2], forward[0], forward[1], forward[2], up[0],
               up[1], up[2], AngleBetweenDegrees(forward, VIEW_REFERENCE_FORWARD),
               AngleBetweenDegrees(up, VIEW_REFERENCE_UP), m_view_max_rotation_deg,
               m_view_max_translation);

  // The maxima are per reporting window, so they mean "the worst frame in the
  // last 60" rather than "the worst frame ever".
  m_view_max_rotation_deg = 0.0f;
  m_view_max_translation = 0.0f;
}

void RemixApi::ResolveDominantModelview()
{
  m_modelview_top_valid = false;
  m_modelview_camera_valid = false;
  m_modelview_mesh_uses = 0;
  m_modelview_top_meshes = 0;
  m_modelview_top_draws = 0;
  m_modelview_top_vertices = 0;
  m_modelview_top_slots = 0;
  m_modelview_second_meshes = 0;
  m_modelview_second_draws = 0;
  m_modelview_third_meshes = 0;
  m_modelview_third_draws = 0;
  if (!m_trace_modelviews)
    return;

  const ModelviewBucket* ranked[3] = {nullptr, nullptr, nullptr};
  for (const auto& entry : m_modelviews)
  {
    const ModelviewBucket& bucket = entry.second;
    // Not the number of distinct meshes in the frame: one mesh drawn under two
    // matrices counts in both buckets. It is the right denominator anyway - the
    // question is what share of the frame's mesh-to-matrix pairings the winner
    // accounts for.
    m_modelview_mesh_uses += static_cast<u32>(bucket.meshes.size());
    for (int place = 0; place < 3; ++place)
    {
      if (ranked[place] != nullptr && !BucketOutranks(bucket, *ranked[place]))
        continue;
      for (int shift = 2; shift > place; --shift)
        ranked[shift] = ranked[shift - 1];
      ranked[place] = &bucket;
      break;
    }
  }
  if (ranked[0] == nullptr)
  {
    m_modelview_camera_streak = 0;
    return;
  }

  // Copied out as values rather than kept as pointers into the map: the log
  // pass runs at the far end of the frame, and a member pointing into a
  // container that gets cleared in between is a trap waiting to be sprung.
  m_modelview_top = ranked[0]->matrix;
  m_modelview_top_meshes = static_cast<u32>(ranked[0]->meshes.size());
  m_modelview_top_draws = ranked[0]->draws;
  m_modelview_top_vertices = ranked[0]->vertices;
  m_modelview_top_slots = ranked[0]->slots;
  m_modelview_top_mesh_set.clear();
  m_modelview_top_mesh_set.insert(ranked[0]->meshes.begin(), ranked[0]->meshes.end());
  if (ranked[1] != nullptr)
  {
    m_modelview_second_meshes = static_cast<u32>(ranked[1]->meshes.size());
    m_modelview_second_draws = ranked[1]->draws;
  }
  if (ranked[2] != nullptr)
  {
    m_modelview_third_meshes = static_cast<u32>(ranked[2]->meshes.size());
    m_modelview_third_draws = ranked[2]->draws;
  }
  m_modelview_top_valid = true;

  // --- Is the winner trustworthy enough to BE the camera? -------------------
  // Not a close call to arbitrate - a scene either has a shared authoring space
  // or it does not. Wind Waker measures 160-293 meshes against a runner-up
  // pinned at 14, an 11-20x margin, so these floors sit far under any real
  // positive and exist to catch the frame where the space is simply absent.
  //
  // The third test is that a view matrix carries no model scale, which is what
  // stops a large scaled object's space from being adopted as the camera. A
  // genuine V passes it by construction.
  const bool gate_passed =
      m_modelview_top_meshes >= MIN_MODELVIEW_CAMERA_MESHES &&
      m_modelview_second_meshes * MODELVIEW_CAMERA_DOMINANCE <= m_modelview_top_meshes &&
      AffineIsRigid(m_modelview_top);
  if (!gate_passed)
  {
    m_modelview_camera_streak = 0;
    return;
  }

  // A warm-up streak before anything is believed. The gate is a per-frame test
  // and a startup logo or a single transition frame can satisfy it by accident
  // with a handful of meshes around the origin - which is not hypothetical: the
  // first attempt latched the world offset below on exactly such a frame, at
  // t ~ 0, so the offset came out as the identity and did nothing at all. A
  // scene that really has a shared space holds it for many frames in a row.
  if (++m_modelview_camera_streak < MODELVIEW_CAMERA_WARMUP)
    return;

  // --- Move the world origin to where the camera started --------------------
  // The game's own view matrices carry translations of order 1e5 - Wind Waker's
  // Great Sea sits near (-207000, 0, 272000) - and using one directly puts every
  // instance out there too. That is not just a tracer precision tax: W = V^-1*MV
  // then differences two ~1e5 quantities down to a small one, which in float32
  // costs about 0.03 units of absolute error and puts the 5e-3 stability check
  // below its own noise floor, so the one metric available to judge this by
  // stops working. Post-multiplying by a CONSTANT translation fixes it exactly:
  //   V = A * O,  O = [I | p0]   =>   W = V^-1 * MV = O^-1 * M
  // so every world transform is just shifted by -p0 and nothing else changes.
  // Latched once, so the world frame never moves again; p0 is the camera's own
  // world position at the lock, which makes the camera start at the origin.
  if (!m_modelview_world_offset_latched)
  {
    const Affine& a = m_modelview_top;
    const float tx = a[3], ty = a[7], tz = a[11];
    m_modelview_world_offset = {1.0f, 0.0f, 0.0f, -(a[0] * tx + a[4] * ty + a[8] * tz),
                                0.0f, 1.0f, 0.0f, -(a[1] * tx + a[5] * ty + a[9] * tz),
                                0.0f, 0.0f, 1.0f, -(a[2] * tx + a[6] * ty + a[10] * tz)};
    m_modelview_world_offset_latched = true;
  }
  m_modelview_camera = AffineMultiply(m_modelview_top, m_modelview_world_offset);
  // Belt and braces: SetupCamera extracts the basis assuming R^-1 == R^T, and
  // AffineIsRigid above only holds the rows to 0.02.
  AffineOrthonormalize(m_modelview_camera, m_gx_preserve_handedness);
  m_modelview_camera_valid = true;
}

void RemixApi::LogModelviewHistogram()
{
  if (!m_trace_modelviews)
    return;

  // --- Follow the dominant bucket across the frame boundary -----------------
  // If the candidate really is the view matrix then its own inter-frame delta IS
  // the camera delta, and it should agree with what the estimator claims. Mesh
  // overlap is what says the two frames' winners are the same thing rather than
  // two unrelated matrices that each happened to win.
  u32 kept = 0;
  float candidate_rotation = 0.0f;
  float candidate_translation = 0.0f;
  bool candidate_delta_rigid = false;
  Affine candidate_previous_inverse = {};
  bool have_previous_inverse = false;
  if (m_modelview_top_valid && m_modelview_top_previous_valid)
  {
    for (const u64 mesh_hash : m_modelview_top_meshes_previous)
      kept += static_cast<u32>(m_modelview_top_mesh_set.count(mesh_hash));

    have_previous_inverse = AffineInvert(m_modelview_top_previous, candidate_previous_inverse);
    if (have_previous_inverse)
    {
      const Affine delta = AffineMultiply(m_modelview_top, candidate_previous_inverse);
      candidate_rotation = AffineRotationDegrees(delta);
      candidate_translation = AffineTranslationLength(delta);
      candidate_delta_rigid = AffineIsRigid(delta);
    }
  }

  const bool report =
      m_log_stats && m_modelview_top_valid && (m_frame_index % MODELVIEW_LOG_INTERVAL) == 0;

  // --- Score the candidate as a camera --------------------------------------
  // Exactly the metric EstimateView reports, with the recovered V replaced by
  // this candidate: W = A^-1 * MV, and a static object's W must not change
  // between frames. Run over the same electorate so the two percentages are
  // directly comparable - a candidate that scores far higher than the estimator
  // is the answer to this whole workstream.
  //
  // Worth stating what it cannot distinguish: any A = V*K for a CONSTANT K
  // scores identically, because K falls out of the ratio. A perfect score
  // identifies the view matrix only up to a fixed change of world basis, which
  // costs the geometry nothing and merely rotates the sky.
  //
  // Rotation and translation are counted SEPARATELY, and the residual
  // translation is reported as a magnitude rather than a pass/fail. At a
  // candidate translation of order 1e5 the ratio's translation is the difference
  // of two nearly equal large numbers in float32, so its absolute error is
  // roughly |t| * 1e-7 ~ 0.04 - an order of magnitude over the 5e-3 epsilon,
  // which would fail every static object on arithmetic alone. The residual is
  // what separates the two readings: ~0.1 is that precision floor, ~50 is the
  // objects genuinely moving relative to the candidate.
  u32 candidate_stable = 0;
  u32 candidate_stable_rotation = 0;
  u32 candidate_compared = 0;
  float candidate_residual = 0.0f;
  Affine candidate_inverse = {};
  if (report && have_previous_inverse && AffineInvert(m_modelview_top, candidate_inverse))
  {
    for (const auto& [mesh_hash, current] : m_view_samples)
    {
      // A hash submitted twice pairs two arbitrary members of a set of identical
      // props, so its "motion" is meaningless. EstimateView already drops these
      // when the electorate fix is on; check anyway, since this instrument has to
      // read the same with camera recovery off.
      if (m_view_duplicate_hashes.count(mesh_hash) != 0)
        continue;
      const auto previous = m_view_samples_previous.find(mesh_hash);
      if (previous == m_view_samples_previous.end())
        continue;
      Affine previous_world_inverse = {};
      if (!AffineInvert(AffineMultiply(candidate_previous_inverse, previous->second),
                        previous_world_inverse))
      {
        continue;
      }
      ++candidate_compared;
      const Affine ratio =
          AffineMultiply(AffineMultiply(candidate_inverse, current), previous_world_inverse);
      if (AffineSimilar(ratio, IDENTITY_AFFINE))
        ++candidate_stable;
      if (AffineRotationDegrees(ratio) < 0.2f)
      {
        // Only meaningful for objects the candidate already holds still
        // rotationally - averaging in a genuinely moving object's translation
        // would drown the precision floor we are trying to read.
        ++candidate_stable_rotation;
        candidate_residual += AffineTranslationLength(ratio);
      }
    }
    if (candidate_stable_rotation != 0)
      candidate_residual /= static_cast<float>(candidate_stable_rotation);
  }

  if (report)
  {
    const u32 share = m_modelview_mesh_uses != 0 ?
                          (m_modelview_top_meshes * 100) / m_modelview_mesh_uses : 0;
    INFO_LOG_FMT(VIDEO,
                 "Remix frame {} modelviews: draws {} (palette {}, overflow {}) | {} distinct "
                 "matrices over {} mesh-uses | TOP {} meshes ({}%), {} draws, {} verts, slots "
                 "{:#x} | #2 {} meshes {} draws | #3 {} meshes {} draws | camera {} (accepted "
                 "{}, held {} since last report)",
                 m_frame_index, m_stats.modelview_samples, m_stats.modelview_palette,
                 m_modelview_overflow, m_modelviews.size(), m_modelview_mesh_uses,
                 m_modelview_top_meshes, share, m_modelview_top_draws, m_modelview_top_vertices,
                 m_modelview_top_slots, m_modelview_second_meshes, m_modelview_second_draws,
                 m_modelview_third_meshes, m_modelview_third_draws,
                 !m_camera_from_modelview ? "estimator (mode off)" :
                     (m_modelview_camera_valid ? "HISTOGRAM" : "held (gate missed)"),
                 m_modelview_camera_accepted, m_modelview_camera_held);
    m_modelview_camera_accepted = 0;
    m_modelview_camera_held = 0;

    const u32 candidate_pct =
        candidate_compared != 0 ? (candidate_stable * 100) / candidate_compared : 0;
    const u32 estimator_pct =
        m_stats.w_compared != 0 ? (m_stats.w_stable * 100) / m_stats.w_compared : 0;
    INFO_LOG_FMT(VIDEO,
                 "Remix frame {} modelview TOP: [{:.4f} {:.4f} {:.4f} {:.2f} / {:.4f} {:.4f} "
                 "{:.4f} {:.2f} / {:.4f} {:.4f} {:.4f} {:.2f}] | rigid {} det {:.4f} | kept {}/{} "
                 "meshes | its delta rot {:.3f} deg trans {:.2f} (rigid {}) vs estimator rot "
                 "{:.3f} trans {:.2f} | stable W {}/{} ({}%) [rot-only {}, mean residual "
                 "{:.4f}] vs estimator {}/{} ({}%)",
                 m_frame_index, m_modelview_top[0], m_modelview_top[1], m_modelview_top[2],
                 m_modelview_top[3], m_modelview_top[4], m_modelview_top[5], m_modelview_top[6],
                 m_modelview_top[7], m_modelview_top[8], m_modelview_top[9], m_modelview_top[10],
                 m_modelview_top[11],
                 AffineIsRigid(m_modelview_top) ? "yes" : "NO", AffineDeterminant(m_modelview_top),
                 kept,
                 m_modelview_top_meshes_previous.size(), candidate_rotation, candidate_translation,
                 candidate_delta_rigid ? "yes" : "NO", m_view_delta_rotation_deg,
                 m_view_delta_translation, candidate_stable, candidate_compared, candidate_pct,
                 candidate_stable_rotation, candidate_residual, m_stats.w_stable,
                 m_stats.w_compared, estimator_pct);
  }

  // Roll the state EVERY frame, printing or not: a candidate delta measured
  // across a 30-frame gap would not be a camera delta.
  if (m_modelview_top_valid)
  {
    m_modelview_top_previous = m_modelview_top;
    m_modelview_top_meshes_previous.swap(m_modelview_top_mesh_set);
    m_modelview_top_previous_valid = true;
  }
  else
  {
    m_modelview_top_previous_valid = false;
  }
  m_modelviews.clear();
  m_modelview_overflow = 0;
}

// Union of every projection variant's depth range this frame. Taking the union
// is what stops a draw with a longer far plane from being clipped by whichever
// projection happened to latch the reference; every variant is walked, so the
// reference is included wherever the gate let it land in the table.
bool RemixApi::ComputeDepthRange(float& near_plane, float& far_plane) const
{
  bool depth_valid = false;
  for (u32 i = 0; i < m_projection_variant_count; ++i)
  {
    const std::array<float, 6>& variant = m_projection_variants[i].raw;
    if (std::abs(variant[4]) <= 0.0000001f || std::abs(variant[4] - 1.0f) <= 0.0000001f)
      continue;
    const float variant_far = variant[5] / variant[4];
    const float variant_near = variant[5] / (variant[4] - 1.0f);
    // Never let a transient degenerate frustum reach the tracer.
    if (!(variant_near > 0.0001f) || !(variant_far > variant_near))
      continue;
    near_plane = depth_valid ? std::min(near_plane, variant_near) : variant_near;
    far_plane = depth_valid ? std::max(far_plane, variant_far) : variant_far;
    depth_valid = true;
  }
  return depth_valid;
}

void RemixApi::SetupCamera()
{
  // GX has no separate view matrix - xfmem.posMatrices hold combined
  // object-to-view transforms - so v1 uses an identity view: instances carry
  // the modelview, and the camera sits at the origin looking down -Z (GC view
  // space is right-handed with -Z forward; the perspective matrix's m[14] is
  // -1). Submitting the camera in parameterized form keeps GC's depth-range and
  // reversed-Z conventions from ever crossing the API.
  remixapi_CameraInfoParameterizedEXT params = {};
  params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
  params.pNext = nullptr;
  params.position = {0.0f, 0.0f, 0.0f};
  params.forward = {0.0f, 0.0f, -1.0f};
  params.up = {0.0f, 1.0f, 0.0f};
  params.right = {1.0f, 0.0f, 0.0f};
  if (m_camera_recovery)
  {
    // Handedness, because it is not symmetric and the defaults above hide it:
    // the runtime builds viewToWorld with rows (right, up, forward, position)
    // and projects with PROJ_LEFT_HANDED, so REMIX view space is left-handed
    // looking down +Z (rtx_remix_api.cpp, toRtCamera). GC view space is
    // right-handed looking down -Z. The identity-view defaults above bridge
    // that by passing forward = -Z, which makes viewToWorld diag(1,1,-1) - a
    // reflection, not a rotation. That is deliberate and load-bearing.
    //
    // Generalising it: we need worldToView_remix = F * m_view with
    // F = diag(1,1,-1), so viewToWorld_remix = m_view^-1 * F. Writing m_view as
    // [R|t] and reading off the columns of R^T * F gives the basis below - the
    // world direction of each view axis is the corresponding ROW of R, with the
    // third negated by F. The camera position is the point mapping to the view
    // origin, -R^T t.
    //
    // With m_view identity this reproduces the constants above term for term,
    // which is the cheapest standing check that the convention has not drifted.
    const Affine& v = m_view;
    const float tx = v[3], ty = v[7], tz = v[11];
    params.position = {-(v[0] * tx + v[4] * ty + v[8] * tz),
                       -(v[1] * tx + v[5] * ty + v[9] * tz),
                       -(v[2] * tx + v[6] * ty + v[10] * tz)};
    params.right = {v[0], v[1], v[2]};
    params.up = {v[4], v[5], v[6]};
    params.forward = {-v[8], -v[9], -v[10]};
  }
  params.fovYInDegrees = 60.0f;
  params.aspect = 16.0f / 9.0f;
  params.nearPlane = 1.0f;
  params.farPlane = 10000.0f;

  if (m_projection_latched)
  {
    const float* raw = m_raw_projection.data();
    // The GX perspective matrix Dolphin builds is
    //   m[0] = raw[0] = 1 / (tan(fovY/2) * aspect)
    //   m[5] = raw[2] = 1 / tan(fovY/2)
    //   m[10] = raw[4] = -n / (f - n)
    //   m[11] = raw[5] = -f * n / (f - n)
    // hence fovY = 2*atan(1/raw[2]), aspect = raw[2]/raw[0],
    // far = raw[5]/raw[4] and near = raw[5]/(raw[4] - 1).
    float fov_y_deg = 0.0f;
    float aspect = 0.0f;
    if (RecoverFovAspect(m_raw_projection, fov_y_deg, aspect))
    {
      params.fovYInDegrees = fov_y_deg;
      params.aspect = aspect;
    }
    // Depth is the one part of the projection that folding onto the instance
    // transform cannot carry: the correction leaves view-space z alone, so each
    // draw keeps its own depth range and the camera has to span all of them.
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    const bool depth_valid = ComputeDepthRange(near_plane, far_plane);
    if (depth_valid)
    {
      params.nearPlane = std::clamp(near_plane, 0.01f, 100.0f);
      params.farPlane = std::clamp(far_plane, params.nearPlane + 1.0f, 10000000.0f);
    }

    // Log the recovered frustum once: the near/far recovery above is derived,
    // not measured, and these numbers are how you check it against the game.
    if (!m_camera_valid)
    {
      INFO_LOG_FMT(VIDEO,
                   "Remix: first camera - fovY {:.2f} deg, aspect {:.3f}, near {:.3f}, far {:.1f} "
                   "(raw {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f})",
                   params.fovYInDegrees, params.aspect, params.nearPlane, params.farPlane, raw[0],
                   raw[1], raw[2], raw[3], raw[4], raw[5]);
    }
    m_camera_valid = true;
  }
  // Whatever frustum ended up being submitted - recovered or fallback - is what
  // the world-space UI plane has to fill, so publish it rather than letting
  // BuildWorldUiTransform derive its own and drift.
  m_camera_fov_y_deg = params.fovYInDegrees;
  m_camera_aspect = params.aspect;

  // If no perspective draw has ever been seen the hardcoded default above
  // stands, which is what keeps the Milestone 0 triangle visible.

  remixapi_CameraInfo camera = {};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &params;
  camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
  CallGuarded("SetupCamera", [&] { m_interface.SetupCamera(&camera); });
}

void RemixApi::SubmitLights()
{
  // GX lights are specified in view space. With recovery off that IS our world
  // space and they need no basis change; with it on they have to be carried
  // into world space by V^-1 exactly as the geometry is, or the lighting stays
  // welded to the camera while the world holds still.
  //
  // The enable mask and the attenuation function both live in the per-channel
  // lighting configs rather than on the lights themselves, which makes them
  // per-DRAW state. Reading them here, at frame end, saw only whatever the last
  // draw of the frame happened to leave in xfmem - and in Wind Waker that is a
  // channel with lighting switched off, so every light in the scene was dropped
  // while the geometry that needed them rendered on the fallback. The draw path
  // accumulates them instead; see NoteDrawLights.
  //
  // The light REGISTERS are accumulated the same way - see NoteDrawLights, which
  // snapshots each slot at the draw that first claimed it. Nothing here reads
  // xfmem.lights directly any more.
  const u32 light_mask = m_frame_light_mask;
  const std::array<u8, 8>& attenuation = m_frame_light_attenuation;

  u32 drawn = 0;
  for (u32 i = 0; i < m_lights.size(); ++i)
  {
    if ((light_mask & (1u << i)) == 0)
      continue;

    const Light& src = m_frame_lights[i];
    // xfmem light colors are packed abgr in u8[4].
    const float r = static_cast<float>(src.color[3]) / 255.0f;
    const float g = static_cast<float>(src.color[2]) / 255.0f;
    const float b = static_cast<float>(src.color[1]) / 255.0f;
    if (r <= 0.0f && g <= 0.0f && b <= 0.0f)
      continue;
    // Claimed only by an alpha channel: GX would feed this light into a
    // channel's alpha alone (color[0]), not its RGB. Counted, not acted on.
    if ((m_frame_light_color_mask & (1u << i)) == 0)
      ++m_stats.lights_alpha_only;

    // GX has no distinct light TYPES - the attenuation function decides what the
    // same bytes mean, and getting this wrong is not subtle:
    //
    //   None / Dir : attn is literally 1.0 (TransformUnit.cpp CalculateLightAttn)
    //                - no distance falloff whatsoever. Games build suns this way,
    //                as an ordinary light parked hundreds of thousands of units
    //                away. Handing that to a sphere light applies real 1/r^2 and
    //                the sun contributes essentially nothing, so the scene goes
    //                black - while still counting as "a light was drawn" and thus
    //                suppressing the fallback that would have saved it.
    //   Spec       : dpos/ddir are the OTHER arm of a union - they are sdir and
    //                shalfangle, i.e. DIRECTIONS. Read as a position, a unit
    //                vector puts the light basically at the view origin.
    //   Spot       : a genuine positional light with a cone, cosatt/distatt.
    //
    // So only Spot is really a point light. The rest are directional.
    const AttenuationFunc attnfunc = static_cast<AttenuationFunc>(attenuation[i]);
    if (attnfunc == AttenuationFunc::Spec)
      ++m_stats.lights_spec;

    // ...and a fourth case the list above misses, because it is a Spot by
    // declaration only. GX_DA_OFF leaves distatt = (1,0,0), so the distance
    // polynomial 1/(c + b*d + a*d^2) is the constant 1; GX_AF_NONE leaves
    // cosatt = (1,0,0), so the angular polynomial is constant too. A Spot with
    // both switched off therefore has no falloff of ANY kind - it is a
    // directional light wearing a positional light's clothes, which is exactly
    // how a GC title builds a sun. Wind Waker's is at |dpos| 24873 with
    // distatt (1,0,0) and cosatt (1,0,0), i.e. a 180-degree cone.
    //
    // Handing that to a Remix sphere applies a real 1/r^2 the console never
    // applied, so at 25000 units the sun contributes essentially nothing - and
    // because it still counts as "a light was drawn" it also suppresses
    // rtx.fallbackLightMode = NoLightsPresent. The scene renders black with a
    // light in it, which is the state Wind Waker was actually in.
    //
    // Only the fully falloff-free case is rerouted. A Spot that keeps a genuine
    // cone still has angular shaping worth reproducing, and Remix's distant
    // light cannot express one, so that stays a sphere.
    constexpr float ATTENUATION_EPSILON = 1e-6f;
    const bool has_distance_falloff = std::abs(src.distatt[1]) > ATTENUATION_EPSILON ||
                                      std::abs(src.distatt[2]) > ATTENUATION_EPSILON;
    const bool has_angular_falloff = std::abs(src.cosatt[1]) > ATTENUATION_EPSILON ||
                                     std::abs(src.cosatt[2]) > ATTENUATION_EPSILON;
    const bool falloff_free_spot =
        attnfunc == AttenuationFunc::Spot && !has_distance_falloff && !has_angular_falloff;
    if (falloff_free_spot)
      ++m_stats.lights_falloff_free;

    // The distant branch below reads `vector` as a DIRECTION (w = 0), which is
    // what this case needs: dpos is the light's view-space position, the scene
    // sits at the view origin, so -normalize(R^-1 * dpos) is the direction of
    // travel from a light that far away. Flipping is_positional therefore does
    // the whole translation - position handling, radiance convention and all.
    const bool is_positional =
        attnfunc == AttenuationFunc::Spot && !(m_gx_light_no_falloff_distant && falloff_free_spot);

    // Drop the game's own suns, so an atmosphere mod owns the key light.
    //
    // A GC title's sun is a baked-in directional light with a fixed colour and
    // direction that knows nothing about a physically-modelled sky. Run both and
    // they double up: the scene reads far too bright and the game's flat white
    // fights whatever the atmosphere is doing. Numos is the case this exists for.
    //
    // Positional lights are kept. Lamps, glows and cone lights are local set
    // dressing an atmosphere model does not replace, and dropping them would put
    // the scene's interiors in the dark.
    //
    // Note this can leave a frame with NO lights at all, which lets the runtime's
    // rtx.fallbackLightMode reach NoLightsPresent - the opposite of the trap
    // described above, and here it is the desired outcome rather than an
    // accident. If the scene goes black instead of sky-lit, that fallback is the
    // first thing to check.
    if (!is_positional && m_gx_light_drop_distant)
    {
      ++m_stats.lights_dropped_distant;
      continue;
    }

    // Remix always renders a clamped N.L. GX's other two diffuse functions have
    // no translation at all - None makes the light behave as pure ambient, Sign
    // lets it darken a surface - so a scene lit mainly by them reads dark and
    // these counters are how the log says so.
    const DiffuseFunc diffusefunc = static_cast<DiffuseFunc>(m_frame_light_diffuse[i]);
    if (diffusefunc == DiffuseFunc::None)
      ++m_stats.lights_diffuse_none;
    else if (diffusefunc == DiffuseFunc::Sign)
      ++m_stats.lights_diffuse_sign;

    float vector[3] = {src.dpos[0], src.dpos[1], src.dpos[2]};
    const float source_distance =
        std::sqrt(src.dpos[0] * src.dpos[0] + src.dpos[1] * src.dpos[1] + src.dpos[2] * src.dpos[2]);
    if (m_camera_recovery)
    {
      // Positions take the translation, directions do not.
      const Affine& iv = m_view_inverse;
      const float x = vector[0], y = vector[1], z = vector[2];
      const float w = is_positional ? 1.0f : 0.0f;
      vector[0] = iv[0] * x + iv[1] * y + iv[2] * z + iv[3] * w;
      vector[1] = iv[4] * x + iv[5] * y + iv[6] * z + iv[7] * w;
      vector[2] = iv[8] * x + iv[9] * y + iv[10] * z + iv[11] * w;
    }

    remixapi_LightInfoDistantEXT distant = {};
    remixapi_LightInfoSphereEXT sphere = {};
    void* light_ext = nullptr;

    // Radiance. Pre-fix this was the raw 0-1 colour, which is roughly two orders
    // of magnitude short of what the runtime's own legacy-light conversion
    // produces for the same attenuation curve; the calibrated path solves the
    // GX distance-attenuation polynomial for where the light ends and derives
    // the radiance from that. Colours stay in gamma space deliberately, exactly
    // as rtx_light_utils.cpp:190-193 does.
    const float brightness = std::max({r, g, b});
    float radiance[3] = {r * m_light_scale, g * m_light_scale, b * m_light_scale};
    const float sphere_radius = m_gx_light_fix ? SPHERE_LIGHT_RADIUS : 5.0f;
    float end_distance = 0.0f;
    if (m_gx_light_fix && brightness > 0.0f)
    {
      // A distant light has no falloff to solve, so the runtime's directional
      // precedent applies instead: normalized colour at a fixed intensity
      // (rtx_lights_data.cpp:359 with lightConversionDistantLightFixedIntensity,
      // rtx_light_manager.h:245, whose default is 1.0). m_light_scale plays that
      // multiplier here so RemixLightScale stays the single brightness knob and
      // its own default of 1.0 reproduces the runtime's number exactly.
      float intensity = 1.0f;
      if (is_positional)
      {
        end_distance = SolveLightEndDistance(src.distatt, brightness, m_light_range);
        intensity = LightEndDistanceToRadiance(end_distance, sphere_radius);
      }
      radiance[0] = (r / brightness) * intensity * m_light_scale;
      radiance[1] = (g / brightness) * intensity * m_light_scale;
      radiance[2] = (b / brightness) * intensity * m_light_scale;
    }

    if (is_positional)
    {
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.pNext = nullptr;
      sphere.position = {vector[0], vector[1], vector[2]};
      // GX point lights are analytically infinitesimal; give them a small but
      // non-zero radius so the tracer produces soft rather than hard shadows.
      // The fixed 4.0 is the runtime's own converted-light radius
      // (rtx_light_manager.h:243), and the radiance formula above is derived
      // against it, so the two are not independently choosable.
      sphere.radius = sphere_radius;
      sphere.volumetricRadianceScale = 1.0f;

      // The spot cone is the angle at which the cosatt polynomial in
      // cos(theta) reaches zero. Solve it rather than guessing a width; fall
      // back to a hemisphere when the polynomial has no usable root.
      //
      // Sign: both Dolphin renderers compute attn = max(0, dot(ldir, ddir))
      // with ldir = normalize(light.pos - vertex) (TransformUnit.cpp:245-255,
      // LightingShaderGen.cpp:43-49), so xfmem's ddir points from the SCENE
      // TOWARD the light. Remix's shaping direction is the EMISSION axis
      // (rtx_lights_data.cpp:399-411) - the other way round. Pre-fix this
      // submitted +ddir, i.e. a cone aimed away from everything it was lighting.
      const float ddir_sign = m_gx_light_fix ? -1.0f : 1.0f;
      float direction[3] = {ddir_sign * src.ddir[0], ddir_sign * src.ddir[1],
                            ddir_sign * src.ddir[2]};
      if (m_camera_recovery)
      {
        const Affine& iv = m_view_inverse;
        const float x = direction[0], y = direction[1], z = direction[2];
        direction[0] = iv[0] * x + iv[1] * y + iv[2] * z;
        direction[1] = iv[4] * x + iv[5] * y + iv[6] * z;
        direction[2] = iv[8] * x + iv[9] * y + iv[10] * z;
      }
      Normalize3(direction);
      if (direction[0] != 0.0f || direction[1] != 0.0f || direction[2] != 0.0f)
      {
        float outer_degrees = 180.0f;
        float inner_degrees = 180.0f;
        SolveSpotCone(src.cosatt, outer_degrees, inner_degrees);
        sphere.shaping_hasvalue = 1;
        sphere.shaping_value.direction = {direction[0], direction[1], direction[2]};
        sphere.shaping_value.coneAngleDegrees = outer_degrees;
        // cos(inner) - cos(outer), the runtime's own D3D9 formula
        // (rtx_lights_data.cpp:419). Pre-fix this was 0, a hard-edged cone.
        sphere.shaping_value.coneSoftness =
            m_gx_light_fix ? std::max(0.0f, std::cos(inner_degrees * DEG_TO_RAD) -
                                                std::cos(outer_degrees * DEG_TO_RAD)) :
                             0.0f;
        sphere.shaping_value.focusExponent = 0.0f;
      }
      light_ext = &sphere;
      ++m_stats.lights_sphere;
      if (sphere.shaping_hasvalue != 0)
        ++m_stats.lights_spot;
    }
    else
    {
      // Direction of TRAVEL, so the light points from its position toward the
      // scene: the negated, normalised light vector.
      float direction[3] = {-vector[0], -vector[1], -vector[2]};
      Normalize3(direction);
      if (direction[0] == 0.0f && direction[1] == 0.0f && direction[2] == 0.0f)
        continue;
      distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
      distant.pNext = nullptr;
      distant.direction = {direction[0], direction[1], direction[2]};
      // Roughly the sun's angular size, which is what this usually is.
      distant.angularDiameterDegrees = 0.5f;
      distant.volumetricRadianceScale = 1.0f;
      light_ext = &distant;
      ++m_stats.lights_distant;
    }

    remixapi_LightInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = light_ext;
    info.hash = XF_LIGHT_HASH_BASE + i;
    info.radiance = {radiance[0], radiance[1], radiance[2]};
    info.isDynamic = 1;
    info.ignoreViewModel = 0;

    LightEntry& entry = m_lights[i];

    // One line per light whose translation materially changed - a create, a kind
    // change, a recoloured or re-shaped light. Self-silencing and capped: the
    // fingerprint deliberately leaves the POSITION out, so a light that merely
    // moves stays quiet, and the cap stops a light that flickers every frame
    // from owning the log. This is the evidence for the cone axis and the
    // radiance calibration, and the first thing to read on any future "the
    // lights look wrong" report.
    if (m_log_stats)
    {
      u64 trace_key = static_cast<u64>(attenuation[i]) | (static_cast<u64>(diffusefunc) << 8) |
                      (static_cast<u64>(is_positional ? 1 : 0) << 16);
      trace_key = FoldHash(trace_key, static_cast<u64>(src.color[0]) |
                                          (static_cast<u64>(src.color[1]) << 8) |
                                          (static_cast<u64>(src.color[2]) << 16) |
                                          (static_cast<u64>(src.color[3]) << 24));
      // The world-space AXIS is folded in, quantized to ~0.02, so that a light
      // whose direction changes materially re-logs. It was deliberately left out
      // along with the position, on the reasoning that a light which merely
      // moves should stay quiet - but for a DISTANT light the axis is the whole
      // translation, and excluding it meant the sun's direction was only ever
      // logged once, at frame 500, before the camera had warmed up and before
      // the game reached 3D. That is not a reading of anything.
      const float axis[3] = {is_positional ? sphere.shaping_value.direction.x : distant.direction.x,
                             is_positional ? sphere.shaping_value.direction.y : distant.direction.y,
                             is_positional ? sphere.shaping_value.direction.z :
                                             distant.direction.z};
      const float quantized[7] = {sphere.shaping_value.coneAngleDegrees,
                                  sphere.shaping_value.coneSoftness,
                                  radiance[0],
                                  end_distance,
                                  std::round(axis[0] * 50.0f),
                                  std::round(axis[1] * 50.0f),
                                  std::round(axis[2] * 50.0f)};
      trace_key = XXH64(quantized, sizeof(quantized), trace_key);
      if (trace_key != entry.trace_key && s_light_trace_count < LIGHT_TRACE_LOG_CAP)
      {
        ++s_light_trace_count;
        INFO_LOG_FMT(VIDEO,
                     "Remix light {} @frame {}: attn {} diffuse {} | {} | colour ({:.3f} {:.3f} "
                     "{:.3f}) | |dpos| {:.1f} | cosatt ({:.4f} {:.4f} {:.4f}) distatt ({:.6f} "
                     "{:.6f} {:.6f}) | cone {:.1f} deg softness {:.4f} axis ({:.3f} {:.3f} {:.3f}) "
                     "| end {:.1f} -> radiance ({:.3f} {:.3f} {:.3f})",
                     i, m_frame_index, attnfunc, diffusefunc,
                     is_positional ? "sphere" : "distant", r, g, b, source_distance, src.cosatt[0],
                     src.cosatt[1], src.cosatt[2], src.distatt[0], src.distatt[1], src.distatt[2],
                     sphere.shaping_value.coneAngleDegrees, sphere.shaping_value.coneSoftness,
                     is_positional ? sphere.shaping_value.direction.x : distant.direction.x,
                     is_positional ? sphere.shaping_value.direction.y : distant.direction.y,
                     is_positional ? sphere.shaping_value.direction.z : distant.direction.z,
                     end_distance, radiance[0], radiance[1], radiance[2]);
      }
      entry.trace_key = trace_key;
    }
    // A light can change kind between frames when the channel referencing it
    // changes attenuation function. Remix bakes the light type in at create
    // time, so an update cannot carry a sphere handle over to a distant one -
    // the handle has to be retired and remade.
    if (entry.handle != nullptr && entry.positional != is_positional)
    {
      remixapi_LightHandle stale = entry.handle;
      CallGuarded("DestroyLight(kind change)", [&] { m_interface.DestroyLight(stale); });
      entry.handle = nullptr;
    }
    entry.positional = is_positional;

    if (entry.handle == nullptr)
    {
      remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      CallGuarded("CreateLight",
                  [&] { status = m_interface.CreateLight(&info, &entry.handle); });
      if (status != REMIXAPI_ERROR_CODE_SUCCESS)
        entry.handle = nullptr;
    }
    else if (m_interface.UpdateLightDefinition != nullptr)
    {
      // Handles are hashes and immutable at create time, so per-frame parameter
      // changes go through the update path rather than a re-create.
      remixapi_LightHandle handle = entry.handle;
      CallGuarded("UpdateLightDefinition",
                  [&] { m_interface.UpdateLightDefinition(handle, &info); });
    }

    if (entry.handle != nullptr)
    {
      remixapi_LightHandle handle = entry.handle;
      CallGuarded("DrawLightInstance", [&] { m_interface.DrawLightInstance(handle); });
      ++drawn;
    }
  }

  if (drawn != 0)
    return;

  // The fallback exists for a scene with no light at all. An atmosphere mod IS
  // that light, so inventing another one on top of it is exactly the clash the
  // drop knob was turned on to remove - and dropping the game's suns only to
  // replace them with ours would make that knob do nothing visible.
  //
  // This is why RemixGxLightDropDistant appeared not to work: dropping all four
  // of BFBB's suns left `drawn` at zero, so this fired and put a distant light
  // straight back. The Remix light statistics showed 2 distant lights with the
  // backend reporting `lights 0 distant, 4 dropped`, which is the tell.
  if (!m_fallback_light_enabled || m_gx_light_drop_distant)
    return;

  // Nothing lit the scene this frame (very common: many GC titles bake lighting
  // into vertex colors and enable no XF lights at all). Fall back to one
  // persistent distant light so the path tracer has something to integrate.
  if (m_fallback_light == nullptr)
  {
    remixapi_LightInfoDistantEXT distant = {};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.pNext = nullptr;
    // Down and slightly behind the camera, in view space.
    distant.direction = {0.19611613f, -0.58834841f, -0.78446454f};
    distant.angularDiameterDegrees = 0.53f;
    distant.volumetricRadianceScale = 1.0f;

    remixapi_LightInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &distant;
    info.hash = FALLBACK_LIGHT_HASH;
    info.radiance = {3.0f * m_light_scale, 3.0f * m_light_scale, 3.0f * m_light_scale};
    info.isDynamic = 0;
    info.ignoreViewModel = 0;

    remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    CallGuarded("CreateLight(fallback)",
                [&] { status = m_interface.CreateLight(&info, &m_fallback_light); });
    if (status != REMIXAPI_ERROR_CODE_SUCCESS)
      m_fallback_light = nullptr;
  }

  if (m_fallback_light != nullptr)
  {
    remixapi_LightHandle handle = m_fallback_light;
    CallGuarded("DrawLightInstance(fallback)",
                [&] { m_interface.DrawLightInstance(handle); });
  }
}

bool RemixApi::EnsureFallbackMesh()
{
  if (m_fallback_mesh != nullptr)
    return true;

  // Alpha test 7 == none: the fallback triangle must never be cut away.
  const MaterialRef material = EnsureMaterial(nullptr, 1, 1, 1, 7, 0);
  if (material.handle == nullptr)
    return false;

  // One triangle a couple of units down -Z, i.e. straight ahead of the
  // identity-view camera.
  std::array<remixapi_HardcodedVertex, 3> vertices = {};
  const float positions[3][3] = {
      {-0.5f, -0.5f, -2.0f},
      {0.5f, -0.5f, -2.0f},
      {0.0f, 0.5f, -2.0f},
  };
  const float texcoords[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
  for (size_t i = 0; i < vertices.size(); ++i)
  {
    vertices[i].position[0] = positions[i][0];
    vertices[i].position[1] = positions[i][1];
    vertices[i].position[2] = positions[i][2];
    vertices[i].normal[0] = 0.0f;
    vertices[i].normal[1] = 0.0f;
    vertices[i].normal[2] = 1.0f;
    vertices[i].texcoord[0] = texcoords[i][0];
    vertices[i].texcoord[1] = texcoords[i][1];
    vertices[i].color = 0xFFFFFFFFu;
  }
  const std::array<u32, 3> indices = {0, 1, 2};

  remixapi_MeshInfoSurfaceTriangles surface = {};
  surface.vertices_values = vertices.data();
  surface.vertices_count = vertices.size();
  surface.indices_values = indices.data();
  surface.indices_count = indices.size();
  surface.skinning_hasvalue = 0;
  surface.material = material.handle;

  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  info.pNext = nullptr;
  info.hash = FALLBACK_MESH_HASH;
  info.surfaces_values = &surface;
  info.surfaces_count = 1;

  remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  const bool have_batched = m_interface.CreateMeshBatched != nullptr;
  CallGuarded(have_batched ? "CreateMeshBatched(fallback)" : "CreateMesh(fallback)", [&] {
    status = have_batched ? m_interface.CreateMeshBatched(&info, &m_fallback_mesh) :
                            m_interface.CreateMesh(&info, &m_fallback_mesh);
  });
  if (status != REMIXAPI_ERROR_CODE_SUCCESS)
  {
    m_fallback_mesh = nullptr;
    return false;
  }
  return true;
}

void RemixApi::SubmitFallbackTriangle()
{
  if (!EnsureFallbackMesh())
    return;

  // The triangle's vertices are placed in front of the identity-view camera, so
  // under camera recovery it needs the same V^-1 the geometry gets to stay
  // there rather than being left behind at the world origin.
  const remixapi_Transform transform =
      m_camera_recovery ? ToRemixTransform(m_view_inverse) : ToRemixTransform(IDENTITY_AFFINE);

  remixapi_InstanceInfo instance = {};
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.pNext = nullptr;
  instance.categoryFlags = 0;
  instance.mesh = m_fallback_mesh;
  instance.transform = transform;
  instance.doubleSided = 1;
  CallGuarded("DrawInstance(fallback)", [&] { m_interface.DrawInstance(&instance); });
}

void RemixApi::ReapIdleMeshes()
{
  for (auto it = m_meshes.begin(); it != m_meshes.end();)
  {
    if (m_frame_index - it->second.last_used_frame <= MESH_IDLE_FRAMES_BEFORE_DESTROY)
    {
      ++it;
      continue;
    }
    if (it->second.handle != nullptr)
    {
      remixapi_MeshHandle handle = it->second.handle;
      CallGuarded("DestroyMesh(idle)", [&] { m_interface.DestroyMesh(handle); });
    }
    // If this mesh IS a promoted lineage's single entry, the lineage record
    // dies with it - a dangling dynamic_mesh_key would otherwise be looked up
    // (and demoted) on the topology's next appearance, which works but wastes
    // the promotion the lineage had already earned. Unpromoted records are
    // left to the idle sweep below: another live mesh may share the topology.
    if (it->second.topology_key != 0)
    {
      if (const auto topo_it = m_topology.find(it->second.topology_key);
          topo_it != m_topology.end() && topo_it->second.dynamic_mesh_key == it->first)
      {
        m_topology.erase(topo_it);
      }
    }
    m_mesh_bytes_live -= it->second.gpu_bytes;
    ++m_meshes_destroyed_total;
    it = m_meshes.erase(it);
  }

  // Same idleness rule for the lineage records themselves, so one-off meshes
  // (and topologies whose meshes were reaped above) cannot grow the map for
  // the rest of the session.
  for (auto it = m_topology.begin(); it != m_topology.end();)
  {
    if (m_frame_index - it->second.last_seen_frame > MESH_IDLE_FRAMES_BEFORE_DESTROY)
      it = m_topology.erase(it);
    else
      ++it;
  }

  // And the same rule again for textures that exist only as dev-menu
  // thumbnails. A game's 2D layer is a succession of screens, so without this
  // every menu and every loading screen ever visited would hold its textures in
  // VRAM for the rest of the session, for a grid the user is probably no longer
  // looking at. Only overlay-originated hashes are in this map at all - a
  // material's texture was erased from it by EnsureMaterial - so this can never
  // destroy a texture the scene still samples.
  //
  // A tag survives the reap: tags are hash-keyed configuration living in the
  // runtime's option layers, not state on the texture, so the draw keeps being
  // routed and regains its thumbnail on its next sighting.
  if (m_interface.DestroyTexture != nullptr)
  {
    for (auto it = m_overlay_texture_frames.begin(); it != m_overlay_texture_frames.end();)
    {
      if (m_frame_index - it->second <= MESH_IDLE_FRAMES_BEFORE_DESTROY)
      {
        ++it;
        continue;
      }
      if (const auto tex = m_textures.find(it->first); tex != m_textures.end())
      {
        remixapi_TextureHandle handle = tex->second;
        CallGuarded("DestroyTexture(idle)", [&] { m_interface.DestroyTexture(handle); });
        m_textures.erase(tex);
      }
      it = m_overlay_texture_frames.erase(it);
    }
  }
}

void RemixApi::OnAfterFrame()
{
  if (!m_valid)
    return;

  // CPU-thread counters, moved into this frame's stats. Drained every frame -
  // including dropped minor frames - so one frame's accesses can never leak
  // into the next frame's numbers.
  RemixEFBInterface::DrainAccessCounters(m_stats.efb_peeks, m_stats.efb_pokes);

  // Some games present a frame that carries only the 2D layer between full
  // world frames (Skylanders alternates ~900 world instances with 34 HUD
  // quads and zero lights, every other frame). Traced as a scene, that second
  // present strobes the world at half rate and resets the denoiser's history
  // on every frame. Judged against the recent peak, never absolutely: in a
  // menu the few dozen quads ARE the scene and the floor keeps it presenting.
  // The peak is read before this frame's count is stored, so the reference is
  // strictly "the frames before this one".
  u32 recent_peak = 0;
  for (const u32 count : m_recent_pending)
    recent_peak = std::max(recent_peak, count);
  const u32 pending_count = static_cast<u32>(m_pending_instances.size());
  m_recent_pending[m_frame_index % m_recent_pending.size()] = pending_count;
  const bool minor_frame = m_skip_minor_frames && recent_peak >= MINOR_FRAME_FLOOR &&
                           pending_count * MINOR_FRAME_RATIO < recent_peak;
  if (minor_frame)
  {
    // Dropped whole: no camera work, no instances, no lights, no Present. The
    // runtime keeps displaying the last full frame, and consecutive full
    // frames become adjacent for its temporal stack. The modelview histogram
    // is cleared here WITHOUT rolling its previous-frame state (and
    // FinishFrame discards this frame's view samples the same way), so the
    // next full frame's deltas pair with the previous full frame - the
    // alternation is exactly what starved the estimator to 2 matched deltas.
    m_pending_instances.clear();
    m_modelviews.clear();
    m_modelview_overflow = 0;
    if (m_log_stats)
    {
      INFO_LOG_FMT(VIDEO,
                   "Remix frame {} MINOR dropped: draws {} pending inst {} orthoskip {} | "
                   "copies {} | recent peak {}",
                   m_frame_index, m_stats.draws_seen, pending_count, m_stats.skipped_ortho,
                   m_stats.efb_copies, recent_peak);
    }
    FinishFrame(true);
    return;
  }

  // Order matters: the camera has to be known before any instance can be placed
  // relative to it, and the estimate needs the whole frame's draws. So the
  // frame runs estimate -> camera -> instances rather than emitting instances
  // as they arrive. The histogram is resolved first because when it is driving,
  // its winner is what EstimateView installs as the view matrix.
  ResolveDominantModelview();
  EstimateView();
  SetupCamera();
  FlushPendingInstances();
  SubmitLights();

  // Keep the tracer fed when no game geometry made it through the classifier -
  // this is also the Milestone 0 signal: a lit, denoised triangle in the render
  // window means DLL load, device registration, camera and Present all work.
  if (m_stats.instances_drawn == 0)
    SubmitFallbackTriangle();

  ReapIdleMeshes();

  // Before Present, not after: the runtime stages the overlay and drains it at
  // the next present boundary, so a submission after Present would land a frame
  // late.
  SubmitScreenOverlay();

  remixapi_PresentInfo present_info = {};
  present_info.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
  present_info.pNext = nullptr;
  present_info.hwndOverride = nullptr;
  CallGuarded("Present", [&] { m_interface.Present(&present_info); });

  // One line EVERY frame, not sampled. The periodic reports below cannot see a
  // game that alternates its content between even and odd frames: their
  // even intervals land on one parity forever. The dominant modelview's |t| is
  // the cheap content classifier - a view matrix carries a translation of
  // camera order (~1e2 here) while a screen-pixel 2D matrix carries half-screen
  // offsets and a far push (~1e3), so the value alone names the pass.
  if (m_log_stats)
  {
    INFO_LOG_FMT(VIDEO,
                 "Remix frame {} brief: draws {} inst {} orthoskip {} | xfb {} of {} copies | "
                 "lights {} | top {} meshes rigid {} |t| {:.1f}",
                 m_frame_index, m_stats.draws_seen, m_stats.instances_drawn,
                 m_stats.skipped_ortho, m_stats.efb_copies_xfb, m_stats.efb_copies,
                 m_stats.lights_distant, m_modelview_top_valid ? m_modelview_top_meshes : 0,
                 !m_modelview_top_valid ? "-" : (AffineIsRigid(m_modelview_top) ? "yes" : "NO"),
                 m_modelview_top_valid ? AffineTranslationLength(m_modelview_top) : 0.0f);
  }

  if (m_log_stats && (m_frame_index % 61) == 0)
  {
    INFO_LOG_FMT(VIDEO,
                 "Remix frame {}: draws {} | skipped ortho {} prim {} efb {} efbdisc {} aux {} "
                 "empty {} "
                 "invisible {} "
                 "scissor {} "
                 "| meshes created {} (live {}) | skinned {} ({} new mesh) | updated {} | "
                 "instances {} (sky {}) "
                 "| colour {} vertex, {} "
                 "register, {} none, {} split | tev colour {} folded, {} tinted, {} identity, {} bailed, {} "
                 "later-stage tex | texgen {} ({} non-trivial) | blended {} tested {} cutout {} "
                 "mask {} "
                 "logicop {} "
                 "| flat normals {} ({} flipped) | lights {} distant, {} sphere ({} spot, {} "
                 "falloff-free->distant, {} dropped), draws "
                 "enabled mask {:#04x} | lights rewritten {} conflicted {} alpha-only {} | "
                 "diffuse none {} sign {} | spec {} | GX ambient {} | UI {} draws ({} "
                 "unplaceable, {} preworld, {} tev-alpha, {} ztest, bail s{}/k{}/c{}/r{}, skipped {} "
                 "dstalpha + {} "
                 "efbcopytex, raster {} us cached {}, upload {} us) | "
                 "ui-tags ui {} ign {} world {} tagmode {} strict {} persp {} wref {} pskin {} | "
                 "ui-heur preworld {} "
                 "fullscr {} | tex-reg {} | tagsets {}/{}/{} uistate {} | "
                 "efb copies {} ({} non-xfb+clear | xfb {} depth {} int {} scene {} 2d {} | "
                 "exec {} disc {}, {} us, {} folds) | efb peeks {} pokes {}"
                 " | viewport changes {} (corrected {}, refused {}, "
                 "mirrored {}, depth-only {}, ref-deferred {}) | sky auto: candidates {}, "
                 "classified {}, tagged {} ({} ignored), pushed {} (x{:.0f}), emissive {} (mode {})",
                 m_frame_index, m_stats.draws_seen, m_stats.skipped_ortho,
                 m_stats.skipped_non_triangle, m_stats.skipped_efb_texture,
                 m_stats.skipped_efb_discarded, m_stats.skipped_aux_pass,
                 m_stats.skipped_degenerate,
                 m_stats.skipped_invisible, m_stats.skipped_scissor,
                 m_stats.meshes_created,
                 m_meshes.size(), m_stats.draws_skinned, m_stats.draws_skinned_new_mesh,
                 m_stats.meshes_updated,
                 m_stats.instances_drawn,
                 m_stats.sky_draws, m_stats.color_vertex,
                 m_stats.color_register, m_stats.color_none, m_stats.ras_channel_split,
                 m_stats.tev_color_folded, m_stats.tev_color_tinted,
                 m_stats.tev_color_identity, m_stats.tev_color_bailed,
                 m_stats.texture_later_stage, m_stats.texgen_generated,
                 m_stats.texgen_nontrivial, m_stats.blended, m_stats.alpha_tested,
                 m_stats.alpha_cutout, m_stats.alpha_mask_folded,
                 m_stats.logic_op, m_stats.normals_generated, m_stats.normals_flipped,
                 m_stats.lights_distant, m_stats.lights_sphere, m_stats.lights_spot,
                 m_stats.lights_falloff_free, m_stats.lights_dropped_distant,
                 m_stats.draw_light_mask, m_stats.lights_rewritten, m_stats.lights_conflicted,
                 m_stats.lights_alpha_only, m_stats.lights_diffuse_none,
                 m_stats.lights_diffuse_sign, m_stats.lights_spec,
                 m_stats.ambient_bright ? "bright" : "dim", m_stats.ui_placed,
                 m_stats.ui_unplaceable, m_stats.ui_dropped_pre_world, m_stats.ui_tev_alpha,
                 m_stats.ui_depth_tested, m_stats.tev_bail_stages,
                 m_stats.tev_bail_konst, m_stats.tev_bail_compare, m_stats.tev_bail_rasterized,
                 m_stats.ui_skipped_dst_alpha, m_stats.ui_skipped_efb_copy_tex,
                 m_stats.ui_raster_us, m_stats.ui_raster_cached ? 1 : 0,
                 m_stats.ui_upload_us,
                 // Tag routing: one number per decision path, plus the two
                 // inputs those paths read (how big the runtime says each tag
                 // set is, and whether its dev menu is open). With all three
                 // sets empty and the menu shut, every counter here reads zero
                 // and the 2D path is byte-identical to the pre-feature build -
                 // which is what makes this group the regression floor as well
                 // as the diagnostic.
                 //
                 // The same holds one layer down for the perspective arm: with
                 // RemixUiTagPerspective off, `persp`, `wref` and `pskin` all
                 // read zero and routing is byte-identical to the ortho-only
                 // build, so that knob's off position is checkable from this
                 // line alone.
                 m_stats.ui_tagged_ui, m_stats.ui_tagged_ignore, m_stats.ui_tagged_world,
                 m_stats.ui_tagmode_world, m_stats.ui_dropped_strict, m_stats.ui_tagged_persp,
                 m_stats.ui_persp_refused_w, m_stats.ui_persp_skinned,
                 m_stats.ui_dropped_preworld, m_stats.ui_dropped_fullscreen,
                 m_stats.ui_overlay_tex_registered, m_tag_ui.size(), m_tag_ignore.size(),
                 m_tag_world_ui.size(), m_runtime_ui_state,
                 m_stats.efb_copies, m_stats.efb_copies_scratch, m_stats.efb_copies_xfb,
                 m_stats.efb_copies_depth, m_stats.efb_copies_intensity, m_stats.efb_copies_scene,
                 m_stats.efb_copies_composed2d, m_stats.efb_copies_executed,
                 m_stats.efb_copies_discarded, m_stats.efb_encode_us, m_stats.efb_ui_folds,
                 m_stats.efb_peeks, m_stats.efb_pokes, m_stats.viewport_changed,
                 m_stats.viewport_corrected, m_stats.viewport_uncorrectable,
                 m_stats.viewport_mirrored, m_stats.viewport_depth_changed,
                 m_stats.viewport_ref_deferred,
                 m_stats.sky_auto_candidates,
                 m_stats.sky_auto_classified, m_stats.sky_auto_tagged, m_stats.sky_auto_ignored,
                 m_stats.sky_pushed, m_sky_at_infinity ? m_sky_infinity_scale : 0.0f,
                 m_stats.sky_emissive, m_sky_auto_detect);
  }

  LogResourceRegistry();
  LogProjectionVariants();
  LogCameraRecovery();
  // Reads m_stats and both sample maps, so it has to run before the reset below
  // and before this frame's samples become last frame's.
  LogModelviewHistogram();

  FinishFrame(false);
}

void RemixApi::FinishFrame(bool minor_frame)
{
  // Learn next frame's reference gate and aux-pass rects from this frame,
  // FIRST, while the viewport table and the XFB rect are still this frame's -
  // everything below resets them. Gated on the XFB copy having happened:
  // frames that presented nothing (loading stutters, minor frames) neither
  // refresh nor destroy what the last real frame taught, which is the same
  // keep-the-last-known-good discipline m_presented_width already follows.
  if (m_xfb_frame_valid)
  {
    bool covered = false;
    for (u32 i = 0; i < m_viewport_variant_count && !covered; ++i)
    {
      covered = ViewportCoverageOfRect(m_viewport_variants[i].viewport, m_xfb_frame_rect) > 0.5f;
    }
    // A presented frame with world draws but NO majority viewport is a layout
    // this machinery has no safe answer for (split screen: two exact halves),
    // and one with no world draws at all is a menu; both stand the gate down,
    // which returns the latch to first-draw-wins and disarms the aux drop.
    m_ref_gate_valid = covered;
    m_presented_rect = m_xfb_frame_rect;
    m_presented_rect_valid = true;
    m_scratch_clear_rects_previous.swap(m_scratch_clear_rects);
    m_scratch_clear_rects.clear();
  }

  m_stats = {};
  m_frame_light_mask = 0;
  m_frame_light_color_mask = 0;
  m_frame_light_attenuation = {};
  m_frame_light_diffuse = {};
  m_frame_lights = {};
  m_projection_latched = false;
  m_reference_usable = false;
  m_projection_variants = {};
  m_projection_variant_count = 0;
  m_reference_viewport = {};
  m_reference_viewport_usable = false;
  m_viewport_variants = {};
  m_viewport_variant_count = 0;
  // This frame's samples become next frame's history; reuse the old map's
  // storage rather than reallocating a few hundred entries every frame. A
  // dropped minor frame's samples are discarded instead of rolled: its 2D
  // matrices must not become the history the next full frame's deltas are
  // measured against.
  if (minor_frame)
  {
    m_view_samples.clear();
  }
  else
  {
    m_view_samples.swap(m_view_samples_previous);
    m_view_samples.clear();
  }
  m_view_duplicate_hashes.clear();
  m_ui_frame_begun = false;
  // The EFB-space recorder, same discipline. Anything recorded but never folded
  // (a frame whose copies were all discarded, or that had no copy at all) is
  // dropped rather than carried into the next frame, where it would composite
  // last frame's HUD into this frame's copy. Begin() next frame does the actual
  // clearing; this flag is what makes it happen.
  m_efb_ui_frame_begun = false;
  // The Scene-vs-Composed2D signal is frame-local by definition: "has world
  // geometry been drawn YET, this frame".
  m_frame_world_draws = 0;
  // Promote this frame's XFB copy extent to "the region the console presents",
  // which the next frame's UI draws are mapped onto. It has to happen here and
  // not in the draw path: the XFB copy is the event that ENDS a frame
  // (after_frame_event is triggered from inside its handler, BPStructs.cpp:353),
  // so during a frame the only rect available is the previous one's - and using
  // it is correct, because a game changes its presented size at a mode switch,
  // not between two draws.
  if (m_xfb_frame_valid)
  {
    const u32 width = static_cast<u32>(std::max(1, m_xfb_frame_rect.GetWidth()));
    const u32 height = static_cast<u32>(std::max(1, m_xfb_frame_rect.GetHeight()));
    // Once, and only when it actually differs from what the pre-fix mapping
    // assumed - that is the case where this changes where anything lands.
    if (!m_presented_logged && (width != EFB_WIDTH || height != EFB_HEIGHT))
    {
      INFO_LOG_FMT(VIDEO,
                   "Remix: presented region is [{},{} -> {},{}] ({}x{}), not the EFB's {}x{}; "
                   "UI overlay scaled to it (RemixUiScaleToXfb {})",
                   m_xfb_frame_rect.left, m_xfb_frame_rect.top, m_xfb_frame_rect.right,
                   m_xfb_frame_rect.bottom, width, height, EFB_WIDTH, EFB_HEIGHT,
                   m_ui_scale_to_xfb ? "on" : "off");
      m_presented_logged = true;
    }
    m_presented_width = width;
    m_presented_height = height;
    m_xfb_frame_valid = false;
  }
  // Frame-scoped audit logs. Cleared here, at the same point the overlay's own
  // frame flag is, so the next frame's copies and footprints start empty.
  m_efb_copies.clear();
  m_ui_footprints.clear();
  ++m_frame_index;

  // The Scene-vs-Composed2D signal's tag-routing counterpart: how many pre-world
  // 2D draws this frame recorded without a UI tag. Frame-local by the same
  // definition, so it resets with everything else.
  m_frame_preworld_unprotected = 0;

  // Last, so this frame ran on one consistent set of values and the next frame
  // picks up any edit made in the meantime.
  RefreshLiveConfig();
  // After RefreshLiveConfig, because whether to poll at all is one of the knobs
  // it re-reads: flipping RemixUiTagRouting off must stop the polling on the
  // same boundary it stops the routing.
  PollRuntimeTagState();
  // After RefreshLiveConfig too, and for a stronger reason: that call has just
  // overwritten m_ui_world_view from the Dolphin setting, so the dev menu's
  // answer has to land on top of it or the checkbox would never hold. Not
  // gated on RemixUiTagRouting - the tagging view is how an untextured element
  // is reached in the first place, and it stays reachable even with routing off
  // so a tag can be placed before the routing that consumes it is turned on.
  SyncTaggingWorldView();
}

void RemixApi::UpdateOverlaySurface()
{
  // The live client rect. A minimized window reads 0x0 - keep the last known
  // size rather than collapsing the overlay to a pixel; the first frame after a
  // restore recomputes. The 1280x720 fallback only ever applies before a real
  // rect has been seen (headless, or a window mid-creation), matching what
  // Initialize assumed before this function existed.
  u32 width = 0;
  u32 height = 0;
  RECT client_rect = {};
  if (m_hwnd != nullptr && GetClientRect(m_hwnd, &client_rect) &&
      client_rect.right > client_rect.left && client_rect.bottom > client_rect.top)
  {
    width = static_cast<u32>(client_rect.right - client_rect.left);
    height = static_cast<u32>(client_rect.bottom - client_rect.top);
  }
  if (width == 0 || height == 0)
  {
    if (m_surface_width != 0)
      return;
    width = 1280;
    height = 720;
  }

  // Remix owns the D3D9 swapchain, so the headless common backend cannot
  // discover its dimensions through GetSurfaceInfo(). Feed its actual client
  // area back into Dolphin's presenter instead. Besides keeping resize and
  // controller mapping correct, this resolves the graphics-panel aspect mode
  // and its widescreen-hack projection multipliers.
  if (g_presenter)
  {
    if (g_presenter->GetBackbufferWidth() != static_cast<int>(width) ||
        g_presenter->GetBackbufferHeight() != static_cast<int>(height))
    {
      g_presenter->SetBackbuffer(static_cast<int>(width), static_cast<int>(height));
    }
    else
    {
      // Presenter::Present is intentionally skipped for headless backends, so
      // it would otherwise never notice a live aspect-ratio setting change.
      g_presenter->UpdateDrawRectangle();
    }
  }

  // Scaled down on request: the rasterizer is fill-rate bound and the runtime
  // rescales the overlay when it composites, so this trades sharpness for
  // frame time. Same clamp as the settings GUI offers.
  const float ui_scale = std::clamp(Config::Get(Config::GFX_REMIX_UI_OVERLAY_SCALE), 0.1f, 1.0f);
  float surface_w = static_cast<float>(width) * ui_scale;
  float surface_h = static_cast<float>(height) * ui_scale;

  // The native cap: GC UI is authored for a 640x528 framebuffer, so rasterizing
  // it at a 2560-wide window is ~4x the fill for edge sharpness the art does
  // not contain. 2x native keeps scaled edges clean and bounds the cost at any
  // window size - F-Zero GX's menu is 47 screens of overlapping fill per frame,
  // which at full 2560x1506 was 84 ms of CPU and the whole frame budget.
  // Aspect is preserved so the runtime's composite stretch stays uniform.
  if (Config::Get(Config::GFX_REMIX_UI_OVERLAY_CAP))
  {
    constexpr float kCapWidth = 1280.0f;   // 2x EFB_WIDTH
    constexpr float kCapHeight = 1056.0f;  // 2x EFB_HEIGHT
    const float factor = std::min({1.0f, kCapWidth / surface_w, kCapHeight / surface_h});
    surface_w *= factor;
    surface_h *= factor;
  }

  const u32 new_width = std::max(1u, static_cast<u32>(surface_w));
  const u32 new_height = std::max(1u, static_cast<u32>(surface_h));
  if (new_width != m_surface_width || new_height != m_surface_height)
  {
    // Once per actual change, so a live resize is verifiable from the log.
    if (m_surface_width != 0)
    {
      INFO_LOG_FMT(VIDEO, "Remix: UI overlay surface {}x{} -> {}x{} (window {}x{}, scale {:.2f})",
                   m_surface_width, m_surface_height, new_width, new_height, width, height,
                   ui_scale);
    }
    m_surface_width = new_width;
    m_surface_height = new_height;
  }
}

void RemixApi::RefreshLiveConfig()
{
  // Exactly the knobs nothing bakes: UI mode is routing consumed per draw, and
  // the rest gate log lines. Adding a knob here means proving that nothing
  // built at Initialize time - a mesh, a material, a light - was derived from
  // it. (The overlay surface size used to be the canonical example; it is now
  // re-derived every frame by UpdateOverlaySurface below, which is what makes
  // the scale knob and the window size live.) The settings GUI greys out every
  // other knob while a game runs, so this list and the metadata table's
  // Liveness column have to agree.
  m_ui_mode = Config::Get(Config::GFX_REMIX_UI_MODE);
  // Consumed once per frame at the present decision; nothing built at
  // Initialize time depends on it, so it is safe to flip mid-game - which is
  // also the intended A/B for diagnosing a strobing world.
  m_skip_minor_frames = Config::Get(Config::GFX_REMIX_SKIP_MINOR_FRAMES);
  m_log_stats = Config::Get(Config::GFX_REMIX_LOG_STATS);
  m_trace_projections = Config::Get(Config::GFX_REMIX_TRACE_PROJECTIONS);
  m_trace_modelviews = Config::Get(Config::GFX_REMIX_TRACE_MODELVIEWS);
  m_trace_colors = Config::Get(Config::GFX_REMIX_TRACE_COLORS);
  m_trace_efb_copies = Config::Get(Config::GFX_REMIX_TRACE_EFB_COPIES);
  m_ui_dump_frame = Config::Get(Config::GFX_REMIX_UI_DUMP_FRAME);
  // Tag-driven routing and its policies. All five qualify under the rule above:
  // they are consumed per draw at classification time, and nothing built at
  // Initialize - mesh, material, light, overlay surface - is derived from any of
  // them. RemixUiStrict being live is the whole point of it: it is the lever for
  // finding out which 2D elements are worth keeping, and an A/B that needs a
  // restart is an A/B nobody runs. RemixUiTagPerspective is live for the same
  // reason: flipping it off mid-game is how a tagged 3D element is compared
  // against itself as ordinary world geometry.
  m_ui_tag_routing = Config::Get(Config::GFX_REMIX_UI_TAG_ROUTING);
  m_ui_tag_perspective = Config::Get(Config::GFX_REMIX_UI_TAG_PERSPECTIVE);
  m_ui_drop_pre_world = Config::Get(Config::GFX_REMIX_UI_DROP_PRE_WORLD);
  m_ui_drop_fullscreen_opaque = Config::Get(Config::GFX_REMIX_UI_DROP_FULL_SCREEN_OPAQUE);
  m_ui_strict = Config::Get(Config::GFX_REMIX_UI_STRICT);
  // Also per-draw-consumed (the depth fields are stamped into each recorded
  // DrawCall), so live for the same A/B reason: on-vs-off is how a wrong-order
  // HUD is diagnosed without a restart.
  m_ui_depth = Config::Get(Config::GFX_REMIX_UI_DEPTH);
  // Stamped into every recorded DrawCall, so live for the same reason the depth
  // fields are: on-vs-off is the whole diagnosis for a scene hidden behind an
  // additive full-screen pass.
  m_ui_additive_light_coverage = Config::Get(Config::GFX_REMIX_UI_ADDITIVE_LIGHT_COVERAGE);
  // The tagging view has to be live or it is useless: flip on, click, flip off.
  m_ui_world_view = Config::Get(Config::GFX_REMIX_UI_WORLD_VIEW);
  // The quarter-screen fixes. Both are consumed per draw against state learned
  // at the frame boundary; nothing built at Initialize derives from either, and
  // live is the A/B - flip one off mid-game and the next frame is the old
  // behaviour, no restart.
  m_viewport_ref_xfb = Config::Get(Config::GFX_REMIX_VIEWPORT_REF_XFB);
  m_efb_drop_aux_pass = Config::Get(Config::GFX_REMIX_EFB_DROP_AUX_PASS);
  // Safe live because the rasterizer's records and pixels are frame-scoped:
  // flipping it only changes whether the NEXT Flush may serve the previous
  // composite, and the off position is byte-identical to the pre-cache build.
  m_ui_frame_cache = Config::Get(Config::GFX_REMIX_UI_FRAME_CACHE);
  // The overlay surface, re-derived from the live window and knobs - the frame
  // boundary is the one point where no recorded draw is in flight, so all of a
  // frame's draws map through one consistent size.
  UpdateOverlaySurface();
  // Same coupling as Initialize: the histogram is what the camera is read out
  // of, so turning the trace off must not be able to take the camera down with
  // it.
  if (m_camera_from_modelview)
    m_trace_modelviews = true;
}

}  // namespace Remix
