// Desktop dev tool: drives the same core the Switch app uses against a fake SD folder.
//   mmcli <sd-dir> list   <botw|totk> [page]
//   mmcli <sd-dir> search <botw|totk> <query>
//   mmcli <sd-dir> show   <modId>
//   mmcli <sd-dir> install <botw|totk> <modId> [fileIndex] [variantIndex,...]
//   mmcli <sd-dir> mods   <botw|totk>
//   mmcli <sd-dir> enable|disable|remove <id>
//   mmcli <sd-dir> up|down <id>
//   mmcli <sd-dir> saves  <botw|totk> <archive>     (save files an archive holds)
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "compat.hpp"
#include "gamebanana.hpp"
#include "installer.hpp"
#include "library.hpp"
#include "saves.hpp"

using namespace mm;

static Game parseGame(const std::string& s) {
    auto g = gameFromKey(s);
    if (!g) throw std::runtime_error("game must be botw or totk");
    return *g;
}

static void printPage(const gb::Page& p) {
    for (const auto& m : p.mods)
        std::cout << m.id << "\t" << m.name << "  [" << m.category << "] by " << m.author << (m.nsfw ? " (nsfw)" : m.hasContentRatings ? " (rated)" : "") << "\n";
    std::cout << "(" << p.totalCount << " total)\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: see top of tools/mmcli.cpp\n";
        return 2;
    }
    try {
        fs::path sd = argv[1];
        std::string cmd = argv[2];
        std::vector<std::string> args(argv + 3, argv + argc);
        CurlHttpClient http("Mozilla/5.0 (compatible; patchwork/0.1)");
        gb::Client gbc(http);
        Library lib(sd, sd / "switch/patchwork");
        lib.load();

        if (cmd == "list") {
            printPage(gbc.list(parseGame(args.at(0)), gb::Sort::MostDownloaded, args.size() > 1 ? std::stoi(args[1]) : 1));
        } else if (cmd == "search") {
            printPage(gbc.search(parseGame(args.at(0)), args.at(1), 1));
        } else if (cmd == "show") {
            auto d = gbc.details(std::stoll(args.at(0)));
            std::cout << d.summary.name << " by " << d.summary.author << "\n";
            auto vers = mentionedGameVersions(d.descriptionHtml, Game::Totk);
            std::cout << "game versions mentioned:";
            for (auto& v : vers) std::cout << " " << v;
            std::cout << "\n";
            for (size_t i = 0; i < d.files.size(); ++i) {
                std::cout << " [" << i << "] {";
                for (auto& v : mentionedGameVersions(d.files[i].description, Game::Totk)) std::cout << v << " ";
                std::cout << "} " << d.files[i].filename << " (" << d.files[i].size / 1024 << " KiB) " << d.files[i].description << "\n";
            }
        } else if (cmd == "install") {
            auto game = parseGame(args.at(0));
            auto d = gbc.details(std::stoll(args.at(1)));
            size_t fi = args.size() > 2 ? std::stoul(args[2]) : 0;
            const auto& file = d.files.at(fi);
            auto dl = lib.dataDir() / "downloads" / (std::to_string(file.id) + "-" + file.filename);
            std::cout << "downloading " << file.filename << "...\n";
            http.download(file.downloadUrl, dl);
            auto slug = slugify(d.summary.name);
            auto prep = prepareArchive(dl, lib.dataDir() / "work", game, slug);
            std::cout << packageKindMessage(prep.layout.kind) << "\n";
            for (auto& w : prep.layout.warnings) std::cout << "warning: " << w << "\n";
            for (size_t i = 0; i < prep.layout.variants.size(); ++i)
                std::cout << " variant [" << i << "] " << prep.layout.variants[i].name << " (" << prep.layout.variants[i].files.size() << " files)\n";
            if (prep.layout.kind != PackageKind::Installable) return 1;
            std::vector<size_t> sel;
            if (args.size() > 3) {
                std::stringstream ss(args[3]);
                std::string tok;
                while (std::getline(ss, tok, ',')) sel.push_back(std::stoul(tok));
            } else {
                sel.push_back(0);
            }
            ModRecord r;
            r.id = "gb" + std::to_string(d.summary.id) + "-" + std::to_string(file.id);
            r.game = game;
            r.name = d.summary.name;
            r.author = d.summary.author;
            r.version = file.version.empty() ? d.version : file.version;
            r.sourceFile = file.filename;
            r.profileUrl = d.summary.profileUrl;
            r.gamebananaModId = d.summary.id;
            r.gamebananaFileId = file.id;
            r.gameVersionsNamed = mentionedGameVersions(d.descriptionHtml + " " + file.description, game);
            for (auto i : sel) r.variants.push_back(prep.layout.variants.at(i).name);
            auto& rec = lib.add(r, mergeVariants(prep.layout, sel), true);
            std::cout << "installed " << rec.id << " with " << rec.files.size() << " files\n";
            removeTree(prep.extractDir);
            fs::remove(dl);
        } else if (cmd == "saves") {
            auto game = parseGame(args.at(0));
            auto prep = prepareArchive(args.at(1), lib.dataDir() / "work", game, "x");
            std::cout << packageKindMessage(prep.layout.kind) << "\n";
            for (const auto& c : findSaves(prep.extractDir, game, &prep.fileTimes)) {
                std::cout << " save \"" << c.name << "\" slot=" << (c.slot.empty() ? "-" : c.slot) << (c.masterMode() ? " (master mode)" : "") << " t=" << c.savedTime;
                for (const auto& p : c.photoDirs) std::cout << " +" << p;
                std::cout << "\n";
            }
            removeTree(prep.extractDir);
        } else if (cmd == "mods") {
            auto game = parseGame(args.at(0));
            for (auto* m : lib.mods(game))
                std::cout << (m->enabled ? "[x] " : "[ ] ") << m->priority << " " << m->id << "  " << m->name << " (" << m->files.size() << " files)\n";
            for (auto& c : lib.conflicts(game)) {
                std::cout << "conflict " << c.target << ":";
                for (auto& id : c.modIds) std::cout << " " << id;
                std::cout << "\n";
            }
            for (auto& o : lib.patchOverlaps(game)) std::cout << "both patch exe " << o.buildId << "\n";
        } else if (cmd == "enable" || cmd == "disable") {
            lib.setEnabled(args.at(0), cmd == "enable");
        } else if (cmd == "remove") {
            lib.remove(args.at(0));
        } else if (cmd == "up" || cmd == "down") {
            lib.move(args.at(0), cmd == "up" ? 1 : -1);
        } else {
            std::cerr << "unknown command\n";
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
