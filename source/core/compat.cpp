#include "compat.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

#include "util.hpp"

namespace mm {

namespace {

int maxMinor(Game game) {
    // Highest minor version each game has shipped, with headroom for TOTK updates.
    return game == Game::Botw ? 6 : 9;
}

std::vector<int> versionParts(const std::string& v) {
    std::vector<int> out;
    std::stringstream ss(v);
    std::string part;
    while (std::getline(ss, part, '.')) out.push_back(std::atoi(part.c_str()));
    while (out.size() < 3) out.push_back(0);
    return out;
}

// "1.1.0-1.4.3" -> {"1.1.0", "1.4.3"}; a single version -> {v, v}.
std::pair<std::string, std::string> rangeEnds(const std::string& v) {
    auto dash = v.find('-');
    return dash == std::string::npos ? std::make_pair(v, v) : std::make_pair(v.substr(0, dash), v.substr(dash + 1));
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
    // "… 1.1.0 - 1.4.3", "1.2.0 to 1.4.1": the second number ends a range.
    static const std::regex rangeEndRe(
        "^\\s*(?:-|\xe2\x80\x93|\xe2\x80\x94|~|to|through|thru|until)\\s*(?:v|ver\\.?\\s*|version\\s*)?"
        "(\\d+)\\.(\\d{1,2})(\\.\\d{1,2})?(\\.x)?(?![\\d.]*\\d)");
    std::vector<std::string> out;
    size_t consumed = 0;  // a range's end isn't read again on its own
    for (std::sregex_iterator it(text.begin(), text.end(), versionRe), end; it != end; ++it) {
        const auto& m = *it;
        if (static_cast<size_t>(m.position(3)) < consumed) continue;
        std::string keyword = m[1].matched ? m[1].str() : "";
        bool vPrefix = m[2].matched;
        bool versionWord = keyword == "version" || keyword == "ver" || keyword == "ver.";
        bool gameWord = !keyword.empty() && !versionWord;
        // A bare "Version 1.0" / "v1.0" is usually the mod's own version; game versions are
        // written with three parts (1.2.1, 1.6.0), so only those count without a game word.
        bool threeParts = m[5].matched;
        bool accept = gameWord || (!strict && threeParts &&
                                   ((versionWord && !versionWordIsModVersion) || (vPrefix && keyword.empty())));
        if (!accept) continue;
        if (m[3].str() != "1" || std::stoi(m[4].str()) > maxMinor(game)) continue;
        auto v = "1." + std::to_string(std::stoi(m[4].str())) + (m[5].matched ? m[5].str() : "");
        std::smatch r;
        auto after = text.cbegin() + m.position() + m.length();
        if (std::regex_search(after, text.cend(), r, rangeEndRe) && r[1].str() == "1" && std::stoi(r[2].str()) <= maxMinor(game)) {
            auto to = "1." + std::to_string(std::stoi(r[2].str())) + (r[3].matched ? r[3].str() : "");
            if (versionParts(to) > versionParts(v)) {
                v += "-" + to;
                consumed = static_cast<size_t>(m.position() + m.length() + r.position() + r.length());
            }
        }
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
    if (named.find('-') != std::string::npos) {
        auto [from, to] = rangeEnds(named);
        auto v = versionParts(installed), hi = versionParts(to);
        if (std::count(to.begin(), to.end(), '.') == 1) hi[2] = 99;  // "… - 1.4": every 1.4.x
        return v >= versionParts(from) && v <= hi;
    }
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
            auto [from, to] = rangeEnds(v);
            if (versionParts(from) > installed) continue;
            auto parts = std::min(versionParts(to), installed);  // a range up to or past it: the installed one
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

bool isRestoreOriginalsFile(const std::string& filename, const std::string& description) {
    auto check = [](const std::string& text) {
        std::vector<std::string> words;
        std::string word;
        for (char c : toLower(text) + " ") {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                word += c;
            } else if (!word.empty()) {
                words.push_back(word);
                word.clear();
            }
        }
        auto has = [&](const char* w) { return std::find(words.begin(), words.end(), w) != words.end(); };
        if (has("uninstall") || has("uninstaller") || has("restore") || has("restorer")) return true;
        // "vanilla" alone is also a style ("vanilla-friendly"); only with a word about files.
        return has("vanilla") && (has("files") || has("file") || has("backup") || has("original") || has("originals"));
    };
    auto dot = filename.rfind('.');
    return check(dot == std::string::npos ? filename : filename.substr(0, dot)) || check(description);
}

bool saysTkmmOnly(const std::string& raw) {
    auto text = plainText(raw);
    // TKMM, or its package format (.tkcl): "TKCL file", "emotewheelv1tkcl.zip".
    if (text.find("tkmm") == std::string::npos && text.find("tkcl") == std::string::npos) return false;
    static const std::regex without(R"(\b(without|w/o|no|non|not|sans)[\s\-_]*(a\s+|the\s+)?(tkmm|tkcl))");
    // One archive with both: "(TKMM file included)", "Includes TKCL file. RomFS built for 1.2.0".
    static const std::regex included(R"((tkmm|tkcl)\s+(file\s+)?included|\bincludes?\s+(a\s+|the\s+)?(tkmm|tkcl))");
    // Plain files that only advise TKMM for merging: "romfs version, install still with tkmm",
    // but not "TKCL file (will upload romfs version later)".
    static const std::regex files(R"(\b(romfs|exefs)\b)");
    static const std::regex noFiles(R"(\b(without|w/o|no|non|not|upload)[\s\-_]*(a\s+|the\s+)?(romfs|exefs)\b)");
    if (std::regex_search(text, without) || std::regex_search(text, included)) return false;
    return !(std::regex_search(text, files) && !std::regex_search(text, noFiles));
}

int languageMatch(const std::string& raw, const std::string& language) {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> names = {
        {"en", {"english"}},
        {"de", {"german", "deutsch"}},
        {"fr", {"french", "fran\xc3\xa7" "ais", "francais"}},
        {"es", {"spanish", "espa\xc3\xb1" "ol", "espanol", "castellano"}},
        {"it", {"italian", "italiano"}},
        {"nl", {"dutch", "nederlands"}},
        {"ru", {"russian"}},
        {"ja", {"japanese"}},
        {"zh", {"chinese"}},
        {"ko", {"korean"}},
    };
    auto text = plainText(raw);
    int result = 0;
    for (const auto& [code, words] : names)
        for (const auto& w : words) {
            auto at = text.find(w);
            // a whole word: "english", not part of a longer one
            bool word = at != std::string::npos && (at == 0 || !std::isalpha(static_cast<unsigned char>(text[at - 1]))) &&
                        (at + w.size() == text.size() || !std::isalpha(static_cast<unsigned char>(text[at + w.size()])));
            if (!word) continue;
            if (code == language) return 1;
            result = -1;
        }
    return result;
}

}  // namespace mm
