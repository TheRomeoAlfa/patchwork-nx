#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "archive.hpp"
#include "games.hpp"
#include "util.hpp"

namespace mm {

// Save files shared on GameBanana ("100% save", "all shrines unlocked"). They go into the
// game's save data for one user, not onto the SD card, so they're handled apart from mods:
// one slot is replaced, after the whole save has been backed up to the SD card.
//
// Layouts seen in uploads: a slot folder ("slot_00", "7") holding the slot's files, the
// slot's files loose or in a named folder, a whole save (title id folder, or a JKSV export
// with every slot), and packs of several named saves.

struct SaveSlot {
    std::string folder;  // inside the save data: "slot_00" (TOTK), "0" (BOTW)
    std::string label;   // "Slot 1", "Master Mode 1"
    bool masterMode = false;
};
const std::vector<SaveSlot>& saveSlots(Game game);
const SaveSlot* findSaveSlot(Game game, const std::string& folder);

struct SaveCandidate {
    std::string name;          // from the folders around it; "" when it's the archive's only content
    fs::path dir;              // holds the slot's files
    std::string slot;          // the slot folder it came in ("slot_05", "7"), or ""
    fs::path photosRoot;       // where photoDirs are
    std::vector<std::string> photoDirs;  // "album", "pict_book": shared by every slot
    int64_t savedTime = 0;     // newest file time in the archive (unix seconds), 0 = unknown
    bool masterMode() const;   // came from a BOTW Master Mode slot
};

// Every save for `game` in an extracted archive, in path order. Uploads are often a whole
// save, every slot an autosave from a different point of one playthrough; `times` (from
// extraction) tells which is the latest.
std::vector<SaveCandidate> findSaves(const fs::path& root, Game game, const FileTimes* times = nullptr);
// The one to offer first: the latest save, else the first.
size_t newestSave(const std::vector<SaveCandidate>& saves);

// Called after each change to the save data; on the Switch that commits it (the save's
// journal only holds so much, so large writes are committed file by file).
using CommitFn = std::function<void()>;
using SaveProgressFn = std::function<void(size_t done, size_t total)>;

// Does the slot hold a save?
bool slotInUse(const fs::path& saveRoot, const std::string& slotFolder);

// Replace one slot with the candidate's files (and, if `photos`, the shared photo folders).
void writeSaveSlot(const SaveCandidate& save, const fs::path& saveRoot, const std::string& slotFolder, bool photos,
                   const CommitFn& commit, const SaveProgressFn& progress = {});

// Copy the whole save to a folder on the SD card and check the copy. Throws on failure.
void backupSave(const fs::path& saveRoot, const fs::path& dest, const SaveProgressFn& progress = {});
// Replace the whole save with a backup.
void restoreSave(const fs::path& backup, const fs::path& saveRoot, const CommitFn& commit, const SaveProgressFn& progress = {});

// Backups live in <dir>/<game>/<timestamp>-<user id>/ with info.json beside the copy.
struct SaveBackup {
    fs::path dir;      // the backup's folder; the save itself is in dir/"save"
    Game game = Game::Totk;
    std::string userId;
    std::string userName;
    std::string reason;  // "Before installing X into Slot 1"
    int64_t time = 0;    // unix seconds
};
fs::path newBackupDir(const fs::path& backupsRoot, Game game, const std::string& userId, int64_t time);
void writeBackupInfo(const SaveBackup& b);
std::vector<SaveBackup> listSaveBackups(const fs::path& backupsRoot, Game game);  // newest first

}  // namespace mm
