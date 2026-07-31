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
  g_backend_info.bSupportsBBox = true;
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
  g_remix_api.reset();
  ShutdownShared();
}

std::string VideoBackend::GetDisplayName() const
{
  // i18n: Remix refers to NVIDIA RTX Remix, the path tracing runtime this
  // backend submits the emulated scene to.
  return _trans("Remix (Experimental)");
}
}  // namespace Remix
