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

TEST("library: files already on the SD card can be found and imported") {
    Fixture f;
    const std::string botw = "atmosphere/contents/01007EF00011E000/romfs/";
    const std::string dlc = "atmosphere/contents/01007ef00011f001/romfs/";  // other casing on purpose
    write(f.sd / (botw + "Pack/Bootup.pack"), "randomizer");
    write(f.sd / (botw + "Actor/x"), "randomizer-x");
    write(f.sd / (dlc + "Map/y"), "randomizer-y");
    write(f.sd / "atmosphere/contents/01007EF00011E000/romfs_metadata.bin", "cache");
    write(f.sd / (kRomfs + "totk-file"), "other game");

    // A Patchwork mod overrides one of the randomizer's files.
    f.addMod("m", {botw + "Actor/x"}, true, Game::Botw);
    auto found = f.lib.unmanagedFiles(Game::Botw);
    CHECK_EQ(found.targets.size(), 3u);  // Bootup.pack, DLC file, and the backed-up Actor/x

    const auto& imported = f.lib.importUnmanaged(Game::Botw, "Existing files");
    std::string id = imported.id;
    CHECK(f.lib.unmanagedFiles(Game::Botw).targets.empty());
    CHECK_EQ(f.lib.mods(Game::Botw).back()->id, id);  // lowest priority
    CHECK_EQ(read(f.sd / (botw + "Actor/x")), "m:" + botw + "Actor/x");

    // Turning the mod off brings back the randomizer's version from the imported copy.
    f.lib.setEnabled("m", false);
    CHECK_EQ(read(f.sd / (botw + "Actor/x")), std::string("randomizer-x"));
    // Turning the import off removes its files; turning it on restores them.
    f.lib.setEnabled(id, false);
    CHECK(!fs::exists(f.sd / (botw + "Pack/Bootup.pack")));
    CHECK(fs::exists(f.sd / "atmosphere/contents/01007EF00011E000/romfs_metadata.bin"));
    f.lib.setEnabled(id, true);
    CHECK_EQ(read(f.sd / (botw + "Pack/Bootup.pack")), std::string("randomizer"));
    CHECK_EQ(read(f.sd / (dlc + "Map/y")), std::string("randomizer-y"));
    CHECK_EQ(read(f.sd / (kRomfs + "totk-file")), std::string("other game"));

    // Stop managing: files stay where they are and show up as unmanaged again.
    f.lib.setEnabled("m", true);
    f.lib.forget(id);
    CHECK(f.lib.find(id) == nullptr);
    CHECK_EQ(read(f.sd / (botw + "Pack/Bootup.pack")), std::string("randomizer"));
    CHECK_EQ(f.lib.unmanagedFiles(Game::Botw).targets.size(), 3u);
    f.lib.remove("m");  // the randomizer's Actor/x comes back from backup
    CHECK_EQ(read(f.sd / (botw + "Actor/x")), std::string("randomizer-x"));
}

TEST("library: a failed copy to the SD card is rolled back and the mod kept off") {
    Fixture f;
    // Something the copy can't overwrite: a directory where a file should go.
    fs::create_directories(f.sd / (kRomfs + "z_blocked"));
    bool threw = false;
    try {
        f.addMod("a", {kRomfs + "a_ok", kRomfs + "z_blocked"});
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!fs::exists(f.sd / (kRomfs + "a_ok")));  // nothing half-installed
    const auto* m = f.lib.find("a");
    CHECK(m != nullptr);
    if (m) CHECK(!m->enabled);
    CHECK(!fs::exists(f.sd / "switch/mm/library/totk/a.incoming"));
}

TEST("util: removeTree deletes nested folders and tolerates missing paths") {
    t::TempDir d;
    write(d.path / "x/a/b/c.txt", "1");
    write(d.path / "x/a/d.txt", "2");
    write(d.path / "x/e.txt", "3");
    CHECK(removeTree(d.path / "x"));
    CHECK(!fs::exists(d.path / "x"));
    CHECK(removeTree(d.path / "missing"));
}

TEST("library: a failed import leaves the user's originals in place") {
    Fixture f;
    const std::string botw = "atmosphere/contents/01007EF00011E000/romfs/";
    write(f.sd / (botw + "a.bin"), "orig-a");
    write(f.sd / (botw + "b.bin"), "orig-b");
    f.addMod("m", {botw + "a.bin", botw + "b.bin"}, true, Game::Botw);  // originals now in backup/
    // Make one backup unreadable so the import fails half-way.
    auto bk = f.sd / "switch/mm/backup" / (botw + "b.bin");
    fs::permissions(bk, fs::perms::none);
    bool threw = false;
    try {
        f.lib.importUnmanaged(Game::Botw, "Existing");
    } catch (const std::exception&) {
        threw = true;
    }
    fs::permissions(bk, fs::perms::owner_all);
    CHECK(threw);
    f.lib.setEnabled("m", false);
    CHECK_EQ(read(f.sd / (botw + "a.bin")), std::string("orig-a"));
    CHECK_EQ(read(f.sd / (botw + "b.bin")), std::string("orig-b"));
}

