// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

// Where the Remix runtime keeps a game's files.
//
// Everything the runtime writes or reads outside of Dolphin - its config layer,
// its mods, its captures, its log - lives in one directory tree per game, so two
// titles cannot overwrite each other's settings. That mattered enough to build:
// a stale skybox tag left over from one game deleted another game's sky, and
// nothing announced it, because both games shared one rtx.conf.
//
// The layout is fixed by an external constraint, not by taste. The RTX Remix
// Toolkit's project wizard binds a mod project to a folder literally named
// `rtx-remix` holding `mods`, `captures` and `logs`, and it does so by creating
// SYMLINKS in both directions. So:
//
//   <root>/<GameID>/rtx.conf              the game's own settings layer
//   <root>/<GameID>/rtx-remix/mods        what the Toolkit is pointed at
//   <root>/<GameID>/rtx-remix/captures
//   <root>/<GameID>/rtx-remix/logs        where remix-dxvk.log lands
//
// with <root> defaulting to `Remix` next to Dolphin.exe. Once a project has been
// bound, RENAMING OR DELETING ONE OF THESE FOLDERS BREAKS THE SYMLINK PAIR and
// the user's mod project with it - so nothing here ever removes or relocates a
// directory, and there is deliberately no "clean up unused games" operation.
//
// This is pure path arithmetic plus directory creation; the environment
// variables that hand these paths to the runtime are set in the Remix video
// backend, which is the only place that knows when the runtime is about to load.
namespace RemixPaths
{
// Every path the runtime is told about, absolute, with no trailing separator.
struct GamePaths
{
  // <root>/<GameID>.
  std::string game_dir;
  // The game's own rtx.conf: a full, independent copy seeded once from the
  // global template, never a delta over it. See SeedFromGlobals.
  std::string rtx_conf;
  // The game's own user.conf, and the more important of the two in practice:
  // every edit made in the Remix dev menu targets the USER layer, so this is
  // where a texture tagged in-game actually lands. Sharing it across games is
  // what lets one game's mesh hash delete another game's sky.
  std::string user_conf;
  std::string mods;
  std::string captures;
  std::string logs;
};

// The files next to Dolphin.exe. The two .conf files are TEMPLATES: a game's
// folder is seeded from them the first time it is set up, and from then on the
// game reads and writes only its own copy. They are never loaded alongside a
// game's config and nothing here ever writes to them, so they can be curated as
// a known-good starting point and left alone.
//
// (They are also still where NVIDIA's own documentation says to put an rtx.conf,
// so a runtime launched outside Dolphin finds one where it expects to.)
std::string GlobalRtxConf();
std::string GlobalUserConf();
// The shared cross-game mods directory, which is NOT a template - it is used in
// place, and only when RemixPerGameMods is off.
std::string GlobalModsDir();

// GetRoot() honours the RemixPerGameRoot override, and otherwise puts the tree
// next to Dolphin.exe. Falls back to the user directory when the executable's
// own directory cannot be written to - a portable build on read-only media, or
// a development build whose executable sits in a build output folder.
std::string GetRoot();

// Folder name for a game ID. Dolphin's IDs are six alphanumeric characters
// (GQPP78), which need no escaping, but ELF/DOL boots and homebrew can produce
// anything, and this ends up in a path.
std::string SanitizeGameId(std::string_view game_id);

// Empty game_id yields an empty GamePaths; the caller is expected to treat that
// as "no per-game separation for this boot" rather than to invent a folder.
GamePaths ForGame(std::string_view game_id);

// Creates the four directories, eagerly, so that the Toolkit's project wizard
// has something to select before the game has ever been run. Returns false if
// any of them could not be created.
bool CreateDirectories(const GamePaths& paths);

// Copies the global rtx.conf and user.conf into the game's folder, but ONLY
// where the game does not already have that file. Returns the number copied.
//
// This is the whole shape of the feature and the reason it is a copy rather than
// a layer. The globals are a starting point, not a live dependency: once a game
// has its own pair it owns them outright, so a hash tagged for one title cannot
// reach another, and deleting something a game does not want actually deletes it
// rather than being re-supplied from underneath. The price is that a later
// improvement to the template does not reach games that already exist, which is
// the intended trade - it is the same reason the globals are never written to.
//
// Seeding only when the file is ABSENT is what makes it safe to run on every
// boot: re-copying would wipe out everything tagged in-game since.
size_t SeedFromGlobals(const GamePaths& paths);
}  // namespace RemixPaths
