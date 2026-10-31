#include <archive.h>
#include <archive_entry.h>

#include <fstream>

#include "archive.hpp"
#include "compat.hpp"
#include "gamebanana.hpp"
#include "installer.hpp"
#include "md5.hpp"
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

TEST("compat: mod versions vs game versions") {
    // UltraCam labels its files with its own version numbers.
    std::vector<std::string> ultracam = {"Version 2.6.1", "Version 2.6", "Version 2.1.4", "VERSION 1.1.1"};
    CHECK(versionWordIsModVersion(ultracam, Game::Totk));
    CHECK(mentionedGameVersions("VERSION 1.1.1", Game::Totk, false, true).empty());
    // Unbreakable Equipment labels files by game version.
    std::vector<std::string> unbreakable = {"v1.0.0", "v1.2.1", "v1.4.1"};
    CHECK(!versionWordIsModVersion(unbreakable, Game::Totk));
    CHECK_EQ(mentionedGameVersions("v1.2.1", Game::Totk).size(), 1u);
    // Descriptions: changelog numbers don't count, game words do; "server" isn't "ver".
    CHECK(mentionedGameVersions("Changelog: v1.0 initial, v1.1 fixes", Game::Totk, true).empty());
    CHECK(mentionedGameVersions("server 1.2 is down", Game::Totk).empty());
    CHECK_EQ(mentionedGameVersions("Requires TotK 1.2.1", Game::Totk, true).size(), 1u);
    CHECK(saysAllVersions("Base Defogger, All Game Update Versions"));
    CHECK(!saysAllVersions("Only affects Interiors"));
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

TEST("archive: names a FAT SD card can't store are made safe") {
    t::TempDir d;
    auto zip = d.path / "m.zip";
    makeZip(zip, {{"Mod/Option: 60fps?/romfs/a.txt", "x"}, {"C:/evil.txt", "bad"}});
    auto out = d.path / "out";
    extractArchive(zip, out);
    CHECK(fs::exists(out / "Mod/Option_ 60fps_/romfs/a.txt"));
    CHECK(!fs::exists(out / "C_"));
}

TEST("md5: known vectors") {
    CHECK_EQ(md5String(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(md5String("abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
    CHECK_EQ(md5String(std::string(1000, 'a')), std::string("cabe45dcc9ae5b66ba86600cca6b8ba8"));
}

TEST("gamebanana: mod details from ProfilePage carry the NSFW rating") {
    // Trimmed ProfilePage responses: a rated mod hidden by default, and an ordinary one.
    auto rated = gb::parseModDetails(R"({"_sName":"Rated","_sInitialVisibility":"hide",
        "_aContentRatings":{"pn":"Partial Nudity"},"_aGame":{"_idRow":7617},"_aFiles":[]})", 1);
    CHECK(rated.summary.nsfw);
    CHECK_EQ(rated.summary.gameId, int64_t(7617));
    auto plain = gb::parseModDetails(R"({"_sName":"Plain","_sInitialVisibility":"show","_aFiles":[]})", 2);
    CHECK(!plain.summary.nsfw);
    auto ratedButShown = gb::parseModDetails(R"({"_sName":"Gore","_sInitialVisibility":"show",
        "_aContentRatings":{"bg":"Blood and Gore"},"_aFiles":[]})", 3);
    CHECK(!ratedButShown.summary.nsfw);
}

TEST("compat: TOTK size tables missing for the installed version") {
    const std::string res = "atmosphere/contents/0100F2C0115B6000/romfs/System/Resource/";
    std::vector<std::string> targets = {
        "atmosphere/contents/0100F2C0115B6000/romfs/Model/Npc.bfres.mc",
        res + "ResourceSizeTable.Product.120.rsizetable.zs", res + "ResourceSizeTable.Product.110.rsizetable.zs",
        res + "ResourceSizeTable.Product.112.rsizetable.zs", res + "ResourceSizeTable.Product.111.rsizetable.zs"};
    auto c = checkSizeTables(targets, Game::Totk, "1.2.1");
    CHECK(c.missing);
    CHECK_EQ(c.versions.size(), 4u);
    CHECK_EQ(c.versions.front(), std::string("1.1.0"));
    CHECK_EQ(c.versions.back(), std::string("1.2.0"));
    CHECK(!checkSizeTables(targets, Game::Totk, "1.2.0").missing);
    CHECK(!checkSizeTables(targets, Game::Totk, "").missing);  // version unknown: don't guess
    targets.push_back(res + "resourcesizetable.product.121.rsizetable.zs");
    CHECK(!checkSizeTables(targets, Game::Totk, "1.2.1").missing);
    CHECK(!checkSizeTables({"atmosphere/contents/0100F2C0115B6000/romfs/Pack/a.pack.zs"}, Game::Totk, "1.2.1").missing);
    CHECK(checkSizeTables({res + "ResourceSizeTable.Product.120.rsizetable.zs"}, Game::Botw, "1.6.0").versions.empty());
}

TEST("compat: closest per-version option when none matches exactly") {
    std::vector<std::string> opts = {
        "Serfrosts Interior Defogger (Game Version 1.0.0)", "Serfrosts Interior Defogger (Game Version 1.1.0)",
        "Serfrosts Interior Defogger (Game Version 1.1.1)", "Serfrosts Interior Defogger (Game Version 1.1.2)",
        "Serfrosts Interior Defogger (Game Version 1.2.0)"};
    CHECK_EQ(closestVersionOption(opts, Game::Totk, "1.2.1"), size_t(4));
    CHECK_EQ(closestVersionOption(opts, Game::Totk, "1.1.1"), size_t(2));
    CHECK_EQ(closestVersionOption(opts, Game::Totk, ""), opts.size());
    CHECK_EQ(closestVersionOption({"Main", "Add-on for 1.2.0"}, Game::Totk, "1.2.1"), size_t(2));  // only one names a version
    CHECK_EQ(closestVersionOption({"For 1.3.0", "For 1.4.0"}, Game::Totk, "1.2.1"), size_t(2));      // all newer
}

TEST("compat: add-on files vs newer versions of the same file") {
    CHECK(isAddOnFileOf("dark_title_screen_serfrosts_base_defogger_v2-4.zip", "serfrosts_base_defogger_v2-4.zip"));
    CHECK(isAddOnFileOf("puffy_jacket_no_backpack_v1.zip", "puffy_jacket.zip"));
    CHECK(!isAddOnFileOf("fierce_deity_s_rebirth_v102.7z", "fierce_deity_s_rebirth.7z"));
    CHECK(!isAddOnFileOf("fierce_deity_s_rebirth_v1.7z", "fierce_deity_s_rebirth.7z"));
    CHECK(!isAddOnFileOf("mymod-1.2.3.zip", "mymod.zip"));
    CHECK(!isAddOnFileOf("mymod v2.0b.zip", "mymod.zip"));
    CHECK(!isAddOnFileOf("short_v2.zip", "abc.zip"));  // too short to compare
    CHECK(!isAddOnFileOf("defogger.zip", "dark_defogger.zip"));
}

TEST("gamebanana: mods that only come as PC packages") {
    auto files = gb::parseFiles(R"([{"_sFile":"fastswim.bnp"},{"_sFile":"old.zip","_bIsArchived":true}])");
    CHECK_EQ(files.size(), 1u);
    std::vector<std::string> names;
    for (const auto& f : files) names.push_back(f.filename);
    CHECK(gb::needsPcTool(names));
    CHECK(gb::pcToolMessage(names).find("BCML") != std::string::npos);
    CHECK(!gb::needsPcTool({"mod.bnp", "mod-switch.zip"}));  // has something installable
    CHECK(gb::needsPcTool({"a.TKCL"}));
    CHECK(!gb::needsPcTool({}));  // no files listed: don't hide
}

TEST("compat: downloads that restore the original files") {
    CHECK(isRestoreOriginalsFile("uninstall_zeldas_ballad_switch_helm.rar"));
    CHECK(isRestoreOriginalsFile("Helm-Pack-Restore.zip"));
    CHECK(isRestoreOriginalsFile("vanilla_files.zip"));
    CHECK(isRestoreOriginalsFile("helm_pack_b.zip", "Uninstall: copy these over to go back to vanilla"));
    CHECK(!isRestoreOriginalsFile("install_zeldas_ballad_switch_helmpa.rar"));
    CHECK(!isRestoreOriginalsFile("vanilla_plus_armor.zip"));
    CHECK(!isRestoreOriginalsFile("restoration_project.zip"));
}

TEST("compat: a bare two-part version is the mod's, not the game's") {
    CHECK(mentionedGameVersions("Version: 1.0", Game::Botw).empty());
    CHECK(mentionedGameVersions("v1.5", Game::Botw).empty());
    CHECK_EQ(mentionedGameVersions("Version 1.6.0", Game::Botw).size(), 1u);
    CHECK_EQ(mentionedGameVersions("For game 1.6", Game::Botw).size(), 1u);
    CHECK_EQ(mentionedGameVersions("TOTK 1.2", Game::Totk).size(), 1u);
}

TEST("compat: version ranges") {
    // Shinobi no Satori (GameBanana 641335): "Now works on Version 1.1.0 - 1.4.3".
    // Read strictly (a whole mod page), "Version" alone is the mod's own: nothing.
    CHECK(mentionedGameVersions("Now works on Version 1.1.0 - 1.4.3 With Tkmm", Game::Totk, true).empty());
    auto v = mentionedGameVersions("Now works on Version 1.1.0 - 1.4.3", Game::Totk);
    CHECK(v == std::vector<std::string>({"1.1.0-1.4.3"}));
    CHECK(checkCompat("1.2.1", v) == Compat::Likely);
    CHECK(checkCompat("1.1.0", v) == Compat::Likely);
    CHECK(checkCompat("1.4.3", v) == Compat::Likely);
    CHECK(checkCompat("1.0.0", v) == Compat::Mismatch);
    CHECK(checkCompat("1.4.4", v) == Compat::Mismatch);
    // Other ways of writing it; an open end ("1.4") covers its whole x.
    CHECK(mentionedGameVersions("TotK 1.2.0 to 1.4.1", Game::Totk) == std::vector<std::string>({"1.2.0-1.4.1"}));
    CHECK(mentionedGameVersions("game v1.1.0\xe2\x80\x93v1.2.1", Game::Totk) == std::vector<std::string>({"1.1.0-1.2.1"}));
    auto open = mentionedGameVersions("TotK 1.2 - 1.4", Game::Totk);
    CHECK(open == std::vector<std::string>({"1.2-1.4"}));
    CHECK(checkCompat("1.4.2", open) == Compat::Likely);
    // Not ranges: a list, a backwards pair, a changelog line after a version.
    CHECK(mentionedGameVersions("TotK 1.2.1, 1.4.1", Game::Totk, true).size() == 1u);
    CHECK(mentionedGameVersions("TotK 1.4.1 - 1.2.0", Game::Totk) == std::vector<std::string>({"1.4.1"}));
    CHECK(mentionedGameVersions("TotK 1.2.1 - v3.0 adds swords", Game::Totk) == std::vector<std::string>({"1.2.1"}));
    // The closest per-version option counts a range up to the installed version.
    CHECK_EQ(closestVersionOption({"Game Version 1.0.0", "Game Version 1.1.0 - 1.4.3"}, Game::Totk, "1.2.1"), size_t(1));
}

TEST("compat: files labeled for TKMM, and file languages") {
    CHECK(saysTkmmOnly("TKMM Only"));
    CHECK(saysTkmmOnly("For TKMM (merge on PC)"));
    CHECK(!saysTkmmOnly("Without TKMM English"));
    CHECK(!saysTkmmOnly("No TKMM version"));
    CHECK(!saysTkmmOnly("non-tkmm romfs"));
    CHECK(!saysTkmmOnly("Atmosphere romfs"));
    // Labels from GameBanana's TOTK mods.
    CHECK(saysTkmmOnly("[TKMM Only] TKCL for Game Versions 1.4.0-1.4.3 (Includes Mod Options)"));
    CHECK(saysTkmmOnly("Main File (TKCL)"));
    CHECK(saysTkmmOnly("weapons_of_legend_redux.zip Main File (TKCL)"));
    CHECK(saysTkmmOnly("emotewheelv1tkcl.zip"));
    CHECK(saysTkmmOnly("Compressed TKCL for use with TKMM"));
    CHECK(saysTkmmOnly("TKCL with No Host Included"));
    CHECK(saysTkmmOnly("Use this for TKMM - includes both versions of the mod"));
    CHECK(saysTkmmOnly("TKCL file (will upload romfs version if internet get's better)"));
    CHECK(saysTkmmOnly("TKCL FILE - TKMM ONLY"));
    CHECK(!saysTkmmOnly("Made with game version 1.2.1. (TKMM file included)"));
    CHECK(!saysTkmmOnly("Initial release, TKMM file included"));
    CHECK(!saysTkmmOnly("Includes TKCL file. RomFS built for game version v1.2.0"));
    CHECK(!saysTkmmOnly("This is the raw file download for 1.1.2, this is the most flexible version without a tkcl"));
    CHECK(!saysTkmmOnly("romfs version, 1.2.1 only, install still with tkmm to merge with other mods"));
    CHECK(!saysTkmmOnly("ROMFS 1.2.1 only - USE TKMM"));
    CHECK(!saysTkmmOnly("Piano Mod, no romfs edits"));  // no TKMM mention at all
    CHECK_EQ(languageMatch("Without TKMM English", "en"), 1);
    CHECK_EQ(languageMatch("Without TKMM German", "en"), -1);
    CHECK_EQ(languageMatch("Deutsch", "de"), 1);
    CHECK_EQ(languageMatch("Atmosphere romfs", "en"), 0);
    CHECK_EQ(languageMatch("Englishman's armor", "en"), 0);  // a whole word only
}
