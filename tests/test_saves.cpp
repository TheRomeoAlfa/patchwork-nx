#include <fstream>

#include "saves.hpp"
#include "test.hpp"

using namespace mm;

static void put(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

static std::string get(const fs::path& p) { return readFile(p); }

TEST("saves: finds slots in the layouts people upload") {
    t::TempDir d;
    // Whole save with a title id folder, album beside the slot.
    put(d.path / "a" / "0100F2C0115B6000" / "slot_00" / "progress.sav", "p");
    put(d.path / "a" / "0100F2C0115B6000" / "album" / "000_Photo.jpg", "j");
    // Loose files in a named folder.
    put(d.path / "b" / "4 - Paraglider Get" / "progress.sav", "p");
    put(d.path / "b" / "4 - Paraglider Get" / "caption.sav", "c");
    // A pack of named saves.
    put(d.path / "c" / "TOTK Save Files" / "100% Save" / "slot_02" / "progress.sav", "p");
    put(d.path / "c" / "TOTK Save Files" / "After Intro" / "slot_00" / "progress.sav", "p");
    // Files at the archive root.
    put(d.path / "d" / "progress.sav", "p");

    auto a = findSaves(d.path / "a", Game::Totk);
    CHECK_EQ(a.size(), 1u);
    CHECK_EQ(a[0].slot, std::string("slot_00"));
    CHECK_EQ(a[0].name, std::string(""));
    CHECK_EQ(a[0].photoDirs.size(), 1u);

    auto b = findSaves(d.path / "b", Game::Totk);
    CHECK_EQ(b.size(), 1u);
    CHECK_EQ(b[0].slot, std::string(""));
    CHECK_EQ(b[0].name, std::string("4 - Paraglider Get"));
    CHECK(b[0].photoDirs.empty());

    auto c = findSaves(d.path / "c", Game::Totk);
    CHECK_EQ(c.size(), 2u);
    CHECK_EQ(c[0].name, std::string("TOTK Save Files / 100% Save"));
    CHECK_EQ(c[0].slot, std::string("slot_02"));
    CHECK_EQ(c[1].name, std::string("TOTK Save Files / After Intro"));

    // A zip inside the download, unpacked next to it.
    put(d.path / "e" / "Paraglider.zip.extracted" / "slot_00" / "progress.sav", "p");
    CHECK_EQ(findSaves(d.path / "e", Game::Totk).at(0).name, std::string("Paraglider"));

    auto root = findSaves(d.path / "d", Game::Totk);
    CHECK_EQ(root.size(), 1u);
    CHECK_EQ(root[0].name, std::string(""));
    CHECK(findSaves(d.path, Game::Botw).empty());  // TOTK saves aren't BOTW saves
}

TEST("saves: BOTW slots and Master Mode") {
    t::TempDir d;
    put(d.path / "7" / "game_data.sav", "g");
    put(d.path / "7" / "caption.sav", "c");
    put(d.path / "7" / "caption.jpg", "j");
    put(d.path / "1" / "game_data.sav", "g");
    put(d.path / "pict_book" / "Enemy_Keese.jpg", "k");
    put(d.path / "option.sav", "o");
    auto s = findSaves(d.path, Game::Botw);
    CHECK_EQ(s.size(), 2u);
    CHECK_EQ(s[0].slot, std::string("1"));
    CHECK(!s[0].masterMode());
    CHECK(s[1].masterMode());
    CHECK_EQ(s[1].photoDirs.size(), 1u);
    CHECK_EQ(saveSlots(Game::Botw).size(), 8u);
    CHECK(findSaveSlot(Game::Botw, "6")->masterMode);
    CHECK(findSaveSlot(Game::Totk, "slot_05") != nullptr);
    CHECK(findSaveSlot(Game::Totk, "slot_06") == nullptr);
}

TEST("saves: write a slot, keep the others, photos only when asked") {
    t::TempDir d;
    auto src = d.path / "src";
    put(src / "slot_05" / "progress.sav", "new progress");
    put(src / "slot_05" / "caption.sav", "new caption");
    put(src / "slot_05" / "readme.txt", "not a save file");
    put(src / "album" / "000_Photo.jpg", "new photo");
    auto save = d.path / "save";
    put(save / "slot_00" / "progress.sav", "mine");
    put(save / "slot_01" / "progress.sav", "old");
    put(save / "slot_01" / "footprint.sav", "old footprint");
    put(save / "album" / "000_Photo.jpg", "my photo");
    put(save / "album" / "001_Photo.jpg", "my photo 2");
    put(save / "option.sav", "options");

    auto c = findSaves(src, Game::Totk).at(0);
    int commits = 0;
    size_t lastDone = 0, lastTotal = 0;
    writeSaveSlot(c, save, "slot_01", false, [&] { ++commits; }, [&](size_t done, size_t total) {
        lastDone = done;
        lastTotal = total;
    });
    CHECK_EQ(get(save / "slot_01" / "progress.sav"), std::string("new progress"));
    CHECK(!fs::exists(save / "slot_01" / "footprint.sav"));  // the old slot's leftovers are gone
    CHECK(!fs::exists(save / "slot_01" / "readme.txt"));
    CHECK_EQ(get(save / "slot_00" / "progress.sav"), std::string("mine"));
    CHECK_EQ(get(save / "album" / "000_Photo.jpg"), std::string("my photo"));
    CHECK_EQ(get(save / "option.sav"), std::string("options"));
    CHECK_EQ(lastDone, 2u);
    CHECK_EQ(lastTotal, 2u);
    CHECK_EQ(commits, 3);  // after clearing, then each file
    CHECK(slotInUse(save, "slot_01"));
    CHECK(!slotInUse(save, "slot_04"));

    writeSaveSlot(c, save, "slot_02", true, {});
    CHECK_EQ(get(save / "album" / "000_Photo.jpg"), std::string("new photo"));
    CHECK(!fs::exists(save / "album" / "001_Photo.jpg"));  // the album is replaced, not mixed
}

TEST("saves: back up and restore the whole save") {
    t::TempDir d;
    auto save = d.path / "save";
    put(save / "slot_00" / "progress.sav", "mine");
    put(save / "album" / "000_Photo.jpg", "photo");
    put(save / "option.sav", "options");
    auto backups = d.path / "backups";
    auto dir = newBackupDir(backups, Game::Totk, "0123456789abcdef", 1760000000);
    backupSave(save, dir / "save");
    writeBackupInfo({dir, Game::Totk, "0123456789abcdef", "Alex", "Before installing X into Slot 1", 1760000000});
    CHECK(newBackupDir(backups, Game::Totk, "0123456789abcdef", 1760000000) != dir);  // never reuses a folder

    // Mess it up, then restore.
    put(save / "slot_00" / "progress.sav", "modded");
    put(save / "slot_03" / "progress.sav", "extra");
    int commits = 0;
    restoreSave(dir / "save", save, [&] { ++commits; });
    CHECK_EQ(get(save / "slot_00" / "progress.sav"), std::string("mine"));
    CHECK(!fs::exists(save / "slot_03"));
    CHECK_EQ(get(save / "option.sav"), std::string("options"));
    CHECK_EQ(commits, 4);

    auto list = listSaveBackups(backups, Game::Totk);
    CHECK_EQ(list.size(), 1u);
    CHECK_EQ(list[0].userName, std::string("Alex"));
    CHECK_EQ(list[0].reason, std::string("Before installing X into Slot 1"));
    CHECK(listSaveBackups(backups, Game::Botw).empty());

    bool threw = false;
    try {
        backupSave(d.path / "empty-save", d.path / "x");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);  // never report an empty backup as a good one
}

TEST("saves: the latest slot of a whole save comes first") {
    t::TempDir d;
    put(d.path / "slot_00" / "progress.sav", "a");
    put(d.path / "slot_01" / "progress.sav", "b");
    put(d.path / "slot_02" / "progress.sav", "c");
    FileTimes times = {{"slot_00/progress.sav", 100}, {"slot_01/progress.sav", 300}, {"slot_02/progress.sav", 200}};
    auto s = findSaves(d.path, Game::Totk, &times);
    CHECK_EQ(s[1].savedTime, int64_t(300));
    CHECK_EQ(newestSave(s), 1u);
    CHECK_EQ(newestSave(findSaves(d.path, Game::Totk)), 0u);  // no times: the first
}
