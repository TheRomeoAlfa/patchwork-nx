#include <fstream>

#include "sysmodules.hpp"
#include "test.hpp"

using namespace mm;

static void writeBytes(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

TEST("sysmodules: list, names and on/off via boot2.flag") {
    t::TempDir sd;
    auto contents = sd.path / "atmosphere" / "contents";
    writeBytes(contents / "0000000000534C56" / "exefs.nsp", "x");
    writeBytes(contents / "0000000000534C56" / "flags" / "boot2.flag", "");
    writeBytes(contents / "00FF0000636C6BFF" / "exefs.nsp", "x");  // sys-clk, off
    writeBytes(contents / "4200000000000ABC" / "exefs.nsp", "x");
    writeBytes(contents / "4200000000000ABC" / "toolbox.json", R"({"name": "my-module", "tid": "4200000000000ABC"})");
    writeBytes(contents / "4200000000000ABC" / "flags" / "boot2.flag", "");
    writeBytes(contents / "0100F2C0115B6000" / "romfs" / "a.txt", "mod");  // a game's mod folder, not a sysmodule

    auto list = listSysmodules(sd.path);
    CHECK_EQ(list.size(), 3u);
    auto find = [&](const std::string& name) {
        for (const auto& m : listSysmodules(sd.path))
            if (m.name == name) return m;
        return Sysmodule{};
    };
    CHECK(find("SaltyNX").enabled);
    CHECK(!find("sys-clk").enabled);
    CHECK(find("my-module").enabled);
    CHECK(!find("SaltyNX").about.empty());

    setSysmoduleEnabled(sd.path, "0000000000534C56", false);
    CHECK(!find("SaltyNX").enabled);
    CHECK(fs::exists(contents / "0000000000534C56" / "flags" / "boot2.flag.off"));
    setSysmoduleEnabled(sd.path, "0000000000534C56", true);
    CHECK(find("SaltyNX").enabled);
    CHECK(!fs::exists(contents / "0000000000534C56" / "flags" / "boot2.flag.off"));
    setSysmoduleEnabled(sd.path, "00FF0000636C6BFF", true);  // never had a flag
    CHECK(find("sys-clk").enabled);
    // Off twice in a row, with a stale .off left behind.
    writeBytes(contents / "4200000000000ABC" / "flags" / "boot2.flag.off", "");
    setSysmoduleEnabled(sd.path, "4200000000000ABC", false);
    CHECK(!find("my-module").enabled);
}

TEST("sysmodules: finds mod loader crash reports") {
    t::TempDir sd;
    auto dir = sd.path / "atmosphere" / "fatal_errors";
    // Header bytes as written by Atmosphère: magic, error 0xffe, program id (little endian).
    std::string amsMitm("AFE2\xfe\x0f\x00\x00\x00\x00\x53\x4d\x54\x41\x00\x01", 16);
    std::string other("AFE2\xfe\x0f\x00\x00\x00\xbd\x00\x00\x00\x00\x00\x01", 16);
    writeBytes(dir / "report_0000000029e8b6d4.bin", amsMitm + std::string(64, '\0'));
    writeBytes(dir / "report_000000002a171a92.bin", other + std::string(64, '\0'));
    writeBytes(dir / "report_000000002a171a93.bin.zip", amsMitm);
    auto found = modLoaderCrashReports(sd.path);
    CHECK_EQ(found.size(), 1u);
    if (!found.empty()) CHECK_EQ(found[0], std::string("report_0000000029e8b6d4.bin"));
    CHECK(modLoaderCrashReports(sd.path / "missing").empty());
}
