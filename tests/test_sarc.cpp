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

#include "msbt.hpp"

TEST("msbt: write, parse, labels and control tags kept as bytes") {
    msbt::File f;
    f.hasAttributes = true;
    f.hasStyles = true;
    f.attrSize = 0;
    msbt::Message hello;
    hello.text = msbt::toUtf16("Hello ");
    // A control tag (0x0E group 1 type 0, 4 bytes of params including a zero unit), then text.
    std::string tag("\x0E\x00\x01\x00\x00\x00\x04\x00\x00\x00\x05\x00", 12);
    hello.text += tag + msbt::toUtf16("Linkle!");
    f.set("Talk_00", hello);
    msbt::Message second;
    second.text = msbt::toUtf16("Caf\xC3\xA9 \xF0\x9F\x98\x80");  // é and an emoji (surrogate pair)
    second.style = 3;
    f.set("Talk_01", second);
    auto raw = msbt::write(f);
    CHECK(raw.size() % 16 == 0);
    auto back = msbt::parse(raw);
    CHECK_EQ(back.messages.size(), 2u);
    CHECK(back.find("Talk_00")->text == hello.text);
    CHECK_EQ(msbt::toUtf8(back.find("Talk_01")->text), std::string("Caf\xC3\xA9 \xF0\x9F\x98\x80"));
    CHECK_EQ(back.find("Talk_01")->style, 3u);
    CHECK(msbt::write(back) == raw);
    // Replacing keeps the position; new labels go at the end.
    msbt::Message changed;
    changed.text = msbt::toUtf16("Bye");
    back.set("Talk_00", changed);
    back.set("Talk_02", changed);
    auto again = msbt::parse(msbt::write(back));
    CHECK_EQ(again.messages[0].first, std::string("Talk_00"));
    CHECK_EQ(msbt::toUtf8(again.messages[0].second.text), std::string("Bye"));
    CHECK_EQ(again.messages[2].first, std::string("Talk_02"));
}

TEST("sarc: an unchanged copy in a higher mod doesn't undo a lower mod's change") {
    Sarc game;
    game.files = {{"a.txt", "game a"}, {"b.txt", "game b"}};
    Sarc low;  // changes a
    low.files = {{"a.txt", "low a"}};
    Sarc high;  // a complete archive: a unchanged, b changed
    high.files = {{"a.txt", "game a"}, {"b.txt", "high b"}};
    auto merged = Sarc::parse(mergeSarcs({game.write(), low.write(), high.write()}));
    CHECK(merged.find("a.txt")->data == "low a");
    CHECK(merged.find("b.txt")->data == "high b");
    // Without the game's copy every file counts as changed: the top one wins.
    auto noGame = Sarc::parse(mergeSarcs({low.write(), high.write()}, true, false));
    CHECK(noGame.find("a.txt")->data == "game a");
}
