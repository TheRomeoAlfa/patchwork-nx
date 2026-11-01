#include <cstring>

#include "test.hpp"
#include "tkcl.hpp"

using namespace mm;

namespace {

// Writes a package the way TkSharp does.
struct Writer {
    std::string out;
    template <typename T>
    void num(T v) { out.append(reinterpret_cast<const char*>(&v), sizeof v); }
    void str(const std::string& s) { num<int32_t>(int32_t(s.size())); out += s; }
    void ulid(unsigned char last) { out.append(15, '\0'); out += char(last); }
    void list(const std::vector<std::string>& l) { num<int32_t>(int32_t(l.size())); for (auto& s : l) str(s); }
    void changelog(const std::vector<std::string>& files, int versions = 0) {
        num<uint32_t>(0x4C434B54);
        num<int32_t>(3);    // builder version
        num<int32_t>(121);  // game version
        num<int32_t>(int32_t(files.size()));
        for (auto& f : files) {
            str(f);
            num<int32_t>(0);  // changelog entry
            num<uint32_t>(tkcl::HasZsExtension);
            num<int32_t>(2);  // dictionary
            num<uint8_t>(uint8_t(versions));
            for (int v = 0; v < versions; ++v) num<int32_t>(110 + v * 10);
            list({"Pack/Actor/Foo.pack"});
        }
        list({"Mals/USen.Product.sarc"});
        num<int32_t>(1);  // one exefs patch
        str("ABCDEF");
        num<int32_t>(1);
        num<uint32_t>(0x1000);
        num<uint32_t>(0xD503201F);
        num<int32_t>(0);  // cheats
        list({}); list({}); list({}); list({});
    }
};

}  // namespace

