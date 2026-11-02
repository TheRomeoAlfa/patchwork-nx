#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "byml.hpp"

namespace mm::tkmm {

// TKMM's mergers for Tears of the Kingdom's data tables, after TkSharp (MIT). Each takes the
// game's file and the mods' changelogs (lowest priority first) and returns the merged file,
// in the game file's byte order and version.

// RSDB row tables (RSDB/ActorInfo.Product.rstbl.byml…): a list of rows, each with a key
// field ("__RowId", "Name", "FullTagId" or "NameHash"). A changelog maps keys to a row's
// changes, a removal, or a new row. The rows are sorted by key afterwards.
std::string mergeRsdbRows(std::string_view canonical, const std::string& keyName, const std::string& vanilla,
                          const std::vector<std::string_view>& changelogs);

// The key field of an RSDB table TKMM merges by rows, or "" (Tag.Product.rstbl.byml is
// merged by mergeRsdbTags; other tables aren't merged).
std::string rsdbRowKey(std::string_view canonical);

// RSDB/Tag.Product.rstbl.byml: which tags each actor has, as a bit table. Changelogs add tags,
// and add or remove tags on entries (prefix, name, suffix).
std::string mergeRsdbTags(const std::string& vanilla, const std::vector<std::string_view>& changelogs);

// GameData/GameDataList.Product.byml: the game's flags and save data, in tables of rows by
// hash. Changelogs change, remove or add rows; the save data layout (MetaData) is worked out
// again afterwards.
std::string mergeGameData(std::string_view canonical, const std::string& vanilla, const std::vector<std::string_view>& changelogs);

// The save data layout for these tables ("Data" of GameDataList), into `metaData`.
void saveDataMetadata(byml::Node& metaData, const byml::Node& tables);

}  // namespace mm::tkmm
