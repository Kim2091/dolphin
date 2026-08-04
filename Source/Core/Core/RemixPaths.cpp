// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/RemixPaths.h"

#include <algorithm>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "Core/Config/RemixSettings.h"

namespace RemixPaths
{
namespace
{
// Longest folder name a game ID may turn into. Disc IDs are six characters; the
// cap only exists because an ELF/DOL boot can put an arbitrary string here and
// these paths are handed to the runtime through environment variables that are
// read into a MAX_PATH buffer.
constexpr size_t MAX_GAME_ID_LENGTH = 64;

std::string StripTrailingSeparators(std::string path)
{
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
    path.pop_back();
  return path;
}

std::string Join(const std::string& base, std::string_view leaf)
{
  return StripTrailingSeparators(base) + DIR_SEP + std::string{leaf};
}

bool EnsureDirectory(const std::string& path)
{
  // CreateFullPath creates the PARENT of what it is given, so the trailing
  // separator is what makes it create `path` itself.
  return File::CreateFullPath(path + DIR_SEP);
}
}  // namespace

std::string GlobalRtxConf()
{
  return Join(File::GetExeDirectory(), "rtx.conf");
}

std::string GlobalUserConf()
{
  return Join(File::GetExeDirectory(), "user.conf");
}

std::string GlobalModsDir()
{
  return Join(Join(File::GetExeDirectory(), "rtx-remix"), "mods");
}

std::string GetRoot()
{
  const std::string configured = Config::Get(Config::GFX_REMIX_PER_GAME_ROOT);
  if (!configured.empty())
    return StripTrailingSeparators(configured);

  // Next to Dolphin.exe by default, so the whole Remix tree - the global
  // rtx.conf, the global rtx-remix/mods and the per-game folders - sits in one
  // place a user can find and a Toolkit project can be pointed at.
  const std::string next_to_exe = Join(File::GetExeDirectory(), "Remix");
  if (EnsureDirectory(next_to_exe))
    return next_to_exe;

  // A portable build on read-only media, or an installation under Program
  // Files. Symlinks the Toolkit may already have created point at absolute
  // paths, so this fallback changes where NEW games are placed and never moves
  // an existing one; set RemixPerGameRoot explicitly to pin it.
  const std::string in_user_dir = Join(File::GetUserPath(D_USER_IDX), "Remix");
  WARN_LOG_FMT(VIDEO,
               "Remix: cannot create '{}', so per-game files go to '{}' instead. Set "
               "RemixPerGameRoot to choose the location yourself.",
               next_to_exe, in_user_dir);
  return in_user_dir;
}

std::string SanitizeGameId(std::string_view game_id)
{
  std::string out;
  out.reserve(std::min(game_id.size(), MAX_GAME_ID_LENGTH));

  for (const char c : game_id.substr(0, MAX_GAME_ID_LENGTH))
  {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '-' || c == '_';
    out += safe ? c : '_';
  }

  // "." and ".." are directory entries, not names, and a name of nothing but
  // dots is the only way the filter above can produce one.
  if (out.find_first_not_of('.') == std::string::npos)
    return {};

  return out;
}

GamePaths ForGame(std::string_view game_id)
{
  const std::string id = SanitizeGameId(game_id);
  if (id.empty())
    return {};

  GamePaths paths;
  paths.game_dir = Join(GetRoot(), id);
  paths.rtx_conf = Join(paths.game_dir, "rtx.conf");
  paths.user_conf = Join(paths.game_dir, "user.conf");

  // The `rtx-remix` name and its three children are the Toolkit's contract, not
  // ours to shorten.
  const std::string remix_dir = Join(paths.game_dir, "rtx-remix");
  paths.mods = Join(remix_dir, "mods");
  paths.captures = Join(remix_dir, "captures");
  paths.logs = Join(remix_dir, "logs");
  return paths;
}

bool CreateDirectories(const GamePaths& paths)
{
  if (paths.game_dir.empty())
    return false;

  // Deliberately all four rather than stopping at the first failure: a partial
  // tree is still worth having, and the log should name everything that is
  // missing rather than only the first thing.
  bool ok = EnsureDirectory(paths.game_dir);
  ok = EnsureDirectory(paths.mods) && ok;
  ok = EnsureDirectory(paths.captures) && ok;
  ok = EnsureDirectory(paths.logs) && ok;

  if (!ok)
    ERROR_LOG_FMT(VIDEO, "Remix: could not create the per-game folders under '{}'", paths.game_dir);

  return ok;
}

size_t SeedFromGlobals(const GamePaths& paths)
{
  if (paths.game_dir.empty())
    return 0;

  const std::pair<std::string, std::string> pairs[] = {
      {GlobalRtxConf(), paths.rtx_conf},
      {GlobalUserConf(), paths.user_conf},
  };

  size_t copied = 0;
  for (const auto& [source, destination] : pairs)
  {
    // Already has one: leave it completely alone. Everything the game has been
    // tagged with since it was set up lives in there.
    if (File::Exists(destination))
      continue;

    // No template to seed from is not a problem. The runtime treats a missing
    // config file as an empty one, and the game will write its own on first save.
    if (!File::Exists(source))
      continue;

    if (File::Copy(source, destination))
    {
      ++copied;
      INFO_LOG_FMT(VIDEO, "Remix: seeded '{}' from '{}'", destination, source);
    }
    else
    {
      ERROR_LOG_FMT(VIDEO, "Remix: could not seed '{}' from '{}'", destination, source);
    }
  }

  return copied;
}
}  // namespace RemixPaths
