#pragma once
// Waikuteru's mods (waikuteru-zelda-mods.com): read from the site's Downloads page and each
// mod's page. The site is a Wix site without an API, so this parses the HTML: if the site
// is redesigned, the parsers below are what needs updating (tests/test_waikuteru.cpp has
// saved copies of the pages).

#include <string>
#include <vector>

#include "gamebanana.hpp"  // reuses ModSummary / ModDetails / ModFile

namespace mm::wk {

inline constexpr const char* kSite = "https://www.waikuteru-zelda-mods.com";
inline constexpr const char* kDownloadsUrl = "https://www.waikuteru-zelda-mods.com/downloads";
inline constexpr const char* kPatreon = "patreon.com/waikuteru";

struct ListEntry {
    gb::ModSummary summary;  // sourceKey "wk:<game>/<slug>", profileUrl = mod page
    Game game = Game::Botw;
    bool onSwitch = true;    // BOTW cards say "Platform: Wii U" or "Wii U & Switch"
};

std::vector<ListEntry> parseDownloadsPage(const std::string& html);
// Files are named "... (Switch) ..." / "... (Wii U) ..."; ModFile::id is a stable hash of
// the link. Sizes aren't on the page (fill them with an HTTP HEAD request).
gb::ModDetails parseModPage(const std::string& html, const gb::ModSummary& summary);

bool isWiiUOnly(const gb::ModFile& f);
// "https://www.waikuteru-zelda-mods.com/downloads/botw/faster-teleport" -> true
bool isModPageUrl(const std::string& url);
std::string sourceKeyForUrl(const std::string& pageUrl);  // "wk:botw/faster-teleport"

}  // namespace mm::wk
