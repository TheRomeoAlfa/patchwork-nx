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
    int64_t gamebananaModId = 0;
    int64_t gamebananaFileId = 0;
    int64_t installedAt = 0;  // unix seconds
    std::vector<std::string> variants;          // which variants of the archive were installed
    std::vector<std::string> gameVersionsNamed;  // game versions the mod page mentions
    bool enabled = false;
    int priority = 0;  // higher wins on conflicts
    std::vector<std::string> files;  // targets, relative to SD root
};

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

using ProgressFn = std::function<void(size_t done, size_t total, const std::string& current)>;

// Owns the mod library on the SD card and keeps the deployed LayeredFS files in
// sync with which mods are enabled. Layout under `dataDir`:
//   state.json                       records + what is deployed
//   library/<game>/<id>/<target>     pristine copy of every mod's files
//   backup/<target>                  files that were on the SD card before we overwrote them
class Library {
public:
    Library(fs::path sdRoot, fs::path dataDir);

    void load();
    void save() const;

    std::vector<const ModRecord*> mods(Game game) const;  // sorted by priority, highest first
    const ModRecord* find(const std::string& id) const;

    // Copy `files` into the library under `record.id` (replacing an existing mod with the
    // same id, keeping its priority) and optionally enable it. Returns the stored record.
    const ModRecord& add(ModRecord record, const std::vector<FileMapping>& files, bool enable,
                         const ProgressFn& progress = {});

    void setEnabled(const std::string& id, bool enabled, const ProgressFn& progress = {});
    void remove(const std::string& id, const ProgressFn& progress = {});
    // Move a mod up (+1) or down (-1) in its game's priority order.
    void move(const std::string& id, int delta, const ProgressFn& progress = {});

    std::vector<Conflict> conflicts(Game game) const;
    std::vector<PatchOverlap> patchOverlaps(Game game) const;

    // Make the SD card match the enabled set. Called by the mutators above; also usable
    // as a "repair" after an interrupted operation.
    void apply(const ProgressFn& progress = {});

    const fs::path& sdRoot() const { return sdRoot_; }
    const fs::path& dataDir() const { return dataDir_; }

private:
    fs::path modDir(const ModRecord& m) const;
    ModRecord* findMut(const std::string& id);
    std::map<std::string, std::string> desiredDeployment() const;
    void renumber(std::vector<ModRecord*> ordered);

    fs::path sdRoot_, dataDir_;
    std::vector<ModRecord> mods_;
    std::map<std::string, std::string> deployed_;  // target -> mod id currently on SD
    std::map<std::string, bool> backedUp_;         // target -> original file saved in backup/
};

}  // namespace mm
