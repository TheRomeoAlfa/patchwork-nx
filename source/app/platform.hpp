#pragma once

#include <cstdint>
#include <string>

#include "games.hpp"
#include "util.hpp"

namespace app {

// Things that differ between the Switch and a desktop dev build.
namespace platform {

void init();
void exit();

// Root of the SD card. On desktop: $MM_SD_ROOT, or ./sd next to the working directory.
mm::fs::path sdRoot();

// Installed version of a game ("1.2.1"), or "" when unknown / not installed.
// On desktop: $MM_BOTW_VERSION / $MM_TOTK_VERSION.
std::string gameVersion(mm::Game game);

uint64_t freeSpaceBytes();

std::string userAgent();

// Certificate bundle shipped with the app ("" = system store).
std::string caBundle();

// A data file shipped with the app (romfs on Switch, ./resources on desktop).
mm::fs::path resourcePath(const std::string& name);

// Read one of the game's own files (path inside its romfs, e.g. "System/Resource/x"). On the
// Switch this only works while Patchwork runs in place of that game (opened by holding R
// while starting it); Atmosphère then passes the game's original files through, update
// included. On desktop: from $MM_GAME_ROMFS_BOTW / $MM_GAME_ROMFS_TOTK. Returns false with
// a reason when it can't.
bool readGameFile(mm::Game game, const std::string& romfsPath, std::string& out, std::string& why);

}  // namespace platform
}  // namespace app
