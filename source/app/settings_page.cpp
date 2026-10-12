#include "settings_page.hpp"

#include "app_state.hpp"
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
