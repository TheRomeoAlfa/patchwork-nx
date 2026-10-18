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

// Read one of BOTW's DLC files (path inside the DLC's romfs). `notInGame` is set when the DLC
// isn't installed or has no such file. On desktop: from $MM_GAME_ROMFS_BOTW_DLC.
bool readDlcFile(const std::string& romfsPath, std::string& out, std::string& why, bool* notInGame = nullptr);
// Whether BOTW's DLC is installed: 1 yes, 0 no, -1 couldn't tell. On desktop: $MM_GAME_ROMFS_BOTW_DLC set.
int botwDlcInstalled();
// Why the DLC couldn't be opened the last time it was tried ("" if it could).
std::string lastDlcError();

// A data file shipped with the app (romfs on Switch, ./resources on desktop).
mm::fs::path resourcePath(const std::string& name);

// Read one of the game's own files (path inside its romfs, e.g. "System/Resource/x"). On the
// Switch this only works while Patchwork runs in place of that game (opened by holding R
// while starting it). The files come with mods applied unless L was held too, which turns
// Atmosphère's LayeredFS off for that launch; update included either way. On desktop: from $MM_GAME_ROMFS_BOTW / $MM_GAME_ROMFS_TOTK. Returns false with
// a reason when it can't.
// `notInGame` is set when the game's files could be opened but this one isn't among them.
bool readGameFile(mm::Game game, const std::string& romfsPath, std::string& out, std::string& why,
                  const std::function<void(uint64_t done, uint64_t total)>& progress = {}, bool* notInGame = nullptr);

// The language BOTW shows its text in, as its message packs name it ("USen", "EUfr"…), from
// the console's language. On desktop: $MM_GAME_LANG, default "USen".
std::string gameTextLanguage();

// Whether Patchwork runs in place of the game (opened by holding R while starting it). On
// desktop: when $MM_GAME_ROMFS_BOTW / _TOTK is set.
bool runningAsGame(mm::Game game);

// Calendar time in the console's time zone. Newlib's localtime() doesn't know it on the
// Switch (it gives UTC), so this asks the system.
std::tm localTime(int64_t unixSeconds);

// User profiles on the console. On desktop: $MM_USERS ("Alex,Sam"), default "Player".
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
    [[maybe_unused]] bool mounted_ = false;  // only unmount what this object mounted (Switch)
};

}  // namespace platform
}  // namespace app
