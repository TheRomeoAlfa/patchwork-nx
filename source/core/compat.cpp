#include "compat.hpp"

#include <algorithm>
#include <regex>
#include <sstream>

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

SizeTableCheck checkSizeTables(const std::vector<std::string>& targets, Game game, const std::string& installedVersion) {
    SizeTableCheck out;
    if (game != Game::Totk) return out;  // BOTW has one table for every version
    static const std::regex re(R"((^|/)romfs/system/resource/resourcesizetable\.product\.(\d)(\d)(\d)\.rsizetable(\.zs)?$)");
    std::smatch m;
    for (const auto& t : targets) {
        auto lower = toLower(t);
        if (!std::regex_search(lower, m, re)) continue;
        auto v = m[2].str() + "." + m[3].str() + "." + m[4].str();
        if (std::find(out.versions.begin(), out.versions.end(), v) == out.versions.end()) out.versions.push_back(v);
    }
    std::sort(out.versions.begin(), out.versions.end());
    out.missing = !out.versions.empty() && !installedVersion.empty() &&
                  std::find(out.versions.begin(), out.versions.end(), installedVersion) == out.versions.end();
    return out;
}

namespace {
std::vector<int> versionParts(const std::string& v) {
    std::vector<int> out;
    std::stringstream ss(v);
    std::string part;
    while (std::getline(ss, part, '.')) out.push_back(std::atoi(part.c_str()));
    while (out.size() < 3) out.push_back(0);
    return out;
}
}  // namespace

size_t closestVersionOption(const std::vector<std::string>& options, Game game, const std::string& installedVersion) {
    if (installedVersion.empty()) return options.size();
    auto installed = versionParts(installedVersion);
    size_t best = options.size(), naming = 0;
    std::vector<int> bestVersion;
    for (size_t i = 0; i < options.size(); ++i) {
        auto named = mentionedGameVersions(options[i], game);
        if (named.empty()) continue;
        ++naming;
        for (const auto& v : named) {
            auto parts = versionParts(v);
            if (parts > installed) continue;
            if (best == options.size() || parts > bestVersion) {
                best = i;
                bestVersion = parts;
            }
        }
    }
    return naming >= 2 ? best : options.size();
}

bool isAddOnFileOf(const std::string& own, const std::string& other) {
    auto stem = [](const std::string& name) {
        auto lower = toLower(name);
        auto dot = lower.rfind('.');
        return dot == std::string::npos ? lower : lower.substr(0, dot);
    };
    auto a = stem(own), b = stem(other);
    if (b.size() < 6 || a.size() <= b.size()) return false;
    auto pos = a.find(b);
    if (pos == std::string::npos) return false;
    // What the longer name adds; a bare version ("_v102", "-1.2", " v2.0b") isn't an add-on.
    auto extra = a.substr(0, pos) + a.substr(pos + b.size());
    static const std::regex versionOnly(R"(^[\s_.\-]*(v|ver|version)?[\s_.\-]*\d[\d._\-]*[a-z]?[\s_.\-]*$)");
    return !std::regex_match(extra, versionOnly);
}

}  // namespace mm
