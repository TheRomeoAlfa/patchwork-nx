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

// ---------------------------------------------------------------- malformed files

#include <chrono>
#include <cstring>

#include "aamp.hpp"
#include "byml.hpp"

namespace {

void le32(std::string& s, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) s[at + i] = char(v >> (8 * i));
}
void le16(std::string& s, size_t at, uint16_t v) {
    s[at] = char(v);
    s[at + 1] = char(v >> 8);
}
// True if `f` throws, and does so quickly (no exponential work before giving up).
template <typename F>
bool rejectsQuickly(F f) {
    auto start = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        f();
    } catch (const std::exception&) {
        threw = true;
    }
    return threw && std::chrono::steady_clock::now() - start < std::chrono::seconds(2);
}

}  // namespace

TEST("parsers: a SARC whose tables run past the end is rejected") {
    std::string d(0x24, '\0');
    std::memcpy(&d[0], "SARC", 4);
    d[6] = char(0xFF);
    d[7] = char(0xFE);
    le16(d, 4, 0x20);  // SFAT in the last 4 bytes: its header doesn't fit
    std::memcpy(&d[0x20], "SFAT", 4);
    CHECK(rejectsQuickly([&] { Sarc::parse(d); }));
    // 2000 entries that all cover the same 1 MB.
    std::string many(0x20, '\0');
    std::memcpy(&many[0], "SARC", 4);
    le16(many, 4, 0x14);
    many[6] = char(0xFF);
    many[7] = char(0xFE);
    std::memcpy(&many[0x14], "SFAT", 4);
    le16(many, 0x18, 0xC);
    le16(many, 0x1A, 2000);
    for (int i = 0; i < 2000; ++i) {
        std::string node(16, '\0');
        le32(node, 12, 1u << 20);
        many += node;
    }
    many += std::string("SFNT\x08\0\0\0", 8);
    le32(many, 0xC, uint32_t(many.size()));
    many += std::string(1 << 20, 'x');
    CHECK(rejectsQuickly([&] { Sarc::parse(many); }));
}

TEST("parsers: BYML and AAMP with shared children don't blow up") {
    // BYML: each array holds the next one twice, 40 deep (2^40 nodes if expanded).
    const int levels = 40;
    std::string b(0x10 + 16 * (levels + 1), '\0');
    std::memcpy(&b[0], "YB", 2);
    b[2] = 2;
    le32(b, 12, 0x10);
    for (int i = 0; i <= levels; ++i) {
        size_t a = 0x10 + 16 * size_t(i);
        b[a] = char(0xC0);
        if (i < levels) {
            b[a + 1] = 2;
            b[a + 4] = b[a + 5] = char(0xC0);
            le32(b, a + 8, uint32_t(a + 16));
            le32(b, a + 12, uint32_t(a + 16));
        }
    }
    CHECK(rejectsQuickly([&] { byml::parse(b); }));
    // An array claiming 16 million entries in a tiny file.
    std::string big(0x20, '\0');
    std::memcpy(&big[0], "YB", 2);
    big[2] = 2;
    le32(big, 12, 0x10);
    big[0x10] = char(0xC0);
    big[0x11] = big[0x12] = big[0x13] = char(0xFF);
    CHECK(rejectsQuickly([&] { byml::parse(big); }));
    // The writer shares identical values; those files still read back.
    byml::Document doc;
    doc.root = byml::Node::makeArray();
    auto inner = byml::Node::makeDict();
    inner.dict["x"] = byml::Node::str(std::string(100, 's'));
    for (int i = 0; i < 2000; ++i) doc.root.array.push_back(inner);
    CHECK(byml::parse(byml::write(doc)).root.array.size() == 2000u);

    // AAMP: each list holds the next level's two lists, which are the same two structs.
    size_t root = 0x34;
    std::string a(root + 12 + 24 * (levels + 1), '\0');
    std::memcpy(&a[0], "AAMP", 4);
    le32(a, 4, 2);
    le32(a, 8, 3);
    le32(a, 0x14, 4);
    auto base = [&](int i) { return root + 12 + 24 * size_t(i); };
    le32(a, root, 0xA4F6CB6C);
    le16(a, root + 4, uint16_t((base(0) - root) / 4));
    le16(a, root + 6, 2);
    for (int i = 0; i < levels; ++i)
        for (int k = 0; k < 2; ++k) {
            size_t s = base(i) + 12 * size_t(k);
            le16(a, s + 4, uint16_t((base(i + 1) - s) / 4));
            le16(a, s + 6, 2);
        }
    CHECK(rejectsQuickly([&] { aamp::parse(a); }));
}

TEST("parsers: an MSBT attribute table bigger than its block is rejected") {
    std::string d(0x20, '\0');
    std::memcpy(&d[0], "MsgStdBn", 8);
    d[8] = char(0xFF);
    d[9] = char(0xFE);
    d[0xC] = 1;
    le16(d, 0xE, 1);
    std::string blk(0x18, '\0');
    std::memcpy(&blk[0], "ATR1", 4);
    le32(blk, 4, 8);
    le32(blk, 0x10, 0x10000000);  // 268 million rows of size 0
    d += blk;
    CHECK(rejectsQuickly([&] { msbt::parse(d); }));
    // A Yaz0 header claiming 4 GB doesn't reserve it up front.
    std::string y(17, '\0');
    std::memcpy(&y[0], "Yaz0", 4);
    y[4] = char(0xFF);
    y[5] = char(0xFF);
    y[6] = char(0xFF);
    y[7] = char(0xF0);
    CHECK(rejectsQuickly([&] { yaz0Decompress(y); }));
}

TEST("parsers: the AAMP writer refuses counts that don't fit in 16 bits") {
    aamp::ParameterIO pio;
    auto& o = pio.root.objectOrAdd(1);
    for (uint32_t i = 0; i < 70000; ++i) o.params.emplace_back(i + 2, aamp::Parameter::s32(1));
    bool threw = false;
    try {
        aamp::write(pio);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}
