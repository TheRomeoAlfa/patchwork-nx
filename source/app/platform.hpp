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

}  // namespace platform
}  // namespace app
