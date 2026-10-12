#include "settings_page.hpp"

#include "app_state.hpp"
#include "platform.hpp"
#include "sysmodules.hpp"
#include "ui.hpp"

namespace app {

namespace {

uint64_t dirSize(const mm::fs::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (mm::fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    return total;
}

}  // namespace

SettingsPage::SettingsPage() : brls::Box(brls::Axis::COLUMN) {
    setGrow(1);
    auto* content = ui::vbox();
    content->setPadding(24, 60, 24, 40);
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setContentView(content);
    addView(scroll);

    auto section = [content](const std::string& title) {
        auto* l = ui::label(title, 16, ui::muted());
        l->setMargins(18, 0, 8, 4);
        content->addView(l);
    };

    section("Browsing");
    auto* nsfw = new brls::BooleanCell();
    nsfw->init("Hide NSFW mods", AppState::get().settings().hideNsfw, [](bool on) {
        AppState::get().settings().hideNsfw = on;
        AppState::get().saveSettings();
    });
    content->addView(nsfw);
    auto* nsfwNote = ui::label("Uses GameBanana's own content ratings: mods with nudity or sexual themes are hidden from Browse and Search.", 14, ui::muted(), false);
    nsfwNote->setMargins(6, 4, 0, 4);
    content->addView(nsfwNote);

    section("Background add-ons");
    auto modules = mm::listSysmodules(platform::sdRoot());
    if (modules.empty()) {
        auto* none = ui::label("No add-ons (sysmodules) found in atmosphere/contents.", 14, ui::muted(), false);
        none->setMargins(6, 4, 0, 4);
        content->addView(none);
    }
    for (const auto& m : modules) {
        auto* cell = new brls::BooleanCell();
        cell->init(m.name, m.enabled, [id = m.titleId, name = m.name, needed = m.needed](bool on) {
            try {
                mm::setSysmoduleEnabled(platform::sdRoot(), id, on);
                if (!on && needed)
                    brls::Application::notify(name + " is off. Some games may not start without it. Restart the Switch to apply.");
                else
                    brls::Application::notify(name + (on ? " turned on" : " turned off") + ". Restart the Switch to apply.");
            } catch (const std::exception& e) {
                brls::Application::notify("Couldn't change " + name + ": " + e.what());
            }
        });
        content->addView(cell);
        if (!m.about.empty() || m.needed) {
            auto* about = ui::label(m.about + (m.needed ? " · Needed to start some games" : ""), 14, ui::muted(), false);
            about->setMargins(2, 4, 4, 4);
            content->addView(about);
        }
    }
    auto* addonsNote = ui::label(
        "These start when the Switch boots, and changes apply after a restart.\n"
        "Game crashing with an Atmosphère error (Program ID 010041544D530000) when mods are on? That's usually "
        "not the mod: add-ons can use up the memory Atmosphère needs to load mods, or not match your Atmosphère "
        "version. Turn them off, restart and try the game. If it works, turn them back on one at a time "
        "(restarting each time) to find the one causing it.",
        14, ui::muted(), false);
    addonsNote->setMargins(8, 4, 0, 4);
    content->addView(addonsNote);

    section("Storage");
    auto* cache = new brls::DetailCell();
    auto cacheDirs = [] {
        auto& st = AppState::get();
        return dirSize(st.downloadsDir()) + dirSize(st.workDir());
    };
    cache->setText("Delete unfinished downloads");
    cache->setDetailText(formatSize(cacheDirs()));
    cache->registerClickAction([cache, cacheDirs](brls::View*) {
        if (AppState::get().fileJobRunning()) {
            brls::Application::notify("Wait for the current install to finish");
            return true;
        }
        mm::removeTree(AppState::get().downloadsDir());
        mm::removeTree(AppState::get().workDir());
        cache->setDetailText(formatSize(cacheDirs()));
        brls::Application::notify("Leftover downloads deleted");
        return true;
    });
    content->addView(cache);

    auto* repair = new brls::DetailCell();
    repair->setText("Re-copy enabled mods to the SD card");
    repair->setDetailText("Fixes missing files");
    repair->registerClickAction([](brls::View*) {
        brls::Application::notify("Checking mod files…");
        AppState::get().runFileJob([](mm::Library& lib) { lib.apply(); }, [](const std::string& error) {
            brls::Application::notify(error.empty() ? "Mod files are up to date" : "Repair failed: " + error);
        });
        return true;
    });
    content->addView(repair);

    auto* where = new brls::DetailCell();
    where->setText("Mod library folder");
    where->setDetailText("SD:/switch/patchwork");
    content->addView(where);

    section("About");
    auto* about = ui::label(
        std::string(APP_TITLE) + " " + APP_VERSION + ", a mod manager for Breath of the Wild and Tears of the Kingdom." +
            "\nMods are downloaded from GameBanana and belong to their authors. Installed mods go into "
            "atmosphere/contents and are only active while running Atmosphère.\n"
            "Mods that need BCML or TKMM (PC merging tools) can't be installed here.",
        16, ui::muted(), false);
    about->setMargins(4, 4, 0, 4);
    content->addView(about);
}

}  // namespace app
