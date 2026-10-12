#include "install_flow.hpp"

#include "app_state.hpp"
#include "compat.hpp"
#include "md5.hpp"
#include "platform.hpp"
#include "waikuteru.hpp"

#include <set>

namespace app {

using mm::gb::ModDetails;
using mm::gb::ModFile;

bool isUnsupportedPackage(const ModFile& f) {
    return mm::endsWithI(f.filename, ".bnp") || mm::endsWithI(f.filename, ".tkcl") || mm::wk::isWiiUOnly(f);
}

bool isLocalFile(const ModFile& f) { return f.downloadUrl.rfind("file://", 0) == 0; }

bool modUsesVersionWord(const ModDetails& d, mm::Game game) {
    std::vector<std::string> labels;
    for (const auto& f : d.files) labels.push_back(f.description + " " + f.version);
    return mm::versionWordIsModVersion(labels, game);
}

FileNote noteFor(const ModFile& f, mm::Game game, const std::string& installed, bool versionWordIsMod) {
    if (mm::endsWithI(f.filename, ".bnp")) return {"Needs BCML on a PC", ui::warn()};
    if (mm::endsWithI(f.filename, ".tkcl")) return {"Needs TKMM on a PC", ui::warn()};
    if (mm::wk::isWiiUOnly(f)) return {"Wii U only", ui::warn()};
    if (mm::saysAllVersions(f.description)) return {"Works on all game versions", ui::ok(), true};
    auto named = mm::mentionedGameVersions(f.description + " " + f.version, game, false, versionWordIsMod);
    switch (mm::checkCompat(installed, named)) {
        case mm::Compat::Likely: return {"Matches your game", ui::ok(), true};
        case mm::Compat::Mismatch: return {"For game " + named.front(), ui::warn()};
        case mm::Compat::Unknown: break;
    }
    return {formatFileSize(f.size), ui::muted()};
}

// The file we'd pick for the user: one made for their game version, else the newest
// one that isn't a PC-only package.
size_t recommendedFile(const ModDetails& d, mm::Game game, const std::string& installed) {
    size_t best = 0;
    int bestScore = -1;
    bool versionWordIsMod = modUsesVersionWord(d, game);
    auto stem = [](const std::string& name) { return mm::toLower(name.substr(0, name.rfind('.'))); };
    for (size_t i = 0; i < d.files.size(); ++i) {
        const auto& f = d.files[i];
        int score = 0;
        if (!isUnsupportedPackage(f)) score += 4;
        if (noteFor(f, game, installed, versionWordIsMod).matches) score += 8;
        // "dark_title_screen_base_defogger" is a variant of "base_defogger": prefer the base.
        bool isVariantOfAnother = false;
        for (size_t j = 0; j < d.files.size(); ++j) {
            auto own = stem(f.filename), other = stem(d.files[j].filename);
            if (j != i && other.size() >= 6 && own.size() > other.size() && own.find(other) != std::string::npos) isVariantOfAnother = true;
        }
        if (!isVariantOfAnother) score += 1;
        if (score > bestScore || (score == bestScore && f.dateAdded > d.files[best].dateAdded)) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

InstallFlow::InstallFlow(brls::Box* pane, ui::FocusSink* sink) : pane_(pane), sink_(sink) {}

InstallFlow::~InstallFlow() {
    *flowAlive_ = false;
    if (cancel_) *cancel_ = true;
    setBusy(false);
}

void InstallFlow::setBusy(bool busy) {
    if (busy == busy_) return;
    busy_ = busy;
    AppState::get().setInstallBusy(busy);
}

void InstallFlow::clear() {
    ui::parkFocusIfInside(pane_, sink_);
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
        bar_ = new ui::ProgressBar(360);
        bar_->setMarginTop(14);
        box->addView(bar_);
        auto* hint = ui::label("Press B to cancel", 15, ui::muted());
        hint->setMarginTop(12);
        box->addView(hint);
        // Focusable so B works and focus doesn't stay on the list behind.
        box->setFocusable(true);
        box->setHideHighlight(true);
        box->registerAction("Cancel", brls::BUTTON_B, [this](brls::View*) {
            if (!busy_) {
                leave();
            } else if (cancel_) {
                *cancel_ = true;
                if (workingLabel_) workingLabel_->setText("Cancelling…");
            } else {
                brls::Application::notify("Copying to the SD card can't be stopped part-way. It's almost done.");
            }
            return true;
        });
        // Stay here until the work finishes or is cancelled: leaving would drop the install.
        for (auto button : {brls::BUTTON_NAV_LEFT, brls::BUTTON_NAV_RIGHT, brls::BUTTON_NAV_UP, brls::BUTTON_NAV_DOWN})
            box->registerAction("", button, [](brls::View*) { return true; }, true);
        // + would quit the app mid-install.
        box->registerAction("", brls::BUTTON_START, [](brls::View*) {
            brls::Application::notify("Finish or cancel the install first");
            return true;
        }, true);
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
    setBusy(false);
    clear();
    auto* box = ui::vbox();
    box->setGrow(1);
    box->setFocusable(true);
    box->setHideHighlight(true);
    box->addView(ui::label(std::string(isError ? ui::icon::warning : ui::icon::check) + "  " + title, 24, isError ? ui::warn() : ui::ok()));
    auto* b = ui::label(body, 17, ui::text(), false);
    b->setMarginTop(14);
    box->addView(b);
    ui::tappable(box, ui::Tap::Act);
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
    bool versionWordIsMod = modUsesVersionWord(d, game);
    brls::View* recRow = nullptr;
    for (size_t i = 0; i < d.files.size(); ++i) {
        const auto file = d.files[i];
        auto note = noteFor(file, game, installed, versionWordIsMod);
        bool downloaded = st.isDownloaded(file);
        auto* row = ui::focusRow();
        ui::tappable(row);
        row->addView(ui::label(ui::icon::file, 24, ui::muted()));
        auto* text = ui::vbox();
        text->setGrow(1);
        text->setShrink(1);
        text->setMarginLeft(12);
        text->addView(ui::label(file.filename, 17, ui::text()));
        auto descText = file.description.empty() ? formatFileSize(file.size) : file.description + " · " + formatFileSize(file.size);
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
    if (mm::wk::isWiiUOnly(file)) {
        showMessage("Wii U only", "This file is for the Wii U version of the game. Pick the Switch file instead.", true);
        return;
    }
    if (!file.avClean) {
        showMessage("File blocked", "GameBanana's virus scan flagged this file, so it won't be installed.", true);
        return;
    }

    auto& st = AppState::get();
    bool cached = st.isDownloaded(file);
    // Rough need: the archive, its unpacked files, the library copy and the SD copy.
    uint64_t free = st.freeSpace();
    if (file.size > 0 && free > 0) {
        uint64_t need = static_cast<uint64_t>(file.size) * (cached ? 2 : 3);
        if (free < need) {
            showMessage("Not enough free space",
                        "Installing this needs about " + formatSize(need) + " free on the SD card, and there's " +
                            formatSize(free) + ". Remove some mods or other files and try again.",
                        true);
            return;
        }
    }
    setBusy(true);
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    setWorking(cached ? "Unpacking…" : "Downloading…", 0);

    PendingInstall pending{st.game(), d, file, st.archivePathFor(file)};
    auto work = st.workDirFor(file.id);
    auto prep = std::make_shared<mm::PreparedArchive>();
    auto cancel = cancel_;
    auto alive = flowAlive_;
    auto slug = mm::slugify(d.summary.name);
    auto update = [this, alive, cancel](std::string msg, float pct) {
        brls::sync([this, alive, cancel, msg, pct] {
            if (*alive && !*cancel && workingLabel_) setWorking(msg, pct);
        });
    };

    st.runFileJob(
        [=](mm::Library&) {
            auto& state = AppState::get();
            if (!cached && !isLocalFile(file)) {
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
                if (!file.md5.empty()) {
                    update("Checking download…", -1);
                    if (!mm::iequals(mm::md5File(pending.archive), file.md5)) {
                        state.discardPending(file.id);
                        throw std::runtime_error("The download was damaged (its checksum doesn't match GameBanana's). Try again.");
                    }
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
            setBusy(false);
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
                std::string message = mm::packageKindMessage(layout.kind);
                if (layout.kind == mm::PackageKind::Empty) {
                    // Maybe it's for the other game (common with links and SD card files).
                    auto other = pending.game == mm::Game::Totk ? mm::Game::Botw : mm::Game::Totk;
                    std::error_code ec;
                    if (mm::fs::exists(work, ec) && mm::detectLayout(work, other, slug).kind == mm::PackageKind::Installable)
                        message = "This looks like a mod for " + std::string(mm::gameInfo(other).displayName) +
                                  ". Switch game with ZL/ZR, then install it again.";
                }
                mm::removeTree(work);
                showMessage("Can't install this file", message, true);
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

    // Pre-select the option made for the user's game version if the options are per-version,
    // otherwise the biggest one (usually the main mod rather than an add-on).
    const auto& variants = prep->layout.variants;
    auto selected = std::make_shared<std::vector<bool>>(variants.size(), false);
    auto game = AppState::get().game();
    const auto& installed = AppState::get().gameVersion(game);
    bool picked = false;
    for (size_t i = 0; i < variants.size(); ++i)
        if (mm::checkCompat(installed, mm::mentionedGameVersions(variants[i].name, game)) == mm::Compat::Likely) {
            (*selected)[i] = true;
            picked = true;
        }
    if (!picked) {
        size_t biggest = 0;
        for (size_t i = 1; i < variants.size(); ++i)
            if (variants[i].files.size() > variants[biggest].files.size()) biggest = i;
        (*selected)[biggest] = true;
    }
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
    list->registerAction("Install", brls::BUTTON_START, install);

    // Option folders often share a long prefix ("BreathOfTheWild_NoFall…", "BreathOfTheWild_
    // Reduced…"): show only the part that differs.
    size_t common = 0;
    if (variants.size() > 1) {
        common = variants[0].name.size();
        for (const auto& v : variants) {
            size_t n = 0;
            while (n < common && n < v.name.size() && v.name[n] == variants[0].name[n]) ++n;
            common = n;
        }
        while (common > 0 && variants[0].name[common - 1] != '_' && variants[0].name[common - 1] != '/' && variants[0].name[common - 1] != ' ')
            --common;  // cut at a word boundary
    }
    brls::View* first = nullptr;
    for (size_t i = 0; i < variants.size(); ++i) {
        const auto& v = variants[i];
        auto* row = ui::focusRow();
        ui::tappable(row, ui::Tap::Act);
        auto* box = ui::label((*selected)[i] ? ui::icon::boxOn : ui::icon::boxOff, 26, ui::accent());
        row->addView(box);
        auto* name = ui::label(v.name.substr(common), 18, ui::text());
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
        list->addView(row);
        if ((*selected)[i] && !first) first = row;  // start on the pre-selected option
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
                         std::vector<size_t> variants, bool sizeTablesOk) {
    auto& st = AppState::get();
    if (!sizeTablesOk) {
        std::vector<std::string> targets;
        for (const auto& f : mm::mergeVariants(prep->layout, variants)) targets.push_back(f.target);
        const auto& installed = st.gameVersion(st.game());
        auto tables = mm::checkSizeTables(targets, st.game(), installed);
        if (tables.missing) {
            std::string list;
            for (size_t i = 0; i < tables.versions.size(); ++i)
                list += (i == 0 ? "" : i + 1 == tables.versions.size() ? " and " : ", ") + tables.versions[i];
            auto* dialog = new brls::Dialog(
                "This mod was made for " + std::string(mm::gameInfo(st.game()).displayName) + " " + list +
                ". It has no size table for " + installed + ", your version, so the game may crash or the mod may not load.\n\n"
                "Check its page for a version that supports " + installed + ".");
            dialog->addButton("Cancel", [this, prep] {
                mm::removeTree(prep->extractDir);  // the download stays (listed in My mods)
                leave();
            });
            dialog->addButton("Install anyway", [this, d, file, prep, variants] { finish(d, file, prep, variants, true); });
            dialog->setCancelable(false);
            dialog->open();
            return;
        }
    }
    bool fromGameBanana = d.summary.sourceKey.rfind("gb:", 0) == 0;
    auto newId = fromGameBanana ? "gb" + std::to_string(d.summary.id) + "-" + std::to_string(file.id)
                                : "src-" + mm::slugify(d.summary.sourceKey, 60) + "-" + std::to_string(file.id % 1000000007);
    // Other files of the same mod already installed (an older version, usually).
    std::vector<std::string> others;
    std::string otherNames;
    for (const auto* m : st.mods(st.game()))
        if (!d.summary.sourceKey.empty() && m->sourceKey == d.summary.sourceKey && m->id != newId) {
            others.push_back(m->id);
            otherNames += (otherNames.empty() ? "" : ", ") + m->sourceFile;
        }

    auto run = [this, d, file, prep, variants, newId, fromGameBanana](std::vector<std::string> replace) {
        auto& st = AppState::get();
        mm::ModRecord r;
        r.id = newId;
        r.game = st.game();
        r.name = d.summary.name;
        r.author = d.summary.author;
        r.version = file.version.empty() ? d.version : file.version;
        r.sourceFile = file.filename;
        r.profileUrl = d.summary.profileUrl;
        r.sourceKey = d.summary.sourceKey;
        if (fromGameBanana) {
            r.gamebananaModId = d.summary.id;
            r.gamebananaFileId = file.id;
        }
        // File labels are specific; the free-form description only counts next to a game word.
        r.gameVersionsNamed = mm::mentionedGameVersions(file.description, r.game, false, modUsesVersionWord(d, r.game));
        if (r.gameVersionsNamed.empty() && !mm::saysAllVersions(file.description))
            r.gameVersionsNamed = mm::mentionedGameVersions(d.descriptionHtml, r.game, true);
        for (auto i : variants) r.variants.push_back(prep->layout.variants.at(i).name);
        auto files = mm::mergeVariants(prep->layout, variants);
        setBusy(true);
        cancel_.reset();  // copying can't be cancelled half-way; the library rolls back on failure
        setWorking("Installing… 0 / " + std::to_string(files.size()) + " files", 0);
        auto alive = flowAlive_;
        st.runFileJob(
            [this, alive, r, files, replace, prep, fileId = file.id](mm::Library& lib) {
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
                // Only now that the new version is in place, take the old one(s) away.
                for (const auto& id : replace) lib.remove(id);
                // Installed: the download and the unpacked copy aren't needed any more.
                AppState::get().discardPending(fileId);
            },
            [this, alive, d](const std::string& error) {
                if (!*alive) {
                    // The page was closed meanwhile; still tell the user how it went.
                    brls::Application::notify(error.empty() ? d.summary.name + " installed" : "Install of " + d.summary.name + " failed");
                    return;
                }
                setBusy(false);
                if (!error.empty()) {
                    showMessage("Install failed", error + "\n\nThe download was kept. Try again from here or from My mods; it won't download again.", true);
                    return;
                }
                showMessage("Installed", d.summary.name + " is installed and turned on. Start the game to try it.\n\nYou can turn it off or change its load order in My mods.", false);
            });
    };

    // Before copying anything, say what this mod will cover up: other mods' files and files
    // already on the SD card (e.g. a randomizer). Nothing is lost either way; this is so the
    // user isn't surprised when one of them stops working.
    auto checkConflicts = [this, d, prep, variants, newId, run](std::vector<std::string> replace) {
        auto& st = AppState::get();
        std::set<std::string> targets;
        for (const auto& f : mm::mergeVariants(prep->layout, variants)) targets.insert(mm::toLower(f.target));
        std::set<std::string> owned;
        std::vector<std::string> clashing;
        for (const auto* m : st.mods(st.game())) {
            bool replaced = std::find(replace.begin(), replace.end(), m->id) != replace.end();
            for (const auto& t : m->files) {
                owned.insert(mm::toLower(t));
                if (m->enabled && !replaced && m->id != newId && targets.count(mm::toLower(t)) &&
                    std::find(clashing.begin(), clashing.end(), m->name) == clashing.end())
                    clashing.push_back(m->name);
            }
        }
        size_t onCard = 0;
        std::error_code ec;
        for (const auto& t : targets)
            if (!owned.count(t) && mm::fs::exists(platform::sdRoot() / mm::fs::path(t), ec)) ++onCard;
        if (clashing.empty() && !onCard) {
            run(replace);
            return;
        }
        std::string text = "Before you install " + d.summary.name + ":";
        if (!clashing.empty()) {
            std::string names;
            for (const auto& n : clashing) names += (names.empty() ? "" : ", ") + n;
            text += "\n\nIt changes files that " + names + " also change. The mod higher in My mods wins; new installs go to the top.";
        }
        if (onCard)
            text += "\n\nIt replaces " + std::to_string(onCard) + (onCard == 1 ? " file" : " files") +
                    " already on your SD card (from a randomizer or another tool). They come back when you turn this mod off or remove it.";
        auto* dialog = new brls::Dialog(text);
        dialog->addButton("Install", [run, replace] { run(replace); });
        dialog->addButton("Cancel", [this, prep] {
            mm::removeTree(prep->extractDir);  // the download stays (listed in My mods)
            leave();
        });
        dialog->setCancelable(false);
        dialog->open();
    };

    if (others.empty()) {
        checkConflicts({});
        return;
    }
    auto* dialog = new brls::Dialog("You already have " + otherNames + " from this mod. Replace it with " + file.filename + "?");
    dialog->addButton("Replace", [checkConflicts, others] { checkConflicts(others); });  // the usual case: an update
    dialog->addButton("Keep both", [checkConflicts] { checkConflicts({}); });
    dialog->setCancelable(false);
    dialog->open();
}

}  // namespace app
