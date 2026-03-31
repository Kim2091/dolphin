// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D9/VideoBackend.h"

#include <memory>
#include <string>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/D3D9/D3D9BoundingBox.h"
#include "VideoBackends/D3D9/D3D9Device.h"
#include "VideoBackends/D3D9/D3D9Gfx.h"
#include "VideoBackends/D3D9/D3D9PerfQuery.h"
#include "VideoBackends/D3D9/D3D9TextureCache.h"
#include "VideoBackends/D3D9/D3D9VertexManager.h"

#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace DX9Remix
{
std::string VideoBackend::GetDisplayName() const
{
  return _trans("Direct3D 9 (RTX Remix)");
}

void VideoBackend::InitBackendInfo(const WindowSystemInfo& wsi)
{
  FillBackendInfo();
}

void VideoBackend::FillBackendInfo()
{
  g_backend_info.api_type = APIType::D3D;
  g_backend_info.MaxTextureSize = 4096;
  g_backend_info.bUsesLowerLeftOrigin = false;
  g_backend_info.bSupportsExclusiveFullscreen = false;
  g_backend_info.bSupportsDualSourceBlend = false;
  g_backend_info.bSupportsPrimitiveRestart = false;
  g_backend_info.bSupportsGeometryShaders = false;
  g_backend_info.bSupportsComputeShaders = false;
  g_backend_info.bSupports3DVision = false;
  g_backend_info.bSupportsPostProcessing = false;
  g_backend_info.bSupportsPaletteConversion = false;
  g_backend_info.bSupportsClipControl = false;
  g_backend_info.bSupportsDepthClamp = true;
  g_backend_info.bSupportsReversedDepthRange = false;
  g_backend_info.bSupportsMultithreading = false;
  g_backend_info.bSupportsGPUTextureDecoding = false;
  g_backend_info.bSupportsCopyToVram = true;
  g_backend_info.bSupportsLargePoints = false;
  g_backend_info.bSupportsDepthReadback = false;
  g_backend_info.bSupportsPartialDepthCopies = false;
  g_backend_info.bSupportsBitfield = false;
  g_backend_info.bSupportsDynamicSamplerIndexing = false;
  g_backend_info.bSupportsFramebufferFetch = false;
  g_backend_info.bSupportsBackgroundCompiling = false;
  g_backend_info.bSupportsST3CTextures = true;
  g_backend_info.bSupportsBPTCTextures = false;
  g_backend_info.bSupportsEarlyZ = false;
  g_backend_info.bSupportsBBox = false;
  g_backend_info.bSupportsFragmentStoresAndAtomics = false;
  g_backend_info.bSupportsGSInstancing = false;
  g_backend_info.bSupportsSSAA = false;
  g_backend_info.bSupportsShaderBinaries = false;
  g_backend_info.bSupportsPipelineCacheData = false;
  g_backend_info.bSupportsCoarseDerivatives = false;
  g_backend_info.bSupportsTextureQueryLevels = false;
  g_backend_info.bSupportsLodBiasInSampler = false;
  g_backend_info.bSupportsLogicOp = false;
  g_backend_info.bSupportsSettingObjectNames = false;
  g_backend_info.bSupportsPartialMultisampleResolve = false;
  g_backend_info.bSupportsDynamicVertexLoader = false;
  g_backend_info.bSupportsHDROutput = false;

  g_backend_info.Adapters.clear();
  g_backend_info.AAModes = {1};
}

bool VideoBackend::Initialize(const WindowSystemInfo& wsi)
{
  if (!wsi.render_surface)
  {
    PanicAlertFmtT("D3D9 backend requires a render surface");
    return false;
  }

  if (!D3D9::Create(static_cast<HWND>(wsi.render_surface)))
    return false;

  FillBackendInfo();
  UpdateActiveConfig();

  auto gfx = std::make_unique<Gfx>(wsi.render_surface_scale);
  auto vertex_manager = std::make_unique<VertexManager>();
  auto perf_query = std::make_unique<PerfQuery>();
  auto bounding_box = std::make_unique<D3D9BoundingBox>();
  auto efb_interface = std::make_unique<D3D9EFBInterface>();
  auto texture_cache = std::make_unique<TextureCache>();

  return InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                          std::move(bounding_box), std::move(efb_interface),
                          std::move(texture_cache));
}

void VideoBackend::Shutdown()
{
  ShutdownShared();
  D3D9::Destroy();
}
}  // namespace DX9Remix
