#include <random>

#include "sarc.hpp"
#include "sizetable.hpp"
#include "test.hpp"

using namespace mm;

static std::string noise(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::string s(n, '\0');
    for (auto& c : s) c = char(rng() % 7);  // compressible but varied
    return s;
}

TEST("yaz0: real compression round-trips and is smaller") {
    for (auto data : {std::string(), std::string("a"), std::string(5000, 'x'), noise(100000, 1), noise(3, 2)}) {
        auto c = yaz0Compress(data);
        CHECK(yaz0Decompress(c) == data);
        if (data.size() > 1000) CHECK(c.size() < data.size());
    }
}

TEST("sarc: write, parse, alignment and lookup by hash order") {
    Sarc s;
    s.files.push_back({"Layout/Title.blyt", "hello"});
    s.files.push_back({"Actor/Pack/Foo.sbactorpack", noise(300, 3)});
    // A Nintendo binary file asking for 0x1000 alignment (BNTX-like header).
    std::string bntx = "BNTX";
    bntx.resize(0x40, '\0');
    bntx[0xC] = char(0xFF);
    bntx[0xD] = char(0xFE);
    bntx[0xE] = 12;
    bntx[0x1C] = 0x40;
    s.files.push_back({"timg/__Combined.bntx", bntx});
    auto raw = s.write();
    auto back = Sarc::parse(raw);
    CHECK_EQ(back.files.size(), 3u);
    CHECK(back.find("Layout/Title.blyt")->data == "hello");
    CHECK(back.find("timg/__Combined.bntx")->data == bntx);
    CHECK(Sarc::parse(yaz0Compress(raw)).files.size() == 3u);
    CHECK(isSarc(raw));
    CHECK(isSarc(yaz0Compress(raw)));
    CHECK(!isSarc(yaz0Compress("hello world")));
    // The aligned file starts on a 0x1000 boundary.
    uint32_t dataOffset = uint8_t(raw[0xC]) | (uint8_t(raw[0xD]) << 8) | (uint8_t(raw[0xE]) << 16);
    size_t at = raw.find(bntx);
    CHECK(at != std::string::npos && at % 0x1000 == 0);
    CHECK(dataOffset % 0x1000 == 0);
}

TEST("sarc: merge like BCML (mods over the game, nested archives file by file)") {
    // The game's Title.pack: a compressed layout archive with two files, plus a sound file.
    Sarc layout;
    layout.files = {{"blyt/Title.bflyt", "vanilla layout"}, {"timg/__Combined.bntx", "vanilla textures"}};
    Sarc game;
    game.files = {{"Layout/Title.sblarc", yaz0Compress(layout.write())}, {"Sound/Title.bfsar", "vanilla sound"}};
    // A mod's Title.pack only holds what it changed: new textures.
    Sarc modLayout;
    modLayout.files = {{"timg/__Combined.bntx", "linkle textures"}};
    Sarc mod;
    mod.files = {{"Layout/Title.sblarc", yaz0Compress(modLayout.write())}};
    // A second, higher-priority mod changes the sound and adds a file.
    Sarc mod2;
    mod2.files = {{"Sound/Title.bfsar", "mod sound"}, {"Extra/new.txt", "new"}};

    for (bool compress : {true, false}) {
        auto merged = Sarc::parse(mergeSarcs({game.write(), mod.write(), mod2.write()}, compress));
        CHECK_EQ(merged.files.size(), 3u);
        CHECK(merged.find("Sound/Title.bfsar")->data == "mod sound");
        CHECK(merged.find("Extra/new.txt")->data == "new");
        const auto& nested = merged.find("Layout/Title.sblarc")->data;
        CHECK(isYaz0(nested));
        auto inner = Sarc::parse(nested);
        CHECK(inner.find("blyt/Title.bflyt")->data == "vanilla layout");  // kept from the game
        CHECK(inner.find("timg/__Combined.bntx")->data == "linkle textures");
    }
    // Nothing to merge in a nested archive: it's kept byte for byte.
    auto onlyGame = Sarc::parse(mergeSarcs({game.write(), mod2.write()}));
    CHECK(onlyGame.find("Layout/Title.sblarc")->data == game.files[0].data);
}
