#include "platform.hpp"

#include <sys/statvfs.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <memory>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace app::platform {

void init() {
#ifdef __SWITCH__
    nsInitialize();
#endif
}

void exit() {
#ifdef __SWITCH__
    accountExit();  // no-op unless profiles were read
    nsExit();
#endif
}

mm::fs::path sdRoot() {
#ifdef __SWITCH__
    return "sdmc:/";
#else
    if (const char* env = std::getenv("MM_SD_ROOT")) return env;
    return mm::fs::current_path() / "sd";
#endif
}

std::string gameVersion(mm::Game game) {
#ifdef __SWITCH__
    auto data = std::make_unique<NsApplicationControlData>();
    u64 size = 0;
    u64 tid = std::stoull(std::string(mm::gameInfo(game).titleId), nullptr, 16);
    if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, tid, data.get(), sizeof(*data), &size)))
        return "";
    return data->nacp.display_version;
#else
    const char* env = std::getenv(game == mm::Game::Botw ? "MM_BOTW_VERSION" : "MM_TOTK_VERSION");
    return env ? env : "";
#endif
}

uint64_t freeSpaceBytes() {
    struct statvfs st {};
    auto root = sdRoot().string();
    if (statvfs(root.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
}

std::string caBundle() {
#ifdef __SWITCH__
    return "romfs:/cacert.pem";
#else
    // Use the bundled file on desktop too, so the same path is exercised during development.
    std::error_code ec;
    return mm::fs::exists("./resources/cacert.pem", ec) ? "./resources/cacert.pem" : "";
#endif
}

bool readGameFile(mm::Game game, const std::string& romfsPath, std::string& out, std::string& why) {
#ifdef __SWITCH__
    u64 current = 0;
    u64 wanted = std::stoull(std::string(mm::gameInfo(game).titleId), nullptr, 16);
    if (R_FAILED(svcGetInfo(&current, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) || current != wanted) {
        why = "Patchwork wasn't opened from " + std::string(mm::gameInfo(game).displayName) + ".";
        return false;
    }
    Result rc = romfsMountFromCurrentProcess("gamefs");
    if (R_FAILED(rc)) {
        why = "Couldn't open the game's files (error " + std::to_string(R_MODULE(rc)) + "-" + std::to_string(R_DESCRIPTION(rc)) + ").";
        return false;
    }
    try {
        out = mm::readFile("gamefs:/" + romfsPath);
    } catch (const std::exception& e) {
        why = std::string("Couldn't read the file: ") + e.what();
    }
    romfsUnmount("gamefs");
    return why.empty();
#else
    const char* env = std::getenv(game == mm::Game::Botw ? "MM_GAME_ROMFS_BOTW" : "MM_GAME_ROMFS_TOTK");
    if (!env) {
        why = "Patchwork wasn't opened from the game.";
        return false;
    }
    try {
        out = mm::readFile(mm::fs::path(env) / romfsPath);
        return true;
    } catch (const std::exception& e) {
        why = e.what();
        return false;
    }
#endif
}

namespace {
#ifdef __SWITCH__
std::string resultCode(Result rc) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04u-%04u", 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return buf;
}

constexpr const char* kSaveDevice = "pwsave";

bool initAccounts() {
    static bool ok = R_SUCCEEDED(accountInitialize(AccountServiceType_Administrator)) ||
                     R_SUCCEEDED(accountInitialize(AccountServiceType_System));
    return ok;
}

std::string uidToId(const AccountUid& uid) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)uid.uid[1], (unsigned long long)uid.uid[0]);
    return buf;
}

bool idToUid(const std::string& id, AccountUid& uid) {
    if (id.size() != 32 || !mm::isHex(id)) return false;
    uid.uid[1] = std::stoull(id.substr(0, 16), nullptr, 16);
    uid.uid[0] = std::stoull(id.substr(16), nullptr, 16);
    return true;
}
#endif
}  // namespace

