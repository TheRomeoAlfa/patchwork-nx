#include "settings_page.hpp"

#include <ctime>

#include "app_state.hpp"
#include "bcml_test.hpp"
#include "debug_script.hpp"
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
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);  // follow focus row by row, like the other lists
    scroll->setContentView(content);
    addView(scroll);
    scroll_ = scroll;

    auto section = [content](const std::string& title) {
        auto* l = ui::label(title, 16, ui::muted());
        l->setMargins(18, 0, 8, 4);
        content->addView(l);
        return l;
    };

    // NSFW: kept out of sight so children don't stumble on it. Holding L + R + minus for
    // 3 seconds on this page shows it until Patchwork closes; every launch starts hidden.
    nsfwViews_.push_back(section("Browsing"));
    auto* nsfw = new brls::BooleanCell();
    nsfw->init("Hide NSFW mods", AppState::get().settings().hideNsfw, [](bool on) { AppState::get().settings().hideNsfw = on; });
    content->addView(nsfw);
    nsfwViews_.push_back(nsfw);
    auto* nsfwNote = ui::label("Uses GameBanana's own content ratings: mods with nudity or sexual themes are hidden from Browse, "
                               "Search and links. Turns back on every time Patchwork starts.",
                               14, ui::muted(), false);
    nsfwNote->setMargins(10, 4, 0, 4);
    content->addView(nsfwNote);
    nsfwViews_.push_back(nsfwNote);
    if (!AppState::get().settings().nsfwSettingRevealed) {
        for (auto* v : nsfwViews_) v->setVisibility(brls::Visibility::GONE);
        holdTimer_.setCallback([this] {
            auto held = [](brls::ControllerButton b) { return brls::Application::getControllerState().buttons[b] || debug::isHeld(b); };
            bool all = held(brls::BUTTON_LB) && held(brls::BUTTON_RB) && held(brls::BUTTON_BACK);
            heldMs_ = all ? heldMs_ + 100 : 0;
            if (heldMs_ >= 3000) revealNsfwSetting();
        });
        holdTimer_.start(100);
    }

    // BCML packages: the one-time setup (reading the game's files) and its state.
    section("BCML packages · Breath of the Wild");
    auto* files = new brls::DetailCell();
    files->setText("Game files");
    {
        auto& st = AppState::get();
        using A = AppState::GameAccess;
        auto kept = st.keptGameFiles();
        auto access = st.botwAccess();
        files->setDetailText(access == A::Ready    ? (kept ? "Can read now · " + std::to_string(kept) + " kept" : "Can read now")
                             : access == A::ModsOn ? "Mods were on: hold L too"
                             : kept                ? std::to_string(kept) + " kept on the SD card"
                                                   : "Not read yet");
    }
    content->addView(files);
    auto* sizes = new brls::DetailCell();
    sizes->setText("Size table");
    sizes->setDetailText(AppState::get().sizeTableStatus());
    content->addView(sizes);
    auto* sizesBar = new ui::ProgressBar(300);
    sizesBar->setMargins(10, 4, 0, 4);
    content->addView(sizesBar);
    auto showSizes = [sizes, sizesBar] {
        float p = AppState::get().sizeTableProgress();
        sizes->setDetailText(AppState::get().sizeTableStatus());
        sizesBar->setVisibility(p >= 0 ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        if (p >= 0) sizesBar->setProgress(p);
        return p >= 0;
    };
    if (showSizes()) {
        sizeTimer_.setCallback([this, showSizes] {
            if (!showSizes()) sizeTimer_.stop();
        });
        sizeTimer_.start(150);
    }
    auto* bcmlHow = ui::label("Most BCML packages (.bnp) are merged with the game's own files, which Patchwork reads from the game "
                              "itself. Before installing your first one (and after a game update):\n" +
                                  AppState::bcmlSetupSteps(false) + "\nThen install BCML packages as usual. Copies of the files are kept on the SD card.",
                              14, ui::muted(), false);
    bcmlHow->setMargins(10, 4, 4, 4);
    content->addView(bcmlHow);

    section("Background add-ons");
    auto modules = mm::listSysmodules(platform::sdRoot());
    if (modules.empty()) {
        auto* none = ui::label("No add-ons (sysmodules) found in atmosphere/contents.", 14, ui::muted(), false);
        none->setMargins(10, 4, 0, 4);
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
            about->setMargins(10, 4, 4, 4);
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
    addonsNote->setMargins(10, 4, 0, 4);
    content->addView(addonsNote);

    section("Save backups · " + std::string(mm::gameInfo(AppState::get().game()).displayName));
    auto* backupsAbout = new brls::DetailCell();
    backupsAbout->setText("Backups folder");
    backupsAbout->setDetailText("SD:/switch/patchwork/save-backups");
    content->addView(backupsAbout);
    backupsAnchor_ = backupsAbout;
    backups_ = ui::vbox();
    content->addView(backups_);
    fillBackups();

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

    section("For testing");
    auto* bcml = new brls::DetailCell();
    bcml->setText("BCML merge test");
    bcml->setDetailText("Run");
    bcml->registerClickAction([this](brls::View*) {
        runBcmlTestNow();
        return true;
    });
    content->addView(bcml);
    bcmlRun_ = bcml;
    auto* bcmlNote = ui::label("Only needed when testing Patchwork: measures how fast this Switch merges BCML packages. Open "
                               "Patchwork by holding L and R while starting Breath of the Wild first. Installs nothing.",
                               14, ui::muted(), false);
    bcmlNote->setMargins(10, 4, 4, 4);
    content->addView(bcmlNote);
    bcmlReport_ = ui::label("", 14, ui::text(), false);
    bcmlReport_->setMargins(6, 4, 4, 4);
    content->addView(bcmlReport_);
    try {
        auto saved = AppState::get().dataDir() / "bcml-test" / "report.txt";
        std::error_code ec;
        if (mm::fs::exists(saved, ec)) bcmlReport_->setText("Last run:\n" + mm::readFile(saved));
    } catch (...) {
    }
    if (bcmlReport_->getFullText().empty()) bcmlReport_->setVisibility(brls::Visibility::GONE);
    bcmlTitle_ = new brls::DetailCell();
    bcmlTitle_->setText("Merged Linkle title screen");
    bcmlTitle_->registerClickAction([this](brls::View*) {
        try {
            if (bcmlTitleInGame()) {
                removeBcmlTitleFromGame();
                brls::Application::notify("Title screen test removed");
            } else {
                putBcmlTitleInGame();
                brls::Application::notify("Start Breath of the Wild normally to see the title screen");
            }
        } catch (const std::exception& e) {
            brls::Application::notify(e.what());
        }
        showBcmlTitleCell();
        return true;
    });
    content->addView(bcmlTitle_);
    showBcmlTitleCell();

    section("About");
    auto* about = ui::label(
        std::string(APP_TITLE) + " " + APP_VERSION + ", a mod manager for Breath of the Wild and Tears of the Kingdom." +
            "\nMods are downloaded from GameBanana and belong to their authors. Installed mods go into "
            "atmosphere/contents and are only active while running Atmosphère.\n"
            "BCML packages (.bnp) are merged with Breath of the Wild's own files right here; TKMM packages (.tkcl) need a PC.",
        16, ui::muted(), false);
    about->setMargins(4, 4, 0, 4);
    content->addView(about);
}

SettingsPage::~SettingsPage() { *alive_ = false; }

void SettingsPage::fillBackups() {
    if (ui::focusInside(backups_)) brls::Application::giveFocus(backupsAnchor_);
    backups_->clearViews();
    auto list = mm::listSaveBackups(AppState::get().saveBackupsDir(), AppState::get().game());
    if (list.empty()) {
        auto* none = ui::label("No backups yet.", 14, ui::muted(), false);
        none->setMargins(10, 4, 0, 4);
        backups_->addView(none);
        return;
    }
    for (const auto& b : list) {
        auto* cell = new brls::DetailCell();
        cell->setText(formatDateTime(b.time) + " · " + b.userName);
        cell->setDetailText("Restore");
        cell->registerClickAction([this, b](brls::View*) {
            auto game = std::string(mm::gameInfo(b.game).displayName);
            auto* dialog = new brls::Dialog("Put back " + b.userName + "'s " + game + " saves from " + formatDateTime(b.time) +
                                            "?\n\nThis replaces all of their " + game + " saves with the backup. Their current saves are "
                                            "backed up first.");
            dialog->addButton("Cancel", [] {});
            dialog->addButton("Restore", [this, b] { restoreBackup(b); });
            dialog->open();
            return true;
        });
        cell->registerAction("Delete", brls::BUTTON_X, [this, b](brls::View*) {
            auto* dialog = new brls::Dialog("Delete the backup from " + formatDateTime(b.time) + "? It can't be undone.");
            dialog->addButton("Cancel", [] {});
            dialog->addButton("Delete", [this, b, alive = alive_] {
                mm::removeTree(b.dir);
                brls::sync([this, alive] {
                    if (*alive) fillBackups();
                });
            });
            dialog->open();
            return true;
        });
        backups_->addView(cell);
        if (!b.reason.empty()) {
            auto* l = ui::label(b.reason, 14, ui::muted(), false);
            l->setMargins(10, 4, 4, 4);
            backups_->addView(l);
        }
    }
}

void SettingsPage::restoreBackup(const mm::SaveBackup& b) {
    auto& st = AppState::get();
    if (st.installBusy()) {
        brls::Application::notify("Wait for the current change to finish");
        return;
    }
    if (auto blocker = platform::saveBlocker(); !blocker.empty()) {
        auto* dialog = new brls::Dialog(blocker);
        dialog->addButton("OK", [] {});
        dialog->open();
        return;
    }
    st.setInstallBusy(true);
    auto busy = ui::busyBox("Putting back your saves…", "Writing the game's save data. Please wait.",
                            "Please wait until your saves are put back");
    scroll_->setVisibility(brls::Visibility::GONE);
    addView(busy.box);
    auto alive = alive_;
    // The dialog hands focus back when it closes, so take it on the next frame too.
    brls::Application::giveFocus(busy.box);
    brls::sync([alive, box = busy.box] {
        if (*alive) brls::Application::giveFocus(box);
    });
    auto backupsRoot = st.saveBackupsDir();
    st.runFileJob(
        [b, busy, alive, backupsRoot](mm::Library&) {
            std::string why;
            auto data = platform::SaveData::open(b.game, b.userId, why);
            if (!data) throw std::runtime_error(why);
            // Keep what's there now too, in case this was the wrong backup.
            auto now = static_cast<int64_t>(std::time(nullptr));
            mm::SaveBackup current{mm::newBackupDir(backupsRoot, b.game, b.userId, now), b.game, b.userId, b.userName,
                                   "Before restoring the backup from " + formatDateTime(b.time), now};
            try {
                mm::backupSave(data->root(), current.dir / "save");
                mm::writeBackupInfo(current);
            } catch (const std::exception& e) {
                mm::removeTree(current.dir);
                throw std::runtime_error(std::string("Couldn't back up the current saves, so nothing was changed: ") + e.what());
            }
            try {
                mm::restoreSave(b.dir / "save", data->root(), [&] { data->commit(); }, [busy, alive](size_t done, size_t total) mutable {
                    brls::sync([busy, alive, done, total]() mutable {
                        if (*alive) busy.update(done, total);
                    });
                });
            } catch (const std::exception& e) {
                throw std::runtime_error(std::string(e.what()) + ". The saves from just before are in the newest backup; try restoring again.");
            }
        },
        [this, alive, box = busy.box, name = b.userName](const std::string& error) {
            AppState::get().setInstallBusy(false);
            if (!*alive) return;
            scroll_->setVisibility(brls::Visibility::VISIBLE);
            brls::Application::giveFocus(backupsAnchor_);
            removeView(box);
            fillBackups();
            if (error.empty()) {
                brls::Application::notify(name + "'s saves are back");
            } else {
                auto* dialog = new brls::Dialog("Restoring failed: " + error);
                dialog->addButton("OK", [] {});
                dialog->open();
            }
        });
}

void SettingsPage::showBcmlTitleCell() {
    bool ready = bcmlTitleReady();
    if (!ready && ui::focusInside(bcmlTitle_)) brls::Application::giveFocus(bcmlRun_);
    bcmlTitle_->setVisibility(ready ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    bcmlTitle_->setDetailText(bcmlTitleInGame() ? "Remove from game" : "Put in game");
}

void SettingsPage::runBcmlTestNow() {
    auto& st = AppState::get();
    if (st.installBusy()) {
        brls::Application::notify("Wait for the current change to finish");
        return;
    }
    st.setInstallBusy(true);
    auto busy = ui::busyBox("Starting…", "Measuring. This takes a minute or two.", "Please wait until the test finishes");
    scroll_->setVisibility(brls::Visibility::GONE);
    addView(busy.box);
    brls::Application::giveFocus(busy.box);
    auto alive = alive_;
    auto report = std::make_shared<std::string>();
    st.runFileJob(
        [busy, alive, report](mm::Library&) {
            *report = runBcmlTest([busy, alive](const std::string& step, float f) {
                brls::sync([busy, alive, step, f] {
                    if (!*alive) return;
                    busy.status->setText(step);
                    busy.bar->setVisibility(brls::Visibility::VISIBLE);
                    busy.bar->setProgress(f);
                });
            });
        },
        [this, alive, box = busy.box, report](const std::string& error) {
            AppState::get().setInstallBusy(false);
            if (!*alive) return;
            scroll_->setVisibility(brls::Visibility::VISIBLE);
            brls::Application::giveFocus(bcmlRun_);
            removeView(box);
            bcmlReport_->setVisibility(brls::Visibility::VISIBLE);
            bcmlReport_->setText(error.empty() ? *report : "The test stopped: " + error);
            showBcmlTitleCell();
            brls::Application::notify(error.empty() ? "Test finished" : "Test stopped");
        });
}

void SettingsPage::revealNsfwSetting() {
    holdTimer_.stop();
    AppState::get().settings().nsfwSettingRevealed = true;
    for (auto* v : nsfwViews_) v->setVisibility(brls::Visibility::VISIBLE);
    brls::Application::giveFocus(nsfwViews_[1]);
}

}  // namespace app
