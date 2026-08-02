add_library(videoremix
  RemixApi.cpp
  RemixApi.h
  RemixBackend.cpp
  RemixBoundingBox.h
  RemixGfx.cpp
  RemixGfx.h
  RemixTexture.cpp
  RemixTexture.h
  RemixTextureCache.cpp
  RemixTextureCache.h
  RemixUiRaster.cpp
  RemixUiRaster.h
  RemixVertexManager.cpp
  RemixVertexManager.h
  PerfQuery.h
  VideoBackend.h
)

target_link_libraries(videoremix
PUBLIC
  common
  videocommon
PRIVATE
  xxhash::xxhash
)

# Note: deliberately NOT linking d3d9.lib. Nothing here imports a d3d9 entry
# point - the D3D9Ex objects are obtained from the Remix runtime through
# dxvk_CreateD3D9 and only used through their COM vtables - and an implicit
# import would make Dolphin.exe load a d3d9.dll at startup, which is exactly
# the DLL we want to load explicitly, from a configurable path, and only when
# this backend is selected.

# The Remix API header is vendored from the user's Remix Plus fork; see
# Externals/remix/remix_c.h. Kept as an include directory (rather than a target)
# because it is header-only, and included with angle brackets so MSVC's
# /external:anglebrackets keeps third-party warnings out of Dolphin's /W4 /WX.
target_include_directories(videoremix PRIVATE "${CMAKE_SOURCE_DIR}/Externals/remix")

if(MSVC)
  # Add precompiled header
  target_link_libraries(videoremix PRIVATE use_pch)
endif()
