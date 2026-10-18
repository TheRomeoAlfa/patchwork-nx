#include "layout.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace mm {

const char* packageKindMessage(PackageKind k) {
    switch (k) {
        case PackageKind::Installable: return "Ready to install";
        case PackageKind::Bcml: return "BCML package (.bnp): must be merged with BCML on a PC first";
        case PackageKind::Tkmm: return "TKMM package (.tkcl): must be merged with TKMM on a PC first";
        case PackageKind::WiiU: return "Wii U / Cemu format: not usable on Switch";
        case PackageKind::Empty: return "No Switch mod files found for this game";
    }
    return "";
}

namespace {

std::string matchTitleId(const std::string& name, Game game) {
    for (const auto& id : titleIdsFor(game))
        if (iequals(name, id)) return id;
    return {};
}

bool isTitleIdLike(const std::string& name) { return name.size() == 16 && isHex(name); }

// IPS patches are named by the executable's build id (usually 32-64 hex chars).
bool isIpsPatchName(const std::string& filename) {
    if (!endsWithI(filename, ".ips")) return false;
    auto stem = filename.substr(0, filename.size() - 4);
    return stem.size() >= 16 && isHex(stem);
}

enum class RootType { Atmosphere, ContentsDir, TitleDir, ModDir, LooseIps, BareRomfs };

struct Root {
    RootType type;
    fs::path path;
    std::string titleId;  // TitleDir, or a ModDir found inside a title folder
};

// Top-level folders of the games' romfs. Some archives contain these with no romfs/ around them.
bool isRomfsTopFolder(const std::string& lowerName) {
    static const std::set<std::string> names = {
        "actor", "pack", "model", "ui", "system", "map", "sound", "event", "eventflow", "font", "effect", "terrain",
        "physics", "movie", "layout", "camera", "cooking", "demo", "voice", "shader", "quest",          // both games / BOTW
        "banc", "mals", "rsdb", "gamedata", "ai", "logic", "textogo", "phive", "sequence", "worldmgr", // TOTK
        "as", "animationevent", "ecosystem", "xlink2", "vfxresource"};
    return names.count(lowerName) > 0;
}

// A folder that looks like the inside of a romfs: mostly (and at least one) romfs folder names.
bool looksLikeBareRomfs(const fs::path& dir) {
    int known = 0, other = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        (isRomfsTopFolder(toLower(it->path().filename().string())) ? known : other)++;
    }
    return known > 0 && known > other;
}

bool hasModFolders(const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        auto n = toLower(it->path().filename().string());
        if (it->is_directory(ec) && (n == "romfs" || n == "exefs" || n == "cheats")) return true;
    }
    return false;
}

struct Scanner {
    Game game;
    std::vector<Root> roots;
    std::vector<std::string>& warnings;
    bool sawBnp = false, sawTkcl = false, sawWiiU = false, sawBcmlLogs = false;

    void scan(const fs::path& dir, int depth) {
        if (depth > 8) return;
        std::vector<fs::directory_entry> entries;
        for (const auto& e : fs::directory_iterator(dir)) entries.push_back(e);
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });

        bool hasModFolder = false, hasRulesTxt = false, hasContentDir = false, hasLooseIps = false;
        bool hasInfoJson = false, hasLogsDir = false;
        for (const auto& e : entries) {
            auto name = toLower(e.path().filename().string());
            if (e.is_directory()) {
                if (name == "atmosphere") {
                    roots.push_back({RootType::Atmosphere, e.path(), {}});
                    return;
                }
                if (name == "contents" || name == "titles") {
                    roots.push_back({RootType::ContentsDir, e.path(), {}});
                    return;
                }
                if (isTitleIdLike(name)) {
                    if (hasModFolders(e.path())) {
                        roots.push_back({RootType::TitleDir, e.path(), name});
                    } else {
                        // Emulator mod folders: <titleid>/<Mod name>/romfs, often several mods.
                        std::error_code ec;
                        for (fs::directory_iterator sub(e.path(), ec), end; sub != end; sub.increment(ec))
                            if (sub->is_directory(ec) && hasModFolders(sub->path()))
                                roots.push_back({RootType::ModDir, sub->path(), name});
                    }
                    continue;
                }
                if (name == "romfs" || name == "exefs" || name == "cheats" || name == "exefs_patches")
                    hasModFolder = true;
                if (name == "content" || name == "aoc") hasContentDir = true;
                if (name == "logs") hasLogsDir = true;
            } else {
                if (endsWithI(name, ".bnp")) sawBnp = true;
                if (endsWithI(name, ".tkcl")) sawTkcl = true;
                if (name == "rules.txt") hasRulesTxt = true;
                if (name == "info.json") hasInfoJson = true;
                if (isIpsPatchName(name)) hasLooseIps = true;
            }
        }
        if (hasInfoJson && hasLogsDir) sawBcmlLogs = true;
        if (hasModFolder) {
            roots.push_back({RootType::ModDir, dir, {}});
            return;
        }
        if (hasContentDir || hasRulesTxt) {
            sawWiiU = true;
            return;
        }
        if (hasLooseIps) roots.push_back({RootType::LooseIps, dir, {}});
        for (const auto& e : entries) {
            auto name = e.path().filename().string();
            if (e.is_directory() && !isTitleIdLike(name)) scan(e.path(), depth + 1);
        }
    }
};

