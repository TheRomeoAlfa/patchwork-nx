#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "games.hpp"
#include "util.hpp"

namespace mm {

// What kind of package an extracted archive turned out to be.
enum class PackageKind {
    Installable,  // LayeredFS / exefs_patches / cheats files we can copy
    Bcml,         // BOTW .bnp package — needs BCML merging on a PC
    Tkmm,         // TOTK .tkcl package — needs TKMM merging on a PC
    WiiU,         // Cemu / Wii U graphic pack layout (content/, rules.txt)
    Empty,        // nothing recognizable for this game
};

const char* packageKindMessage(PackageKind k);

struct FileMapping {
    fs::path source;     // absolute path inside the extraction dir
    std::string target;  // path relative to the SD root, '/'-separated
};

// An archive can hold several independent installable roots (e.g. "60fps/" and
// "30fps/" folders). Each one is offered to the user as a separate option.
struct Variant {
    std::string name;
    std::vector<FileMapping> files;
    std::map<std::string, uint32_t> resourceSizes;  // BCML packages: romfs path -> size (0 = remove)
    fs::path bcmlLogs;  // BCML packages that need merging: this variant's logs folder
};

struct LayoutResult {
    PackageKind kind = PackageKind::Empty;
    std::vector<Variant> variants;
    std::vector<std::string> warnings;
    std::string problem;  // why it can't be installed, when more specific than the kind's message
};

// Inspect an extracted archive and decide where each file belongs on the SD card.
// `modSlug` names the exefs_patches subfolder for loose .ips patches.
LayoutResult detectLayout(const fs::path& extractedRoot, Game game, const std::string& modSlug);

}  // namespace mm
