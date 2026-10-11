#include <archive.h>
#include <archive_entry.h>

#include <fstream>

#include "archive.hpp"
#include "compat.hpp"
#include "gamebanana.hpp"
#include "installer.hpp"
#include "test.hpp"

using namespace mm;

static void makeZip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& entries) {
    auto* a = archive_write_new();
    archive_write_set_format_zip(a);
    archive_write_open_filename(a, path.string().c_str());
    for (const auto& [name, data] : entries) {
        auto* e = archive_entry_new();
        archive_entry_set_pathname(e, name.c_str());
        archive_entry_set_size(e, static_cast<la_int64_t>(data.size()));
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_write_header(a, e);
        archive_write_data(a, data.data(), data.size());
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
}

TEST("archive: extracts files and refuses path traversal") {
    t::TempDir d;
    auto zip = d.path / "m.zip";
    makeZip(zip, {{"Mod/romfs/a.txt", "hello"}, {"../escape.txt", "bad"}, {"__MACOSX/._a.txt", "junk"}});
    auto out = d.path / "out";
    extractArchive(zip, out);
    CHECK_EQ(readFile(out / "Mod/romfs/a.txt"), std::string("hello"));
    CHECK(!fs::exists(d.path / "escape.txt"));
    CHECK(!fs::exists(out / "__MACOSX"));
}

TEST("archive: size limit is enforced") {
    t::TempDir d;
    auto zip = d.path / "big.zip";
    makeZip(zip, {{"a", std::string(4096, 'x')}});
    ExtractLimits lim;
    lim.maxTotalBytes = 1000;
    bool threw = false;
    try {
        extractArchive(zip, d.path / "out", lim);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST("installer: zip of zips gives one variant per inner zip") {
    t::TempDir d;
    auto inner1 = d.path / "src/Blue.zip", inner2 = d.path / "src/Red.zip";
    fs::create_directories(d.path / "src");
    makeZip(inner1, {{"romfs/a", "blue"}});
    makeZip(inner2, {{"romfs/a", "red"}, {"romfs/b", "red"}});
    makeZip(d.path / "outer.zip", {{"Blue.zip", readFile(inner1)}, {"Red.zip", readFile(inner2)}});
    auto p = prepareArchive(d.path / "outer.zip", d.path / "work", Game::Totk, "x");
    CHECK(p.layout.kind == PackageKind::Installable);
    CHECK_EQ(p.layout.variants.size(), 2u);
    CHECK_EQ(p.layout.variants[0].name, std::string("Blue"));
    auto merged = mergeVariants(p.layout, {0, 1});
    CHECK_EQ(merged.size(), 2u);
    for (const auto& m : merged) CHECK_EQ(readFile(m.source), std::string("red"));
}

TEST("compat: picks out game versions, ignores bare numbers") {
    auto v = mentionedGameVersions("<i>TKMM required and only tested on TotK v1.2.1</i>. Mod version 3.0, 1.5x durability", Game::Totk);
    CHECK_EQ(v.size(), 1u);
    if (!v.empty()) CHECK_EQ(v[0], std::string("1.2.1"));
    auto w = mentionedGameVersions("Works on game version 1.6.0 and Update 1.5", Game::Botw);
    CHECK_EQ(w.size(), 2u);
    auto x = mentionedGameVersions("For TotK v1.2.x. Challenge Mode v1.18.0", Game::Totk);
    CHECK_EQ(x.size(), 1u);
    if (!x.empty()) CHECK_EQ(x[0], std::string("1.2"));
    CHECK(checkCompat("1.2.1", {"1.2"}) == Compat::Likely);
    CHECK(checkCompat("1.2.1", {"1.1.2"}) == Compat::Mismatch);
    CHECK(checkCompat("1.2.1", {}) == Compat::Unknown);
    CHECK(checkCompat("1.20", {"1.2"}) == Compat::Mismatch);
}

TEST("gamebanana: parses saved list and detail responses") {
    auto list = gb::parseModList(readFile(fs::path(MM_FIXTURES) / "gb_list.json"));
    CHECK(list.totalCount > 0);
    CHECK(!list.mods.empty());
    if (!list.mods.empty()) {
        CHECK(list.mods[0].id > 0);
        CHECK(!list.mods[0].name.empty());
        CHECK(!list.mods[0].author.empty());
        CHECK_EQ(list.mods[0].gameId, 7617);
    }
    auto det = gb::parseModDetails(readFile(fs::path(MM_FIXTURES) / "gb_mod.json"), 725633);
    CHECK_EQ(det.summary.name, std::string("Master Sword - Ambient Durability Restore"));
    CHECK_EQ(det.files.size(), 1u);
    if (!det.files.empty()) {
        CHECK_EQ(det.files[0].downloadUrl, std::string("https://gamebanana.com/dl/1842372"));
        CHECK(det.files[0].avClean);
    }
    CHECK(!det.screenshotUrls.empty());
}

TEST("gamebanana: urls") {
    auto u = gb::Client::searchUrl(Game::Botw, "master sword", 2);
    CHECK(u.find("_sSearchString=master%20sword") != std::string::npos);
    CHECK(u.find("_idGameRow=6386") != std::string::npos);
}

TEST("gamebanana: NSFW uses GameBanana's visibility, not every content rating") {
    auto page = gb::parseModList(R"({"_aMetadata":{"_nRecordCount":3,"_bIsComplete":true},"_aRecords":[
        {"_idRow":1,"_sModelName":"Mod","_sName":"Plain","_bHasContentRatings":false,"_sInitialVisibility":"show"},
        {"_idRow":2,"_sModelName":"Mod","_sName":"Gore","_bHasContentRatings":true,"_sInitialVisibility":"show"},
        {"_idRow":3,"_sModelName":"Mod","_sName":"Nudity","_bHasContentRatings":true,"_sInitialVisibility":"hide"}]})");
    CHECK_EQ(page.mods.size(), 3u);
    CHECK(!page.mods[0].nsfw);
    CHECK(!page.mods[1].nsfw);
    CHECK(page.mods[2].nsfw);
}
