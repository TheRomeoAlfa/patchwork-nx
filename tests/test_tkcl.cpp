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

TEST("zstd: the game's dictionaries, chosen by each file") {
    // A dictionary like the game's (magic, then its id), trained from similar files.
    std::vector<std::string> samples;
    for (int i = 0; i < 400; ++i) samples.push_back("Actor/Weapon_Sword_" + std::to_string(i) + ".engine__actor__ActorParam.bgyml Components ModelInfo");
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
    auto zsDic = none.compress(pack.write(), 0);

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
