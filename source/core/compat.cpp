#include "compat.hpp"

#include <algorithm>
#include <regex>

#include "util.hpp"

namespace mm {

namespace {

int maxMinor(Game game) {
    // Highest minor version each game has shipped, with headroom for TOTK updates.
    return game == Game::Botw ? 6 : 9;
}

std::string plainText(const std::string& raw) {
    static const std::regex tags("<[^>]*>");
    return toLower(std::regex_replace(raw, tags, " "));
}

}  // namespace

std::vector<std::string> mentionedGameVersions(const std::string& rawText, Game game, bool strict,
                                               bool versionWordIsModVersion) {
    auto text = plainText(rawText);
    static const std::regex versionRe(
        R"((?:\b(tears of the kingdom|breath of the wild|totk|botw|game version|game|update|patch|version|ver\.?))?)"
        R"(\s*[:\-]?\s*(v)?(\d+)\.(\d{1,2})(\.\d{1,2})?(\.x)?(?![\d.]*\d))");
    std::vector<std::string> out;
    for (std::sregex_iterator it(text.begin(), text.end(), versionRe), end; it != end; ++it) {
        const auto& m = *it;
        std::string keyword = m[1].matched ? m[1].str() : "";
        bool vPrefix = m[2].matched;
        bool versionWord = keyword == "version" || keyword == "ver" || keyword == "ver.";
        bool gameWord = !keyword.empty() && !versionWord;
        bool accept = gameWord || (!strict && ((versionWord && !versionWordIsModVersion) || (vPrefix && keyword.empty())));
        if (!accept) continue;
        if (m[3].str() != "1" || std::stoi(m[4].str()) > maxMinor(game)) continue;
        auto v = "1." + std::to_string(std::stoi(m[4].str())) + (m[5].matched ? m[5].str() : "");
        if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    }
    return out;
}

bool versionWordIsModVersion(const std::vector<std::string>& labels, Game game) {
    static const std::regex re(R"(\bver(?:sion)?\.?\s*v?(\d+)\.(\d+))");
    for (const auto& label : labels) {
        auto text = plainText(label);
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
            if ((*it)[1].str() != "1" || std::stoi((*it)[2].str()) > maxMinor(game)) return true;
    }
    return false;
}

bool saysAllVersions(const std::string& raw) {
    static const std::regex re(R"(\b(all|any|every)\s+(game\s+)?(update\s+)?versions?\b|\ball\s+(game\s+)?updates\b)");
    auto text = plainText(raw);
    return std::regex_search(text, re);
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
