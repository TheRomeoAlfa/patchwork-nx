// Desktop dev tool: run real GameBanana mods through Patchwork's install pipeline (download,
// unpack, layout detection, checks) without installing, and print what the app would do.
//   survey <work-dir> <botw|totk> <installed-version> <modId>...
#include <iostream>
#include <map>
#include <set>

#include "archive.hpp"
#include "compat.hpp"
#include "gamebanana.hpp"
#include "http.hpp"
#include "installer.hpp"
#include "util.hpp"

using namespace mm;

namespace {

const char* kindName(PackageKind k) {
    switch (k) {
        case PackageKind::Installable: return "installable";
        case PackageKind::Bcml: return "BCML";
        case PackageKind::Tkmm: return "TKMM";
        case PackageKind::WiiU: return "WiiU";
        case PackageKind::Empty: return "EMPTY";
    }
    return "?";
}

// Where a target lands, for a compact summary ("romfs", "exefs", "exefs_patches", ...).
std::string area(const std::string& target) {
    auto parts = pathParts(target);
    if (parts.size() >= 3 && parts[0] == "atmosphere" && parts[1] == "contents" && parts.size() >= 4)
        return parts[2] + "/" + parts[3];
    if (parts.size() >= 2 && parts[0] == "atmosphere") return "atmosphere/" + parts[1];
    return "OTHER:" + (parts.empty() ? std::string() : parts[0]);
}

bool suspicious(const std::string& target) {
    static const std::set<std::string> exts = {".txt", ".md", ".pdf", ".png", ".jpg", ".jpeg", ".url", ".html", ".htm",
                                               ".rtf", ".docx", ".exe", ".bat", ".7z", ".zip", ".rar", ".json", ".ini", ".gif", ".webp", ".mp4", ".bnp", ".tkcl"};
    auto name = toLower(fs::path(target).filename().string());
    auto ext = fs::path(name).extension().string();
    return exts.count(ext) || name.find("readme") != std::string::npos || name == "thumbs.db" || name == ".ds_store";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: survey <work-dir> <botw|totk> <installed-version> <modId>...\n";
        return 2;
    }
    fs::path work = argv[1];
    Game game = std::string(argv[2]) == "botw" ? Game::Botw : Game::Totk;
    std::string installed = argv[3];
    CurlHttpClient http("Mozilla/5.0 (compatible; patchwork-survey/0.1)");
    gb::Client gbc(http);

    for (int a = 4; a < argc; ++a) {
        int64_t id = std::stoll(argv[a]);
        std::cout << "\n=== " << id << " ";
        gb::ModDetails d;
        try {
            d = gbc.details(id);
        } catch (const std::exception& e) {
            std::cout << "DETAILS FAILED: " << e.what() << "\n";
            continue;
        }
        std::cout << d.summary.name << "  [" << d.summary.category << "] gameId=" << d.summary.gameId
                  << (d.summary.nsfw ? " NSFW" : "") << (d.summary.obsolete ? " OBSOLETE" : "") << "\n";
        auto descVersions = mentionedGameVersions(d.descriptionHtml, game, true);
        std::cout << "  description mentions:";
        for (const auto& v : descVersions) std::cout << " " << v;
        std::cout << (saysAllVersions(d.descriptionHtml) ? " (says all versions)" : "") << "\n";
        int tried = 0;
        for (const auto& f : d.files) {
            std::cout << "  file " << f.filename << " (" << f.size / 1024 << " KiB)";
            auto fileVersions = mentionedGameVersions(f.description + " " + f.filename, game, false);
            if (!fileVersions.empty()) {
                std::cout << " names:";
                for (const auto& v : fileVersions) std::cout << " " << v;
            }
            if (!f.avClean) std::cout << " AV-FLAGGED";
            if (tried >= 4 || f.size > 250ll * 1024 * 1024) {
                std::cout << " (skipped)\n";
                continue;
            }
            ++tried;
            std::cout << "\n";
            auto dl = work / "dl" / (std::to_string(f.id) + "-" + slugify(f.filename, 80));
            try {
                std::error_code ec;
                if (!fs::exists(dl, ec)) {
                    fs::create_directories(dl.parent_path());
                    http.download(f.downloadUrl, dl);
                }
                auto prep = prepareArchive(dl, work / "x", game, slugify(d.summary.name));
                const auto& L = prep.layout;
                std::cout << "    kind=" << kindName(L.kind) << " variants=" << L.variants.size();
                for (const auto& w : L.warnings) std::cout << "\n    WARNING: " << w;
                std::cout << "\n";
                for (const auto& v : L.variants) {
                    std::map<std::string, int> areas;
                    std::vector<std::string> odd;
                    std::vector<std::string> targets;
                    for (const auto& m : v.files) {
                        areas[area(m.target)]++;
                        targets.push_back(m.target);
                        if (suspicious(m.target)) odd.push_back(m.target);
                    }
                    std::cout << "    - \"" << v.name << "\" " << v.files.size() << " files:";
                    for (const auto& [k, n] : areas) std::cout << " " << k << "=" << n;
                    std::cout << "\n";
                    for (size_t i = 0; i < odd.size() && i < 4; ++i) std::cout << "      ODD FILE: " << odd[i] << "\n";
                    if (odd.size() > 4) std::cout << "      (+" << odd.size() - 4 << " more odd files)\n";
                    auto tables = checkSizeTables(targets, game, installed);
                    if (!tables.versions.empty()) {
                        std::cout << "      size tables:";
                        for (const auto& t : tables.versions) std::cout << " " << t;
                        std::cout << (tables.missing ? "  -> MISSING " + installed : "") << "\n";
                    }
                }
            } catch (const std::exception& e) {
                std::cout << "    FAILED: " << e.what() << "\n";
            }
        }
    }
    return 0;
}