TEST("library: originals are restored even when mods use different letter case") {
    Fixture f;
    write(f.sd / (kRomfs + "Foo.bin"), "orig");
    f.addMod("a", {kRomfs + "Foo.bin"});
    f.addMod("b", {kRomfs + "foo.bin"});
    f.lib.setEnabled("a", false);
    f.lib.setEnabled("b", false);
    CHECK_EQ(read(f.sd / (kRomfs + "Foo.bin")), std::string("orig"));
    CHECK(f.lib.unmanagedFiles(Game::Totk).targets.size() == 1);  // the original, back on the card
}

TEST("library: a missing backup doesn't block every later change") {
    Fixture f;
    write(f.sd / (kRomfs + "x"), "orig");
    f.addMod("a", {kRomfs + "x"});
    removeTree(f.sd / "switch/mm/backup");  // e.g. deleted by the user, or restored before a power cut
    f.lib.setEnabled("a", false);           // must not throw
    f.addMod("b", {kRomfs + "y"});          // and the library keeps working
    CHECK(fs::exists(f.sd / (kRomfs + "y")));
}

TEST("library: state survives an interrupted save and a corrupt file") {
    Fixture f;
    f.addMod("a", {kRomfs + "1"});
    auto state = f.sd / "switch/mm/state.json";
    auto tmp = state;
    tmp += ".tmp";
    fs::rename(state, tmp);  // power cut between removing the old file and renaming the new one
    Library again(f.sd, f.sd / "switch/mm");
    CHECK(again.load());
    CHECK(again.find("a") != nullptr);

    write(state, "{ not json");
    fs::remove(tmp);
    Library broken(f.sd, f.sd / "switch/mm");
    CHECK(!broken.load());
    CHECK(fs::exists(f.sd / "switch/mm/state.json.corrupt"));
}

TEST("library: stop managing hands the current file over without bringing back an old original") {
    Fixture f;
    const std::string botw = "atmosphere/contents/01007EF00011E000/romfs/";
    write(f.sd / (botw + "x"), "orig");
    f.addMod("m", {botw + "x"}, true, Game::Botw);
    f.lib.forget("m");
    CHECK_EQ(read(f.sd / (botw + "x")), "m:" + botw + "x");
    auto un = f.lib.unmanagedFiles(Game::Botw);
    CHECK_EQ(un.targets.size(), 1u);
    f.lib.importUnmanaged(Game::Botw, "Existing");
    CHECK_EQ(read(f.sd / (botw + "x")), "m:" + botw + "x");
}

TEST("library: an install that doesn't fit is refused before anything is written") {
    Fixture f;
    uint64_t free = 10ull << 20;  // less than the safety margin
    f.lib.setFreeSpace([&] { return free; });
    bool refused = false;
    try {
        f.addMod("a", {kRomfs + "Actor/x.pack"});
    } catch (const NotEnoughSpaceError& e) {
        refused = true;
        CHECK_EQ(e.free, free);
        CHECK(e.needed > free);
        CHECK(std::string(e.what()).find("Not enough free space") != std::string::npos);
    }
    CHECK(refused);
    CHECK(f.lib.find("a") == nullptr);
    CHECK(!fs::exists(f.sd / "switch/mm/library/totk/a.incoming"));
    CHECK(!fs::exists(f.sd / (kRomfs + "Actor/x.pack")));

    free = 1ull << 30;
    f.addMod("a", {kRomfs + "Actor/x.pack"});
    CHECK(fs::exists(f.sd / (kRomfs + "Actor/x.pack")));
}

