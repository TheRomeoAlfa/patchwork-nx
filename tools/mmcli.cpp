// Desktop dev tool: drives the same core the Switch app uses against a fake SD folder.
//   mmcli <sd-dir> list   <botw|totk> [page]
//   mmcli <sd-dir> search <botw|totk> <query>
//   mmcli <sd-dir> show   <modId>
//   mmcli <sd-dir> install <botw|totk> <modId> [fileIndex] [variantIndex,...]
//   mmcli <sd-dir> mods   <botw|totk>
//   mmcli <sd-dir> enable|disable|remove <id>
//   mmcli <sd-dir> up|down <id>
//   mmcli <sd-dir> saves  <botw|totk> <archive>     (save files an archive holds)
//   mmcli <sd-dir> sarcmerge <out> <game.pack> <mod.pack>...  (BCML pack merge, timed)
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "compat.hpp"
#include "gamebanana.hpp"
#include "installer.hpp"
#include "library.hpp"
#include "saves.hpp"
#include "sarc.hpp"
#include "yaml.hpp"
#include "byml.hpp"
#include "aamp.hpp"
#include "msbt.hpp"
#include "rstbcalc.hpp"
#include "nlohmann/json.hpp"
#include "sizetable.hpp"
#include <chrono>
#include <functional>

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
        } else if (cmd == "rstbcheck") {  // dev: compare our size calculation with a package's rstb.json
            fs::path root = args.at(0);
            auto log = nlohmann::json::parse(readFile(root / "logs" / "rstb.json"));
            std::map<std::string, uint32_t> ours;
            std::function<void(const std::string&, const std::string&, bool)> walk = [&](const std::string& name, const std::string& raw, bool nested) {
                ours[sizeTableName(name)] = resourceSize(name, raw);
                std::string data = isYaz0(raw) ? yaz0Decompress(raw) : raw;
                if (data.compare(0, 4, "SARC") == 0)
                    for (const auto& f : Sarc::parse(data).files) walk(f.name, f.data, true);
                (void)nested;
            };
            for (const char* tid : {"01007EF00011E000", "01007EF00011F001"}) {
                auto base = root / tid / "romfs";
                std::error_code ec;
                for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec))
                    if (it->is_regular_file()) walk(it->path().lexically_relative(base).generic_string(), readFile(it->path()), false);
            }
            std::map<std::string, std::pair<int, int>> byExt;  // ext -> (match, total)
            for (const auto& [canon, v] : log.items()) {
                auto it = ours.find(canon);
                if (it == ours.end()) continue;
                auto dot = canon.rfind('.');
                auto ext = canon.substr(dot);
                auto& e = byExt[ext];
                ++e.second;
                if (it->second == v.get<uint32_t>()) ++e.first;
                else if (e.second - e.first <= 2) std::cout << "  " << canon << ": ours " << it->second << ", log " << v.get<uint32_t>() << "\n";
            }
            for (const auto& [ext, e] : byExt) std::cout << ext << " " << e.first << "/" << e.second << "\n";
        } else if (cmd == "roundtrip") {  // dev: parse + write a BYML/SARC file (or files inside a SARC) and compare
            std::function<void(const std::string&, const std::string&)> check = [&](const std::string& name, const std::string& raw) {
                std::string data = isYaz0(raw) ? yaz0Decompress(raw) : raw;
                if (data.compare(0, 2, "YB") == 0) {
                    auto doc = byml::parse(data);
                    auto out = byml::write(doc);
                    bool same = byml::parse(out).root == doc.root;
                    std::cout << (same ? "ok  " : "BAD ") << (out == data ? "identical " : "differs   ") << data.size() << " -> " << out.size() << "  " << name << "\n";
                } else if (data.compare(0, 4, "AAMP") == 0) {
                    try {
                        auto pio = aamp::parse(data);
                        auto out = aamp::write(pio);
                        auto back = aamp::parse(out);
                        bool same = back.root == pio.root && back.type == pio.type && back.version == pio.version;
                        std::cout << (same ? "ok  " : "BAD ") << (out == data ? "identical " : "differs   ") << data.size() << " -> " << out.size() << "  " << name << "\n";
                    } catch (const std::exception& e) {
                        std::cout << "ERR " << e.what() << "  " << name << "\n";
                    }
                } else if (data.compare(0, 8, "MsgStdBn") == 0) {
                    try {
                        auto f = msbt::parse(data);
                        auto out = msbt::write(f);
                        auto back = msbt::parse(out);
                        bool same = back.messages.size() == f.messages.size();
                        for (size_t k = 0; same && k < f.messages.size(); ++k)
                            same = back.messages[k].first == f.messages[k].first && back.messages[k].second.text == f.messages[k].second.text;
                        std::cout << (same ? "ok  " : "BAD ") << (out == data ? "identical " : "differs   ") << data.size() << " -> " << out.size() << "  " << name
                                  << " (" << f.messages.size() << " msgs, attr " << f.attrSize << ")\n";
                    } catch (const std::exception& e) {
                        std::cout << "ERR " << e.what() << "  " << name << "\n";
                    }
                } else if (data.compare(0, 4, "SARC") == 0) {
                    for (const auto& f : Sarc::parse(data).files) check(name + "//" + f.name, f.data);
                }
            };
            for (const auto& a : args) check(a, readFile(a));
        } else if (cmd == "yaml") {  // dev: parse a YAML file and print its shape
            std::function<void(const yaml::Node&, int)> dump = [&](const yaml::Node& n, int depth) {
                if (depth > 3) return;
                std::string pad(size_t(depth) * 2, ' ');
                if (n.kind == yaml::Node::Kind::Map) {
                    std::cout << pad << n.tag << "{" << n.map.size() << "}\n";
                    for (size_t i = 0; i < n.map.size() && i < 3; ++i) {
                        std::cout << pad << " " << n.map[i].first << ":\n";
                        dump(n.map[i].second, depth + 1);
                    }
                } else if (n.kind == yaml::Node::Kind::Seq) {
                    std::cout << pad << n.tag << "[" << n.items.size() << "]\n";
                    for (size_t i = 0; i < n.items.size() && i < 2; ++i) dump(n.items[i], depth + 1);
                } else {
                    std::cout << pad << n.tag << (n.quoted ? " '" : " ") << n.value.substr(0, 60) << "\n";
                }
            };
            dump(yaml::parse(readFile(args.at(0))), 0);
        } else if (cmd == "sarcmerge") {
            std::vector<std::string> in;
            for (size_t i = 1; i < args.size(); ++i) in.push_back(readFile(args[i]));
            auto t0 = std::chrono::steady_clock::now();
            auto out = mergeSarcs(in, true);
            auto t1 = std::chrono::steady_clock::now();
            writeFileAtomic(args.at(0), out);
            std::cout << "merged " << out.size() << " bytes in " << std::chrono::duration<double>(t1 - t0).count() << " s\n";
            for (const auto& f : Sarc::parse(out).files) std::cout << "  " << f.data.size() << " " << f.name << "\n";
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
