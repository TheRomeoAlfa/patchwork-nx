#include "my_mods_page.hpp"

#include <ctime>

#include "app_state.hpp"
#include "compat.hpp"
#include "install_flow.hpp"
#include "ui.hpp"

namespace app {

MyModsPage::MyModsPage() : brls::Box(brls::Axis::ROW) {
    setGrow(1);

    auto* left = ui::vbox();
    left->setWidth(500);
    left->setShrink(0);
    left->setPadding(20, 14, 0, 16);
    left->setLineRight(1);
    left->setLineColor(ui::line());
    auto* header = ui::hbox();
    header->setMargins(0, 28, 10, 6);  // right: lines up with the rows
    header->addView(ui::label("Load order", 22, ui::text()));
    header->addView(ui::grow());
    count_ = ui::label("top of the list wins", 15, ui::muted());
    header->addView(count_);
    left->addView(header);

    list_ = ui::vbox();
    list_->setPaddingRight(28);  // keeps rows clear of the scroll bar
    scroll_ = new brls::ScrollingFrame();
    scroll_->setGrow(1);
    scroll_->setContentView(list_);
    left->addView(scroll_);
    addView(left);

    pane_ = ui::vbox();
    pane_->setGrow(1);
    pane_->setPadding(24, 30, 20, 30);
    pane_->setBackgroundColor(ui::panelBg());
    addView(pane_);

    sink_ = new ui::FocusSink();
    addView(sink_);
    flow_ = std::make_unique<InstallFlow>(pane_, sink_);
    flow_->onExit = [this] { rebuild(); };
    libSub_ = AppState::get().libraryChanged()->subscribe([this] {
        if (!busy_ && !flow_->active())
            rebuildWhenSafe();
        else
            dirty_ = true;  // catch up when the job or the install screen is done
    });
    rebuild();
}

MyModsPage::~MyModsPage() {
    *alive_ = false;
    AppState::get().libraryChanged()->unsubscribe(libSub_);
}

brls::View* MyModsPage::getDefaultFocus() { return list_->getDefaultFocus(); }

void MyModsPage::rebuild() {
    auto& st = AppState::get();
    auto mods = st.mods(st.game());
    dirty_ = false;
    bool hadFocus = ui::focusInside(this) || ui::focusParked();
    ui::parkFocusIfInside(this, sink_);
    list_->clearViews();
    clearPane();
    if (!st.snapshot().loaded) {
        auto* loading = ui::label("Loading your mods…", 16, ui::muted());
        loading->setMargins(10, 0, 0, 6);
        list_->addView(loading);
        return;  // rebuilt when the list arrives
    }

    // Mod files on the card that Patchwork didn't install (a randomizer, manual copies…).
    brls::View* unmanagedRow = nullptr;
    auto unmanaged = st.snapshot().unmanaged.count(st.game()) ? st.snapshot().unmanaged.at(st.game()) : LibrarySnapshot::Unmanaged{};
    auto addUnmanagedRow = [&] {
        if (!unmanaged.files) return;
        auto* row = ui::focusRow();
        ui::tappable(row);
        row->setMarginTop(10);
        row->addView(ui::label(ui::icon::folder, 24, ui::warn()));
        auto* text = ui::vbox();
        text->setMarginLeft(12);
        text->setGrow(1);
        text->setShrink(1);
        text->addView(ui::label("Files already on SD card", 18, ui::text()));
        auto* sub = ui::label(std::to_string(unmanaged.files) + " files · " + formatSize(unmanaged.bytes) + " · not managed yet", 14, ui::muted());
        sub->setMarginTop(3);
        text->addView(sub);
        row->addView(text);
        row->getFocusEvent()->subscribe([this, unmanaged](brls::View*) {
            if (paneFree()) showUnmanaged(unmanaged.files, unmanaged.bytes);
        });
        row->registerClickAction([this, unmanaged](brls::View*) {
            importUnmanaged(unmanaged.files, unmanaged.bytes);
            return true;
        });
        row->updateActionHint(brls::BUTTON_A, "Import");
        list_->addView(row);
        unmanagedRow = row;
    };

    // Downloads whose install failed or was abandoned.
    brls::View* firstPending = nullptr;
    auto addPendingRows = [&] {
        auto pending = st.pendingInstalls(st.game());
        if (pending.empty()) return;
        auto* head = ui::label("Downloaded, not installed", 16, ui::muted());
        head->setMargins(16, 0, 8, 6);
        list_->addView(head);
        for (const auto& p : pending) {
            auto* row = ui::focusRow();
        ui::tappable(row);
            row->addView(ui::label(ui::icon::file, 24, ui::accent()));
            auto* text = ui::vbox();
            text->setMarginLeft(12);
            text->setGrow(1);
            text->setShrink(1);
            text->addView(ui::label(p.details.summary.name, 18, ui::text()));
            auto* sub = ui::label(p.file.filename + " · " + formatFileSize(p.file.size), 14, ui::muted());
            sub->setMarginTop(3);
            text->addView(sub);
            row->addView(text);
            row->getFocusEvent()->subscribe([this, p](brls::View*) {
                if (paneFree()) showPending(p);
            });
            row->registerClickAction([this, p](brls::View*) {
                flow_->install(p.details, p.file);
                return true;
            });
            row->updateActionHint(brls::BUTTON_A, "Finish installing");
            row->registerAction("Delete download", brls::BUTTON_X, [this, p](brls::View*) {
                discardPending(p);
                return true;
            });
            list_->addView(row);
            if (!firstPending) firstPending = row;
        }
    };

    if (mods.empty() && (unmanaged.files || !st.pendingInstalls(st.game()).empty())) {
        addPendingRows();
        addUnmanagedRow();
        auto* target = firstPending ? firstPending : unmanagedRow;
        if (hadFocus && target) brls::Application::giveFocus(target);
        if (firstPending) {
            showPending(st.pendingInstalls(st.game()).front());
            return;
        }
        if (hadFocus) brls::Application::giveFocus(unmanagedRow);
        showUnmanaged(unmanaged.files, unmanaged.bytes);
        return;
    }
    if (mods.empty()) {
        auto* row = ui::focusRow();
        ui::tappable(row, ui::Tap::Act);
        row->addView(ui::label(ui::icon::browse, 24, ui::accent()));
        auto* l = ui::label("No mods yet. Open Browse to find some.", 17, ui::text());
        l->setMarginLeft(12);
        row->addView(l);
        list_->addView(row);
        pane_->addView(ui::label("Mods you install show up here. You can turn each one on or off, and choose which wins when two mods change the same file.", 18, ui::muted(), false));
        if (hadFocus) brls::Application::giveFocus(row);
        return;
    }

    std::set<std::string> conflicted;
    for (const auto& c : st.snapshot().conflicts.at(st.game()))
        for (const auto& id : c.modIds) conflicted.insert(id);
    for (const auto& o : st.snapshot().overlaps.at(st.game()))
        for (const auto& id : o.modIds) conflicted.insert(id);

    brls::View* focusTarget = nullptr;
    int n = 1;
    for (const auto* m : mods) {
        std::string id = m->id;
        auto* row = ui::focusRow();
        ui::tappable(row);
        row->addView(ui::label(std::to_string(n++), 15, ui::muted()));
        auto* name = ui::label(m->name, 18, m->enabled ? ui::text() : ui::muted());
        name->setMarginLeft(14);
        name->setGrow(1);
        name->setShrink(1);
        row->addView(name);
        if (conflicted.count(id) && m->enabled) row->addView(ui::label(ui::icon::warning, 22, ui::warn()));
        // On/off pill
        auto* pill = ui::hbox();
        pill->setJustifyContent(brls::JustifyContent::CENTER);
        pill->setWidth(64);
        pill->setHeight(30);
        pill->setCornerRadius(15);
        pill->setMarginLeft(10);
        pill->setBackgroundColor(m->enabled ? ui::accent() : ui::line());
        pill->addView(ui::label(m->enabled ? "On" : "Off", 15, m->enabled ? nvgRGB(0x10, 0x28, 0x2d) : ui::muted()));
        row->addView(pill);

        row->getFocusEvent()->subscribe([this, id](brls::View*) {
            focusedId_ = id;
            if (paneFree()) showDetails(id);
        });
        row->registerClickAction([this, id](brls::View*) {
            toggle(id);
            return true;
        });
        row->updateActionHint(brls::BUTTON_A, m->enabled ? "Turn off" : "Turn on");
        row->registerAction("Move up", brls::BUTTON_LB, [this, id](brls::View*) {
            move(id, +1);
            return true;
        });
        row->registerAction("Move down", brls::BUTTON_RB, [this, id](brls::View*) {
            move(id, -1);
            return true;
        });
        row->registerAction(mm::Library::isImported(*m) ? "Stop managing" : "Remove", brls::BUTTON_X, [this, id](brls::View*) {
            remove(id);
            return true;
        });
        list_->addView(row);
        if (id == focusedId_ || !focusTarget) focusTarget = row;
    }
    addPendingRows();
    addUnmanagedRow();
    if (hadFocus && focusTarget) brls::Application::giveFocus(focusTarget);
    showDetails(focusedId_.empty() ? mods.front()->id : focusedId_);
}

void MyModsPage::showUnmanaged(size_t files, uint64_t bytes) {
    clearPane();
    pane_->addView(ui::label("Files already on SD card", 26, ui::text()));
    auto* meta = ui::label(std::to_string(files) + " files · " + formatSize(bytes), 16, ui::muted());
    meta->setMargins(4, 0, 16, 0);
    pane_->addView(meta);
    pane_->addView(ui::label(
        "These mod files were put on the SD card some other way, for example by a randomizer, by copying them by hand, "
        "or by another mod manager. They're active in the game right now.\n\n"
        "Press A to import them as one entry in your list. You can then turn them off and on like any other mod. "
        "Importing keeps a copy in Patchwork's library, so it needs about " + formatSize(bytes) + " of free space.\n\n"
        "If you leave them alone, Patchwork still protects them: mods you install that replace one of these files "
        "put it back when you turn the mod off.",
        16, nvgRGB(0xc8, 0xc8, 0xc8), false));
}

void MyModsPage::rebuildWhenSafe() {
    // While a dialog is open it will hand focus back to one of our rows when it closes, so
    // those rows must survive: catch up once focus is back (onChildFocusGained).
    auto* focus = brls::Application::getCurrentFocus();
    if (focus && focus->getParentActivity() != getParentActivity())
        dirty_ = true;
    else
        rebuild();
}

void MyModsPage::clearPane() {
    ui::parkFocusIfInside(pane_, sink_);
    pane_->clearViews();
}

bool MyModsPage::paneFree() {
    if (!flow_->active()) return true;
    if (flow_->busy()) return false;
    // Moving back into the list abandons an install that's waiting for a choice.
    flow_->onExit = nullptr;
    flow_ = std::make_unique<InstallFlow>(pane_, sink_);
    flow_->onExit = [this] { rebuild(); };
    if (dirty_ && !busy_) {
        // Something was installed meanwhile: show it in the list (next frame, focus is moving).
        dirty_ = false;
        auto alive = alive_;
        brls::sync([this, alive] {
            if (*alive) rebuild();
        });
    }
    return true;
}

void MyModsPage::onChildFocusGained(brls::View* directChild, brls::View* focusedView) {
    brls::Box::onChildFocusGained(directChild, focusedView);
    // Catch up on changes made while a dialog had focus, once it's safe (next frame).
    if (dirty_ && !busy_ && !flow_->active()) {
        dirty_ = false;
        auto alive = alive_;
        brls::sync([this, alive] {
            if (*alive) rebuild();
        });
    }
}

void MyModsPage::showPending(const PendingInstall& p) {
    clearPane();
    pane_->addView(ui::label(p.details.summary.name, 26, ui::text()));
    auto* meta = ui::label(p.file.filename + " · " + formatFileSize(p.file.size), 16, ui::muted());
    meta->setMargins(4, 0, 16, 0);
    pane_->addView(meta);
    auto& st = AppState::get();
    bool bcmlSetup = mm::endsWithI(p.file.filename, ".bnp") && st.botwAccess() != AppState::GameAccess::Ready;
    if (bcmlSetup) {
        // A BCML package waiting for the one-time setup, most likely.
        pane_->addView(ui::label("A BCML package, waiting for Patchwork to read Breath of the Wild's files:\n" +
                                     AppState::bcmlSetupSteps(false) +
                                     "\n4. Come back here and press A to finish installing.\n\nX deletes the download instead.",
                                 16, nvgRGB(0xc8, 0xc8, 0xc8), false));
        return;
    }
    pane_->addView(ui::label(
        "This file was downloaded but isn't installed yet. The install may have failed, been cancelled, "
        "or the app closed part-way.\n\nPress A to finish installing it (no need to download again), "
        "or X to delete the download and free the space.",
        16, nvgRGB(0xc8, 0xc8, 0xc8), false));
}

void MyModsPage::discardPending(const PendingInstall& p) {
    auto* dialog = new brls::Dialog("Delete the download of " + p.details.summary.name + " (" +
                                    formatFileSize(p.file.size) + ")?");
    dialog->addButton("Cancel", [] {});
    dialog->addButton("Delete", [this, fileId = p.file.id] {
        runJob("Deleting…", [fileId](mm::Library&, const auto&) { AppState::get().discardPending(fileId); });
    });
    dialog->open();
}

void MyModsPage::importUnmanaged(size_t files, uint64_t bytes) {
    auto& st = AppState::get();
    uint64_t free = st.freeSpace();
    if (free && bytes + (64ull << 20) > free) {
        auto* d = new brls::Dialog("Not enough free space to import " + formatSize(bytes) + " of files (" + formatSize(free) + " free).");
        d->addButton("OK", [] {});
        d->open();
        return;
    }
    auto* dialog = new brls::Dialog("Import " + std::to_string(files) + " files (" + formatSize(bytes) +
                                    ") so you can turn them on and off? A copy is kept in Patchwork's library.");
    dialog->addButton("Cancel", [] {});
    dialog->addButton("Import", [this] {
        auto game = AppState::get().game();
        auto name = std::string("Existing files (") + std::string(mm::gameInfo(game).displayName) + ")";
        runJob("Importing files…", [game, name](mm::Library& lib, const auto& p) { lib.importUnmanaged(game, name, p); });
    });
    dialog->open();
}

void MyModsPage::showDetails(const std::string& id) {
    auto& st = AppState::get();
    const auto* m = st.findMod(id);
    clearPane();
    if (!m) return;

    pane_->addView(ui::label(m->name, 26, ui::text()));
    bool bcml = !m->bcmlLogs.empty();
    std::string meta = (m->author.empty() ? "" : "by " + m->author + " · ") +
                       (bcml ? std::string("BCML package") : std::to_string(m->files.size()) + " files") + " · " + (m->enabled ? "on" : "off");
    auto* metaL = ui::label(meta, 16, ui::muted());
    metaL->setMargins(4, 0, 16, 0);
    pane_->addView(metaL);

    auto addNote = [this](const std::string& text, NVGcolor color, NVGcolor bg) {
        auto* box = ui::hbox();
        box->setBackgroundColor(bg);
        box->setCornerRadius(4);
        box->setPadding(10, 12, 10, 12);
        box->setMarginBottom(8);
        auto* l = ui::label(text, 16, color, false);
        l->setGrow(1);
        l->setShrink(1);
        box->addView(l);
        pane_->addView(box);
    };
    auto nameOf = [&st](const std::string& modId) {
        auto* other = st.findMod(modId);
        return other ? other->name : modId;
    };

    // Conflicts: who this mod overrides and who overrides it.
    std::map<std::string, int> overrides, overriddenBy;
    for (const auto& c : st.snapshot().conflicts.at(m->game)) {
        auto pos = std::find(c.modIds.begin(), c.modIds.end(), id);
        if (pos == c.modIds.end()) continue;
        if (pos == c.modIds.begin())
            for (auto it = c.modIds.begin() + 1; it != c.modIds.end(); ++it) overrides[*it]++;
        else
            overriddenBy[c.modIds.front()]++;
    }
    auto files = [](int n) { return std::to_string(n) + (n == 1 ? " file" : " files"); };
    for (auto& [other, n] : overrides)
        addNote(std::string(ui::icon::warning) + "  Overrides " + files(n) + " from " + nameOf(other) + ". Move " + nameOf(other) + " above it if you want that mod to win.", ui::warn(), ui::warnBg());
    for (auto& [other, n] : overriddenBy)
        addNote(std::string(ui::icon::warning) + "  " + files(n) + " replaced by " + nameOf(other) + ", which is higher in the list.", ui::warn(), ui::warnBg());
    for (const auto& o : st.snapshot().overlaps.at(m->game)) {
        if (std::find(o.modIds.begin(), o.modIds.end(), id) == o.modIds.end()) continue;
        std::string names;
        for (const auto& other : o.modIds)
            if (other != id) names += (names.empty() ? "" : ", ") + nameOf(other);
        addNote(std::string(ui::icon::warning) + "  Also patches the game code like " + names + ". Using both may crash the game.", ui::warn(), ui::warnBg());
    }

    if (bcml) {
        if (m->enabled && st.snapshot().bcmlWaiting)
            addNote(std::string(ui::icon::warning) + "  Not merged yet: Patchwork needs to read some of Breath of the Wild's own files first.\n" +
                        AppState::bcmlSetupSteps(false),
                    ui::warn(), ui::warnBg());
        else
            addNote(std::string(ui::icon::info) + "  Merged with the game's own files. Patchwork merges again whenever a BCML package is "
                                                  "turned on or off or moved in the list.",
                    ui::accent(), ui::accentDim());
    }

    const auto& installed = st.gameVersion(m->game);
    if (mm::checkCompat(installed, m->gameVersionsNamed) == mm::Compat::Mismatch) {
        std::string named;
        for (const auto& v : m->gameVersionsNamed) named += (named.empty() ? "" : ", ") + v;
        addNote(std::string(ui::icon::warning) + "  Made for game " + named + ". You have " + installed + ".", ui::warn(), ui::warnBg());
    }

    char date[32] = "";
    std::time_t t = static_cast<std::time_t>(m->installedAt);
    if (t) std::strftime(date, sizeof date, "%b %d, %Y", std::localtime(&t));
    std::string info = "Installed " + std::string(date);
    if (!m->sourceFile.empty()) info += "\nFile: " + m->sourceFile;
    if (!m->version.empty()) info += "\nMod version: " + m->version;
    if (m->variants.size() > 1) {
        info += "\nOptions:";
        for (const auto& v : m->variants) info += " " + v + ";";
        info.pop_back();
    }
    auto* infoL = ui::label(info, 16, ui::muted(), false);
    infoL->setMarginTop(8);
    pane_->addView(infoL);
}

void MyModsPage::runJob(const std::string& status, std::function<void(mm::Library&, const mm::ProgressFn&)> job) {
    if (busy_) return;
    busy_ = true;
    AppState::get().setInstallBusy(true);  // also blocks switching game
    clearPane();
    auto busy = ui::busyBox(status,
                            AppState::get().fileJobRunning() ? "Waiting for the current install to finish…" : "Copying files on the SD card. Please wait.",
                            "Please wait until the files are copied");
    auto* box = busy.box;
    pane_->addView(box);
    auto alive = alive_;
    // A dialog that started this job hands focus back when it closes, so take it on the next frame.
    brls::sync([this, alive, box] {
        if (*alive && busy_) brls::Application::giveFocus(box);
    });
    brls::Application::giveFocus(box);

    auto shown = std::make_shared<int>(-1);
    mm::ProgressFn progress = [this, alive, busy, shown, status](size_t done, size_t total, const std::string&) {
        if (!total) return;
        int step = static_cast<int>(done * 50 / total);  // ~2% steps
        if (step == *shown && done != total) return;
        *shown = step;
        brls::sync([this, alive, busy, done, total, status]() mutable {
            if (!*alive || !busy_) return;
            busy.status->setText(status);  // after a merge's progress
            busy.update(done, total);
        });
    };
    auto lastPct = std::make_shared<int>(-1);
    auto mergeProgress = [this, alive, busy, lastPct](float f) {
        int pct = static_cast<int>(f * 100);
        if (pct == *lastPct) return;
        *lastPct = pct;
        brls::sync([this, alive, busy, pct, f] {
            if (!*alive || !busy_) return;
            busy.status->setText("Merging BCML packages with the game's files… " + std::to_string(pct) + "%");
            busy.bar->setVisibility(brls::Visibility::VISIBLE);
            busy.bar->setProgress(f);
            busy.count->setText("");
        });
    };
    AppState::get().runFileJob([job = std::move(job), progress, mergeProgress](mm::Library& lib) {
        lib.setMergeProgress(mergeProgress);
        struct StopProgress {
            mm::Library& lib;
            ~StopProgress() { lib.setMergeProgress({}); }
        } stopProgress{lib};
        job(lib, progress);
    },
                               [this, alive](const std::string& error) {
        AppState::get().setInstallBusy(false);
        if (!*alive) return;
        busy_ = false;
        if (!error.empty() && error.find("hasn't read yet") != std::string::npos) {
            // A BCML merge needs game files: explain the setup rather than show the raw error.
            auto* dialog = new brls::Dialog("This change merges BCML packages with Breath of the Wild's own files, which Patchwork "
                                            "hasn't read yet:\n\n" + AppState::bcmlSetupSteps(false) +
                                            "\n\nThen make the change again.");
            dialog->addButton("OK", [] {});
            dialog->open();
        } else if (!error.empty()) {
            brls::Application::notify("Something went wrong: " + error);
        }
        rebuildWhenSafe();
    });
}

void MyModsPage::toggle(const std::string& id) {
    const auto* m = AppState::get().findMod(id);
    if (!m) return;
    bool enable = !m->enabled;
    runJob(enable ? "Turning on " + m->name + "…" : "Turning off " + m->name + "…",
           [id, enable](mm::Library& lib, const auto& p) { lib.setEnabled(id, enable, p); });
}

void MyModsPage::move(const std::string& id, int delta) {
    runJob("Updating load order…", [id, delta](mm::Library& lib, const auto& p) { lib.move(id, delta, p); });
}

void MyModsPage::remove(const std::string& id) {
    const auto* m = AppState::get().findMod(id);
    if (!m) return;
    if (mm::Library::isImported(*m)) {
        // Never delete files Patchwork didn't download: they may not exist anywhere else.
        if (!m->enabled) {
            brls::Application::notify("Turn it on first so its files are back on the SD card");
            return;
        }
        auto* dialog = new brls::Dialog("Stop managing " + m->name + "? The files stay on your SD card and keep working; Patchwork just stops tracking them.");
        dialog->addButton("Cancel", [] {});
        dialog->addButton("Stop managing", [this, id] {
            if (focusedId_ == id) focusedId_.clear();
            runJob("Updating…", [id](mm::Library& lib, const auto&) { lib.forget(id); });
        });
        dialog->open();
        return;
    }
    auto* dialog = new brls::Dialog("Remove " + m->name + "? Its files are deleted from the SD card. You can download it again from Browse.");
    dialog->addButton("Cancel", [] {});
    dialog->addButton("Remove", [this, id, name = m->name] {
        if (focusedId_ == id) focusedId_.clear();
        runJob("Removing " + name + "…", [id](mm::Library& lib, const auto& p) { lib.remove(id, p); });
    });
    dialog->open();
}

}  // namespace app
