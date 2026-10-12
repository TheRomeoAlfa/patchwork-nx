#include <algorithm>
#include <set>
#include "test.hpp"
#include "util.hpp"
#include "waikuteru.hpp"

using namespace mm;

TEST("waikuteru: downloads page lists mods per game with platform") {
    auto list = wk::parseDownloadsPage(readFile(fs::path(MM_FIXTURES) / "wk_list.html"));
    CHECK_EQ(list.size(), 27u);
    size_t botw = 0, botwSwitch = 0, totk = 0;
    for (const auto& e : list) {
        (e.game == Game::Botw ? botw : totk)++;
        if (e.game == Game::Botw && e.onSwitch) ++botwSwitch;
        CHECK(e.summary.sourceKey.rfind("wk:", 0) == 0);
        CHECK(!e.summary.profileUrl.empty());
        CHECK(e.summary.thumbnailUrl.rfind("https://static.wixstatic.com/media/", 0) == 0);
    }
    std::set<std::string> thumbs;
    for (const auto& e : list) thumbs.insert(e.summary.thumbnailUrl);
    CHECK(thumbs.size() > 20);  // each card has its own picture
    CHECK_EQ(botw, 22u);
    CHECK_EQ(totk, 5u);
    CHECK_EQ(botwSwitch, 12u);
    auto ft = std::find_if(list.begin(), list.end(), [](const auto& e) { return e.summary.sourceKey == "wk:botw/faster-teleport-by-waikuteru"; });
    CHECK(ft != list.end());
    if (ft != list.end()) {
        CHECK_EQ(ft->summary.name, std::string("Faster Teleport by Waikuteru"));
        CHECK(ft->summary.thumbnailUrl.find("696de8_8953e32d") != std::string::npos);
    }
    auto whomp = std::find_if(list.begin(), list.end(), [](const auto& e) { return e.summary.name == "Whomp's Fortress"; });
    CHECK(whomp != list.end());
}

TEST("waikuteru: mod page gives description, image and Switch/Wii U files") {
    gb::ModSummary s;
    s.sourceKey = "wk:botw/faster-teleport-by-waikuteru";
    auto d = wk::parseModPage(readFile(fs::path(MM_FIXTURES) / "wk_mod_botw.html"), s);
    CHECK_EQ(d.files.size(), 2u);
    int sw = 0, wiiu = 0;
    for (const auto& f : d.files) {
        if (wk::isWiiUOnly(f)) ++wiiu; else ++sw;
        CHECK(f.id > 0);
        CHECK(endsWithI(f.filename, ".zip"));
    }
    CHECK_EQ(sw, 1);
    CHECK_EQ(wiiu, 1);
    CHECK(d.descriptionHtml.find("loading screen") != std::string::npos);
    CHECK(!d.screenshotUrls.empty());
    if (!d.screenshotUrls.empty()) CHECK(d.screenshotUrls[0].find("8953e32dc78847f987e96c2b55545ce9") != std::string::npos);  // its title card

    gb::ModSummary t;
    t.sourceKey = "wk:totk/god-mode-by-waikuteru";
    auto g = wk::parseModPage(readFile(fs::path(MM_FIXTURES) / "wk_mod_totk.html"), t);
    CHECK_EQ(g.files.size(), 1u);
    if (!g.files.empty()) CHECK(!wk::isWiiUOnly(g.files[0]));
}

TEST("waikuteru: urls") {
    CHECK(wk::isModPageUrl("https://www.waikuteru-zelda-mods.com/downloads/botw/whomp's-fortress"));
    CHECK(!wk::isModPageUrl("https://www.waikuteru-zelda-mods.com/downloads"));
    CHECK_EQ(wk::sourceKeyForUrl("https://www.waikuteru-zelda-mods.com/downloads/botw/Whomp%27s-Fortress?x=1"), std::string("wk:botw/whomp's-fortress"));
}
