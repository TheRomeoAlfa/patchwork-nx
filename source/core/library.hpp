#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "games.hpp"
#include "layout.hpp"
#include "util.hpp"

namespace mm {

struct ModRecord {
    std::string id;  // "gb<modId>-<fileId>" for GameBanana, "local-<slug>" otherwise
    Game game = Game::Totk;
    std::string name;
    std::string author;
    std::string version;
    std::string sourceFile;  // archive filename it came from
    std::string profileUrl;
    std::string sourceKey;  // see gb::ModSummary::sourceKey; "" for imported files
    int64_t gamebananaModId = 0;
    int64_t gamebananaFileId = 0;
    int64_t installedAt = 0;  // unix seconds
    std::vector<std::string> variants;          // which variants of the archive were installed
    std::vector<std::string> gameVersionsNamed;  // game versions the mod page mentions
    bool enabled = false;
    int priority = 0;  // higher wins on conflicts
    std::vector<std::string> files;  // targets, relative to SD root
    // BCML packages: resource sizes merged into BOTW's size table (romfs path -> bytes, 0 = remove).
    std::map<std::string, uint32_t> resourceSizes;
    // BCML packages that need merging: folders under <mod>/bcml holding the package's logs
    // (the main ones first, then the chosen options'). Empty for everything else.
    std::vector<std::string> bcmlLogs;
};

// BOTW's resource size table. Patchwork merges it from every enabled mod instead of letting
// the top mod's copy win (see mergeSizeTables).
inline constexpr const char* kBotwSizeTable =
    "atmosphere/contents/01007EF00011E000/romfs/System/Resource/ResourceSizeTable.product.srsizetable";

struct Conflict {
    std::string target;
    std::vector<std::string> modIds;  // highest priority (winner) first
};

// Two mods shipping IPS patches for the same executable build both get applied
// by Atmosphère even though their paths differ; reported separately.
struct PatchOverlap {
    std::string buildId;
    std::vector<std::string> modIds;
};

// Files in the game's LayeredFS folders that Patchwork didn't put there (a randomizer,
// mods copied by hand, another manager's installs).
struct UnmanagedFiles {
    std::vector<std::string> targets;  // relative to SD root
    uint64_t bytes = 0;
};

using ProgressFn = std::function<void(size_t done, size_t total, const std::string& current)>;

// A change involving a BCML package couldn't be merged (usually: game files not read yet).
// The library is consistent; the package's files stay off the SD card until it can be.
struct BcmlMergeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The SD card doesn't have room for a change, worked out before anything was written; nothing
// changed. what() is a message for the user.
struct NotEnoughSpaceError : std::runtime_error {
    NotEnoughSpaceError(uint64_t needed, uint64_t free, const std::string& advice);
    uint64_t needed, free;
};

// Owns the mod library on the SD card and keeps the deployed LayeredFS files in
// sync with which mods are enabled. Layout under `dataDir`:
//   state.json                       records + what is deployed
//   library/<game>/<id>/<target>     pristine copy of every mod's files
//   backup/<target>                  files that were on the SD card before we overwrote them
class Library {
public:
    Library(fs::path sdRoot, fs::path dataDir);

    // Reads state.json, falling back to the temp file of an interrupted save. An unreadable
    // file is kept as state.json.corrupt and the library starts empty (returns false).
    bool load();
    void save() const;

    std::vector<const ModRecord*> mods(Game game) const;  // sorted by priority, highest first
    const ModRecord* find(const std::string& id) const;

    // Copy `files` into the library under `record.id` (replacing an existing mod with the
    // same id, keeping its priority) and optionally enable it. Returns the stored record.
    // `internal` files are kept with the mod but never copied to the SD card (targets
    // relative to the mod's library folder, e.g. "bcml/0/packs.json").
    const ModRecord& add(ModRecord record, const std::vector<FileMapping>& files, bool enable,
                         const ProgressFn& progress = {}, const std::vector<FileMapping>& internal = {});

    void setEnabled(const std::string& id, bool enabled, const ProgressFn& progress = {});
    void remove(const std::string& id, const ProgressFn& progress = {});
    // Stop tracking an enabled mod but leave its files on the SD card (the reverse of
    // importUnmanaged). Files another mod currently covers go back to backup/.
    void forget(const std::string& id);
    static bool isImported(const ModRecord& m) { return m.id.rfind("local-existing-", 0) == 0; }
    // Move a mod up (+1) or down (-1) in its game's priority order.
    void move(const std::string& id, int delta, const ProgressFn& progress = {});

    UnmanagedFiles unmanagedFiles(Game game) const;
    // Adopt the unmanaged files as a regular mod (lowest priority, enabled) so they can be
    // turned off and back on. Copies them into the library; the SD card is left as is.
    const ModRecord& importUnmanaged(Game game, const std::string& name, const ProgressFn& progress = {});

