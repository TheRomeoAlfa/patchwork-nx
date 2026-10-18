#include "bcml_test.hpp"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <stdexcept>

#include "app_state.hpp"
#include "archive.hpp"
#include "platform.hpp"
#include "sarc.hpp"
#include "sizetable.hpp"

namespace app {

namespace {

constexpr const char* kTitleTarget = "atmosphere/contents/01007EF00011E000/romfs/Pack/Title.pack";

// The packages: [LINKLE] Title Screen NX (merges into Title.pack, easy to see in the game)
// and Early Master Cycle (a tiny change to one shrine's pack).
struct TestMod {
    int64_t id;
    const char* name;
};
constexpr TestMod kMods[] = {{423509, "Linkle title screen"}, {321704, "Early Master Cycle"}};

mm::fs::path testDir() { return AppState::get().dataDir() / "bcml-test"; }
mm::fs::path titleOutput() { return testDir() / "out" / "Title.pack"; }

double seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

std::string mb(uint64_t bytes) {
    char buf[32];
    if (bytes < 1048576)
        std::snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
    else
        std::snprintf(buf, sizeof buf, "%.1f MB", bytes / 1048576.0);
    return buf;
}

std::string secs(double s) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f s", s);
    return buf;
}

std::string memory() {
    auto used = platform::memoryUsedBytes(), total = platform::memoryTotalBytes();
    return total ? mb(used) + " of " + mb(total) : mb(used) + " (peak)";
}

// Open an archive and everything nested in it; returns the number of files.
size_t openAll(const std::string& data, uint64_t& unpackedBytes) {
    auto s = mm::Sarc::parse(data);
    size_t n = 0;
    for (const auto& f : s.files) {
        ++n;
        unpackedBytes += f.data.size();
        if (mm::isSarc(f.data)) n += openAll(f.data, unpackedBytes);
        else if (mm::isYaz0(f.data)) unpackedBytes += mm::yaz0Decompress(f.data).size();
    }
    return n;
}

bool readGame(const std::string& path, std::string& out, std::string& why) {
    return platform::readGameFile(mm::Game::Botw, path, out, why);
}

}  // namespace

