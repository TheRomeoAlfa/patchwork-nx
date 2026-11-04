#include <algorithm>
#include <fstream>

#include "byml.hpp"
#include "restbl.hpp"
#include "test.hpp"
#include "tkmm.hpp"
#include "tkmm_build.hpp"
#include "totk_fixtures.hpp"

using namespace mm;
using byml::Node;

namespace {

std::string bymlOf(Node root) {
    byml::Document d;
    d.version = 7;
    d.root = std::move(root);
    return byml::write(d);
}

void write(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

Node dictOf(std::vector<std::pair<std::string, Node>> items) {
    auto n = Node::makeDict();
    for (auto& [k, v] : items) n.dict[k] = std::move(v);
    return n;
}

// A small Tears of the Kingdom: name tables, a size table, a horse pack and a loose file.
struct FakeGame {
    fs::path root;
    std::string zsDic = fixtures::fakeZsDic();
    zs::Dictionaries dicts = zs::Dictionaries::load(zsDic);
    Sarc pack;

    FakeGame(const fs::path& game, const fs::path& indexFile) : root(game) {
        write(game / "System/RegionLangMask.txt", "ja\r\n100-121\r\n9b4e43650501a4d4489b4bbfdb740f26af3cf850");
        write(game / "Pack/ZsDic.pack.zs", zsDic);
        auto addresses = Node::makeDict();
        for (std::string name : {"Event/EventFlow/EventFlowFileEntry.Product.byml", "AI/FileEntry/FileEntry.Product.byml",
                                 "Logic/FileEntry/FileEntry.Product.byml", "Sequence/FileEntry/FileEntry.Product.byml",
                                 "Effect/EffectFileInfo.Product.Nin_NX_NVN.byml"}) {
            addresses.dict[name] = Node::str(name + ".121");
            write(game / (name + ".121.zs"), dicts.compress(bymlOf(Node::makeDict()), 1));
        }
        addresses.dict["Mals/USen.Product.sarc"] = Node::str("Mals/USen.Product.121.sarc");
        write(game / "System/AddressTable/Product.121.Nin_NX_NVN.atbl.byml.zs", dicts.compress(bymlOf(addresses), 1));
        write(game / "System/Resource/ResourceSizeTable.Product.121.rsizetable.zs", dicts.compress(restbl::Table().write(), 0));
        pack.files.push_back({"Component/Horse.bgyml", bymlOf(dictOf({{"Speed", Node::integer(1)}, {"Name", Node::str("Horse")}}))});
        pack.files.push_back({"Model/Horse.bfres", "model"});
        pack.files.push_back({"Other.bgyml", bymlOf(dictOf({{"A", Node::integer(1)}}))});
        write(game / "Pack/Actor/Horse.pack.zs", dicts.compress(pack.write(), 3));
        write(game / "GameParameter/Foo.bgyml.zs", dicts.compress(bymlOf(dictOf({{"A", Node::integer(1)}, {"B", Node::integer(1)}})), 1));
        write(indexFile, fixtures::fakePackIndex({{"Component/Horse.bgyml", 0}}, {{"Pack/Actor/Horse.pack", uint8_t(tkcl::HasZsExtension)}}));
    }
};

}  // namespace

TEST("tkmm build: canonical names drop .zs and the game version, as TKMM's") {
    int version;
    uint32_t attributes;
    CHECK_EQ(tkmm::canonicalName("RSDB/Tag.Product.121.rstbl.byml.zs", &version, &attributes), std::string("RSDB/Tag.Product.rstbl.byml"));
    CHECK_EQ(version, 121);
    CHECK(attributes == (tkcl::HasZsExtension | tkcl::IsProductFile));
    CHECK_EQ(tkmm::canonicalName("Pack/Actor/Armor_001_Head.pack.zs", &version, &attributes), std::string("Pack/Actor/Armor_001_Head.pack"));
    CHECK_EQ(version, -1);
    CHECK(attributes == tkcl::HasZsExtension);
    CHECK_EQ(tkmm::canonicalName("Mals/USen.Product.121.sarc.zs"), std::string("Mals/USen.Product.sarc"));
    CHECK_EQ(tkmm::canonicalName("AI/Foo.120.ainb", &version), std::string("AI/Foo.ainb"));
    CHECK_EQ(version, 120);
    CHECK_EQ(tkmm::canonicalName("Model/Armor.bfres.mc", &version, &attributes), std::string("Model/Armor.bfres"));
    CHECK(attributes == tkcl::HasMcExtension);
    CHECK_EQ(tkmm::canonicalName("Tex/Icon.123.txtg"), std::string("Tex/Icon.123.txtg"));
}

TEST("tkmm build: of a mod's copies for each game version, the one for this game is used") {
    t::TempDir dir;
    FakeGame g(dir.path / "game", dir.path / "index.pkcache.zs");
    auto index = totk::PackIndex::load(readFile(dir.path / "index.pkcache.zs"));
    totk::FolderGameFiles files(g.root.string());
    totk::Rom rom(files, index);
    auto pick = [&](std::vector<std::string> paths) {
        std::vector<std::pair<std::string, fs::path>> in;
        for (auto& p : paths) in.push_back({p, dir.path / p});
        std::vector<std::string> out;
        for (auto& [p, f] : tkmm::forThisGame(in, rom)) out.push_back(p);
        return out;
    };
    // This game is 1.2.1 (121): its copy, wherever it is in the list.
    auto chosen = pick({"Mals/USen.Product.100.sarc.zs", "Mals/USen.Product.121.sarc.zs", "Mals/USen.Product.110.sarc.zs", "Model/A.bfres"});
    CHECK_EQ(chosen.size(), size_t(2));
    CHECK_EQ(chosen[0], std::string("Mals/USen.Product.121.sarc.zs"));
    CHECK_EQ(chosen[1], std::string("Model/A.bfres"));
    // No copy for this game: the newest.
    chosen = pick({"Mals/USen.Product.112.sarc.zs", "Mals/USen.Product.100.sarc.zs"});
    CHECK_EQ(chosen.size(), size_t(1));
    CHECK_EQ(chosen[0], std::string("Mals/USen.Product.112.sarc.zs"));
}

TEST("tkmm build: two plain mods changing the same pack both end up in it") {
    t::TempDir dir;
    FakeGame g(dir.path / "game", dir.path / "index.pkcache.zs");
    auto& dicts = g.dicts;
    auto& pack = g.pack;
    auto index = totk::PackIndex::load(readFile(dir.path / "index.pkcache.zs"));
    totk::FolderGameFiles files(g.root.string());
    totk::Rom rom(files, index);

    // Mod A: a faster horse and no model; also changes Foo's A. Mod B: renames the horse, adds
    // a file to the pack, changes Foo's B, ships a size table and a file the game doesn't have.
    Sarc a = pack, b = pack;
    a.files[0].data = bymlOf(dictOf({{"Speed", Node::integer(5)}, {"Name", Node::str("Horse")}}));
    a.files.erase(a.files.begin() + 1);
    b.files[0].data = bymlOf(dictOf({{"Speed", Node::integer(1)}, {"Name", Node::str("B")}}));
    b.files.push_back({"Extra/B.txt", "b"});
    auto modA = dir.path / "modA", modB = dir.path / "modB";
    write(modA / "Pack/Actor/Horse.pack.zs", dicts.compress(a.write(), 3));
    write(modA / "GameParameter/Foo.bgyml.zs", dicts.compress(bymlOf(dictOf({{"A", Node::integer(2)}, {"B", Node::integer(1)}})), 1));
    write(modB / "Pack/Actor/Horse.pack.zs", dicts.compress(b.write(), 3));
    write(modB / "GameParameter/Foo.bgyml.zs", dicts.compress(bymlOf(dictOf({{"A", Node::integer(1)}, {"B", Node::integer(3)}})), 1));
    write(modB / "System/Resource/ResourceSizeTable.Product.121.rsizetable.zs", "ignored");
    write(modB / "Model/New.bfres", "new");

    auto build = [&](const fs::path& mod, const fs::path& out) {
        std::vector<std::pair<std::string, fs::path>> list;
        for (auto& e : fs::recursive_directory_iterator(mod))
            if (e.is_regular_file()) list.push_back({e.path().lexically_relative(mod).generic_string(), e.path()});
        return tkmm::buildChangelog(list, rom, out);
    };
    auto logA = build(modA, dir.path / "logA"), logB = build(modB, dir.path / "logB");
    auto find = [](const tkcl::Changelog& c, const std::string& name) -> const tkcl::Entry* {
        for (const auto& e : c.files)
            if (e.canonical == name) return &e;
        return nullptr;
    };
    CHECK(find(logA, "Component/Horse.bgyml") && find(logA, "Component/Horse.bgyml")->type == tkcl::EntryType::Changelog);
    CHECK(find(logA, "Pack/Actor/Horse.pack") != nullptr);  // the removal
    CHECK(find(logA, "Other.bgyml") == nullptr);            // unchanged
    CHECK(find(logB, "Extra/B.txt") && find(logB, "Extra/B.txt")->type == tkcl::EntryType::Copy);
    CHECK(find(logB, "Model/New.bfres") && find(logB, "Model/New.bfres")->type == tkcl::EntryType::Copy);
    CHECK(find(logB, "System/Resource/ResourceSizeTable.Product.rsizetable") == nullptr);

    auto r = tkmm::merge({{"a", dir.path / "logA", &logA}, {"b", dir.path / "logB", &logB}}, rom, dir.path / "out");
    CHECK(r.warnings.empty());
    auto merged = Sarc::parse(dicts.decompress(readFile(dir.path / "out/romfs/Pack/Actor/Horse.pack.zs")));
    auto* h = merged.find("Component/Horse.bgyml");
    CHECK(h != nullptr);
    if (h) {
        auto root = byml::parse(h->data).root;
        CHECK(root.find("Speed")->i == 5);
        CHECK(root.find("Name")->s == "B");
    }
    CHECK(merged.find("Model/Horse.bfres") == nullptr);
    CHECK(merged.find("Extra/B.txt") && merged.find("Extra/B.txt")->data == "b");
    CHECK(merged.find("Other.bgyml") != nullptr);
    auto foo = byml::parse(dicts.decompress(readFile(dir.path / "out/romfs/GameParameter/Foo.bgyml.zs"))).root;
    CHECK(foo.find("A")->i == 2 && foo.find("B")->i == 3);
    CHECK_EQ(readFile(dir.path / "out/romfs/Model/New.bfres"), std::string("new"));
}

#include "library.hpp"

TEST("tkmm build: plain mods in My mods merge the files they share, and fall back when the game can't be read") {
    t::TempDir dir;
    FakeGame g(dir.path / "game", dir.path / "index.pkcache.zs");
    auto& dicts = g.dicts;
    auto sd = dir.path / "sd", src = dir.path / "src";
    const std::string romfs = "atmosphere/contents/0100F2C0115B6000/romfs/";

    // Two outfit mods change the same pack; each ships its own size table and a model.
    auto modPack = [&](const std::string& speed, const std::string& name) {
        Sarc p = g.pack;
        p.files[0].data = bymlOf(dictOf({{"Speed", Node::integer(std::stoi(speed))}, {"Name", Node::str(name)}}));
        return dicts.compress(p.write(), 3);
    };
    auto sizeTable = [&](const std::string& file, uint32_t size) {
        restbl::Table t;
        t.hashes[restbl::crc32(file)] = size;
        return dicts.compress(t.write(), 0);
    };
    write(src / "a/Pack/Actor/Horse.pack.zs", modPack("5", "Horse"));
    write(src / "a/System/Resource/ResourceSizeTable.Product.121.rsizetable.zs", sizeTable("Model/A.bfres", 100));
    write(src / "a/Model/A.bfres", std::string(5000, 'a'));
    write(src / "b/Pack/Actor/Horse.pack.zs", modPack("1", "B"));
    write(src / "b/System/Resource/ResourceSizeTable.Product.121.rsizetable.zs", sizeTable("Model/B.bfres", 100));
    write(src / "b/Model/B.bfres", std::string(7000, 'b'));

    Library lib(sd, sd / "switch/patchwork");
    lib.load();
    lib.setMergePlainTotkMods(true);
    lib.setTotkPackIndex(dir.path / "index.pkcache.zs");
    bool available = true;
    lib.setGameFileReader([&](const std::string& p) {
        Library::GameFile f;
        const std::string prefix = "0100F2C0115B6000/romfs/";
        if (!available || p.rfind(prefix, 0) != 0) return f;
        auto file = g.root / p.substr(prefix.size());
        f.state = fs::exists(file) ? Library::GameFile::State::Found : Library::GameFile::State::Absent;
        if (f.state == Library::GameFile::State::Found) f.data = readFile(file);
        return f;
    });
    auto add = [&](const std::string& id) {
        ModRecord r;
        r.id = id;
        r.game = Game::Totk;
        r.name = id;
        std::vector<FileMapping> files;
        for (auto& e : fs::recursive_directory_iterator(src / id))
            if (e.is_regular_file()) files.push_back({e.path(), romfs + e.path().lexically_relative(src / id).generic_string()});
        lib.add(r, files, true);
    };
    add("a");
    CHECK(lib.tkmmMergedTargets().empty());  // one mod alone deploys as it is
    add("b");  // higher

    auto contents = sd / romfs;
    auto horse = [&]() {
        auto sarc = Sarc::parse(dicts.decompress(readFile(contents / "Pack/Actor/Horse.pack.zs")));
        return byml::parse(sarc.find("Component/Horse.bgyml")->data).root;
    };
    auto h = horse();
    CHECK(h.find("Speed")->i == 5);  // A's change
    CHECK(h.find("Name")->s == "B");  // and B's
    auto sizes = restbl::Table::parse(dicts.decompress(readFile(contents / "System/Resource/ResourceSizeTable.Product.121.rsizetable.zs")));
    CHECK(sizes.hashes.count(restbl::crc32("Model/A.bfres")) && sizes.hashes.at(restbl::crc32("Model/A.bfres")) >= 5000);  // both models
    CHECK(sizes.hashes.count(restbl::crc32("Model/B.bfres")) && sizes.hashes.at(restbl::crc32("Model/B.bfres")) >= 7000);
    CHECK_EQ(readFile(contents / "Model/A.bfres").size(), size_t(5000));
    CHECK(lib.conflicts(Game::Totk).empty());
    CHECK(lib.tkmmError().empty());

    // The game can't be read and the merge is out of date: the higher mod wins, as before.
    available = false;
    lib.setEnabled("a", false);
    lib.setEnabled("a", true);
    CHECK(!lib.tkmmError().empty());
    CHECK(horse().find("Speed")->i == 1 && horse().find("Name")->s == "B");
    CHECK(!lib.conflicts(Game::Totk).empty());

    // Readable again; then one mod off: no merge, its files go.
    available = true;
    lib.setEnabled("b", true);
    lib.setEnabled("a", false);
    lib.setEnabled("a", true);
    CHECK(horse().find("Speed")->i == 5);
    lib.setEnabled("b", false);
    CHECK(lib.tkmmMergedTargets().empty());
    CHECK(horse().find("Name")->s == "Horse" && horse().find("Speed")->i == 5);
    CHECK(!fs::exists(contents / "Model/B.bfres"));
    CHECK(fs::exists(sd / "switch/patchwork/tkmm-merged") == false);

    // Merging plain mods turned off (the default): the higher mod wins, as before.
    lib.setEnabled("b", true);
    CHECK(horse().find("Speed")->i == 5);
    lib.setMergePlainTotkMods(false);
    lib.apply();
    CHECK(lib.tkmmMergedTargets().empty());
    CHECK(horse().find("Speed")->i == 1 && horse().find("Name")->s == "B");
    CHECK(!lib.conflicts(Game::Totk).empty());
    CHECK(fs::exists(sd / "switch/patchwork/tkmm-merged") == false);
}
