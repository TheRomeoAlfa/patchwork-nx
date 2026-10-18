#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "util.hpp"

namespace mm {

// BCML packages (.bnp): a 7z holding the mod's files in Switch title folders
// (01007EF00011E000/romfs, 01007EF00011F001/romfs), info.json, and logs/ describing changes
// BCML merges into the original game files. Optional parts live in options/<name>/ with
// the same layout. Patchwork installs a package when it can merge every kind of change its
// logs list (see bcml::canMerge); the resource sizes go into a merged size table.
struct BnpCheck {
    bool forSwitch = true;               // false: a Wii U package (content/, aoc/)
    std::vector<std::string> mergeLogs;  // logs that need merging with the game's files
    bool installable() const;
    // What a cache keeps to answer installable() later, when Patchwork can merge more: the
    // merge logs, plus "!wiiu" for Wii U packages.
    std::vector<std::string> summary() const;
};
bool bnpInstallable(const std::vector<std::string>& summary);

// Works from the archive's file list alone, so it can run before downloading.
BnpCheck checkBnp(const std::vector<std::string>& archivePaths);

// Resource sizes the package (and the chosen options) asks for, by romfs path
// ("Model/Foo.bfres" -> bytes). 0 means "remove the entry". Reads logs/rstb.json or the older
// logs/rstb.log.
std::map<std::string, uint32_t> readBnpSizes(const fs::path& extractedRoot, const std::vector<std::string>& optionDirs = {});

// What to tell the user about a package that can't be installed.
std::string bnpProblem(const BnpCheck& check);

}  // namespace mm
