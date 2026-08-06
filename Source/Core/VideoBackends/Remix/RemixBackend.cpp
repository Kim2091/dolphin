// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Remix Backend Documentation
//
// This backend does not rasterize anything. It is shaped like the Null backend
// - all emulated state stays in VideoCommon, in plain CPU memory - and uses
// that to intercept every GX draw call at VertexManagerBase::DrawCurrentBatch,
// translating geometry, stage-0 textures, the camera and XF lights into RTX
// Remix API submissions. The Remix runtime (the user's Remix Plus d3d9.dll)
// path traces the result and presents it into Dolphin's own render window.
//
// See RemixApi.cpp for the runtime plumbing and RemixVertexManager.cpp for the
// GX translation.

#include "VideoBackends/Remix/VideoBackend.h"

#include "Common/Common.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/Remix/PerfQuery.h"
#include "VideoBackends/Remix/RemixApi.h"
#include "VideoBackends/Remix/RemixBoundingBox.h"
#include "VideoBackends/Remix/RemixGfx.h"
#include "VideoBackends/Remix/RemixTextureCache.h"
#include "VideoBackends/Remix/RemixVertexManager.h"

#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace Remix
{
void VideoBackend::InitBackendInfo(const WindowSystemInfo& wsi)
{
  g_backend_info.api_type = APIType::Nothing;
  g_backend_info.MaxTextureSize = 16384;
  g_backend_info.bSupportsExclusiveFullscreen = true;
  g_backend_info.bSupportsDualSourceBlend = true;
  // Deliberately false, unlike the Null backend: with primitive restart
  // disabled, PrepareForAdditionalData routes quads/strips/fans through
  // primitive_from_gx into plain triangle LISTS, so the capture path in
  // RemixVertexManager never has to parse restart indices.
  g_backend_info.bSupportsPrimitiveRestart = false;
  g_backend_info.bSupportsGeometryShaders = true;
  g_backend_info.bSupportsComputeShaders = false;
  g_backend_info.bSupports3DVision = false;
  g_backend_info.bSupportsEarlyZ = true;
  g_backend_info.bSupportsBindingLayout = true;
  // Deliberately false. RemixBoundingBox::Read returns zeros
  // (RemixBoundingBox.h:18-22), and zeros are strictly WORSE than not claiming
  // support at all: with the flag false, InitializeShared skips bbox init
  // (VideoBackendBase.cpp:338) and BoundingBox::Get serves VideoCommon's
  // fallback, which echoes back the SDK's own "no pixels drawn" register writes
  // (BoundingBox.cpp:53-63,75-86) - the mechanism that keeps a game like
  // Ultimate Spider-Man from dividing by a bounding box it never got.
  //
  // The RemixBoundingBox instance stays: it is still a required InitializeShared
  // parameter below, just never consulted. A real bbox computed from
  // post-transform vertices is a future upgrade, not something to fake here.
  g_backend_info.bSupportsBBox = false;
  g_backend_info.bSupportsGSInstancing = true;
  g_backend_info.bSupportsPostProcessing = false;
  g_backend_info.bSupportsPaletteConversion = true;
  g_backend_info.bSupportsClipControl = true;
  g_backend_info.bSupportsSSAA = true;
  g_backend_info.bSupportsDepthClamp = true;
  g_backend_info.bSupportsReversedDepthRange = true;
  g_backend_info.bSupportsMultithreading = false;
  // Must stay false: texel data has to flow through CPU decode into
  // AbstractTexture::Load, which is the only place RemixTexture can capture it.
  g_backend_info.bSupportsGPUTextureDecoding = false;
  g_backend_info.bSupportsST3CTextures = false;
  g_backend_info.bSupportsBPTCTextures = false;
  g_backend_info.bSupportsFramebufferFetch = false;
  g_backend_info.bSupportsBackgroundCompiling = false;
  g_backend_info.bSupportsLogicOp = false;
  g_backend_info.bSupportsLargePoints = false;
  g_backend_info.bSupportsDepthReadback = false;
  g_backend_info.bSupportsPartialDepthCopies = false;
  g_backend_info.bSupportsShaderBinaries = false;
  g_backend_info.bSupportsPipelineCacheData = false;
  g_backend_info.bSupportsCoarseDerivatives = false;
  g_backend_info.bSupportsTextureQueryLevels = false;
  g_backend_info.bSupportsLodBiasInSampler = false;
  g_backend_info.bSupportsSettingObjectNames = false;
  g_backend_info.bSupportsPartialMultisampleResolve = true;
  g_backend_info.bSupportsDynamicVertexLoader = false;

  // aamodes: We only support 1 sample, so no MSAA
  g_backend_info.Adapters.clear();
  g_backend_info.AAModes = {1};
}

bool VideoBackend::Initialize(const WindowSystemInfo& wsi)
{
  if (!InitializeShared(std::make_unique<RemixGfx>(), std::make_unique<VertexManager>(),
                        std::make_unique<PerfQuery>(), std::make_unique<RemixBoundingBox>(),
                        std::make_unique<RemixEFBInterface>(), std::make_unique<TextureCache>()))
  {
    return false;
  }

  // Bring the Remix runtime up after the shared components exist, so the
  // after-frame hook it installs can never fire into a half-built backend. A
  // failure here is not fatal: emulation keeps running (as a Null backend would)
  // and the log says why nothing is being path traced.
  g_remix_api = std::make_unique<RemixApi>();
  if (!g_remix_api->Initialize(wsi))
  {
    ERROR_LOG_FMT(VIDEO, "Remix: runtime initialization failed; no draws will be submitted");
    g_remix_api.reset();
  }

  return true;
}

void VideoBackend::Shutdown()
{
  // ShutdownShared() FIRST, then the runtime - the order every other backend
  // uses (D3D11 D3DMain.cpp:168, D3D12 VideoBackend.cpp:145, Vulkan
  // VKMain.cpp:239, OGL OGLMain.cpp:218, Null NullBackend.cpp:75 all tear the
  // shared state down before their device, and Vulkan waits for device idle on
  // top of that). It is not a style preference: ShutdownShared stops the GPU
  // thread (its last act is Fifo::Shutdown) and destroys g_gfx,
  // g_vertex_manager and g_texture_cache, every one of which reaches into
  // g_remix_api and therefore into the runtime DLL.
  //
  // The reverse order destroyed the runtime and FreeLibrary'd d3d9-remix.dll
  // while the GPU thread was still draining the FIFO. Both consumers null-check
  // g_remix_api exactly once and then dereference it freely for the rest of the
  // call - RemixVertexManager::DrawCurrentBatch checks at :1580 and derefs ~40
  // times after, TextureCache::CopyEFB checks at :72 and derefs through :181 -
  // so a stop landing mid-draw sails past the check and every deref after it
  // lands on a destroyed object, in code that has just been unmapped. That is a
  // crash on stop that no amount of null-guarding at the call sites can fix,
  // because the guard and the use cannot be made atomic against a teardown on
  // another thread. Stopping the thread first removes the race outright.
  //
  // Destroying the dependents before the thing they depend on is also just the
  // right direction: nothing in this backend's teardown needs the runtime to be
  // gone already (no destructor here touches g_remix_api at all).
  ShutdownShared();
  g_remix_api.reset();
}

std::string VideoBackend::GetDisplayName() const
{
  // i18n: Remix refers to NVIDIA RTX Remix, the path tracing runtime this
  // backend submits the emulated scene to.
  return _trans("Remix (Experimental)");
}
}  // namespace Remix