std::tm localTime(int64_t unixSeconds) {
    std::tm tm{};
    std::time_t t = static_cast<std::time_t>(unixSeconds);
#ifdef __SWITCH__
    TimeCalendarTime cal{};
    TimeCalendarAdditionalInfo info{};
    if (unixSeconds > 0 && R_SUCCEEDED(timeToCalendarTimeWithMyRule(static_cast<u64>(unixSeconds), &cal, &info))) {
        tm.tm_year = cal.year - 1900;
        tm.tm_mon = cal.month - 1;
        tm.tm_mday = cal.day;
        tm.tm_hour = cal.hour;
        tm.tm_min = cal.minute;
        tm.tm_sec = cal.second;
        tm.tm_wday = static_cast<int>(info.wday);
        return tm;
    }
    gmtime_r(&t, &tm);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::vector<UserProfile> userProfiles(std::string& why) {
    std::vector<UserProfile> out;
#ifdef __SWITCH__
    if (!initAccounts()) {
        why = "Couldn't read the user profiles.";
        return out;
    }
    AccountUid uids[ACC_USER_LIST_SIZE];
    s32 count = 0;
    Result rc = accountListAllUsers(uids, ACC_USER_LIST_SIZE, &count);
    if (R_FAILED(rc)) {
        why = "Couldn't read the user profiles (error " + resultCode(rc) + ").";
        return out;
    }
    for (s32 i = 0; i < count; ++i) {
        UserProfile p{uidToId(uids[i]), "User " + std::to_string(i + 1)};
        AccountProfile profile;
        if (R_SUCCEEDED(accountGetProfile(&profile, uids[i]))) {
            AccountProfileBase base{};
            if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base)) && base.nickname[0])
                p.name.assign(base.nickname, strnlen(base.nickname, sizeof base.nickname));
            accountProfileClose(&profile);
        }
        out.push_back(p);
    }
#else
    std::string list = std::getenv("MM_USERS") ? std::getenv("MM_USERS") : "Player";
    size_t start = 0;
    while (start <= list.size()) {
        auto end = list.find(',', start);
        auto name = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!name.empty()) out.push_back({mm::slugify(name), name});
        if (end == std::string::npos) break;
        start = end + 1;
    }
#endif
    if (out.empty() && why.empty()) why = "There are no user profiles on this console.";
    return out;
}

std::string saveBlocker() {
    const char* open = "A game is open in the background, and it would write over the change. Close it first: press "
                       "HOME, highlight the game and press X.";
#ifdef __SWITCH__
    // When Patchwork runs in place of a game (opened by holding R), that game is us.
    std::string why;
    if (R_SUCCEEDED(pmdmntInitialize())) {
        u64 pid = 0, self = 0;
        if (R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid)) && R_SUCCEEDED(svcGetProcessId(&self, CUR_PROCESS_HANDLE)) &&
            pid != self)
            why = open;
        pmdmntExit();
    }
    return why;
#else
    return std::getenv("MM_GAME_OPEN") ? open : "";
#endif
}

std::unique_ptr<SaveData> SaveData::open(mm::Game game, const std::string& userId, std::string& why) {
    std::unique_ptr<SaveData> s(new SaveData());
    std::string name = mm::gameInfo(game).displayName.data();
#ifdef __SWITCH__
    AccountUid uid{};
    if (!idToUid(userId, uid)) {
        why = "Unknown user profile.";
        return nullptr;
    }
    u64 tid = std::stoull(std::string(mm::gameInfo(game).titleId), nullptr, 16);
    Result rc = fsdevMountSaveData(kSaveDevice, tid, uid);
    if (R_FAILED(rc)) {
        if (R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 1002)
            why = "This profile has no " + name + " save yet. Start the game once with this profile, then try again.";
        else
            why = "Couldn't open the " + name + " save (error " + resultCode(rc) + "). If the game is open, close it and try again.";
        return nullptr;
    }
    s->root_ = std::string(kSaveDevice) + ":/";
#else
    const char* env = std::getenv("MM_SAVE_ROOT");
    auto root = (env ? mm::fs::path(env) : sdRoot().parent_path() / "saves") / std::string(mm::gameInfo(game).titleId) / userId;
    std::error_code ec;
    if (!mm::fs::is_directory(root, ec)) {
        why = "This profile has no " + name + " save yet. Start the game once with this profile, then try again.";
        return nullptr;
    }
    s->root_ = root;
#endif
    return s;
}

SaveData::~SaveData() {
#ifdef __SWITCH__
    fsdevUnmountDevice(kSaveDevice);
#endif
}

void SaveData::commit() {
#ifdef __SWITCH__
    Result rc = fsdevCommitDevice(kSaveDevice);
    if (R_FAILED(rc)) throw std::runtime_error("couldn't save the changes (error " + resultCode(rc) + ")");
#endif
}

mm::fs::path resourcePath(const std::string& name) {
#ifdef __SWITCH__
    return mm::fs::path("romfs:/") / name;
#else
    return mm::fs::path("./resources") / name;
#endif
}

std::string userAgent() {
#ifdef __SWITCH__
    return "Mozilla/5.0 (Nintendo Switch; patchwork/" APP_VERSION ")";
#else
    return "Mozilla/5.0 (compatible; patchwork/" APP_VERSION ")";
#endif
}

}  // namespace app::platform
