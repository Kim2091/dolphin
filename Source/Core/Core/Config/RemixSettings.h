// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <variant>

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
extern const Info<bool> GFX_REMIX_VIEWPORT_REF_XFB;
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
extern const Info<bool> GFX_REMIX_EFB_DROP_AUX_PASS;
extern const Info<bool> GFX_REMIX_UI_DROP_PRE_WORLD_BLANK;
extern const Info<bool> GFX_REMIX_GX_LIGHT_DROP_DISTANT;
extern const Info<bool> GFX_REMIX_FALLBACK_LIGHT;
extern const Info<bool> GFX_REMIX_PER_GAME_PATHS;
extern const Info<std::string> GFX_REMIX_PER_GAME_ROOT;
extern const Info<bool> GFX_REMIX_PER_GAME_MODS;
extern const Info<bool> GFX_REMIX_RESTART_ON_STOP;
extern const Info<bool> GFX_REMIX_GPU_SKINNING;
extern const Info<bool> GFX_REMIX_DYNAMIC_MESH_IDENTITY;
extern const Info<bool> GFX_REMIX_SKIP_MINOR_FRAMES;
extern const Info<bool> GFX_REMIX_UI_TAG_ROUTING;
extern const Info<bool> GFX_REMIX_UI_TAG_PERSPECTIVE;
extern const Info<bool> GFX_REMIX_UI_DROP_PRE_WORLD;
extern const Info<bool> GFX_REMIX_UI_DROP_FULL_SCREEN_OPAQUE;
extern const Info<bool> GFX_REMIX_UI_STRICT;
extern const Info<bool> GFX_REMIX_UI_DEPTH;
extern const Info<bool> GFX_REMIX_UI_WORLD_VIEW;

// Must equal the number of extern Info<> declarations above. The metadata table
// in RemixSettings.cpp static_asserts against this; if you add a knob, add its
// table row or the build breaks.
inline constexpr size_t kRemixSettingCount = 73;

// One row per knob: how to edit it, where it belongs, whether it can be changed
// while a game is running, and what it is for in plain language.
//
// It lives in Core rather than in DolphinQt so that the settings and their
// descriptions stay one thing. The GUI walks this table and emits a control per
// row, so a knob that is declared but has no row is a build failure rather than
// a silently unreachable option.
struct RemixSettingMeta
{
  // The sections of Source/Core/VideoBackends/Remix/README.md, in the order the
  // GUI shows them.
  enum class Group
  {
    RuntimeScale,
    Files,
    CameraRecovery,
    Sky,
    GxSemantics,
    ProjectionViewport,
    UiOverlay,
    Efb,
    Diagnostics,
  };

  // Whether the backend ever re-reads the value. RequiresRestart knobs are read
  // once when the backend starts and are then baked into meshes, materials,
  // lights and classification state, so editing one mid-game does nothing;
  // Live knobs are re-read at every frame boundary
  // (RemixApi::RefreshLiveConfig).
  enum class Liveness
  {
    Live,
    RequiresRestart,
  };

  enum class Maturity
  {
    Stable,
    Experimental,
    Diagnostic,
  };

  // Which setting this row describes. The engaged alternative also decides
  // which control the GUI emits: bool -> checkbox, int -> spin box or combo box
  // (combo when `choices` is non-empty), float -> slider, string -> line edit.
  std::variant<const Info<bool>*, const Info<int>*, const Info<float>*, const Info<std::string>*>
      setting;
  // What the GUI calls this option. A short descriptive phrase rather than the
  // INI key, because the key names are abbreviations that only mean something
  // once you already know the backend. The key is still shown, as the title of
  // the option's tooltip, so anything on screen can still be found in GFX.ini,
  // in a log line or in a bug report.
  const char* label;
  // Plain, untranslated help text. Labels and knob names live in Core, which
  // cannot reach Qt's translation pipeline, so the whole tab is deliberately
  // English-only; knob names are INI identifiers and must never be translated
  // in any case.
  const char* tooltip;
  Group group;
  Liveness liveness;
  Maturity maturity;
  // Range and granularity of the numeric control. Where RemixApi::Initialize
  // clamps the value these match that clamp exactly, so the GUI cannot offer a
  // value the backend would refuse; elsewhere they are merely generous. Unused
  // on bool and string rows.
  float min;
  float max;
  float step;
  // Non-empty only on int rows that should be a combo box. The stored value is
  // the combo INDEX, so entry n must describe value n.
  std::span<const char* const> choices;
};

// The table itself, in GUI display order. Exactly kRemixSettingCount rows.
std::span<const RemixSettingMeta> GetRemixSettingsMetadata();

}  // namespace Config
