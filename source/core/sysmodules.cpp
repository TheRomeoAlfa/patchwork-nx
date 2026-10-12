#include "sysmodules.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "nlohmann/json.hpp"

namespace mm {

namespace {

struct Known {
    const char* id;
    const char* name;
    const char* about;
    bool needed;
};

const Known kKnown[] = {
    {"0000000000534C56", "SaltyNX", "Lets plugins change running games (FPSLocker uses it)", false},
    {"00FF0000636C6BFF", "sys-clk", "Overclocking: custom CPU/GPU speeds per game", false},
    {"0100000000000352", "emuiibo", "Virtual amiibo", false},
    {"010000000000BD00", "MissionControl", "Pair other consoles' controllers over Bluetooth", false},
    {"420000000000000B", "sys-patch", "Lets the Switch start games and homebrew it would refuse", true},
    {"420000000007E51A", "nx-ovlloader", "Overlay menu (Tesla / Ultrahand)", false},
    {"4200000000000010", "ldn_mitm", "LAN play", false},
    {"420000000000000E", "sys-ftpd", "FTP server", false},
    {"690000000000000D", "sys-con", "USB controllers from other consoles", false},
};

std::string toolboxName(const fs::path& dir) {
    try {
        auto j = nlohmann::json::parse(readFile(dir / "toolbox.json"));
        return j.value("name", "");
    } catch (...) {
        return "";
    }
}

}  // namespace

std::vector<Sysmodule> listSysmodules(const fs::path& sdRoot) {
    std::vector<Sysmodule> out;
    std::error_code ec;
    for (fs::directory_iterator it(sdRoot / "atmosphere" / "contents", ec), end; it != end; it.increment(ec)) {
        auto id = it->path().filename().string();
        if (id.size() != 16 || !isHex(id) || !fs::exists(it->path() / "exefs.nsp", ec)) continue;
        Sysmodule m;
        m.titleId = id;
        m.enabled = fs::exists(it->path() / "flags" / "boot2.flag", ec);
        for (const auto& k : kKnown) {
            if (!iequals(k.id, id)) continue;
            m.name = k.name;
            m.about = k.about;
            m.needed = k.needed;
        }
        if (auto n = toolboxName(it->path()); !n.empty()) m.name = n;
        if (m.name.empty()) m.name = id;
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const Sysmodule& a, const Sysmodule& b) { return toLower(a.name) < toLower(b.name); });
    return out;
}

void setSysmoduleEnabled(const fs::path& sdRoot, const std::string& titleId, bool enabled) {
    auto flags = sdRoot / "atmosphere" / "contents" / titleId / "flags";
    auto on = flags / "boot2.flag", off = flags / "boot2.flag.off";
    std::error_code ec;
    if (enabled) {
        if (fs::exists(on, ec)) return;
        if (fs::exists(off, ec)) {
            fs::rename(off, on);
        } else {
            fs::create_directories(flags);
            writeFileAtomic(on, "");
        }
    } else {
        if (!fs::exists(on, ec)) return;
        fs::remove(off, ec);  // FAT won't rename over an existing file
        fs::rename(on, off);
    }
}

std::vector<std::string> modLoaderCrashReports(const fs::path& sdRoot) {
    // Each report starts with "AFE2" (or "AFE1"/"AFE0"), a 32-bit error code and the 64-bit
    // program id of the module that crashed.
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(sdRoot / "atmosphere" / "fatal_errors", ec), end; it != end; it.increment(ec)) {
        auto name = it->path().filename().string();
        if (name.rfind("report_", 0) != 0 || !endsWithI(name, ".bin")) continue;
        std::ifstream in(it->path(), std::ios::binary);
        char head[16] = {};
        if (!in.read(head, sizeof head) || std::memcmp(head, "AFE", 3) != 0) continue;
        unsigned long long programId = 0;
        for (int i = 7; i >= 0; --i) programId = (programId << 8) | static_cast<unsigned char>(head[8 + i]);
        if (programId == kModLoaderProgramId) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace mm
