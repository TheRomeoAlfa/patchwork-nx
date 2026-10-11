// Read-only hardware check: prints installed BOTW/TOTK versions and the top GameBanana
// mods for each. Writes nothing to the SD card.
#include <switch.h>

#include <cstdio>
#include <memory>
#include <string>

#include "gamebanana.hpp"

using namespace mm;

static std::string installedVersion(const GameInfo& g) {
    auto data = std::make_unique<NsApplicationControlData>();
    u64 size = 0;
    u64 tid = std::stoull(std::string(g.titleId), nullptr, 16);
    if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, tid, data.get(), sizeof(*data), &size)))
        return "not installed";
    return data->nacp.display_version;
}

int main() {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    socketInitializeDefault();
    nsInitialize();

    std::printf("patchwork smoke test\n\n");
    for (const auto& g : kGames) std::printf("%s: %s\n", std::string(g.displayName).c_str(), installedVersion(g).c_str());
    consoleUpdate(nullptr);

    try {
        CurlHttpClient http("Mozilla/5.0 (Nintendo Switch; patchwork/0.1)");
        gb::Client client(http);
        for (const auto& g : kGames) {
            std::printf("\nTop %s mods on GameBanana:\n", std::string(g.key).c_str());
            consoleUpdate(nullptr);
            auto page = client.list(g.game, gb::Sort::MostDownloaded, 1, 5);
            for (const auto& m : page.mods) std::printf("  %.60s\n", m.name.c_str());
            std::printf("  (%lld total)\n", static_cast<long long>(page.totalCount));
            consoleUpdate(nullptr);
        }
        std::printf("\nNetwork + GameBanana OK.\n");
    } catch (const std::exception& e) {
        std::printf("\nERROR: %s\n", e.what());
    }

    std::printf("\nPress + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    nsExit();
    socketExit();
    consoleExit(nullptr);
    return 0;
}
