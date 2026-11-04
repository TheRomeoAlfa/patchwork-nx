#include <fstream>

#include "byml.hpp"
#include "library.hpp"
#include "restbl.hpp"
#include "test.hpp"
#include "tkcl.hpp"
#include "totk_fixtures.hpp"

using namespace mm;
using byml::Node;

namespace {

const std::string kNso = "9b4e43650501a4d4489b4bbfdb740f26af3cf850";  // TOTK 1.2.1

// Writes a package header the way TkSharp does (the reverse of tkcl::read).
struct HeaderWriter {
    std::string out;
    template <typename T>
    void num(T v) { out.append(reinterpret_cast<const char*>(&v), sizeof v); }
    void str(const std::string& s) { num<int32_t>(int32_t(s.size())); out += s; }
    void list(const std::vector<std::string>& l) { num<int32_t>(int32_t(l.size())); for (auto& s : l) str(s); }
    void ulid(unsigned char last) { out.append(15, '\0'); out += char(last); }
    void changelog(const tkcl::Changelog& c) {
        num<uint32_t>(0x4C434B54);
        num<int32_t>(c.builderVersion);
        num<int32_t>(c.gameVersion);
        num<int32_t>(int32_t(c.files.size()));
        for (const auto& e : c.files) {
            str(e.canonical);
            num<int32_t>(int32_t(e.type));
            num<uint32_t>(e.attributes);
            num<int32_t>(e.zsDictionaryId);
            num<uint8_t>(uint8_t(e.versions.size()));
            for (auto v : e.versions) num<int32_t>(v);
            list(e.archiveCanonicals);
        }
        list(c.malsFiles);
        num<int32_t>(int32_t(c.patches.size()));
        for (const auto& p : c.patches) {
            str(p.nsoBinaryId);
            num<int32_t>(int32_t(p.entries.size()));
            for (auto [a, v] : p.entries) {
                num<uint32_t>(a);
                num<uint32_t>(v);
            }
        }
        num<int32_t>(0);  // cheats
        list(c.subSdkFiles);
        list(c.exeFiles);
        list({});
        list({});
    }
    // One option group (multi choice) whose options get ids ending in 2, 3…
    void package(const std::string& name, const tkcl::Changelog& main, const std::vector<tkcl::Changelog>& options) {
        num<uint32_t>(0x504D4B54);
        num<uint32_t>(0x20);
        ulid(1);
        str(name);
        str("");
        num<uint8_t>(0);
        changelog(main);
        str("1.0");
        str("Someone");
        num<int32_t>(0);
        num<int32_t>(options.empty() ? 0 : 1);
        if (!options.empty()) {
            str("Extras");
            str("");
            num<uint8_t>(0);
            num<int32_t>(int32_t(tkcl::GroupType::Multi));
            num<int32_t>(-1);
            num<int32_t>(0);
            num<int32_t>(int32_t(options.size()));
            for (size_t i = 0; i < options.size(); ++i) {
                ulid(uint8_t(2 + i));
                str("Option " + std::to_string(i));
                str("");
                num<uint8_t>(0);
                changelog(options[i]);
                num<int32_t>(0);
            }
            num<int32_t>(0);  // no defaults
            num<int32_t>(0);
        }
        num<int32_t>(0);
    }
};

tkcl::Entry entry(std::string canonical, tkcl::EntryType type, uint32_t attributes, int dict, std::vector<std::string> archives = {}) {
    tkcl::Entry e;
    e.canonical = std::move(canonical);
    e.type = type;
    e.attributes = attributes;
    e.zsDictionaryId = dict;
    e.archiveCanonicals = std::move(archives);
    return e;
}

std::string bymlFile(Node root) {
    byml::Document d;
    d.version = 7;
    d.root = std::move(root);
    return byml::write(d);
}

void put(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

}  // namespace

TEST("tkmm: packages merge into the game's files on the SD card, and the merge follows the mods") {
    using fixtures::fakePackIndex;
    using fixtures::fakeZsDic;
    t::TempDir dir;
    auto game = dir.path / "game", sd = dir.path / "sd", data = sd / "switch/patchwork", src = dir.path / "src";
    auto zsDic = fakeZsDic();
    auto dicts = zs::Dictionaries::load(zsDic);
    const uint32_t Z = tkcl::HasZsExtension;

    // The game: name tables, a size table, a pack and a loose file.
    put(game / "System/RegionLangMask.txt", "ja\r\n100-121\r\n" + kNso);
    put(game / "Pack/ZsDic.pack.zs", zsDic);
    auto addresses = Node::makeDict();
    for (std::string name : {"Event/EventFlow/EventFlowFileEntry.Product.byml", "AI/FileEntry/FileEntry.Product.byml",
                             "Logic/FileEntry/FileEntry.Product.byml", "Sequence/FileEntry/FileEntry.Product.byml",
                             "Effect/EffectFileInfo.Product.Nin_NX_NVN.byml"}) {
        addresses.dict[name] = Node::str(name + ".121");
        put(game / (name + ".121.zs"), dicts.compress(bymlFile(Node::makeDict()), 1));
    }
    put(game / "System/AddressTable/Product.121.Nin_NX_NVN.atbl.byml.zs", dicts.compress(bymlFile(addresses), 1));
    put(game / "System/Resource/ResourceSizeTable.Product.121.rsizetable.zs", dicts.compress(restbl::Table().write(), 0));
    auto horse = Node::makeDict();
    horse.dict["Speed"] = Node::integer(1);
    horse.dict["Name"] = Node::str("Horse");
    Sarc pack;
    pack.files.push_back({"Component/Horse.bgyml", bymlFile(horse)});
    pack.files.push_back({"Other.bgyml", "untouched"});
    put(game / "Pack/Actor/Horse.pack.zs", dicts.compress(pack.write(), 3));
    auto foo = Node::makeDict();
    foo.dict["A"] = Node::integer(1);
    put(game / "GameParameter/Foo.bgyml.zs", dicts.compress(bymlFile(foo), 1));
    put(dir.path / "index.pkcache.zs", fakePackIndex({{"Component/Horse.bgyml", 0}}, {{"Pack/Actor/Horse.pack", uint8_t(Z)}}));

    // Package A: makes the horse faster, adds a model, patches the game's code.
    tkcl::Changelog a;
    a.gameVersion = 121;
    a.files = {entry("Component/Horse.bgyml", tkcl::EntryType::Changelog, Z, 3, {"Pack/Actor/Horse.pack"}),
               entry("Model/A.bfres", tkcl::EntryType::Copy, 0, 0)};
    a.patches.push_back({kNso, {{0x1000, 0xDEADBEEF}}});
    auto faster = Node::makeDict();
    faster.dict["Speed"] = Node::integer(5);
    put(src / "a/files/romfs/Component/Horse.bgyml", bymlFile(faster));
    put(src / "a/files/romfs/Model/A.bfres", "model");
    HeaderWriter ha;
    ha.package("A", a, {});
    put(src / "a/package.tkcl", ha.out);
    // Package B: renames the horse; its option changes a loose file.
    tkcl::Changelog b, bOption;
    b.files = {entry("Component/Horse.bgyml", tkcl::EntryType::Changelog, Z, 3, {"Pack/Actor/Horse.pack"})};
    bOption.files = {entry("GameParameter/Foo.bgyml", tkcl::EntryType::Changelog, Z, 1)};
    auto renamed = Node::makeDict();
    renamed.dict["Name"] = Node::str("B");
    put(src / "b/files/romfs/Component/Horse.bgyml", bymlFile(renamed));
    auto foo2 = Node::makeDict();
    foo2.dict["A"] = Node::integer(2);
    auto optionId = tkcl::ulidText(std::string(15, '\0') + char(2));
    put(src / "b/files" / optionId / "romfs/GameParameter/Foo.bgyml", bymlFile(foo2));
    HeaderWriter hb;
    hb.package("B", b, {bOption});
    put(src / "b/package.tkcl", hb.out);
    CHECK_EQ(tkcl::read(hb.out).groups.at(0).options.at(0).id, optionId);

    Library lib(sd, data);
    lib.load();
    lib.setTotkPackIndex(dir.path / "index.pkcache.zs");
    bool available = true;
    lib.setGameFileReader([&](const std::string& p) {
        Library::GameFile f;
        const std::string prefix = "0100F2C0115B6000/romfs/";
        if (!available || p.rfind(prefix, 0) != 0) return f;
        auto file = game / p.substr(prefix.size());
        if (!fs::exists(file)) {
            f.state = Library::GameFile::State::Absent;
            return f;
        }
        f.state = Library::GameFile::State::Found;
        f.data = readFile(file);
        return f;
    });
    auto internal = [&](const std::string& pkg) {
        std::vector<FileMapping> out{{src / pkg / "package.tkcl", "tkmm/package.tkcl"}};
        for (auto& e : fs::recursive_directory_iterator(src / pkg / "files"))
            if (e.is_regular_file()) out.push_back({e.path(), "tkmm/files/" + e.path().lexically_relative(src / pkg / "files").generic_string()});
        return out;
    };
    auto record = [](const std::string& id, std::vector<std::string> chosen) {
        ModRecord r;
        r.id = id;
        r.game = Game::Totk;
        r.name = id;
        r.tkmm = std::move(chosen);
        return r;
    };
    lib.add(record("tkA", {"main"}), {}, true, {}, internal("a"));
    lib.add(record("tkB", {"main", optionId}), {}, true, {}, internal("b"));  // higher

    auto contents = sd / "atmosphere/contents/0100F2C0115B6000";
    auto horseNow = [&]() -> Node {
        auto p = contents / "romfs/Pack/Actor/Horse.pack.zs";
        if (!fs::exists(p)) return Node();
        auto sarc = Sarc::parse(dicts.decompress(readFile(p)));
        auto* f = sarc.find("Component/Horse.bgyml");
        CHECK(sarc.find("Other.bgyml") != nullptr);
        return f ? byml::parse(f->data).root : Node();
    };
    auto h = horseNow();
    CHECK(h.find("Speed") && h.find("Speed")->i == 5);  // A's change
    CHECK(h.find("Name") && h.find("Name")->s == "B");  // and B's, in the same file
    CHECK_EQ(readFile(contents / "romfs/Model/A.bfres"), std::string("model"));
    CHECK_EQ(byml::parse(dicts.decompress(readFile(contents / "romfs/GameParameter/Foo.bgyml.zs"))).root.find("A")->i, int64_t(2));
    auto sizes = restbl::Table::parse(dicts.decompress(readFile(contents / "romfs/System/Resource/ResourceSizeTable.Product.121.rsizetable.zs")));
    CHECK(sizes.hashes.count(restbl::crc32("Pack/Actor/Horse.pack")));
    auto ips = readFile(sd / "atmosphere/exefs_patches/patchwork-tkmm" / (toUpper(kNso) + ".ips"));
    CHECK(ips.compare(0, 5, "IPS32") == 0 && ips.find(std::string("\x00\x00\x11\x00\x00\x04\xDE\xAD\xBE\xEF", 10)) != std::string::npos);
    CHECK(lib.tkmmError().empty() && !lib.tkmmPending());

    // A off: merged again without it.
    lib.setEnabled("tkA", false);
    h = horseNow();
    CHECK(h.find("Speed")->i == 1 && h.find("Name")->s == "B");
    CHECK(!fs::exists(contents / "romfs/Model/A.bfres"));

    // The game's files can't be read now: A waits, B's merge stays on the card.
    available = false;
    bool waited = false;
    try {
        lib.setEnabled("tkA", true);
    } catch (const BcmlMergeError& e) {
        waited = std::string(e.what()).find("Hold L and R") != std::string::npos;
    }
    CHECK(waited);
    CHECK(lib.tkmmPending());
    CHECK(horseNow().find("Name")->s == "B");

    // All off: the merged files go.
    available = true;
    lib.setEnabled("tkA", false);
    lib.setEnabled("tkB", false);
    CHECK(!fs::exists(contents / "romfs/Pack/Actor/Horse.pack.zs"));
    CHECK(!fs::exists(sd / "atmosphere/exefs_patches/patchwork-tkmm" / (toUpper(kNso) + ".ips")));
}

#include "msbt.hpp"
#include "tkmm.hpp"

TEST("tkmm: a package's text, in the game's language, merges into the game's") {
    using fixtures::fakePackIndex;
    using fixtures::fakeZsDic;
    t::TempDir dir;
    auto game = dir.path / "game", pkg = dir.path / "pkg";
    auto zsDic = fakeZsDic();
    auto dicts = zs::Dictionaries::load(zsDic);
    put(game / "System/RegionLangMask.txt", "ja\r\n100-121\r\n" + kNso);
    put(game / "Pack/ZsDic.pack.zs", zsDic);
    auto addresses = Node::makeDict();
    for (std::string name : {"Event/EventFlow/EventFlowFileEntry.Product.byml", "AI/FileEntry/FileEntry.Product.byml",
                             "Logic/FileEntry/FileEntry.Product.byml", "Sequence/FileEntry/FileEntry.Product.byml",
                             "Effect/EffectFileInfo.Product.Nin_NX_NVN.byml"}) {
        addresses.dict[name] = Node::str(name + ".121");
        put(game / (name + ".121.zs"), dicts.compress(bymlFile(Node::makeDict()), 1));
    }
    addresses.dict["Mals/EUfr.Product.sarc"] = Node::str("Mals/EUfr.Product.121.sarc");
    put(game / "System/AddressTable/Product.121.Nin_NX_NVN.atbl.byml.zs", dicts.compress(bymlFile(addresses), 1));
    put(game / "System/Resource/ResourceSizeTable.Product.121.rsizetable.zs", dicts.compress(restbl::Table().write(), 0));
    auto messages = [](std::vector<std::pair<std::string, std::string>> items) {
        msbt::File f;
        for (auto& [label, text] : items) f.set(label, {msbt::toUtf16(text), "", 0});
        return msbt::write(f);
    };
    Sarc gameText;
    gameText.files.push_back({"ActorMsg/PouchContent.msbt", messages({{"Armor_001_Head_Name", "Capuche"}})});
    gameText.files.push_back({"ActorMsg/Other.msbt", messages({{"A", "a"}})});
    put(game / "Mals/EUfr.Product.121.sarc.zs", dicts.compress(gameText.write(), 1));

    // The package has English and French text: the French is picked.
    Sarc fr, en;
    fr.files.push_back({"ActorMsg/PouchContent.msbt", messages({{"Armor_900_Head_Name", "Masque"}})});
    en.files.push_back({"ActorMsg/PouchContent.msbt", messages({{"Armor_900_Head_Name", "Mask"}})});
    put(pkg / "romfs/Mals/EUfr.Product.sarc", fr.write());
    put(pkg / "romfs/Mals/USen.Product.sarc", en.write());
    tkcl::Changelog c;
    c.malsFiles = {"Mals/USen.Product.sarc", "Mals/EUfr.Product.sarc"};
    CHECK(tkmm::unsupportedFiles(c).empty());

    put(dir.path / "index.pkcache.zs", fakePackIndex({}, {}));
    auto index = totk::PackIndex::load(readFile(dir.path / "index.pkcache.zs"));
    totk::FolderGameFiles files(game.string());
    totk::Rom rom(files, index);
    auto r = tkmm::merge({{"pkg", pkg, &c}}, rom, dir.path / "out", {}, "EUfr");
    CHECK(r.warnings.empty());
    auto merged = Sarc::parse(dicts.decompress(readFile(dir.path / "out/romfs/Mals/EUfr.Product.121.sarc.zs")));
    auto pouch = msbt::parse(merged.find("ActorMsg/PouchContent.msbt")->data);
    CHECK(pouch.find("Armor_001_Head_Name") && msbt::toUtf8(pouch.find("Armor_001_Head_Name")->text) == "Capuche");
    CHECK(pouch.find("Armor_900_Head_Name") && msbt::toUtf8(pouch.find("Armor_900_Head_Name")->text) == "Masque");
    CHECK(merged.find("ActorMsg/Other.msbt") != nullptr);
}