struct Mapper {
    Game game;
    std::string modSlug;
    std::vector<std::string>& warnings;
    std::map<std::string, fs::path> files;  // target -> source (later wins)
    int skippedOutside = 0, skippedOtherTitle = 0, skippedPchtxt = 0;

    void add(const fs::path& source, const std::string& target) { files[target] = source; }

    template <typename F>
    void walk(const fs::path& root, F&& fn) {
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file()) continue;
            auto rel = e.path().lexically_relative(root);
            fn(e.path(), pathParts(rel));
        }
    }

    static std::string join(const std::vector<std::string>& parts, size_t from) {
        std::string out;
        for (size_t i = from; i < parts.size(); ++i) {
            if (!out.empty()) out += '/';
            out += parts[i];
        }
        return out;
    }

    // Atmosphère only loads patches from exefs_patches/<name>/, so a file sitting directly
    // in exefs_patches/ gets a folder named after the mod.
    void addExefsPatch(const fs::path& src, const std::vector<std::string>& parts, size_t from) {
        if (from >= parts.size()) return;
        if (parts.size() - from == 1)
            add(src, "atmosphere/exefs_patches/" + modSlug + "/" + parts.back());
        else
            add(src, "atmosphere/exefs_patches/" + join(parts, from));
    }

    // Map files under a "<titleid>/" folder: romfs/, exefs/, cheats/.
    void mapTitleContents(const fs::path& src, const std::vector<std::string>& parts, size_t from,
                          const std::string& titleId) {
        if (from >= parts.size()) return;
        auto top = toLower(parts[from]);
        std::string base = "atmosphere/contents/" + titleId + "/";
        if (top == "romfs" || top == "cheats") {
            add(src, base + top + "/" + join(parts, from + 1));
        } else if (top == "exefs") {
            // Emulator-style packages put .ips files in exefs/; Atmosphère wants them in exefs_patches/.
            auto fname = parts.back();
            if (endsWithI(fname, ".ips"))
                add(src, "atmosphere/exefs_patches/" + modSlug + "/" + fname);
            else if (endsWithI(fname, ".pchtxt"))
                ++skippedPchtxt;
            else
                add(src, base + "exefs/" + join(parts, from + 1));
        } else if (top == "exefs_patches") {
            addExefsPatch(src, parts, from + 1);
        } else if (endsWithI(parts.back(), ".pchtxt")) {
            ++skippedPchtxt;
        } else {
            ++skippedOutside;
        }
    }

    void mapContentsDir(const fs::path& src, const std::vector<std::string>& parts, size_t from) {
        if (from >= parts.size()) return;
        auto id = matchTitleId(parts[from], game);
        if (id.empty()) {
            ++skippedOtherTitle;
            return;
        }
        mapTitleContents(src, parts, from + 1, id);
    }

    void map(const Root& root) {
        switch (root.type) {
            case RootType::Atmosphere:
                walk(root.path, [&](const fs::path& src, const std::vector<std::string>& parts) {
                    auto top = toLower(parts[0]);
                    if (top == "contents" || top == "titles")
                        mapContentsDir(src, parts, 1);
                    else if (top == "exefs_patches" && parts.size() > 1)
                        addExefsPatch(src, parts, 1);
                    else
                        ++skippedOutside;
                });
                break;
            case RootType::ContentsDir:
                walk(root.path, [&](const fs::path& src, const std::vector<std::string>& parts) {
                    mapContentsDir(src, parts, 0);
                });
                break;
            case RootType::TitleDir: {
                auto id = matchTitleId(root.titleId, game);
                if (id.empty()) {
                    ++skippedOtherTitle;
                    break;
                }
                walk(root.path, [&](const fs::path& src, const std::vector<std::string>& parts) {
                    mapTitleContents(src, parts, 0, id);
                });
                break;
            }
            case RootType::ModDir: {
                auto id = root.titleId.empty() ? std::string(gameInfo(game).titleId) : matchTitleId(root.titleId, game);
                if (id.empty()) {
                    ++skippedOtherTitle;
                    break;
                }
                walk(root.path, [&](const fs::path& src, const std::vector<std::string>& parts) {
                    mapTitleContents(src, parts, 0, id);
                });
                break;
            }
            case RootType::BareRomfs:
                walk(root.path, [&](const fs::path& src, const std::vector<std::string>& parts) {
                    if (parts.size() > 1 && isRomfsTopFolder(toLower(parts[0])))
                        add(src, "atmosphere/contents/" + std::string(gameInfo(game).titleId) + "/romfs/" + join(parts, 0));
                    else
                        ++skippedOutside;
                });
                break;
            case RootType::LooseIps:
                for (const auto& e : fs::directory_iterator(root.path)) {
                    auto fname = e.path().filename().string();
                    if (e.is_regular_file() && isIpsPatchName(fname))
                        add(e.path(), "atmosphere/exefs_patches/" + modSlug + "/" + toUpper(fname.substr(0, fname.size() - 4)) + ".ips");
                }
                break;
        }
    }
};

