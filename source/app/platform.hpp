#pragma once

#include <cstdint>
#include <functional>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "games.hpp"
#include "util.hpp"

namespace app {

// Things that differ between the Switch and a desktop dev build.
namespace platform {

void init();
void exit();

// Root of the SD card. On desktop: $MM_SD_ROOT, or ./sd next to the working directory.
mm::fs::path sdRoot();

// Installed version of a game ("1.2.1"), or "" when unknown / not installed.
// On desktop: $MM_BOTW_VERSION / $MM_TOTK_VERSION.
std::string gameVersion(mm::Game game);

uint64_t freeSpaceBytes();

std::string userAgent();

// Certificate bundle shipped with the app ("" = system store).
std::string caBundle();

// A data file shipped with the app (romfs on Switch, ./resources on desktop).
mm::fs::path resourcePath(const std::string& name);

// Read one of the game's own files (path inside its romfs, e.g. "System/Resource/x"). On the
// Switch this only works while Patchwork runs in place of that game (opened by holding R
// while starting it); Atmosphère then passes the game's original files through, update
// included. On desktop: from $MM_GAME_ROMFS_BOTW / $MM_GAME_ROMFS_TOTK. Returns false with
// a reason when it can't.
bool readGameFile(mm::Game game, const std::string& romfsPath, std::string& out, std::string& why,
                  const std::function<void(uint64_t done, uint64_t total)>& progress = {});

// Calendar time in the console's time zone. Newlib's localtime() doesn't know it on the
// Switch (it gives UTC), so this asks the system.
std::tm localTime(int64_t unixSeconds);

// User profiles on the console. On desktop: $MM_USERS ("Ryan,Kid"), default "Player".
struct UserProfile {
    std::string id;    // stable, for backups
    std::string name;
};
std::vector<UserProfile> userProfiles(std::string& why);

// Why game saves can't be changed right now, or "" when they can: a game open in the
// background holds its save and would write over the change. On desktop: $MM_GAME_OPEN.
std::string saveBlocker();

// One user's save data for a game, open for writing while this object lives. Changes are
// only kept once commit() has been called. On desktop: $MM_SAVE_ROOT/<title id>/<user id>.
class SaveData {
public:
    // nullptr with a reason when it can't be opened (e.g. the user has never played the game).
    static std::unique_ptr<SaveData> open(mm::Game game, const std::string& userId, std::string& why);
    ~SaveData();
    SaveData(const SaveData&) = delete;
    SaveData& operator=(const SaveData&) = delete;

    const mm::fs::path& root() const { return root_; }
    void commit();  // throws

private:
    SaveData() = default;
    mm::fs::path root_;
};

}  // namespace platform
}  // namespace app
