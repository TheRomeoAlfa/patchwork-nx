#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tkcl.hpp"
#include "totk.hpp"
#include "util.hpp"

namespace mm::tkmm {

// TKMM's name for a file of a mod's romfs ("RSDB/Tag.Product.121.rstbl.byml.zs" ->
// "RSDB/Tag.Product.rstbl.byml", version 121, product file with ".zs"), after TkSharp's
// TkPathExtensions.GetCanonical (MIT).
std::string canonicalName(std::string_view romfsPath, int* version = nullptr, uint32_t* attributes = nullptr);

// Of a mod's files, (romfs path, file), the ones the game reads: where a mod has a copy of a
// file for each game version ("Mals/USen.Product.100.sarc.zs" … ".121."), only the one with
// this game's real name, or else its newest.
std::vector<std::pair<std::string, fs::path>> forThisGame(const std::vector<std::pair<std::string, fs::path>>& files, const totk::Rom& rom);

// Changelogs for an ordinary romfs mod's files, as TKMM makes them when importing one
// (TkChangelogBuilder): each file compared with the game's copy and stored as only its
// changes (BYML, RSDB tables, game data, text, SARC archives, the files inside packs), or
// whole when it can't be. `files`: (path in the romfs, as the mod has it; the file). Writes
// the changelog files under `outDir` ("romfs/…") and returns the changelog listing them.
tkcl::Changelog buildChangelog(const std::vector<std::pair<std::string, fs::path>>& files, totk::Rom& rom, const fs::path& outDir);

}  // namespace mm::tkmm
