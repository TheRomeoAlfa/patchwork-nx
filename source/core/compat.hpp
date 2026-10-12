#pragma once

#include <string>
#include <vector>

#include "games.hpp"

namespace mm {

// Pull game versions out of a mod page or file description, e.g. "only tested on TotK
// v1.2.1" -> {"1.2.1"}. "1.2.x" is returned as "1.2". Only versions the game could
// plausibly have are returned.
//
// strict: only numbers next to a game word (TotK, BotW, game, update, patch) count. Use it
//         for free-form mod descriptions, which are full of changelog numbers.
// Otherwise a leading "v" or the word "version" also counts (file labels like "v1.2.1"),
// unless versionWordIsModVersion says this mod uses "Version x" for its own releases.
std::vector<std::string> mentionedGameVersions(const std::string& text, Game game, bool strict = false,
                                               bool versionWordIsModVersion = false);

// True if a mod's file labels use "Version x.y" for the mod's own releases (some number
// that can't be a game version, like "Version 2.6.1").
bool versionWordIsModVersion(const std::vector<std::string>& labels, Game game);

// "All Game Update Versions", "works on any version", ...
bool saysAllVersions(const std::string& text);

enum class Compat { Unknown, Likely, Mismatch };

// Compare the installed game version (e.g. "1.2.1") against versions a mod names.
// A mod naming "1.2" matches any 1.2.x.
Compat checkCompat(const std::string& installedVersion, const std::vector<std::string>& named);

// TOTK mods that change file sizes ship a resource size table per game version
// (romfs/System/Resource/ResourceSizeTable.Product.121.rsizetable.zs for 1.2.1). If a mod has
// tables but none for the installed version, the game falls back to its own table and the
// mod's bigger files can crash the game or fail to load.
struct SizeTableCheck {
    bool missing = false;               // tables present, but not for the installed version
    std::vector<std::string> versions;  // versions the mod has tables for, e.g. "1.2.0"
};
SizeTableCheck checkSizeTables(const std::vector<std::string>& targets, Game game, const std::string& installedVersion);

// Options named per game version ("Defogger (Game Version 1.1.2)", "… 1.2.0") with none for
// the installed version: the option for the newest version not newer than it, if any.
// Needs at least two options naming versions, so a lone version number isn't over-read.
// Returns options.size() when there's nothing sensible to pick.
size_t closestVersionOption(const std::vector<std::string>& options, Game game, const std::string& installedVersion);

// True if file `own` looks like an add-on or variant of file `other` from the same mod
// ("dark_title_screen_defogger.zip" of "defogger.zip"), as opposed to just another version
// of it ("fierce_deity_v102.7z" of "fierce_deity.7z"). Compares names without extensions.
bool isAddOnFileOf(const std::string& own, const std::string& other);

// True for downloads that put the game's original files back ("uninstall_helm_pack.rar",
// "vanilla_files.zip"), meant for undoing a hand-copied install. Patchwork undoes mods itself.
bool isRestoreOriginalsFile(const std::string& filename, const std::string& description = "");

}  // namespace mm
