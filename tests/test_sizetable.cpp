#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <fstream>
#include <random>

#include "bnp.hpp"
#include "installer.hpp"
#include "http.hpp"
#include "library.hpp"
#include "remote7z.hpp"
#include "sizetable.hpp"
#include "test.hpp"

using namespace mm;

namespace {

void write(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

std::string table(const std::map<std::string, uint32_t>& entries, const std::map<std::string, uint32_t>& named = {}) {
    SizeTable t;
    for (const auto& [name, size] : entries) t.crc[sizeTableCrc(name)] = size;
    t.named = named;
    return yaz0Store(t.write());
}

uint32_t sizeOf(const std::string& raw, const std::string& name) {
    auto t = SizeTable::parse(raw);
    auto n = t.named.find(name);
    if (n != t.named.end()) return n->second;
    auto c = t.crc.find(sizeTableCrc(name));
    return c == t.crc.end() ? 0 : c->second;
}

void makeArchive(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& entries) {
    auto* a = archive_write_new();
    archive_write_set_format_7zip(a);
    archive_write_open_filename(a, path.string().c_str());
    for (const auto& [name, data] : entries) {
        auto* e = archive_entry_new();
        archive_entry_set_pathname(e, name.c_str());
        archive_entry_set_size(e, static_cast<la_int64_t>(data.size()));
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_write_header(a, e);
        archive_write_data(a, data.data(), data.size());
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
}

}  // namespace

TEST("sizetable: Yaz0 back-references and stored output") {
    // "abcabcabc": three literals, then copy 6 bytes from 3 back.
    std::string comp = std::string("Yaz0") + std::string("\0\0\0\x09", 4) + std::string(8, '\0') + "\xE0" "abc" "\x40\x02";
    CHECK_EQ(yaz0Decompress(comp), std::string("abcabcabc"));
    std::string data;
    for (int i = 0; i < 1000; ++i) data += char(i * 7);
    CHECK(yaz0Decompress(yaz0Store(data)) == data);
    CHECK(yaz0Decompress(yaz0Store("")).empty());
}

TEST("sizetable: parse, write and merge rules") {
    auto a = table({{"Model/A.bfres", 100}, {"Model/B.bfres", 200}}, {{"Collide/x.bphysics", 50}});
    auto t = SizeTable::parse(a);
    CHECK_EQ(t.crc.size(), 2u);
    CHECK(t.has("Collide/x.bphysics"));
    CHECK(yaz0Decompress(a) == t.write());

    auto b = table({{"Model/A.bfres", 150}, {"Model/C.bfres", 10}});
    auto merged = mergeSizeTables({a, b}, {{{"Model/B.bfres", 150}, {"Model/D.bfres", 999}, {"Collide/x.bphysics", 70}},
                                           {{"Model/C.bfres", 0}}});
    CHECK_EQ(sizeOf(merged, "Model/A.bfres"), 150u);  // largest of the tables
    CHECK_EQ(sizeOf(merged, "Model/B.bfres"), 200u);  // a package never lowers a size
    CHECK_EQ(sizeOf(merged, "Model/D.bfres"), 999u);  // new entry
    CHECK_EQ(sizeOf(merged, "Collide/x.bphysics"), 70u);
    CHECK(!SizeTable::parse(merged).has("Model/C.bfres"));  // 0 removes
    // An unreadable table (say, Wii U) is skipped rather than failing the merge.
    CHECK_EQ(sizeOf(mergeSizeTables({a, yaz0Store("garbage")}), "Model/A.bfres"), 100u);
}

TEST("bnp: which BCML packages can be installed") {
    auto simple = checkBnp({"01007EF00011E000/romfs/Model/A.sbfres", "info.json", "logs/rstb.json"});
    CHECK(simple.installable());
    CHECK(checkBnp({"01007EF00011E000/romfs/Model/A.sbfres", "info.json"}).installable());
    auto merge = checkBnp({"01007EF00011E000/romfs/Pack/TitleBG.pack", "logs/packs.json", "logs/rstb.json", "logs/deepmerge.aamp"});
    CHECK(merge.installable());  // both merged on the console
    CHECK_EQ(merge.mergeLogs.size(), 2u);
    CHECK(!checkBnp({"logs/packs.json", "logs/packs.log"}).installable());  // BCML 2 format
    CHECK(!bnpInstallable({"packs.json", "!wiiu"}));
    CHECK(!checkBnp({"content/Model/A.sbfres", "logs/rstb.json"}).forSwitch);
    CHECK(!checkBnp({"01007EF00011E000/romfs/A", "options/Extra/01007EF00011E000/romfs/B", "options/Extra/logs/map.yml"}).installable());
    CHECK(checkBnp({"01007EF00011E000/romfs/A", "options/Extra/logs/rstb.log"}).installable());
}

TEST("bnp: prepare a simple package with an option") {
    t::TempDir d;
    auto bnp = d.path / "mod.bnp";
    makeArchive(bnp, {{"01007EF00011E000/romfs/Model/A.sbfres", "a"},
                      {"01007EF00011F001/romfs/Pack/AocMainField.pack", "dlc"},
                      {"info.json", R"({"name":"Mod","platform":"switch"})"},
                      {"logs/rstb.json", R"({"Model/A.bfres": 1234})"},
                      {"options/Extra/01007EF00011E000/romfs/Model/B.sbfres", "b"},
                      {"options/Extra/logs/rstb.log", "name,rstb,path\nModel/B.bfres,77,01007EF00011E000/romfs/Model/B.sbfres\n"}});
    auto p = prepareArchive(bnp, d.path / "work", Game::Botw, "mod");
    CHECK(p.layout.kind == PackageKind::Installable);
    CHECK_EQ(p.layout.variants.size(), 2u);
    if (p.layout.variants.size() == 2) {
        CHECK_EQ(p.layout.variants[0].name, std::string("Main"));
        CHECK_EQ(p.layout.variants[0].files.size(), 2u);  // base game and DLC
        CHECK_EQ(p.layout.variants[0].resourceSizes.at("Model/A.bfres"), 1234u);
        CHECK_EQ(p.layout.variants[1].resourceSizes.at("Model/B.bfres"), 77u);
        auto sizes = mergeVariantSizes(p.layout, {0, 1});
        CHECK_EQ(sizes.size(), 2u);
    }
    auto merge = d.path / "merge.bnp";
    makeArchive(merge, {{"01007EF00011E000/romfs/Pack/TitleBG.pack", "x"}, {"logs/packs.json", "{}"}});
    auto q = prepareArchive(merge, d.path / "work2", Game::Botw, "m");
    CHECK(q.layout.kind == PackageKind::Installable);
    if (!q.layout.variants.empty()) {
        CHECK(!q.layout.variants[0].bcmlLogs.empty());
        std::vector<std::string> folders;
        auto logs = variantLogFiles(q.layout, {0}, folders);
        CHECK_EQ(folders.size(), 1u);
        CHECK(logs.size() == 1 && logs[0].target == "bcml/0/packs.json");
    }
    auto unsupported = d.path / "unsupported.bnp";
    makeArchive(unsupported, {{"01007EF00011E000/romfs/Pack/TitleBG.pack", "x"}, {"logs/packs.json", "{}"}, {"logs/packs.log", "x"}});
    auto u = prepareArchive(unsupported, d.path / "work3", Game::Botw, "m");
    CHECK(u.layout.kind == PackageKind::Bcml);
    CHECK(u.layout.problem.find("packs.log") != std::string::npos);
}

TEST("library: BOTW size table is merged from all enabled mods") {
    t::TempDir tmp;
    auto sd = tmp.path / "sd", src = tmp.path / "src";
    Library lib(sd, sd / "switch/mm");
    const std::string tbl = kBotwSizeTable;
    const std::string model = "atmosphere/contents/01007EF00011E000/romfs/Model/";
    auto original = tmp.path / "original.srsizetable";
    write(original, table({{"Model/A.bfres", 100}, {"Model/B.bfres", 100}}));
    // The user's own table on the card (e.g. a randomizer's), with one larger entry.
    write(sd / tbl, table({{"Model/A.bfres", 100}, {"Model/B.bfres", 300}}));

    auto add = [&](const std::string& id, std::vector<std::pair<std::string, std::string>> files,
                   std::map<std::string, uint32_t> sizes) {
        std::vector<FileMapping> maps;
        for (auto& [t, data] : files) {
            write(src / id / t, data);
            maps.push_back({src / id / t, t});
        }
        ModRecord r;
        r.id = id;
        r.game = Game::Botw;
        r.resourceSizes = std::move(sizes);
        return lib.add(r, maps, true);
    };

    // A BCML package with sizes but no table: needs a base.
    ModRecord probe;
    probe.game = Game::Botw;
    probe.resourceSizes = {{"Model/C.bfres", 5}};
    CHECK(lib.canMergeSizes(probe));  // the card's own table will do
    add("bcml", {{model + "C.sbfres", "c"}}, {{"Model/C.bfres", 500}, {"Model/A.bfres", 50}});
    auto onCard = readFile(sd / tbl);
    CHECK_EQ(sizeOf(onCard, "Model/C.bfres"), 500u);
    CHECK_EQ(sizeOf(onCard, "Model/A.bfres"), 100u);  // not lowered
    CHECK_EQ(sizeOf(onCard, "Model/B.bfres"), 300u);  // the user's table kept

    // A regular mod that ships its own whole table: merged too, not "top wins".
    add("zip", {{tbl, table({{"Model/A.bfres", 400}, {"Model/B.bfres", 100}})}}, {});
    onCard = readFile(sd / tbl);
    CHECK_EQ(sizeOf(onCard, "Model/A.bfres"), 400u);
    CHECK_EQ(sizeOf(onCard, "Model/B.bfres"), 300u);
    CHECK_EQ(sizeOf(onCard, "Model/C.bfres"), 500u);
    CHECK(lib.conflicts(Game::Botw).empty());

    // With the game's original captured, it's part of the base as well.
    lib.setOriginalSizeTable(original);
    lib.apply();
    CHECK_EQ(sizeOf(readFile(sd / tbl), "Model/C.bfres"), 500u);

    // Turning mods off rebuilds; with all off the user's own table comes back untouched.
    lib.setEnabled("zip", false);
    CHECK_EQ(sizeOf(readFile(sd / tbl), "Model/A.bfres"), 100u);
    lib.setEnabled("bcml", false);
    CHECK_EQ(sizeOf(readFile(sd / tbl), "Model/B.bfres"), 300u);
    CHECK(!SizeTable::parse(readFile(sd / tbl)).has("Model/C.bfres"));
    CHECK(!fs::exists(sd / (model + "C.sbfres")));
}

TEST("library: a size list with nothing to build on is refused") {
    t::TempDir tmp;
    Library lib(tmp.path / "sd", tmp.path / "sd/switch/mm");
    ModRecord r;
    r.game = Game::Botw;
    r.resourceSizes = {{"Model/C.bfres", 5}};
    CHECK(!lib.canMergeSizes(r));
    lib.setOriginalSizeTable(tmp.path / "missing");
    CHECK(!lib.canMergeSizes(r));
    write(tmp.path / "orig", table({{"Model/A.bfres", 1}}));
    lib.setOriginalSizeTable(tmp.path / "orig");
    CHECK(lib.canMergeSizes(r));
}

namespace {
// Serves byte ranges of a local file, counting what was asked for.
struct RangeServer : HttpClient {
    std::string data;
    size_t served = 0;
    std::string get(const std::string&) override { throw std::runtime_error("no full downloads"); }
    void download(const std::string&, const fs::path&, const DownloadProgressFn&) override { throw std::runtime_error("no"); }
    int64_t contentLength(const std::string&) override { return int64_t(data.size()); }
    std::string getRange(const std::string&, uint64_t from, uint64_t to, std::string*) override {
        auto piece = data.substr(size_t(from), size_t(to - from + 1));
        served += piece.size();
        return piece;
    }
};
}  // namespace

TEST("remote7z: list a 7z from its header ranges only") {
    t::TempDir d;
    auto path = d.path / "pkg.bnp";
    std::string big;
    std::mt19937 rng(42);
    for (int i = 0; i < 200000; ++i) big += char(rng());  // not compressible
    makeArchive(path, {{"01007EF00011E000/romfs/Model/A.sbfres", big},
                       {"logs/rstb.json", "{}"},
                       {"info.json", "{}"}});
    RangeServer server;
    server.data = readFile(path);
    auto names = remote7zListing(server, "https://example/pkg.bnp");
    CHECK_EQ(names.size(), 3u);
    CHECK(std::find(names.begin(), names.end(), "logs/rstb.json") != names.end());
    CHECK(server.served < server.data.size() / 10);
    CHECK(checkBnp(names).installable());
    server.data = "PK\x03\x04 not a 7z at all, just a zip header padded out..........";
    bool threw = false;
    try {
        remote7zListing(server, "x");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}
