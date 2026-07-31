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

void RemixApi::LatchProjection(const std::array<float, 6>& raw_projection)
{
  if (m_projection_latched)
    return;
  m_raw_projection = raw_projection;
  m_projection_latched = true;
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
                                     u8 wrap_mode_v)
{
  MaterialRef result;
  if (!m_valid)
    return result;

  u64 texture_hash = 0;
  if (texture != nullptr && texture->HasData() && UploadTexture(*texture))
    texture_hash = texture->GetContentHash();

  // Sampler state participates in material identity because Remix bakes it into
  // the material, and the same texture is legitimately sampled with different
  // wrap modes by different draws.
  u64 material_hash;
  if (texture_hash != 0)
  {
    material_hash = FoldHash(texture_hash, (static_cast<u64>(filter_mode) << 16) |
                                               (static_cast<u64>(wrap_mode_u) << 8) |
                                               static_cast<u64>(wrap_mode_v));
  }
  else
  {
    material_hash = FALLBACK_MATERIAL_HASH;
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
  opaque_ext.useDrawCallAlphaState = 0;
  // 7 == "always pass": v1 renders everything opaque, TEV alpha handling is out
  // of scope.
  opaque_ext.alphaTestType = 7;
  opaque_ext.alphaReferenceValue = 0;

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

void RemixApi::SubmitMesh(const MaterialRef& material,
                          const std::vector<remixapi_HardcodedVertex>& vertices,
                          const std::vector<u32>& indices, const remixapi_Transform& transform)
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

  remixapi_InstanceInfo instance = {};
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.pNext = nullptr;
  instance.categoryFlags = 0;
  instance.mesh = mesh_handle;
  instance.transform = transform;
  // v1 pins double-sided: GC winding under our right-handed identity view is
  // not verified, and a wrong guess would silently cull whole scenes.
  instance.doubleSided = 1;

  const int guard =
      CallGuarded("DrawInstance", [&] { m_interface.DrawInstance(&instance); });
  if (guard != 0)
  {
    m_poisoned_meshes.insert(mesh_hash);
    return;
  }
  ++m_stats.instances_drawn;
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
    if (raw[2] > 0.0001f && raw[0] > 0.0001f)
    {
      const float fov_y_deg =
          2.0f * std::atan(1.0f / raw[2]) * (180.0f / 3.14159265358979323846f);
      const float aspect = raw[2] / raw[0];
      if (fov_y_deg >= 5.0f && fov_y_deg <= 170.0f && aspect >= 0.25f && aspect <= 8.0f)
      {
        params.fovYInDegrees = fov_y_deg;
        params.aspect = aspect;
      }
    }
    if (std::abs(raw[4]) > 0.0000001f && std::abs(raw[4] - 1.0f) > 0.0000001f)
    {
      const float far_plane = raw[5] / raw[4];
      const float near_plane = raw[5] / (raw[4] - 1.0f);
      // Never let a transient degenerate frustum reach the tracer.
      if (near_plane > 0.0001f && far_plane > near_plane)
      {
        params.nearPlane = std::clamp(near_plane, 0.01f, 100.0f);
        params.farPlane = std::clamp(far_plane, params.nearPlane + 1.0f, 10000000.0f);
      }
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
  // GX lights are specified in view space, which is exactly our identity-view
  // world, so they need no basis change at all. The enable mask lives in the
  // per-channel lighting configs rather than on the lights themselves.
  u32 light_mask = 0;
  for (u32 i = 0; i < xfmem.numChan.numColorChans && i < 2; ++i)
  {
    light_mask |= xfmem.color[i].GetFullLightMask();
    light_mask |= xfmem.alpha[i].GetFullLightMask();
  }

  u32 drawn = 0;
  for (u32 i = 0; i < m_lights.size(); ++i)
  {
    if ((light_mask & (1u << i)) == 0)
      continue;

    const Light& src = xfmem.lights[i];
    // xfmem light colors are packed abgr in u8[4].
    const float r = static_cast<float>(src.color[3]) / 255.0f;
    const float g = static_cast<float>(src.color[2]) / 255.0f;
    const float b = static_cast<float>(src.color[1]) / 255.0f;
    if (r <= 0.0f && g <= 0.0f && b <= 0.0f)
      continue;

    remixapi_LightInfoSphereEXT sphere = {};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.pNext = nullptr;
    sphere.position = {src.dpos[0], src.dpos[1], src.dpos[2]};
    // GX point lights are analytically infinitesimal; give them a small but
    // non-zero radius so the tracer produces soft rather than hard shadows.
    sphere.radius = 5.0f;
    sphere.shaping_hasvalue = 0;
    sphere.volumetricRadianceScale = 1.0f;

    remixapi_LightInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &sphere;
    info.hash = XF_LIGHT_HASH_BASE + i;
    info.radiance = {r * m_light_scale, g * m_light_scale, b * m_light_scale};
    info.isDynamic = 1;
    info.ignoreViewModel = 0;

    LightEntry& entry = m_lights[i];
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

  const MaterialRef material = EnsureMaterial(nullptr, 1, 1, 1);
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

  remixapi_Transform transform = {};
  transform.matrix[0][0] = 1.0f;
  transform.matrix[1][1] = 1.0f;
  transform.matrix[2][2] = 1.0f;

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

  SetupCamera();
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
                 "Remix frame {}: draws {} | skipped ortho {} prim {} efb {} empty {} | meshes "
                 "created {} (live {}) | instances {}",
                 m_frame_index, m_stats.draws_seen, m_stats.skipped_ortho,
                 m_stats.skipped_non_triangle, m_stats.skipped_efb_texture,
                 m_stats.skipped_degenerate, m_stats.meshes_created, m_meshes.size(),
                 m_stats.instances_drawn);
  }

  m_stats = {};
  m_projection_latched = false;
  ++m_frame_index;
}

}  // namespace Remix
