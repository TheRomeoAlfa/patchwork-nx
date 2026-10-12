#include "install_flow.hpp"

#include "app_state.hpp"
#include "compat.hpp"

namespace app {

using mm::gb::ModDetails;
using mm::gb::ModFile;

bool isUnsupportedPackage(const ModFile& f) {
    return mm::endsWithI(f.filename, ".bnp") || mm::endsWithI(f.filename, ".tkcl");
}

FileNote noteFor(const ModFile& f, mm::Game game, const std::string& installed) {
    if (mm::endsWithI(f.filename, ".bnp")) return {"Needs BCML on a PC", ui::warn()};
    if (mm::endsWithI(f.filename, ".tkcl")) return {"Needs TKMM on a PC", ui::warn()};
    auto named = mm::mentionedGameVersions(f.description + " " + f.version, game);
    switch (mm::checkCompat(installed, named)) {
        case mm::Compat::Likely: return {"Matches your game", ui::ok(), true};
        case mm::Compat::Mismatch: return {"For game " + named.front(), ui::warn()};
        case mm::Compat::Unknown: break;
    }
    return {formatSize(f.size), ui::muted()};
}

// The file we'd pick for the user: one made for their game version, else the newest
// one that isn't a PC-only package.
size_t recommendedFile(const ModDetails& d, mm::Game game, const std::string& installed) {
    size_t best = 0;
    int bestScore = -1;
    for (size_t i = 0; i < d.files.size(); ++i) {
        const auto& f = d.files[i];
        int score = 0;
        if (!isUnsupportedPackage(f)) score += 2;
        if (noteFor(f, game, installed).matches) score += 4;
        if (score > bestScore || (score == bestScore && f.dateAdded > d.files[best].dateAdded)) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

InstallFlow::InstallFlow(brls::Box* pane) : pane_(pane) {}

InstallFlow::~InstallFlow() {
    *flowAlive_ = false;
    if (cancel_) *cancel_ = true;
}

void InstallFlow::clear() {
    pane_->clearViews();
    workingLabel_ = nullptr;
    bar_ = nullptr;
}

void InstallFlow::leave() {
    if (busy_) return;
    active_ = false;
    cancel_.reset();
    if (onExit) onExit();
}

void InstallFlow::showLoading(const std::string& text) {
    active_ = true;
    setWorking(text);
}

void InstallFlow::setWorking(const std::string& status, float progress) {
    if (!workingLabel_) {
        clear();
        auto* box = ui::vbox();
        box->setGrow(1);
        box->setJustifyContent(brls::JustifyContent::CENTER);
        box->setAlignItems(brls::AlignItems::CENTER);
        box->addView(new brls::ProgressSpinner());
        workingLabel_ = ui::label(status, 20, ui::text());
        workingLabel_->setMarginTop(16);
        box->addView(workingLabel_);
        bar_ = new ui::ProgressBar();
        bar_->setWidth(420);
        bar_->setMarginTop(14);
        box->addView(bar_);
        auto* hint = ui::label("Press B to cancel", 15, ui::muted());
        hint->setMarginTop(12);
        box->addView(hint);
        // Focusable so B works and focus doesn't stay on the list behind.
        box->setFocusable(true);
        box->setHideHighlight(true);
        box->registerAction("Cancel", brls::BUTTON_B, [this](brls::View*) {
            if (cancel_) *cancel_ = true;
            if (workingLabel_) workingLabel_->setText("Cancelling…");
            if (!busy_) leave();
            return true;
        });
        pane_->addView(box);
        brls::Application::giveFocus(box);
    } else {
        workingLabel_->setText(status);
    }
    bar_->setVisibility(progress >= 0 ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
    if (progress >= 0) bar_->setProgress(progress);
}

void InstallFlow::showMessage(const std::string& title, const std::string& body, bool isError) {
    active_ = true;
    busy_ = false;
    clear();
    auto* box = ui::vbox();
    box->setGrow(1);
    box->setFocusable(true);
    box->setHideHighlight(true);
    box->addView(ui::label(std::string(isError ? ui::icon::warning : ui::icon::check) + "  " + title, 24, isError ? ui::warn() : ui::ok()));
    auto* b = ui::label(body, 17, ui::text(), false);
    b->setMarginTop(14);
    box->addView(b);
    box->registerAction("OK", brls::BUTTON_A, [this](brls::View*) {
        leave();
        return true;
    });
    box->registerAction("Back", brls::BUTTON_B, [this](brls::View*) {
        leave();
        return true;
    });
    pane_->addView(box);
    brls::Application::giveFocus(box);
}

void InstallFlow::showFilePicker(const ModDetails& d) {
    active_ = true;
    clear();
    auto& st = AppState::get();
    auto game = st.game();
    const auto& installed = st.gameVersion(game);

    if (d.files.empty()) {
        showMessage("Nothing to install", "This mod has no downloadable files on GameBanana.", true);
        return;
    }

    pane_->addView(ui::label("Install " + d.summary.name, 24, ui::text()));
    auto* sub = ui::label("Choose a file. The highlighted one is the best match for your game.", 16, ui::muted(), false);
    sub->setMargins(6, 0, 16, 0);
    pane_->addView(sub);

    auto* list = ui::vbox();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setContentView(list);
    list->registerAction("Back", brls::BUTTON_B, [this](brls::View*) {
        leave();
        return true;
    });
    pane_->addView(scroll);

    size_t rec = recommendedFile(d, game, installed);
    brls::View* recRow = nullptr;
    for (size_t i = 0; i < d.files.size(); ++i) {
        const auto file = d.files[i];
        auto note = noteFor(file, game, installed);
        bool downloaded = st.isDownloaded(file);
        auto* row = ui::focusRow();
        row->addView(ui::label(ui::icon::file, 24, ui::muted()));
        auto* text = ui::vbox();
        text->setGrow(1);
        text->setShrink(1);
        text->setMarginLeft(12);
        text->addView(ui::label(file.filename, 17, ui::text()));
        auto descText = file.description.empty() ? formatSize(file.size) : file.description + " · " + formatSize(file.size);
        auto* desc = ui::label(descText, 14, ui::muted());
        desc->setMarginTop(3);
        text->addView(desc);
        row->addView(text);
        auto* n = downloaded ? ui::label("Downloaded", 15, ui::accent())
                             : ui::label((note.matches ? std::string(ui::icon::check) + " " : "") + note.text, 15, note.color);
        n->setMarginLeft(10);
        row->addView(n);
        auto details = d;
        row->registerClickAction([this, details, file](brls::View*) {
            install(details, file);
            return true;
        });
        row->updateActionHint(brls::BUTTON_A, downloaded ? "Finish installing" : "Install");
        list->addView(row);
        if (i == rec) recRow = row;
    }
    brls::Application::giveFocus(recRow);
}

void InstallFlow::install(const ModDetails& d, const ModFile& file) {
    active_ = true;
    if (isUnsupportedPackage(file)) {
        bool bnp = mm::endsWithI(file.filename, ".bnp");
        showMessage(bnp ? "Needs BCML" : "Needs TKMM",
                    std::string("This file is a ") + (bnp ? "BCML" : "TKMM") +
                        " package. It has to be merged on a PC with that tool, then copied to the SD card. "
                        "Pick a different file if the mod offers a standalone version.",
                    true);
        return;
    }
    if (!file.avClean) {
        showMessage("File blocked", "GameBanana's virus scan flagged this file, so it won't be installed.", true);
        return;
    }

    auto& st = AppState::get();
    bool cached = st.isDownloaded(file);
    busy_ = true;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    setWorking(cached ? "Unpacking…" : "Downloading…", 0);

    PendingInstall pending{st.game(), d, file, st.archivePathFor(file)};
    auto work = st.workDirFor(file.id);
    auto prep = std::make_shared<mm::PreparedArchive>();
    auto cancel = cancel_;
    auto alive = flowAlive_;
    auto slug = mm::slugify(d.summary.name);
    auto update = [this, alive](std::string msg, float pct) {
        brls::sync([this, alive, msg, pct] {
            if (*alive && workingLabel_) setWorking(msg, pct);
        });
    };

    st.runFileJob(
        [=](mm::Library&) {
            auto& state = AppState::get();
            if (!cached) {
                state.savePending(pending);  // remember it even if the app closes mid-download
                int lastPct = -1;
                try {
                    state.http().download(file.downloadUrl, pending.archive, [&](uint64_t done, uint64_t total) {
                        if (!total && file.size > 0) total = static_cast<uint64_t>(file.size);
                        int pct = total ? static_cast<int>(done * 100 / total) : -1;
                        if (pct != lastPct) {
                            lastPct = pct;
                            update(pct >= 0 ? "Downloading… " + std::to_string(pct) + "%" : "Downloading… " + formatSize(done),
                                   pct >= 0 ? pct / 100.0f : -1);
                        }
                        return !*cancel;
                    });
                } catch (...) {
                    state.discardPending(file.id);  // nothing usable was kept
                    throw;
                }
            }
            update("Unpacking… 0%", 0);
            int lastPct = -1;
            try {
                *prep = mm::prepareArchive(pending.archive, work, pending.game, slug, [&](uint64_t done, uint64_t total, const std::string&) {
                    int pct = total ? static_cast<int>(done * 100 / total) : 0;
                    if (pct != lastPct) {
                        lastPct = pct;
                        update("Unpacking… " + std::to_string(pct) + "%", pct / 100.0f);
                    }
                    return !*cancel;
                });
            } catch (...) {
                mm::removeTree(work);  // the download stays for another try
                throw;
            }
        },
        [=, this](const std::string& error) {
            if (!*alive) return;
            busy_ = false;
            if (*cancel) {
                mm::removeTree(work);
                leave();
                return;
            }
            if (!error.empty()) {
                bool kept = AppState::get().isDownloaded(file);
                showMessage("Install failed", error + (kept ? "\n\nThe download was kept. Try again from here or from My mods; it won't download again." : ""), true);
                return;
            }
            const auto& layout = prep->layout;
            if (layout.kind != mm::PackageKind::Installable) {
                mm::removeTree(work);
                showMessage("Can't install this file", mm::packageKindMessage(layout.kind), true);
                return;
            }
            if (layout.variants.size() == 1)
                finish(d, file, prep, {0});
            else
                showVariantPicker(d, file, prep);
        });
}

void InstallFlow::showVariantPicker(const ModDetails& d, const ModFile& file, std::shared_ptr<mm::PreparedArchive> prep) {
    clear();
    pane_->addView(ui::label("Choose options", 24, ui::text()));
    auto* sub = ui::label("This file contains several versions or add-ons. Pick the ones you want, then press + to install.", 16, ui::muted(), false);
    sub->setMargins(6, 0, 16, 0);
    pane_->addView(sub);

    auto selected = std::make_shared<std::vector<bool>>(prep->layout.variants.size(), false);
    (*selected)[0] = true;
    auto* list = ui::vbox();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setContentView(list);
    pane_->addView(scroll);

    auto install = [this, d, file, prep, selected](brls::View*) {
        std::vector<size_t> chosen;
        for (size_t i = 0; i < selected->size(); ++i)
            if ((*selected)[i]) chosen.push_back(i);
        if (chosen.empty()) {
            brls::Application::notify("Pick at least one option");
            return true;
        }
        finish(d, file, prep, chosen);
        return true;
    };
    // Backing out keeps the download (it shows up under My mods) but drops the unpacked copy.
    list->registerAction("Back", brls::BUTTON_B, [this, prep](brls::View*) {
        mm::removeTree(prep->extractDir);
        leave();
        return true;
    });

    brls::View* first = nullptr;
    for (size_t i = 0; i < prep->layout.variants.size(); ++i) {
        const auto& v = prep->layout.variants[i];
        auto* row = ui::focusRow();
        auto* box = ui::label((*selected)[i] ? ui::icon::boxOn : ui::icon::boxOff, 26, ui::accent());
        row->addView(box);
        auto* name = ui::label(v.name, 18, ui::text());
        name->setMarginLeft(12);
        name->setGrow(1);
        name->setShrink(1);
        row->addView(name);
        row->addView(ui::label(std::to_string(v.files.size()) + " files", 15, ui::muted()));
        row->registerClickAction([selected, i, box](brls::View*) {
            (*selected)[i] = !(*selected)[i];
            box->setText((*selected)[i] ? ui::icon::boxOn : ui::icon::boxOff);
            return true;
        });
        row->updateActionHint(brls::BUTTON_A, "Select");
        row->registerAction("Install", brls::BUTTON_START, install);
        list->addView(row);
        if (!first) first = row;
    }
    auto* button = new brls::Button();
    button->setText("Install selected");
    button->setMarginTop(12);
    button->registerClickAction(install);
    list->addView(button);
    for (const auto& w : prep->layout.warnings) {
        auto* l = ui::label(std::string(ui::icon::warning) + " " + w, 14, ui::muted(), false);
        l->setMarginTop(6);
        list->addView(l);
    }
    brls::Application::giveFocus(first);
}

void InstallFlow::finish(const ModDetails& d, const ModFile& file, std::shared_ptr<mm::PreparedArchive> prep,
                         std::vector<size_t> variants) {
    auto& st = AppState::get();
    // Other files of the same mod already installed (an older version, usually).
    std::vector<std::string> others;
    std::string otherNames;
    auto newId = "gb" + std::to_string(d.summary.id) + "-" + std::to_string(file.id);
    for (const auto* m : st.mods(st.game()))
        if (m->gamebananaModId == d.summary.id && m->id != newId) {
            others.push_back(m->id);
            otherNames += (otherNames.empty() ? "" : ", ") + m->sourceFile;
        }

    auto run = [this, d, file, prep, variants, newId](std::vector<std::string> replace) {
        auto& st = AppState::get();
        mm::ModRecord r;
        r.id = newId;
        r.game = st.game();
        r.name = d.summary.name;
        r.author = d.summary.author;
        r.version = file.version.empty() ? d.version : file.version;
        r.sourceFile = file.filename;
        r.profileUrl = d.summary.profileUrl;
        r.gamebananaModId = d.summary.id;
        r.gamebananaFileId = file.id;
        r.gameVersionsNamed = mm::mentionedGameVersions(file.description + " " + d.descriptionHtml, r.game);
        for (auto i : variants) r.variants.push_back(prep->layout.variants.at(i).name);
        auto files = mm::mergeVariants(prep->layout, variants);
        busy_ = true;
        cancel_.reset();  // copying can't be cancelled half-way; the library rolls back on failure
        setWorking("Installing… 0 / " + std::to_string(files.size()) + " files", 0);
        auto alive = flowAlive_;
        st.runFileJob(
            [this, alive, r, files, replace, prep, fileId = file.id](mm::Library& lib) {
                for (const auto& id : replace) lib.remove(id);
                size_t lastStep = 0;
                try {
                lib.add(r, files, true, [&](size_t done, size_t total, const std::string&) {
                    size_t step = total ? done * 50 / total : 0;  // ~2% steps
                    if (step == lastStep) return;
                    lastStep = step;
                    auto msg = "Installing… " + std::to_string(done) + " / " + std::to_string(total) + " files";
                    float pct = total ? static_cast<float>(done) / total : 0;
                    brls::sync([this, alive, msg, pct] {
                        if (*alive && workingLabel_) setWorking(msg, pct);
                    });
                });
                } catch (...) {
                    mm::removeTree(prep->extractDir);  // the download itself stays for another try
                    throw;
                }
                // Installed: the download and the unpacked copy aren't needed any more.
                AppState::get().discardPending(fileId);
            },
            [this, alive, d](const std::string& error) {
                if (!*alive) return;
                busy_ = false;
                if (!error.empty()) {
                    showMessage("Install failed", error + "\n\nThe download was kept. Try again from here or from My mods; it won't download again.", true);
                    return;
                }
                showMessage("Installed", d.summary.name + " is installed and turned on. Start the game to try it.\n\nYou can turn it off or change its load order in My mods.", false);
            });
    };

    if (others.empty()) {
        run({});
        return;
    }
    auto* dialog = new brls::Dialog("You already have " + otherNames + " from this mod. Replace it with " + file.filename + "?");
    dialog->addButton("Keep both", [run] { run({}); });
    dialog->addButton("Replace", [run, others] { run(others); });
    dialog->setCancelable(false);
    dialog->open();
}

}  // namespace app
