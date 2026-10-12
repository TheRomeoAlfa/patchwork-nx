#include "platform.hpp"

#include <sys/statvfs.h>

#include <cstdlib>
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

std::string userAgent() {
#ifdef __SWITCH__
    return "Mozilla/5.0 (Nintendo Switch; patchwork/" APP_VERSION ")";
#else
    return "Mozilla/5.0 (compatible; patchwork/" APP_VERSION ")";
#endif
}

}  // namespace app::platform
