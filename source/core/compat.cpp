#include "compat.hpp"

#include <algorithm>
#include <regex>

#include "util.hpp"

namespace mm {

std::vector<std::string> mentionedGameVersions(const std::string& rawText, Game game) {
    // Strip HTML tags GameBanana descriptions contain.
    static const std::regex tags("<[^>]*>");
    auto text = toLower(std::regex_replace(rawText, tags, " "));

    static const std::regex versionRe(
        R"((totk|botw|game|version|ver\.?|update|patch)?\s*[:\-]?\s*(v)?(1\.\d{1,2}(?:\.\d{1,2})?)(\.x)?(?![\d.]*\d))");
    // Highest minor version each game has shipped, with some headroom for future updates.
    const int maxMinor = game == Game::Botw ? 6 : 9;
    std::vector<std::string> out;
    for (std::sregex_iterator it(text.begin(), text.end(), versionRe), end; it != end; ++it) {
        const auto& m = *it;
        bool keyword = m[1].matched, vPrefix = m[2].matched;
        if (!keyword && !vPrefix) continue;
        auto v = m[3].str();
        if (std::stoi(v.substr(2)) > maxMinor) continue;
        if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    }
    return out;
}

static bool versionMatches(const std::string& installed, const std::string& named) {
    if (installed == named) return true;
    // "1.2" covers "1.2.0", "1.2.1", ...
    return installed.size() > named.size() && installed.compare(0, named.size(), named) == 0 &&
           installed[named.size()] == '.';
}

Compat checkCompat(const std::string& installedVersion, const std::vector<std::string>& named) {
    if (installedVersion.empty() || named.empty()) return Compat::Unknown;
    for (const auto& n : named)
        if (versionMatches(installedVersion, n)) return Compat::Likely;
    return Compat::Mismatch;
}

}  // namespace mm