TEST("library: turning a mod on needs room; turning one off never does") {
    Fixture f;
    uint64_t free = 1ull << 30;
    f.lib.setFreeSpace([&] { return free; });
    f.addMod("a", {kRomfs + "Actor/x.pack"}, false);
    free = 1ull << 20;
    bool refused = false;
    try {
        f.lib.setEnabled("a", true);
    } catch (const NotEnoughSpaceError&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(!fs::exists(f.sd / (kRomfs + "Actor/x.pack")));

    free = 1ull << 30;
    f.lib.setEnabled("a", true);
    CHECK(fs::exists(f.sd / (kRomfs + "Actor/x.pack")));
    free = 1;  // full card
    f.lib.setEnabled("a", false);
    CHECK(!fs::exists(f.sd / (kRomfs + "Actor/x.pack")));
}

TEST("library: no room for the game-folder copy keeps the mod, turned off, with advice") {
    Fixture f;
    // add()'s own check sees unknown free space (0) and lets the library copy through; the
    // check in apply() then refuses the copy to the game's folder.
    int calls = 0;
    f.lib.setFreeSpace([&]() -> uint64_t { return ++calls == 1 ? 0 : 1ull << 20; });
    std::string msg;
    try {
        f.addMod("a", {kRomfs + "Actor/x.pack"}, true);
    } catch (const NotEnoughSpaceError& e) {
        msg = e.what();
    }
    CHECK(msg.find("turn it on in My mods") != std::string::npos);
    const auto* m = f.lib.find("a");
    CHECK(m != nullptr);
    if (m) CHECK(!m->enabled);
    CHECK(!fs::exists(f.sd / (kRomfs + "Actor/x.pack")));
}

TEST("library: a reorder that doesn't fit keeps the old order") {
    Fixture f;
    f.addMod("a", {kRomfs + "shared"});
    f.addMod("b", {kRomfs + "shared"});  // b wins
    // Make a's copy much bigger so swapping the winner needs room.
    write(f.sd / "switch/mm/library/totk/a" / (kRomfs + "shared"), std::string(4 << 20, 'x'));
    f.lib.setFreeSpace([] { return uint64_t(1) << 20; });
    bool refused = false;
    try {
        f.lib.move("a", +1);
    } catch (const NotEnoughSpaceError&) {
        refused = true;
    }
    CHECK(refused);
    CHECK_EQ(f.lib.mods(Game::Totk).front()->id, std::string("b"));
    CHECK_EQ(read(f.sd / (kRomfs + "shared")), "b:" + kRomfs + "shared");
}

TEST("library: options, kept package and reinstalling a zip mod in place") {
    Fixture f;
    // A zip with two versions, installed with "Fast", the archive kept.
    write(f.src / "opt/archive.zip", "the archive");
    write(f.src / "opt/Fast.bfres", "fast");
    write(f.src / "opt/Slow.bfres", "slow");
    ModRecord r;
    r.id = "opt";
    r.name = "Options mod";
    r.game = Game::Totk;
    r.variants = {"Fast"};
    r.chosen = {"Fast"};
    r.hasOptions = true;
    r.package = "package/archive.zip";
    f.lib.add(r, {{f.src / "opt/Fast.bfres", kRomfs + "Model/Fast.bfres"}}, true, {}, {{f.src / "opt/archive.zip", r.package}});
    f.addMod("other", {kRomfs + "Model/Other.bfres"});  // above it
    CHECK(!f.lib.keptPackage(*f.lib.find("opt")).empty());
    CHECK_EQ(read(f.lib.keptPackage(*f.lib.find("opt"))), std::string("the archive"));

    // Saved and read back.
    Library again(f.sd, f.sd / "switch/mm");
    again.load();
    const auto* back = again.find("opt");
    CHECK(back && back->hasOptions && back->package == "package/archive.zip");
    CHECK(back && back->chosen == std::vector<std::string>({"Fast"}));

    // Reinstalled with "Slow", from the kept copy, turned off: same place, old version's file gone.
    auto kept = f.lib.keptPackage(*f.lib.find("opt"));
    int priority = f.lib.find("opt")->priority;
    r.variants = r.chosen = {"Slow"};
    f.lib.add(r, {{f.src / "opt/Slow.bfres", kRomfs + "Model/Slow.bfres"}}, false, {}, {{kept, r.package}});
    CHECK_EQ(f.lib.find("opt")->priority, priority);
    CHECK(!f.lib.find("opt")->enabled);
    CHECK(!fs::exists(f.sd / (kRomfs + "Model/Fast.bfres")));
    CHECK(!fs::exists(f.sd / (kRomfs + "Model/Slow.bfres")));
    CHECK_EQ(read(f.lib.keptPackage(*f.lib.find("opt"))), std::string("the archive"));  // copied over itself fine
    f.lib.setEnabled("opt", true);
    CHECK_EQ(read(f.sd / (kRomfs + "Model/Slow.bfres")), std::string("slow"));
}

TEST("library: records from before options were kept still know they have options") {
    t::TempDir tmp;
    auto data = tmp.path / "sd/switch/mm";
    write(data / "state.json", R"({"version":1,"mods":[
        {"id":"zip","game":"totk","variants":["Fast","Slow"],"enabled":true},
        {"id":"one","game":"totk","variants":["Main"],"enabled":true},
        {"id":"bnp","game":"botw","variants":["Main","options/SA3"],"bcmlLogs":["0","1"],"enabled":true},
        {"id":"bnp-plain","game":"botw","variants":["Main"],"bcmlLogs":["0"],"enabled":true}]})");
    Library lib(tmp.path / "sd", data);
    lib.load();
    CHECK(lib.find("zip")->hasOptions);
    CHECK(!lib.find("one")->hasOptions);
    CHECK(lib.find("bnp")->hasOptions);
    CHECK(!lib.find("bnp-plain")->hasOptions);
    CHECK(lib.keptPackage(*lib.find("zip")).empty());
}