TEST("tkcl: reads a package's mod, changelog and options") {
    Writer w;
    w.num<uint32_t>(0x504D4B54);
    w.num<uint32_t>(0x20);
    w.ulid(1);
    w.str("Rideable Things");
    w.str("Ride them");
    w.num<uint8_t>(1);  // thumbnail
    w.str("thumb.png");
    w.changelog({"Component/Foo.bgyml", "Actor/Foo.engine__actor__ActorParam.bgyml"}, 2);
    w.str("1.0");
    w.str("Author");
    w.num<int32_t>(1);
    w.str("Helper");
    w.str("Testing");
    w.num<int32_t>(1);  // one option group
    w.str("Speed");
    w.str("How fast");
    w.num<uint8_t>(0);
    w.num<int32_t>(int32_t(tkcl::GroupType::SingleRequired));
    w.num<int32_t>(-1);  // no icon
    w.num<int32_t>(5);
    w.num<int32_t>(2);
    for (unsigned char id : {2, 3}) {
        w.ulid(id);
        w.str(id == 2 ? "Slow" : "Fast");
        w.str("");
        w.num<uint8_t>(0);
        w.changelog({"AI/Foo.ainb"});
        w.num<int32_t>(0);
    }
    w.num<int32_t>(1);
    w.num<int32_t>(1);  // "Fast" is the default
    w.num<int32_t>(0);  // group dependencies
    w.num<int32_t>(0);  // mod dependencies
    auto zipAt = w.out.size();
    w.out += "PK\x03\x04";

    auto p = tkcl::read(w.out);
    CHECK_EQ(p.id, std::string("00000000000000000000000001"));
    CHECK_EQ(p.name, std::string("Rideable Things"));
    CHECK_EQ(p.version, std::string("1.0"));
    CHECK_EQ(p.author, std::string("Author"));
    CHECK_EQ(p.zipOffset, zipAt);
    CHECK_EQ(p.changelog.gameVersion, 121);
    CHECK_EQ(p.changelog.files.size(), size_t(2));
    CHECK_EQ(p.changelog.files[0].canonical, std::string("Component/Foo.bgyml"));
    CHECK(p.changelog.files[0].type == tkcl::EntryType::Changelog);
    CHECK(p.changelog.files[0].versions == std::vector<int32_t>({110, 120}));
    CHECK(p.changelog.files[0].archiveCanonicals == std::vector<std::string>({"Pack/Actor/Foo.pack"}));
    CHECK(p.changelog.malsFiles == std::vector<std::string>({"Mals/USen.Product.sarc"}));
    CHECK_EQ(p.changelog.patches.at(0).entries.at(0x1000), 0xD503201Fu);
    CHECK_EQ(p.contributors.size(), size_t(1));
    CHECK_EQ(p.groups.size(), size_t(1));
    const auto& g = p.groups[0];
    CHECK(g.single() && g.required());
    CHECK_EQ(g.priority, 5);
    CHECK_EQ(g.options.size(), size_t(2));
    CHECK_EQ(g.options[1].name, std::string("Fast"));
    CHECK_EQ(g.options[1].id, std::string("00000000000000000000000003"));
    CHECK(g.defaults == std::vector<size_t>({1}));

    // Damaged or cut short: a clear error, never a crash or a huge allocation.
    for (size_t cut : {size_t(3), size_t(30), zipAt / 2, zipAt - 1}) {
        bool threw = false;
        try { tkcl::read(std::string_view(w.out).substr(0, cut)); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
    auto huge = w.out;
    std::memset(&huge[24 + 4 + 15 + 4 + 9 + 1 + 4 + 9 + 12], 0x7f, 4);  // the changelog's file count
    bool threw = false;
    try { tkcl::read(huge); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    // Older TKMM betas wrote another version: say so.
    auto old = w.out;
    old[4] = 0x10;
    try { tkcl::read(old); } catch (const std::runtime_error& e) { CHECK(std::string(e.what()).find("older") != std::string::npos); }
}

TEST("tkcl: ULIDs as text") {
    std::string bytes(16, '\0');
    CHECK_EQ(tkcl::ulidText(bytes), std::string(26, '0'));
    bytes.assign(16, '\xff');
    CHECK_EQ(tkcl::ulidText(bytes), std::string("7ZZZZZZZZZZZZZZZZZZZZZZZZZ"));
}

#include <zdict.h>

#include "sarc.hpp"
#include "zstd.hpp"

namespace {

std::vector<std::string> dictionarySamples() {
    std::vector<std::string> samples;
    for (int i = 0; i < 400; ++i) samples.push_back("Actor/Weapon_Sword_" + std::to_string(i) + ".engine__actor__ActorParam.bgyml Components ModelInfo");
    return samples;
}

// A Pack/ZsDic.pack.zs like the game's: dictionaries 1 and 3 (magic, then their id), trained
// from similar files, and a file that isn't one.
std::string fakeZsDic() {
    auto samples = dictionarySamples();
    std::string all;
    std::vector<size_t> sizes;
    for (auto& s : samples) { all += s; sizes.push_back(s.size()); }
    auto makeDict = [&](unsigned id) {
        std::string dict(4096, '\0');
        ZDICT_params_t params{};
        params.dictID = id;
        params.compressionLevel = 7;
        auto content = all.substr(0, 2048);
        auto n = ZDICT_finalizeDictionary(dict.data(), dict.size(), content.data(), content.size(), all.data(), sizes.data(),
                                          unsigned(sizes.size()), params);
        CHECK(!ZDICT_isError(n));
        dict.resize(ZDICT_isError(n) ? 0 : n);
        return dict;
    };
    Sarc pack;
    pack.files.push_back({"zs.zsdic", makeDict(1)});
    pack.files.push_back({"pack.zsdic", makeDict(3)});
    pack.files.push_back({"readme.txt", "not a dictionary"});
    zs::Dictionaries none;
    return none.compress(pack.write(), 0);
}

}  // namespace

TEST("zstd: the game's dictionaries, chosen by each file") {
    auto samples = dictionarySamples();
    zs::Dictionaries none;
    auto zsDic = fakeZsDic();

    auto dicts = zs::Dictionaries::load(zsDic);
    CHECK_EQ(dicts.size(), size_t(2));
    CHECK(dicts.has(1) && dicts.has(3) && !dicts.has(2));
    auto file = samples[7] + samples[300];
    for (unsigned id : {0u, 1u, 3u}) {
        auto c = dicts.compress(file, id);
        CHECK(zs::isZstd(c));
        CHECK_EQ(zs::frameDictionary(c), id);
        CHECK(dicts.decompress(c) == file);
        if (id) {
            bool threw = false;  // without its dictionary: a clear error
            try { none.decompress(c); } catch (const std::runtime_error&) { threw = true; }
            CHECK(threw);
        }
    }
    bool threw = false;
    try { dicts.decompress(std::string("\x28\xb5\x2f\xfd garbage", 12)); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

#include "byml.hpp"

TEST("byml v7: hash maps, aligned binary, 64-bit values and TKMM changelogs") {
    using byml::Node;
    byml::Document doc;
    doc.version = 7;
    doc.root = Node::makeDict();
    Node h32;
    h32.type = Node::Type::HashMap32;
    h32.hashes[0xDEADBEEF] = Node::integer(-5);
    h32.hashes[7] = Node::str("seven");
    Node h64;
    h64.type = Node::Type::HashMap64;
    Node big;
    big.type = Node::Type::UInt64;
    big.u = 0x1122334455667788ull;
    h64.hashes[0xFFFFFFFF00000001ull] = big;
    h64.hashes[2] = h32;
    doc.root.dict["H32"] = h32;
    doc.root.dict["H64"] = h64;
    Node aligned;
    aligned.type = Node::Type::BinaryAligned;
    aligned.u = 0x80;
    aligned.s = std::string("\x01\x02\x03", 3);
    doc.root.dict["Aligned"] = aligned;
    Node changes;
    changes.type = Node::Type::ArrayChangelog;
    byml::ArrayChange add;
    add.index = 3;
    add.change = Node::Change::Add;
    add.node = Node::makeDict();
    add.node.dict["Name"] = Node::str("Sword");
    add.key = Node::str("Sword");
    add.key2.type = Node::Type::None;
    byml::ArrayChange edit;
    edit.index = 0;
    edit.change = Node::Change::Edit;
    edit.node = Node::real(1.5f);
    edit.key.type = edit.key2.type = Node::Type::None;
    changes.changes = {add, edit};
    doc.root.dict["List"] = changes;
    doc.root.dict["Gone"] = Node::removed();

    auto bytes = byml::write(doc);
    auto back = byml::parse(bytes);
    CHECK_EQ(back.version, uint16_t(7));
    CHECK(back.root == doc.root);
    CHECK_EQ(back.root.find("H64")->hashes.at(2).hashes.at(0xDEADBEEF).i, int64_t(-5));
    CHECK(back.root.find("List")->changes.at(0).key2.type == Node::Type::None);
    CHECK(back.root.find("Gone")->type == Node::Type::Changelog);
    // The aligned data starts on its alignment in the file.
    auto at = bytes.find(std::string("\x01\x02\x03", 3));
    CHECK(at != std::string::npos && at % 0x80 == 0);
    CHECK(byml::write(back) == bytes);
}

#include <fstream>

#include "totk.hpp"
#include "util.hpp"

namespace {

// TKMM's pack index format: "PKCH", entry count, string table offset, string count, then
// entries (first/last letter, hash of the UTF-16 name, pack index) and "name\0<attributes>".
std::string fakePackIndex(const std::vector<std::pair<std::string, uint16_t>>& files,
                          const std::vector<std::pair<std::string, uint8_t>>& packs) {
    std::string d;
    auto put = [&](auto v) { d.append(reinterpret_cast<const char*>(&v), sizeof v); };
    put(uint32_t(0x48434B50));
    put(uint32_t(files.size()));
    put(uint32_t(0x10 + 8 * files.size()));
    put(uint32_t(packs.size()));
    for (const auto& [name, pack] : files) {
        put(uint16_t((uint8_t(name.front()) << 8) | uint8_t(name.back())));
        put(totk::xxHash32(totk::utf16le(name)));
        put(pack);
    }
    for (const auto& [name, attributes] : packs) {
        d += name;
        d += '\0';
        d += char(attributes);
    }
    zs::Dictionaries none;
    return none.compress(d, 0);
}

}  // namespace

TEST("totk: hashes as TKMM makes them") {
    CHECK_EQ(totk::xxHash32(""), 0x02CC5D05u);
    CHECK_EQ(totk::xxHash32("abc"), 0x32D153FFu);
    CHECK_EQ(totk::xxHash32("Nobody inspects the spammish repetition"), 0xE2293B2Fu);
    CHECK(totk::utf16le("A\xc3\xa9") == std::string("A\0\xe9\0", 4));
    CHECK(totk::utf16le("\xf0\x9f\x98\x80") == std::string("\x3d\xd8\x00\xde", 4));  // a surrogate pair
    std::string nso;
    CHECK_EQ(totk::parseVersion("ja\r\n100-121\r\n082CE09B06E33A12", &nso), 121);
    CHECK_EQ(nso, std::string("082CE09B06E33A12"));
}

TEST("totk: the game's files by their TKMM names, and files inside packs") {
    using byml::Node;
    t::TempDir dir;
    auto root = dir.path / "romfs";
    auto zsDic = fakeZsDic();
    auto dicts = zs::Dictionaries::load(zsDic);
    auto put = [&](const std::string& path, const std::string& data) {
        auto p = root / path;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << data;
    };
    auto table = [&](Node n) {
        byml::Document doc;
        doc.version = 7;
        doc.root = std::move(n);
        return dicts.compress(byml::write(doc), 1);
    };
    put("System/RegionLangMask.txt", "ja\r\n100-121\r\n082CE09B06E33A12");
    put("Pack/ZsDic.pack.zs", zsDic);
    auto addresses = Node::makeDict();
    for (std::string name : {"Event/EventFlow/EventFlowFileEntry.Product.byml", "AI/FileEntry/FileEntry.Product.byml",
                             "Logic/FileEntry/FileEntry.Product.byml", "Sequence/FileEntry/FileEntry.Product.byml",
                             "GameData/GameDataList.Product.byml"}) {
        auto real = name.substr(0, name.size() - 5) + ".121.byml";
        addresses.dict[name] = Node::str(real);
    }
    addresses.dict["Effect/EffectFileInfo.Product.Nin_NX_NVN.byml"] = Node::str("Effect/EffectFileInfo.Product.121.Nin_NX_NVN.byml");
    put("System/AddressTable/Product.121.Nin_NX_NVN.atbl.byml.zs", table(addresses));
    auto flows = Node::makeDict();
    flows.dict["Versions"] = Node::makeDict();
    flows.dict["Versions"].dict["Demo001"] = Node::str("110");
    put("Event/EventFlow/EventFlowFileEntry.Product.121.byml.zs", table(flows));
    auto effects = Node::makeDict();
    effects.dict["BinaryDict"] = Node::makeDict();
    effects.dict["BinaryDict"].dict["Fire"] = Node::str("Fire.Product.120");
    effects.dict["StaticEsetb"] = Node::makeArray();
    effects.dict["StaticEsetb"].array.push_back(Node::str("Static.Product.100"));
    put("Effect/EffectFileInfo.Product.121.Nin_NX_NVN.byml.zs", table(effects));
    auto ai = Node::makeDict();
    ai.dict["AI/Horse.ainb"] = Node::str("AI/Horse.120.ainb");
    put("AI/FileEntry/FileEntry.Product.121.byml.zs", table(ai));
    put("Logic/FileEntry/FileEntry.Product.121.byml.zs", table(Node::makeDict()));
    put("Sequence/FileEntry/FileEntry.Product.121.byml.zs", table(Node::makeDict()));
    put("GameData/GameDataList.Product.121.byml.zs", dicts.compress("game data", 1));
    Sarc actor;
    actor.files.push_back({"Component/Horse.bgyml", "neigh"});
    actor.files.push_back({"Actor/Horse.engine__actor__ActorParam.bgyml", "horse"});
    put("Pack/Actor/Horse.pack.zs", dicts.compress(actor.write(), 3));

    auto index = totk::PackIndex::load(fakePackIndex(
        {{"Component/Horse.bgyml", 0}, {"Actor/Horse.engine__actor__ActorParam.bgyml", 0}, {"Component/Lost.bgyml", 1}},
        {{"Pack/Actor/Horse.pack", uint8_t(tkcl::HasZsExtension)}, {"Pack/Actor/Gone.pack", uint8_t(tkcl::HasZsExtension)}}));
    CHECK_EQ(index.size(), size_t(3));
    CHECK(index.packFor("Component/Horse.bgyml")->first == "Pack/Actor/Horse.pack");
    CHECK(!index.packFor("Component/Nothing.bgyml"));

    totk::FolderGameFiles files(root.string());
    totk::Rom rom(files, index, 1);  // a tiny cache: one pack at a time
    CHECK_EQ(rom.version(), 121);
    CHECK_EQ(rom.nsoBinaryId(), std::string("082CE09B06E33A12"));
    const uint32_t zs = tkcl::HasZsExtension, product = tkcl::IsProductFile;
    CHECK_EQ(rom.relativePath("GameData/GameDataList.Product.byml", zs | product), std::string("GameData/GameDataList.Product.121.byml.zs"));
    CHECK_EQ(rom.relativePath("AI/Horse.ainb", zs), std::string("AI/Horse.120.ainb.zs"));
    CHECK_EQ(rom.relativePath("Event/EventFlow/Demo001.bfevfl", zs), std::string("Event/EventFlow/Demo001.110.bfevfl.zs"));
    CHECK_EQ(rom.relativePath("Effect/Fire.Product.Nin_NX_NVN.esetb.byml", zs | product),
             std::string("Effect/Fire.Product.120.Product.Nin_NX_NVN.esetb.byml.zs"));
    CHECK_EQ(rom.relativePath("Model/Horse.bfres", tkcl::HasMcExtension), std::string("Model/Horse.bfres.mc"));
    CHECK(*rom.vanilla("GameData/GameDataList.Product.byml", zs | product) == "game data");
    CHECK(*rom.vanilla("Component/Horse.bgyml", 0) == "neigh");
    CHECK(*rom.vanilla("Actor/Horse.engine__actor__ActorParam.bgyml", 0) == "horse");
    CHECK(!rom.vanilla("Component/Lost.bgyml", 0));  // its pack isn't there
    CHECK(!rom.vanilla("Component/Nothing.bgyml", 0));

    // Without the tables: a clear error.
    fs::remove(root / "AI/FileEntry/FileEntry.Product.121.byml.zs");
    bool threw = false;
    try { totk::Rom again(files, index); } catch (const std::runtime_error& e) { threw = std::string(e.what()).find("FileEntry") != std::string::npos; }
    CHECK(threw);
}

TEST("byml: big-endian files stay big-endian (GameDataList)") {
    using byml::Node;
    byml::Document doc;
    doc.version = 7;
    doc.bigEndian = true;
    doc.root = Node::makeDict();
    Node h64;
    h64.type = Node::Type::HashMap64;
    Node big;
    big.type = Node::Type::Int64;
    big.i = -0x123456789ll;
    h64.hashes[0x0102030405060708ull] = big;
    h64.hashes[9] = Node::uinteger(0xAABBCCDD);
    doc.root.dict["Bool"] = h64;
    doc.root.dict["Name"] = Node::str("Hello");
    auto bytes = byml::write(doc);
    CHECK(bytes.compare(0, 4, std::string("BY\0\x07", 4)) == 0);
    CHECK(bytes.find(std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8)) != std::string::npos);  // the hash, big-endian
    auto back = byml::parse(bytes);
    CHECK(back.bigEndian);
    CHECK(back.root == doc.root);
    CHECK(byml::write(back) == bytes);
}

#include "restbl.hpp"

TEST("restbl: the size table round-trips, and changed files get TKMM's sizes") {
    restbl::Table t;
    t.hashes[restbl::crc32("Pack/Actor/Armor_001_Head.pack")] = 110592;
    t.hashes[5] = 100;
    t.names["Some/Collision.bgyml"] = 64;
    auto bytes = t.write();
    CHECK(bytes.compare(0, 6, "RESTBL") == 0);
    CHECK_EQ(bytes.size(), size_t(22 + 2 * 8 + 1 * (0xA0 + 4)));
    auto back = restbl::Table::parse(bytes);
    CHECK(back.hashes == t.hashes && back.names == t.names);
    CHECK(back.write() == bytes);

    CHECK_EQ(restbl::crc32("abc"), 0x352441C2u);
    CHECK_EQ(restbl::resourceName("Pack/Actor/X.pack.zs"), std::string("Pack/Actor/X.pack"));
    CHECK_EQ(restbl::resourceName("Model/X.bfres.mc"), std::string("Model/X.bfres"));
    CHECK_EQ(restbl::resourceSize("Pack/Actor/X.pack.zs", 1000, ""), (1024u + 0x180));
    CHECK_EQ(restbl::resourceSize("Component/X.bgyml", 100, ""), (128u + 2000) * 8);
    CHECK_EQ(restbl::resourceSize("Some/X.unknown", 32, ""), (32u + 1500) * 4);
    std::string ainb(0x48, '\0');
    CHECK_EQ(restbl::resourceSize("AI/X.ainb", 0x48, ainb), 0x60u + 392 + 16);
    CHECK_EQ(restbl::resourceSize("AI/X.ainb", 0x48, "short"), 0x60u + 392 + 16);  // damaged: no crash

    restbl::Collector c(t);
    c.setVanillaCheck([](const std::string& path, std::string_view) { return path == "Same.txt"; });
    c.collect("Pack/Actor/Armor_001_Head.pack.zs", 200000, "data");  // the game's entry is replaced
    c.collect("New/File.txtg.zs", 10, "");                           // added
    c.collect("Same.txt", 10, "x");                                  // same as the game's: left alone
    c.collect("Pack/ZsDic.pack.zs", 10, "");                         // never listed
    c.collect("Some/Collision.bgyml", 10, "", 999);                  // spelled out: stays spelled out
    const auto& r = c.result();
    CHECK_EQ(r.hashes.at(restbl::crc32("Pack/Actor/Armor_001_Head.pack")), (200000u + 0x180));
    CHECK_EQ(r.hashes.at(restbl::crc32("New/File.txtg")), (32u + 0x100));
    CHECK(!r.hashes.count(restbl::crc32("Same.txt")) && !r.hashes.count(restbl::crc32("Pack/ZsDic.pack")));
    CHECK_EQ(r.names.at("Some/Collision.bgyml"), 999u);
}