    std::vector<Conflict> conflicts(Game game) const;
    std::vector<PatchOverlap> patchOverlaps(Game game) const;

    // Make the SD card match the enabled set. Called by the mutators above; also usable
    // as a "repair" after an interrupted operation.
    void apply(const ProgressFn& progress = {});

    // The game's own size table, read from the console; the base for BCML packages' sizes.
    void setOriginalSizeTable(fs::path path) { originalSizeTable_ = std::move(path); }
    // Reads the game's own files ("01007EF00011E000/romfs/Pack/Bootup.pack"), for merging BCML
    // packages.
    struct GameFile {
        enum class State { Found, Absent, Unavailable } state = State::Unavailable;  // Absent: the game has no such file
        std::string data;
    };
    using GameFileReader = std::function<GameFile(const std::string& path)>;
    void setGameFileReader(GameFileReader reader) { gameFiles_ = std::move(reader); }
    // One game file through the reader (which keeps a copy the first time it can read it).
    GameFile gameFile(const std::string& path) const { return gameFiles_ ? gameFiles_(path) : GameFile{}; }
    // The game's text language, for merging BCML packages' text changes.
    void setTextLanguage(std::string language) { textLanguage_ = std::move(language); }
    // Told 0..1 and what's happening while BCML packages are merged (it can take a while);
    // empty to stop.
    using MergeProgressFn = std::function<void(float, const std::string&)>;
    void setMergeProgress(MergeProgressFn progress) { mergeProgress_ = std::move(progress); }
    // Game files merging would need for a package with these log folders, that can't be read
    // now (package-style paths). Files the game doesn't have don't count.
    std::vector<std::string> missingGameFiles(const std::vector<fs::path>& logDirs) const;
    // False if enabling this mod would need a size table to build on and there's none: not the
    // game's original, not one already on the SD card, and no enabled mod ships one.
    bool canMergeSizes(const ModRecord& mod) const;

    // Free bytes on the SD card (0 = unknown). When set, add() and apply() check there's room
    // before writing and throw NotEnoughSpaceError instead of filling the card part-way.
    using FreeSpaceFn = std::function<uint64_t()>;
    void setFreeSpace(FreeSpaceFn fn) { freeSpace_ = std::move(fn); }

    const fs::path& sdRoot() const { return sdRoot_; }
    const fs::path& dataDir() const { return dataDir_; }

private:
    fs::path modDir(const ModRecord& m) const;
    ModRecord* findMut(const std::string& id);
    std::map<std::string, std::string> desiredDeployment() const;
    void renumber(std::vector<ModRecord*> ordered);
    fs::path backupPath(const std::string& storedTarget) const { return dataDir_ / "backup" / fs::path(storedTarget); }
    void dropBackup(const std::string& key);
    // Owner id for the merged size table, or "" if no merging is needed. The id changes
    // whenever the inputs do, so apply() rebuilds it.
    std::string sizeTableOwner() const;
    // Re-merge the enabled BCML packages if they changed since the last merge. Throws when a
    // game file is missing.
    void refreshBcml();
    std::string bcmlKey() const;
public:
    // Enabled BCML packages that haven't been merged yet (a game file was missing). Their files
    // stay off the SD card until they are; bcmlError() says why.
    bool bcmlPending() const { return bcmlKey() != bcmlKey_; }
    // Merge BCML packages again on the next apply() (the game files they read changed).
    void invalidateBcml() { bcmlKey_.clear(); }
    const std::string& bcmlError() const { return bcmlError_; }
private:
    void writeMergedSizeTable(const fs::path& sdPath) const;
    void checkSpace(uint64_t needed, const std::string& advice) const;
    bool hasOriginalSizeTableOnCard() const;

    fs::path sdRoot_, dataDir_, originalSizeTable_;
    GameFileReader gameFiles_;
    std::string textLanguage_ = "USen";
    MergeProgressFn mergeProgress_;
    FreeSpaceFn freeSpace_;
    // The last BCML merge: its inputs, the SD targets it produced (staged under
    // <dataDir>/bcml-merged) and their size table entries.
    std::string bcmlKey_;
    std::vector<std::string> bcmlOutputs_;
    std::map<std::string, uint32_t> bcmlSizes_;
    std::string bcmlError_;  // why the last merge didn't happen
    bool bcmlMerged(const ModRecord& m) const;  // part of the last merge
    // Every package in the last merge is still enabled (as installed then): its files may stay.
    bool bcmlStillUsable() const;
    std::vector<ModRecord> mods_;
    std::map<std::string, std::string> deployed_;  // target -> mod id currently on SD
    // Originals saved in backup/: lower-cased target (FAT ignores case) -> target as stored.
    std::map<std::string, std::string> backedUp_;
};

}  // namespace mm
