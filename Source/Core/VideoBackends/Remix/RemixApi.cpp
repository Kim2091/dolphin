// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Remix/RemixApi.h"

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <exception>
#include <iterator>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <xxhash.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/WindowSystemInfo.h"

#include "Core/Config/GraphicsSettings.h"

#include "VideoBackends/Remix/RemixTexture.h"

#include "VideoCommon/VideoEvents.h"
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
void AffineOrthonormalize(Affine& m)
{
  float x[3] = {m[0], m[1], m[2]};
  float y[3] = {m[4], m[5], m[6]};
  Normalize3(x);
  const float xy = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
  for (int k = 0; k < 3; ++k)
    y[k] -= xy * x[k];
  Normalize3(y);
  const float z[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2],
                      x[0] * y[1] - x[1] * y[0]};
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
  m_trace_projections = Config::Get(Config::GFX_REMIX_TRACE_PROJECTIONS);
  m_camera_recovery = Config::Get(Config::GFX_REMIX_CAMERA_RECOVERY);
  m_gx_color = Config::Get(Config::GFX_REMIX_GX_COLOR);
  m_gx_texgen = Config::Get(Config::GFX_REMIX_GX_TEXGEN);
  m_gx_blend = Config::Get(Config::GFX_REMIX_GX_BLEND);
  m_gx_light_fix = Config::Get(Config::GFX_REMIX_GX_LIGHT_FIX);
  m_light_range = std::max(1.0f, Config::Get(Config::GFX_REMIX_LIGHT_RANGE));
  // World space starts as view space and drifts away from it as the estimator
  // integrates. Frame 0 is therefore exactly the identity-view behaviour.
  m_view = IDENTITY_AFFINE;
  m_view_inverse = IDENTITY_AFFINE;
  m_view_inverse_previous = IDENTITY_AFFINE;
  m_sky_mode = Config::Get(Config::GFX_REMIX_SKY_MODE);

  // "0xabc,0xdef" -> the set of stage-0 texture hashes to treat as skybox.
  // Separators are anything that is not a hex digit or an 'x', so commas,
  // spaces and newlines all work.
  m_sky_textures.clear();
  const std::string sky_list = Config::Get(Config::GFX_REMIX_SKY_TEXTURES);
  for (size_t i = 0; i < sky_list.size();)
  {
    if (std::isxdigit(static_cast<unsigned char>(sky_list[i])) == 0)
    {
      ++i;
      continue;
    }
    size_t consumed = 0;
    try
    {
      const u64 hash = std::stoull(sky_list.substr(i), &consumed, 16);
      if (hash != 0)
        m_sky_textures.insert(hash);
    }
    catch (const std::exception&)
    {
      // Malformed or out-of-range entry: skip this token rather than take the
      // whole backend down over a typo in a config string.
      consumed = 0;
    }
    i += std::max<size_t>(consumed, 1);
  }
  if (!m_sky_textures.empty())
    INFO_LOG_FMT(VIDEO, "Remix: {} sky texture hash(es) configured", m_sky_textures.size());
  m_light_scale = Config::Get(Config::GFX_REMIX_LIGHT_SCALE);

  const std::string dll_path_utf8 = Config::Get(Config::GFX_REMIX_DLL_PATH);
  const std::wstring dll_path = UTF8ToWString(dll_path_utf8);

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
  if (m_interface.CreateMaterial == nullptr || m_interface.DestroyMaterial == nullptr ||
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

  // Shutdown() + FreeLibrary in one call, so switching backends does not leak
  // the device or leave the runtime resident.
  remixapi_lib_shutdownAndUnloadRemixDll(&m_interface, m_dll);
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
  m_meshes.clear();
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
}  // namespace

