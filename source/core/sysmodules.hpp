#pragma once

#include <string>
#include <vector>

#include "util.hpp"

namespace mm {

// Background add-ons ("sysmodules") in atmosphere/contents/<id>/exefs.nsp. Atmosphère starts
// one at boot when it has flags/boot2.flag; Patchwork turns one off by renaming that file to
// boot2.flag.off, so turning it back on restores exactly what was there.
struct Sysmodule {
    std::string titleId;  // folder name, as found on the card
    std::string name;     // from toolbox.json, a known id, or the id itself
    std::string about;    // one line for known add-ons, else ""
    bool enabled = false;
    bool needed = false;  // turning it off can stop games from starting (sys-patch)
};

std::vector<Sysmodule> listSysmodules(const fs::path& sdRoot);
// Takes effect after the Switch restarts.
void setSysmoduleEnabled(const fs::path& sdRoot, const std::string& titleId, bool enabled);

// Program id of Atmosphère's ams_mitm, which serves mod files to games (LayeredFS).
inline constexpr unsigned long long kModLoaderProgramId = 0x010041544D530000ull;

// File names in atmosphere/fatal_errors of crash reports from Atmosphère's mod loader.
std::vector<std::string> modLoaderCrashReports(const fs::path& sdRoot);

}  // namespace mm
