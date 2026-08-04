// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/Config/Config.h"

namespace Config
{
// Remix video backend
extern const Info<std::string> GFX_REMIX_DLL_PATH;
extern const Info<float> GFX_REMIX_SCENE_SCALE;
extern const Info<float> GFX_REMIX_LIGHT_SCALE;
extern const Info<bool> GFX_REMIX_LOG_STATS;
extern const Info<int> GFX_REMIX_SKY_MODE;
extern const Info<std::string> GFX_REMIX_SKY_TEXTURES;
extern const Info<bool> GFX_REMIX_PROJECTION_FIX;
extern const Info<bool> GFX_REMIX_VIEWPORT_FIX;
extern const Info<bool> GFX_REMIX_TRACE_PROJECTIONS;
extern const Info<bool> GFX_REMIX_TRACE_MODELVIEWS;
extern const Info<bool> GFX_REMIX_CAMERA_FROM_MODELVIEW;
extern const Info<int> GFX_REMIX_UI_MODE;
extern const Info<float> GFX_REMIX_UI_OVERLAY_SCALE;
extern const Info<int> GFX_REMIX_UI_DUMP_FRAME;
extern const Info<float> GFX_REMIX_WORLD_UI_DISTANCE;
extern const Info<bool> GFX_REMIX_WORLD_UI_FLIP_Y;
extern const Info<bool> GFX_REMIX_CAMERA_RECOVERY;
extern const Info<bool> GFX_REMIX_GX_COLOR;
extern const Info<bool> GFX_REMIX_GX_TEXGEN;
extern const Info<bool> GFX_REMIX_GX_BLEND;
extern const Info<bool> GFX_REMIX_GX_LIGHT_FIX;
extern const Info<float> GFX_REMIX_LIGHT_RANGE;
extern const Info<bool> GFX_REMIX_VIEW_ELECTORATE_FIX;
extern const Info<bool> GFX_REMIX_VIEW_HOLD_ON_MISS;
extern const Info<bool> GFX_REMIX_VIEW_TIE_BREAK;
extern const Info<int> GFX_REMIX_SKY_AUTO_DETECT;
extern const Info<int> GFX_REMIX_SKY_AUTO_FRAMES;
extern const Info<float> GFX_REMIX_SKY_AUTO_MIN_EXTENT;
extern const Info<std::string> GFX_REMIX_SKY_VETO_HASHES;
extern const Info<bool> GFX_REMIX_SKY_AUTO_UNTEXTURED_IGNORE;
extern const Info<bool> GFX_REMIX_SKY_AT_INFINITY;
extern const Info<float> GFX_REMIX_SKY_INFINITY_SCALE;
extern const Info<bool> GFX_REMIX_GX_LIGHT_NO_FALLOFF_DISTANT;
extern const Info<bool> GFX_REMIX_SKY_EMISSIVE;
extern const Info<float> GFX_REMIX_SKY_EMISSIVE_INTENSITY;
extern const Info<bool> GFX_REMIX_TRACE_EFB_COPIES;
extern const Info<bool> GFX_REMIX_UI_DROP_DST_ALPHA;
extern const Info<bool> GFX_REMIX_UI_DROP_EFB_COPY_TEXTURES;
extern const Info<bool> GFX_REMIX_UI_SCALE_TO_XFB;
extern const Info<bool> GFX_REMIX_GX_RAS_CHANNEL;
extern const Info<bool> GFX_REMIX_UI_RAS_CHANNEL;
extern const Info<bool> GFX_REMIX_WORLD_SCISSOR_SKIP;
extern const Info<bool> GFX_REMIX_TRACE_COLORS;
extern const Info<bool> GFX_REMIX_GX_TEV_COLOR;
extern const Info<bool> GFX_REMIX_GX_TEXTURE_STAGE;
extern const Info<bool> GFX_REMIX_DEBUG_COLOR_ROUTES;
extern const Info<bool> GFX_REMIX_EFB_EMULATION;
extern const Info<bool> GFX_REMIX_EFB_COPY_2D;
extern const Info<bool> GFX_REMIX_EFB_COPY_SCENE;
extern const Info<bool> GFX_REMIX_EFB_COPY_DEPTH;
extern const Info<bool> GFX_REMIX_EFB_COPY_INTENSITY;
extern const Info<bool> GFX_REMIX_EFB_XFB_ENCODE;
extern const Info<bool> GFX_REMIX_EFB_UI_COMPOSE;
extern const Info<bool> GFX_REMIX_EFB_SKIP_DISCARDED_TEX;
extern const Info<bool> GFX_REMIX_UI_DROP_PRE_WORLD_BLANK;
extern const Info<bool> GFX_REMIX_GX_LIGHT_DROP_DISTANT;
extern const Info<bool> GFX_REMIX_FALLBACK_LIGHT;

}  // namespace Config