std::string variantName(const fs::path& rootPath, const fs::path& extractedRoot, RootType type) {
    auto anchor = rootPath;
    // For atmosphere/contents/<tid> roots the meaningful name is the folder above them.
    if (type == RootType::Atmosphere || type == RootType::ContentsDir || type == RootType::TitleDir)
        anchor = rootPath.parent_path();
    auto rel = anchor.lexically_relative(extractedRoot).generic_string();
    // "Option A.zip.extracted/..." (from a nested archive) reads better as "Option A/..."
    for (const char* ext : {".zip.extracted", ".7z.extracted", ".rar.extracted"}) {
        size_t pos;
        while ((pos = toLower(rel).find(ext)) != std::string::npos) rel.erase(pos, std::string(ext).size());
    }
    // Title-id folders ("0100F2C0115B6000/Mod name") say nothing to the user.
    std::string cleaned;
    for (const auto& part : pathParts(fs::path(rel)))
        if (!isTitleIdLike(part) && part != ".") cleaned += (cleaned.empty() ? "" : "/") + part;
    rel = cleaned;
    return (rel.empty() || rel == ".") ? "Main" : rel;
}

}  // namespace

LayoutResult detectLayout(const fs::path& extractedRoot, Game game, const std::string& modSlug) {
    LayoutResult result;
    Scanner scanner{game, {}, result.warnings};
    scanner.scan(extractedRoot, 0);
    // Last resort: game folders (Actor/, Model/, UI/...) with no romfs/ folder around them.
    bool bare = false;
    if (scanner.roots.empty() && !scanner.sawBnp && !scanner.sawTkcl && !scanner.sawWiiU && !scanner.sawBcmlLogs) {
        std::vector<fs::path> level{extractedRoot};
        for (int depth = 0; depth < 4 && scanner.roots.empty() && !level.empty(); ++depth) {
            std::vector<fs::path> next;
            for (const auto& dir : level) {
                if (looksLikeBareRomfs(dir)) {
                    scanner.roots.push_back({RootType::BareRomfs, dir, {}});
                    continue;
                }
                std::error_code ec;
                for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec))
                    if (it->is_directory(ec)) next.push_back(it->path());
            }
            std::sort(next.begin(), next.end());
            level = std::move(next);
        }
        bare = !scanner.roots.empty();
    }

    // Group roots that share an anchor folder (e.g. a TitleDir and loose IPS side by side).
    std::map<std::string, std::vector<Root>> groups;
    std::vector<std::string> order;
    for (const auto& r : scanner.roots) {
        auto name = variantName(r.path, extractedRoot, r.type);
        if (!groups.count(name)) order.push_back(name);
        groups[name].push_back(r);
    }

    int otherTitle = 0, outside = 0, pchtxt = 0;
    for (const auto& name : order) {
        Mapper mapper{game, modSlug, result.warnings, {}};
        for (const auto& r : groups[name]) mapper.map(r);
        otherTitle += mapper.skippedOtherTitle;
        outside += mapper.skippedOutside;
        pchtxt += mapper.skippedPchtxt;
        if (mapper.files.empty()) continue;
        Variant v;
        v.name = name;
        for (auto& [target, source] : mapper.files) v.files.push_back({source, target});
        result.variants.push_back(std::move(v));
    }

    if (otherTitle) result.warnings.push_back("Skipped files for a different game (" + std::to_string(otherTitle) + ")");
    if (outside) result.warnings.push_back("Skipped files outside the game's mod folders (" + std::to_string(outside) + ")");
    if (pchtxt) result.warnings.push_back("Skipped emulator-only .pchtxt patches (" + std::to_string(pchtxt) + ")");
    if (bare && !result.variants.empty())
        result.warnings.push_back("This archive has game folders but no romfs folder; they were placed in the game's romfs");

    if (!result.variants.empty())
        result.kind = PackageKind::Installable;
    else if (scanner.sawBnp || scanner.sawBcmlLogs)
        result.kind = PackageKind::Bcml;
    else if (scanner.sawTkcl)
        result.kind = PackageKind::Tkmm;
    else if (scanner.sawWiiU)
        result.kind = PackageKind::WiiU;
    else
        result.kind = PackageKind::Empty;
    return result;
}

}  // namespace mm
