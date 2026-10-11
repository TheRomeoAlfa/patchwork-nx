#include <fstream>

#include "library.hpp"
#include "test.hpp"

using namespace mm;

namespace {

void write(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

std::string read(const fs::path& p) { return fs::exists(p) ? readFile(p) : std::string("<missing>"); }

const std::string kRomfs = "atmosphere/contents/0100F2C0115B6000/romfs/";

struct Fixture {
    t::TempDir tmp;
    fs::path sd = tmp.path / "sd";
    fs::path src = tmp.path / "src";
    Library lib{sd, sd / "switch/mm"};

    // Create a mod whose files contain "<id>:<file>".
    const ModRecord& addMod(const std::string& id, const std::vector<std::string>& files, bool enable = true,
                            Game game = Game::Totk) {
        std::vector<FileMapping> maps;
        for (const auto& f : files) {
            auto s = src / id / f;
            write(s, id + ":" + f);
            maps.push_back({s, f});
        }
        ModRecord r;
        r.id = id;
        r.name = id;
        r.game = game;
        return lib.add(r, maps, enable);
    }
};

}  // namespace

TEST("library: enable copies files, disable removes them and empty dirs") {
    Fixture f;
    f.addMod("a", {kRomfs + "Actor/x.pack"});
    CHECK_EQ(read(f.sd / (kRomfs + "Actor/x.pack")), std::string("a:" + kRomfs + "Actor/x.pack"));
    f.lib.setEnabled("a", false);
    CHECK(!fs::exists(f.sd / (kRomfs + "Actor/x.pack")));
    CHECK(!fs::exists(f.sd / "atmosphere/contents"));
    f.lib.setEnabled("a", true);
    CHECK(fs::exists(f.sd / (kRomfs + "Actor/x.pack")));
}

TEST("library: newer mod wins conflicts; disabling it restores the older one") {
    Fixture f;
    f.addMod("a", {kRomfs + "shared", kRomfs + "onlyA"});
    f.addMod("b", {kRomfs + "shared"});
    CHECK_EQ(read(f.sd / (kRomfs + "shared")), "b:" + kRomfs + "shared");
    auto c = f.lib.conflicts(Game::Totk);
    CHECK_EQ(c.size(), 1u);
    CHECK_EQ(c[0].modIds[0], std::string("b"));

    f.lib.setEnabled("b", false);
    CHECK_EQ(read(f.sd / (kRomfs + "shared")), "a:" + kRomfs + "shared");
    CHECK(f.lib.conflicts(Game::Totk).empty());
}

TEST("library: moving priority changes the winner") {
    Fixture f;
    f.addMod("a", {kRomfs + "shared"});
    f.addMod("b", {kRomfs + "shared"});
    f.lib.move("a", +1);
    CHECK_EQ(read(f.sd / (kRomfs + "shared")), "a:" + kRomfs + "shared");
    CHECK_EQ(f.lib.mods(Game::Totk)[0]->id, std::string("a"));
    f.lib.move("a", -1);
    CHECK_EQ(read(f.sd / (kRomfs + "shared")), "b:" + kRomfs + "shared");
}

TEST("library: pre-existing user files are backed up and restored") {
    Fixture f;
    write(f.sd / (kRomfs + "mine"), "user's own file");
    f.addMod("a", {kRomfs + "mine"});
    CHECK_EQ(read(f.sd / (kRomfs + "mine")), "a:" + kRomfs + "mine");
    f.lib.remove("a");
    CHECK_EQ(read(f.sd / (kRomfs + "mine")), std::string("user's own file"));
    CHECK(f.lib.find("a") == nullptr);
    CHECK(!fs::exists(f.sd / "switch/mm/library/totk/a"));
}

TEST("library: targets differing only by case are one file") {
    Fixture f;
    f.addMod("a", {kRomfs + "Actor/X.pack"});
    f.addMod("b", {kRomfs + "actor/x.pack"});
    CHECK_EQ(f.lib.conflicts(Game::Totk).size(), 1u);
    f.lib.remove("b");
    // On a case-sensitive host only the original-cased path holds a's content again.
    CHECK_EQ(read(f.sd / (kRomfs + "Actor/X.pack")), "a:" + kRomfs + "Actor/X.pack");
}

TEST("library: state survives reload; reinstall keeps priority") {
    Fixture f;
    f.addMod("a", {kRomfs + "1"});
    f.addMod("b", {kRomfs + "2"}, false);
    Library again(f.sd, f.sd / "switch/mm");
    again.load();
    CHECK_EQ(again.mods(Game::Totk).size(), 2u);
    CHECK(again.find("a")->enabled);
    CHECK(!again.find("b")->enabled);

    int prio = f.lib.find("a")->priority;
    f.addMod("a", {kRomfs + "1-new"});
    CHECK_EQ(f.lib.find("a")->priority, prio);
    CHECK(!fs::exists(f.sd / (kRomfs + "1")));
    CHECK(fs::exists(f.sd / (kRomfs + "1-new")));
}

TEST("library: overlapping IPS patches are reported") {
    Fixture f;
    f.addMod("a", {"atmosphere/exefs_patches/a/AABBCCDDEEFF0011.ips"});
    f.addMod("b", {"atmosphere/exefs_patches/b/aabbccddeeff0011.ips"});
    CHECK(f.lib.conflicts(Game::Totk).empty());
    auto o = f.lib.patchOverlaps(Game::Totk);
    CHECK_EQ(o.size(), 1u);
    CHECK_EQ(o[0].modIds.size(), 2u);
}

TEST("library: games are independent") {
    Fixture f;
    f.addMod("t", {kRomfs + "x"});
    f.addMod("b", {"atmosphere/contents/01007EF00011E000/romfs/x"}, true, Game::Botw);
    CHECK_EQ(f.lib.mods(Game::Totk).size(), 1u);
    CHECK_EQ(f.lib.mods(Game::Botw).size(), 1u);
    CHECK_EQ(f.lib.find("b")->priority, 0);
}
