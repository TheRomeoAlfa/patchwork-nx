#pragma once

#include <string>
#include <vector>

#include "games.hpp"

namespace mm {

// Pull game versions a mod page or file description names, e.g. "only tested on TotK
// v1.2.1" -> {"1.2.1"}. Only numbers next to words like "version", "update", "totk",
// "botw" or a leading "v" count, and only versions the game could plausibly have, so a
// mod's own version number is mostly ignored. "1.2.x" is returned as "1.2".
std::vector<std::string> mentionedGameVersions(const std::string& text, Game game);

enum class Compat { Unknown, Likely, Mismatch };

// Compare the installed game version (e.g. "1.2.1") against versions a mod names.
// A mod naming "1.2" matches any 1.2.x.
Compat checkCompat(const std::string& installedVersion, const std::vector<std::string>& named);

}  // namespace mm