std::string runBcmlTest(const std::function<void(const std::string&, float)>& progress) {
    std::ostringstream r;
    auto& st = AppState::get();
    std::error_code ec;
    mm::fs::create_directories(testDir() / "out", ec);
    r << "BCML merge test · Patchwork " << APP_VERSION << " · BOTW " << st.gameVersion(mm::Game::Botw) << "\n";
    r << "Memory at start: " << memory() << "\n";

    // The game's own files, unmodded: Title.pack must not be the SD card's.
    std::string probe, why;
    if (!readGame("Pack/Title.pack", probe, why))
        throw std::runtime_error(why + " Hold L and R while starting Breath of the Wild, then open Patchwork from the Homebrew Menu.");
    auto onCard = platform::sdRoot() / kTitleTarget;
    if (mm::fs::exists(onCard, ec) && mm::readFile(onCard) == probe)
        throw std::runtime_error("Got a mod's Title.pack instead of the game's. Hold L as well as R while starting the game.");
    probe.clear();

    for (size_t m = 0; m < std::size(kMods); ++m) {
        const auto& mod = kMods[m];
        float base = 0.6f * m / std::size(kMods), span = 0.6f / std::size(kMods);
        r << "\n" << mod.name << " (mod " << mod.id << ")\n";
        progress("Downloading " + std::string(mod.name) + "…", base);
        mm::gb::ModFile bnp;
        for (const auto& f : st.gamebanana().files(mod.id))
            if (mm::endsWithI(f.filename, ".bnp")) bnp = f;
        if (bnp.downloadUrl.empty()) throw std::runtime_error(std::string("no .bnp file for ") + mod.name);
        auto archive = testDir() / (std::to_string(mod.id) + ".bnp");
        if (!mm::fs::exists(archive, ec) || (bnp.size > 0 && mm::fs::file_size(archive, ec) != uint64_t(bnp.size)))
            st.http().download(bnp.downloadUrl, archive, [&](uint64_t done, uint64_t total) {
                if (!total && bnp.size > 0) total = uint64_t(bnp.size);
                if (total) progress("Downloading " + std::string(mod.name) + "… " + std::to_string(done * 100 / total) + "%",
                                    base + span * 0.3f * done / total);
                return true;
            });
        auto dir = testDir() / std::to_string(mod.id);
        mm::removeTree(dir);
        auto t = std::chrono::steady_clock::now();
        mm::extractArchive(archive, dir);
        r << "  unpack package: " << secs(seconds(t)) << "\n";

        auto romfs = dir / "01007EF00011E000" / "romfs";
        std::vector<std::string> packs;
        for (mm::fs::recursive_directory_iterator it(romfs, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && mm::isSarc(mm::readFile(it->path())))
                packs.push_back(it->path().lexically_relative(romfs).generic_string());
        for (const auto& pack : packs) {
            progress("Merging " + pack + "…", base + span * 0.3f);
            std::string game;
            t = std::chrono::steady_clock::now();
            if (!readGame(pack, game, why)) throw std::runtime_error("couldn't read the game's " + pack + ": " + why);
            r << "  " << pack << ": game's copy " << mb(game.size()) << ", read in " << secs(seconds(t)) << "\n";
            auto modPack = mm::readFile(romfs / pack);

            t = std::chrono::steady_clock::now();
            auto stored = mm::mergeSarcs({game, modPack}, false);
            r << "  merge, stored: " << secs(seconds(t)) << " → " << mb(stored.size()) << "\n";
            progress("Merging " + pack + " (compressed)…", base + span * 0.6f);
            t = std::chrono::steady_clock::now();
            auto packed = mm::mergeSarcs({game, modPack}, true);
            r << "  merge, compressed: " << secs(seconds(t)) << " → " << mb(packed.size()) << "\n";
            r << "  memory: " << memory() << "\n";

            if (pack == "Pack/Title.pack") {
                t = std::chrono::steady_clock::now();
                mm::writeFileAtomic(titleOutput(), packed);
                r << "  write to SD: " << secs(seconds(t)) << "\n";
                // Is the merged layout within the game's size table entry?
                try {
                    auto table = mm::SizeTable::parse(mm::readFile(st.dataDir() / "original" / "botw" / "ResourceSizeTable.product.srsizetable"));
                    auto merged = mm::Sarc::parse(packed);
                    if (const auto* layout = merged.find("Layout/Title.sblarc")) {
                        auto size = mm::yaz0Decompress(layout->data).size();
                        auto crc = mm::sizeTableCrc("Layout/Title.blarc");
                        auto it = table.crc.find(crc);
                        r << "  Layout/Title.blarc: " << size << " bytes, size table allows "
                          << (it == table.crc.end() ? std::string("(no entry)") : std::to_string(it->second)) << "\n";
                    }
                } catch (const std::exception& e) {
                    r << "  size table check skipped: " << e.what() << "\n";
                }
            }
        }
        mm::removeTree(dir);
    }

    // The game's biggest archives: how long just opening everything in them takes.
    r << "\nOpening the game's big files\n";
    const char* big[] = {"Pack/Bootup.pack", "Pack/TitleBG.pack", "Actor/ActorInfo.product.sbyml"};
    for (size_t i = 0; i < std::size(big); ++i) {
        progress(std::string("Opening ") + big[i] + "…", 0.6f + 0.4f * i / std::size(big));
        std::string data;
        auto t = std::chrono::steady_clock::now();
        if (!readGame(big[i], data, why)) {
            r << "  " << big[i] << ": couldn't read (" << why << ")\n";
            continue;
        }
        double readTime = seconds(t);
        t = std::chrono::steady_clock::now();
        uint64_t unpacked = 0;
        size_t files = 0;
        try {
            if (mm::isSarc(data))
                files = openAll(data, unpacked);
            else
                unpacked = mm::yaz0Decompress(data).size();
        } catch (const std::exception& e) {
            r << "  " << big[i] << ": " << e.what() << "\n";
            continue;
        }
        r << "  " << big[i] << ": " << mb(data.size()) << ", read " << secs(readTime) << ", unpacked " << mb(unpacked);
        if (files) r << " in " << files << " files";
        r << ", " << secs(seconds(t)) << "; memory " << memory() << "\n";
    }
    r << "\nMemory at end: " << memory() << "\n";
    auto report = r.str();
    mm::writeFileAtomic(testDir() / "report.txt", report);
    return report;
}

bool bcmlTitleReady() {
    std::error_code ec;
    return mm::fs::exists(titleOutput(), ec);
}

bool bcmlTitleInGame() {
    std::error_code ec;
    auto target = platform::sdRoot() / kTitleTarget;
    return bcmlTitleReady() && mm::fs::exists(target, ec) && mm::fs::file_size(target, ec) == mm::fs::file_size(titleOutput(), ec) &&
           mm::readFile(target) == mm::readFile(titleOutput());
}

void putBcmlTitleInGame() {
    std::error_code ec;
    auto target = platform::sdRoot() / kTitleTarget;
    if (mm::fs::exists(target, ec)) throw std::runtime_error("A mod already has a Title.pack on the SD card.");
    mm::copyFileCreatingDirs(titleOutput(), target);
}

void removeBcmlTitleFromGame() {
    if (!bcmlTitleInGame()) return;
    auto target = platform::sdRoot() / kTitleTarget;
    mm::fs::remove(target);
    mm::pruneEmptyDirs(target.parent_path(), platform::sdRoot() / "atmosphere" / "contents");
}

}  // namespace app