int RemixApi::ObserveProjection(const std::array<float, 6>& raw_projection)
{
  if (!m_projection_latched)
  {
    m_raw_projection = raw_projection;
    m_projection_latched = true;
    float fov_y_deg = 0.0f;
    float aspect = 0.0f;
    m_reference_usable = RecoverFovAspect(raw_projection, fov_y_deg, aspect);
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
  const bool interesting = m_projection_variant_count > 1 || m_stats.projection_oblique > 0 ||
                           m_stats.projection_overflow > 0 || m_stats.projection_uncorrectable > 0;
  if (!m_trace_projections && (!interesting || (m_frame_index % 60) != 0))
    return;

  INFO_LOG_FMT(VIDEO,
               "Remix frame {} projections: {} variant(s), reference #0 | corrected {} draw(s), "
               "off-centre {}, UNCORRECTABLE {} | table overflow {} | fix {}",
               m_frame_index, m_projection_variant_count, m_stats.projection_corrected,
               m_stats.projection_oblique, m_stats.projection_uncorrectable,
               m_stats.projection_overflow, m_projection_fix ? "on" : "off");

  for (u32 i = 0; i < m_projection_variant_count; ++i)
  {
    const ProjectionVariant& variant = m_projection_variants[i];
    // raw[1] and raw[3] are the off-centre shear terms. They are the reason a
    // parameterized camera alone cannot reproduce the frame: it has no field
    // for them, so on the pre-fix path they were silently dropped.
    INFO_LOG_FMT(VIDEO,
                 "  #{}{} raw {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} | draws {} verts {}{}", i,
                 i == 0 ? " (ref)" : "     ", variant.raw[0], variant.raw[1], variant.raw[2],
                 variant.raw[3], variant.raw[4], variant.raw[5], variant.draws, variant.vertices,
                 (variant.raw[1] != 0.0f || variant.raw[3] != 0.0f) ? "  <-- off-centre" : "");
  }
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
  return true;
}

MaterialRef RemixApi::EnsureMaterial(const RemixTexture* texture, u8 filter_mode, u8 wrap_mode_u,
                                     u8 wrap_mode_v, u8 alpha_test_type, u8 alpha_reference)
{
  MaterialRef result;
  if (!m_valid)
    return result;

  u64 texture_hash = 0;
  if (texture != nullptr && texture->HasData() && UploadTexture(*texture))
    texture_hash = texture->GetContentHash();

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
  material_hash = NonZeroHash(material_hash);

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
  // Untextured draws get a neutral mid-grey so they are visibly present rather
  // than black; textured draws leave albedo entirely to the texture.
  opaque_ext.albedoConstant =
      texture_hash != 0 ? remixapi_Float3D{1.0f, 1.0f, 1.0f} : remixapi_Float3D{0.5f, 0.5f, 0.5f};
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
  info.emissiveTexture = nullptr;
  info.emissiveIntensity = 0.0f;
  info.emissiveColorConstant = {0.0f, 0.0f, 0.0f};
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

void RemixApi::SubmitMesh(const MaterialRef& material,
                          const std::vector<remixapi_HardcodedVertex>& vertices,
                          const std::vector<u32>& indices, const remixapi_Transform& transform,
                          remixapi_InstanceCategoryFlags category_flags,
                          const DrawBlendState& blend, const float* raw_modelview)
{
  if (!m_valid || material.handle == nullptr || vertices.empty() || indices.empty())
    return;

  // Mesh identity = geometry bytes folded with the material hash. The material
  // has to participate: Remix handles ARE hashes and a mesh bakes its surface
  // material at create time, so two material variants of the same geometry must
  // not collapse onto one handle (whichever registered first would win).
  u64 mesh_hash = XXH64(vertices.data(), vertices.size() * sizeof(remixapi_HardcodedVertex), 0);
  mesh_hash ^= XXH64(indices.data(), indices.size() * sizeof(u32),
                     static_cast<u64>(sizeof(remixapi_HardcodedVertex)));
  mesh_hash = NonZeroHash(FoldHash(mesh_hash, material.hash));

  if (m_poisoned_meshes.count(mesh_hash) != 0)
    return;

  remixapi_MeshHandle mesh_handle = nullptr;
  if (const auto it = m_meshes.find(mesh_hash); it != m_meshes.end())
  {
    mesh_handle = it->second.handle;
    it->second.last_used_frame = m_frame_index;
  }
  else
  {
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

    m_meshes.emplace(mesh_hash, MeshEntry{mesh_handle, m_frame_index});
    ++m_stats.meshes_created;
  }

  // Sample this draw for the camera estimator before queueing it. The mesh hash
  // is the correspondence key across frames, and it works precisely because the
  // non-palette path hashes OBJECT-space vertices: a static object keeps the
  // same hash however the camera moves. Palette draws have no single modelview
  // and their baked view-space vertices re-hash every frame, so they cannot
  // take part either way.
  if (raw_modelview != nullptr && m_view_samples.size() < MAX_VIEW_SAMPLES &&
      vertices.size() >= MIN_VIEW_SAMPLE_VERTICES)
  {
    Affine modelview = {};
    std::memcpy(modelview.data(), raw_modelview, sizeof(float) * 12);
    m_view_samples.emplace(mesh_hash, modelview);
  }

  m_pending_instances.push_back(
      PendingInstance{mesh_handle, transform, category_flags, blend, mesh_hash});
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

    remixapi_InstanceInfo instance = {};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    const bool need_blend_ext = m_gx_color || m_gx_blend;
    instance.pNext = need_blend_ext ? static_cast<void*>(&blend) : static_cast<void*>(&picking);
    instance.categoryFlags = pending.category_flags;
    instance.mesh = pending.mesh;
    // The submitted transform is V^-1 * (C * MV), and the camera is V, so Remix
    // computes P * V * V^-1 * C * MV = P * C * MV. The rendered image is
    // therefore INVARIANT to whatever V the estimator produces - a wrong camera
    // costs temporal quality, never correctness. The one thing that can break
    // the picture is SetupCamera's basis extraction disagreeing with this V,
    // which is why that is the part to suspect if geometry ever moves wrongly.
    instance.transform =
        m_camera_recovery ?
            ToRemixTransform(AffineMultiply(m_view_inverse, FromRemixTransform(pending.transform))) :
            pending.transform;
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
  if (!m_camera_recovery)
    return;

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
    if (inliers > best_inliers)
    {
      best_inliers = inliers;
      best = i;
    }
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
    m_view_miss_streak = 0;
    m_view = AffineMultiply(deltas[best], m_view);
    AffineOrthonormalize(m_view);
  }
  else if (deltas.size() >= 2)
  {
    // No consensus: a cut, a teleport, or a frame where the estimator simply
    // could not tell. Re-anchoring resets world space onto the current camera,
    // which costs one frame of temporal history - strictly better than
    // integrating a delta we do not believe. The streak keeps a single odd
    // frame, or a cutscene in which genuinely everything moves, from tripping
    // it; a frame with almost nothing persisting is not evidence of a cut.
    if (++m_view_miss_streak >= VIEW_MISS_STREAK_BEFORE_REANCHOR)
    {
      m_view_miss_streak = 0;
      m_view = IDENTITY_AFFINE;
      ++m_stats.view_reanchors;
      INFO_LOG_FMT(VIDEO, "Remix: camera re-anchored at frame {} ({} deltas, {} inliers, {} needed)",
                   m_frame_index, deltas.size(), best_inliers, required);
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
  }
}

void RemixApi::LogCameraRecovery()
{
  if (!m_log_stats || !m_camera_recovery || (m_frame_index % 60) != 0)
    return;

  const Affine& v = m_view;
  const float tx = v[3], ty = v[7], tz = v[11];
  const float position[3] = {-(v[0] * tx + v[4] * ty + v[8] * tz),
                             -(v[1] * tx + v[5] * ty + v[9] * tz),
                             -(v[2] * tx + v[6] * ty + v[10] * tz)};
  const u32 stable_pct =
      m_stats.w_compared != 0 ? (m_stats.w_stable * 100) / m_stats.w_compared : 0;

  INFO_LOG_FMT(VIDEO,
               "Remix frame {} camera: samples {} | inliers {}/{} | stable W {}/{} ({}%) | "
               "pos ({:.1f} {:.1f} {:.1f}) fwd ({:.3f} {:.3f} {:.3f})",
               m_frame_index, m_stats.view_samples, m_stats.view_inliers, m_stats.view_candidates,
               m_stats.w_stable, m_stats.w_compared, stable_pct, position[0], position[1],
               position[2], -v[8], -v[9], -v[10]);
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
    // Taking the union across the frame's variants is what stops a draw with a
    // longer far plane from being clipped by whichever projection happened to
    // be latched first. Variant #0 is the reference, so it is always included.
    float near_plane = 0.0f;
    float far_plane = 0.0f;
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
    const bool is_positional = attnfunc == AttenuationFunc::Spot;
    if (attnfunc == AttenuationFunc::Spec)
      ++m_stats.lights_spec;

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
      const float quantized[4] = {sphere.shaping_value.coneAngleDegrees,
                                  sphere.shaping_value.coneSoftness, radiance[0], end_distance};
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
    it = m_meshes.erase(it);
  }
}

void RemixApi::OnAfterFrame()
{
  if (!m_valid)
    return;

  // Order matters: the camera has to be known before any instance can be placed
  // relative to it, and the estimate needs the whole frame's draws. So the
  // frame runs estimate -> camera -> instances rather than emitting instances
  // as they arrive.
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

  remixapi_PresentInfo present_info = {};
  present_info.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
  present_info.pNext = nullptr;
  present_info.hwndOverride = nullptr;
  CallGuarded("Present", [&] { m_interface.Present(&present_info); });

  if (m_log_stats && (m_frame_index % 60) == 0)
  {
    INFO_LOG_FMT(VIDEO,
                 "Remix frame {}: draws {} | skipped ortho {} prim {} efb {} empty {} invisible {} "
                 "| meshes created {} (live {}) | instances {} (sky {}) | colour {} vertex, {} "
                 "register, {} none | texgen {} ({} non-trivial) | blended {} tested {} logicop {} "
                 "| flat normals {} ({} flipped) | lights {} distant, {} sphere ({} spot), draws "
                 "enabled mask {:#04x} | lights rewritten {} conflicted {} alpha-only {} | "
                 "diffuse none {} sign {} | spec {} | GX ambient {}",
                 m_frame_index, m_stats.draws_seen, m_stats.skipped_ortho,
                 m_stats.skipped_non_triangle, m_stats.skipped_efb_texture,
                 m_stats.skipped_degenerate, m_stats.skipped_invisible, m_stats.meshes_created,
                 m_meshes.size(), m_stats.instances_drawn, m_stats.sky_draws, m_stats.color_vertex,
                 m_stats.color_register, m_stats.color_none, m_stats.texgen_generated,
                 m_stats.texgen_nontrivial, m_stats.blended, m_stats.alpha_tested,
                 m_stats.logic_op, m_stats.normals_generated, m_stats.normals_flipped,
                 m_stats.lights_distant, m_stats.lights_sphere, m_stats.lights_spot,
                 m_stats.draw_light_mask, m_stats.lights_rewritten, m_stats.lights_conflicted,
                 m_stats.lights_alpha_only, m_stats.lights_diffuse_none,
                 m_stats.lights_diffuse_sign, m_stats.lights_spec,
                 m_stats.ambient_bright ? "bright" : "dim");
  }

  LogProjectionVariants();
  LogCameraRecovery();

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
  // This frame's samples become next frame's history; reuse the old map's
  // storage rather than reallocating a few hundred entries every frame.
  m_view_samples.swap(m_view_samples_previous);
  m_view_samples.clear();
  ++m_frame_index;
}

}  // namespace Remix
