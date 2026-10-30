#include "install_flow.hpp"

#include "app_state.hpp"
#include "compat.hpp"
#include "md5.hpp"
#include "bcml.hpp"
#include "platform.hpp"
#include "waikuteru.hpp"

#include <algorithm>
#include <ctime>
#include <set>

namespace app {

using mm::gb::ModDetails;
using mm::gb::ModFile;

// A package that has to be merged on a PC (TKMM, or a BCML package that needs merging).
bool needsPcTool(const ModFile& f) {
    return mm::endsWithI(f.filename, ".tkcl") || (mm::endsWithI(f.filename, ".bnp") && f.bcml == ModFile::Bcml::NeedsPc);
}

bool isUnsupportedPackage(const ModFile& f) { return needsPcTool(f) || mm::wk::isWiiUOnly(f); }

bool isLocalFile(const ModFile& f) { return f.downloadUrl.rfind("file://", 0) == 0; }

bool modUsesVersionWord(const ModDetails& d, mm::Game game) {
    std::vector<std::string> labels;
    for (const auto& f : d.files) labels.push_back(f.description + " " + f.version);
    return mm::versionWordIsModVersion(labels, game);
}

FileNote noteFor(const ModFile& f, mm::Game game, const std::string& installed, bool versionWordIsMod) {
    if (mm::endsWithI(f.filename, ".bnp")) {
        if (f.bcml == ModFile::Bcml::NeedsPc) return {"Needs BCML on a PC", ui::warn()};
        if (f.bcml == ModFile::Bcml::Installable)
            return AppState::get().bcmlSetupNeeded() ? FileNote{"BCML package · one-time setup", ui::accent()}
                                                     : FileNote{"BCML package · installs here", ui::ok()};
        return {"BCML package", ui::muted()};
    }
    if (mm::endsWithI(f.filename, ".tkcl") || (game == mm::Game::Totk && mm::saysTkmmOnly(f.description + " " + f.filename)))
        return {"Needs TKMM on a PC", ui::warn()};
    if (mm::wk::isWiiUOnly(f)) return {"Wii U only", ui::warn()};
    if (mm::isRestoreOriginalsFile(f.filename, f.description))
        return {"Original files · not needed", ui::muted()};  // Patchwork undoes mods itself
    if (mm::saysAllVersions(f.description)) return {"Works on all game versions", ui::ok(), true};
    // Not f.version: that's the uploader's version of the mod ("1.0"), easily mistaken for a game version.
    auto named = mm::mentionedGameVersions(f.description, game, false, versionWordIsMod);
    switch (mm::checkCompat(installed, named)) {
        case mm::Compat::Likely: return {"Matches your game", ui::ok(), true};
        case mm::Compat::Mismatch: return {"For game " + named.front(), ui::warn()};
        case mm::Compat::Unknown: break;
    }
    return {formatFileSize(f.size), ui::muted()};
}

// The file we'd pick for the user: one made for their game version, else the newest
// one that isn't a PC-only package, in the console's language if files differ by language.
size_t recommendedFile(const ModDetails& d, mm::Game game, const std::string& installed) {
    size_t best = 0;
    int bestScore = -1000;
    bool versionWordIsMod = modUsesVersionWord(d, game);
    auto language = mm::toLower(platform::gameTextLanguage().substr(2));  // "USen" -> "en"
    for (size_t i = 0; i < d.files.size(); ++i) {
        const auto& f = d.files[i];
        int score = 0;
        // Labeled for TKMM: an archive with a TKMM package inside, needing a PC too.
        bool pcOnly = isUnsupportedPackage(f) || (game == mm::Game::Totk && mm::saysTkmmOnly(f.description + " " + f.filename));
        if (!pcOnly) score += 4;
        score += 2 * mm::languageMatch(f.description + " " + f.filename, language);
        if (mm::isRestoreOriginalsFile(f.filename, f.description)) score -= 20;  // only if it's all there is
        if (noteFor(f, game, installed, versionWordIsMod).matches) score += 8;
        // The author's own advice ("Standalone Version (Not Recommended)").
        auto desc = mm::toLower(f.description + " " + f.filename);
        if (desc.find("not recommend") != std::string::npos || desc.find("not-recommend") != std::string::npos) score -= 2;
        // "dark_title_screen_base_defogger" is a variant of "base_defogger": prefer the base.
        bool isVariantOfAnother = false;
        for (size_t j = 0; j < d.files.size(); ++j)
            if (j != i && mm::isAddOnFileOf(f.filename, d.files[j].filename)) isVariantOfAnother = true;
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
    hint_ = nullptr;
}

void InstallFlow::leave() {
    if (busy_) return;
    active_ = false;
    reinstall_.reset();
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
        workingLabel_ = ui::label(status, 20, ui::text(), false);
        workingLabel_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        workingLabel_->setMarginTop(16);
        box->addView(workingLabel_);
        bar_ = new ui::ProgressBar(360);
        bar_->setMarginTop(14);
        box->addView(bar_);
        hint_ = ui::label("", 15, ui::muted(), false);
        hint_->setMarginTop(12);
        hint_->setMaxWidth(420);
        hint_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        box->addView(hint_);
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
    // Downloading and unpacking can be stopped; installing and merging can't.
    hint_->setText(busy_ && !cancel_ ? "Please wait. Don't close Patchwork or turn off the Switch until this finishes."
                                     : "Press B to cancel");
    bar_->setVisibility(progress >= 0 ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
    if (progress >= 0) bar_->setProgress(progress);
}

namespace {
// Marks an install error that's really the BCML setup step (see showBcmlSetup).
const std::string kSetupMarker = "\x01" "bcml-setup:";
}  // namespace

// What a game file is, in words ("the game's text").
static std::string describeGameFile(const std::string& path) {
    if (path.find("Bootup_") != std::string::npos) return "the game's text";
    if (path.find("Bootup.pack") != std::string::npos) return "the game's data (Bootup.pack)";
    if (path.find("AocMainField") != std::string::npos || path.find("01007EF00011F001") != std::string::npos) return "the DLC's maps";
    if (path.find("Map/") != std::string::npos || path.find("TitleBG") != std::string::npos) return "the game's maps";
    if (path.find("ActorInfo") != std::string::npos) return "the game's actor list";
    if (path.find("size table") != std::string::npos) return "the game's size table";
    auto slash = path.find_last_of('/');
    return "the game's " + (slash == std::string::npos ? path : path.substr(slash + 1));
}

void InstallFlow::showBcmlSetup(bool fromInbox, const std::string& missing) {
    using A = AppState::GameAccess;
    auto& st = AppState::get();
    auto access = st.botwAccess();
    bool setUpBefore = st.keptGameFiles() > 0;
    std::string title = "One-time setup for BCML packages";
    std::string body;
    if (access == A::ModsOn) {
        body = "Patchwork was opened from Breath of the Wild, but L wasn't held, so mods were on and it couldn't read the "
               "game's own files. Try again holding both buttons:\n\n";
    } else if (setUpBefore) {
        title = "More game files needed";
        body = "This package needs " + describeGameFile(missing) +
               ", which Patchwork hasn't read from Breath of the Wild yet. To read it:\n\n";
    } else {
        body = "This mod is a BCML package. Patchwork merges it with Breath of the Wild's own files, which it reads from "
               "the game itself. To let it:\n\n";
    }
    body += AppState::bcmlSetupSteps(!fromInbox);
    if (fromInbox) body += "\n4. Install it again from Add mods.";
    body += "\n\nPatchwork keeps copies of the game files on the SD card, so this is only needed again after a game "
            "update, or if another package needs other files.";
    if (!fromInbox) body += " The download is kept; it won't download again.";
    showMessage(title, body, false);
}

void InstallFlow::showMessage(const std::string& title, const std::string& body, bool isError) {
    active_ = true;
    setBusy(false);
    clear();
    auto* box = ui::vbox();
    box->setGrow(1);
    box->setFocusable(true);
    box->setHideHighlight(true);
    bool setup = title.rfind("One-time setup", 0) == 0 || title == "More game files needed";
    box->addView(ui::label(std::string(isError ? ui::icon::warning : setup ? ui::icon::info : ui::icon::check) + "  " + title, 24,
                           isError ? ui::warn() : setup ? ui::accent() : ui::ok()));
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

// The library id a file of a mod is installed under.
static std::string modIdFor(const ModDetails& d, const ModFile& file) {
    if (d.summary.sourceKey.rfind("gb:", 0) == 0) return "gb" + std::to_string(d.summary.id) + "-" + std::to_string(file.id);
    return "src-" + mm::slugify(d.summary.sourceKey, 60) + "-" + std::to_string(file.id % 1000000007);
}

void InstallFlow::changeOptions(const mm::ModRecord& m, const ModDetails& d, const ModFile& file) {
    reinstall_ = m;
    install(d, file);
}

void InstallFlow::showFilePicker(const ModDetails& d) {
    reinstall_.reset();
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
    list->setPaddingRight(28);  // keeps rows clear of the scroll bar
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
        const auto* installedMod = st.findMod(modIdFor(d, file));
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
        auto* n = installedMod ? ui::label(std::string(ui::icon::check) + " Installed", 15, ui::ok())
                  : downloaded ? ui::label("Downloaded", 15, ui::accent())
                               : ui::label((note.matches ? std::string(ui::icon::check) + " " : "") + note.text, 15, note.color);
        n->setMarginLeft(10);
        row->addView(n);
        auto details = d;
        std::string installedState = installedMod ? (installedMod->enabled ? "on" : "off") : "";
        row->registerClickAction([this, details, file, installedState](brls::View*) {
            if (installedState.empty()) {
                install(details, file);
                return true;
            }
            auto* dialog = new brls::Dialog(file.filename + " is already installed (turned " + installedState +
                                            " in My mods). Download and install it again?");
            dialog->addButton("Cancel", [] {});
            dialog->addButton("Install again", [this, details, file] { install(details, file); });
            dialog->open();
            return true;
        });
        row->updateActionHint(brls::BUTTON_A, installedMod ? "Install again" : downloaded ? "Finish installing" : "Install");
        list->addView(row);
        if (i == rec) recRow = row;
    }
    brls::Application::giveFocus(recRow);
}

void InstallFlow::install(const ModDetails& d, const ModFile& file) {
    active_ = true;
    if (needsPcTool(file)) {
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
                bool kept = !isLocalFile(file) && AppState::get().isDownloaded(file);
                showMessage("Install failed", error + (kept ? "\n\nThe download was kept. Try again from here or from My mods; it won't download again." : ""), true);
                return;
            }
            const auto& layout = prep->layout;
            if (layout.kind != mm::PackageKind::Installable) {
                auto saves = mm::findSaves(prep->extractDir, pending.game, &prep->fileTimes);
                if (!saves.empty()) {
                    showSaveList(d, file, prep, std::move(saves));
                    return;
                }
            }
            if (layout.kind != mm::PackageKind::Installable) {
                std::string message = layout.problem.empty() ? mm::packageKindMessage(layout.kind) : layout.problem;
                if (layout.kind == mm::PackageKind::Empty) {
                    // Maybe it's for the other game (common with links and SD card files).
                    auto other = pending.game == mm::Game::Totk ? mm::Game::Botw : mm::Game::Totk;
                    std::error_code ec;
                    if (mm::fs::exists(work, ec) && mm::detectLayout(work, other, slug).kind == mm::PackageKind::Installable)
                        message = "This looks like a mod for " + std::string(mm::gameInfo(other).displayName) +
                                  ". Switch game with ZL/ZR, then install it again.";
                    else if (mm::fs::exists(work, ec) && !mm::findSaves(work, other).empty())
                        message = "This is a save file for " + std::string(mm::gameInfo(other).displayName) +
                                  ". Switch game with ZL/ZR, then install it again.";
                }
                mm::removeTree(work);
                showMessage("Can't install this file", message, true);
                return;
            }
            // Nothing to choose (one version, or a BCML package without options): install it.
            std::vector<size_t> always;
            for (size_t i = 0; i < layout.variants.size(); ++i)
                if (layout.variants[i].always) always.push_back(i);
            if (layout.variants.size() == 1)
                finish(d, file, prep, {0});
            else if (always.size() == layout.variants.size())
                finish(d, file, prep, always);
            else
                showVariantPicker(d, file, prep);
        });
}

void InstallFlow::showVariantPicker(const ModDetails& d, const ModFile& file, std::shared_ptr<mm::PreparedArchive> prep) {
    clear();
    pane_->addView(ui::label(reinstall_ ? "Change options" : "Choose options", 24, ui::text()));
    auto* sub = ui::label(reinstall_ ? "Your current choices are ticked. Change them, then press + to install the mod again with them."
                                     : "This file contains several versions or add-ons. Pick the ones you want, then press + to install.",
                          16, ui::muted(), false);
    sub->setMargins(6, 0, 16, 0);
    pane_->addView(sub);

    // BCML packages that list their options start on BCML's defaults. Otherwise pre-select the
    // option made for the user's game version if the options are per-version, else the biggest
    // one (usually the main mod rather than an add-on).
    const auto& variants = prep->layout.variants;
    auto selected = std::make_shared<std::vector<bool>>(variants.size(), false);
    auto game = AppState::get().game();
    const auto& installed = AppState::get().gameVersion(game);
    bool listed = std::any_of(variants.begin(), variants.end(), [](const mm::Variant& v) { return !v.label.empty(); });
    bool picked = listed;
    for (size_t i = 0; listed && i < variants.size(); ++i) (*selected)[i] = variants[i].always || variants[i].preselected;
    if (reinstall_) {
        // What's installed now. Older records only list the shown names (folder or label).
        const auto& was = reinstall_->chosen.empty() ? reinstall_->variants : reinstall_->chosen;
        auto had = [&](const mm::Variant& v) {
            return std::find(was.begin(), was.end(), v.name) != was.end() ||
                   (!v.label.empty() && std::find(was.begin(), was.end(), v.label) != was.end());
        };
        for (size_t i = 0; i < variants.size(); ++i) (*selected)[i] = variants[i].always || had(variants[i]);
        // A "pick one" group none of whose options is installed (the mod changed) starts on its default.
        for (size_t i = 0; i < variants.size(); ++i) {
            if (variants[i].group < 0 || !variants[i].preselected) continue;
            bool any = false;
            for (size_t k = 0; k < variants.size(); ++k) any |= variants[k].group == variants[i].group && (*selected)[k];
            if (!any) (*selected)[i] = true;
        }
        picked = true;
    }
    for (size_t i = 0; !listed && i < variants.size(); ++i)
        if (mm::checkCompat(installed, mm::mentionedGameVersions(variants[i].name, game)) == mm::Compat::Likely) {
            (*selected)[i] = true;
            picked = true;
        }
    std::string closestNote;
    if (!picked) {
        // Per-version options but none for this version: the closest older one.
        std::vector<std::string> names;
        for (const auto& v : variants) names.push_back(v.name);
        auto closest = mm::closestVersionOption(names, game, installed);
        if (closest < variants.size()) {
            (*selected)[closest] = true;
            picked = true;
            auto named = mm::mentionedGameVersions(variants[closest].name, game);
            closestNote = "There's no option for " + installed + ", your version. The one for " +
                          (named.empty() ? std::string("the closest version") : named.back()) + " is selected; it may or may not work.";
        }
    }
    if (!picked) {
        size_t biggest = 0;
        for (size_t i = 1; i < variants.size(); ++i)
            if (variants[i].files.size() > variants[biggest].files.size()) biggest = i;
        (*selected)[biggest] = true;
    }
    if (!closestNote.empty()) {
        auto* note = ui::label(std::string(ui::icon::warning) + "  " + closestNote, 15, ui::warn(), false);
        note->setMargins(0, 0, 12, 0);
        pane_->addView(note);
    }
    auto* list = ui::vbox();
    list->setPaddingRight(28);  // keeps rows clear of the scroll bar
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setContentView(list);
    pane_->addView(scroll);

    auto install = [this, d, file, prep, selected](brls::View*) {
        const auto& variants = prep->layout.variants;
        std::vector<size_t> chosen;
        for (size_t i = 0; i < selected->size(); ++i)
            if ((*selected)[i]) chosen.push_back(i);
        if (chosen.empty()) {
            brls::Application::notify("Pick at least one option");
            return true;
        }
        for (size_t i = 0; i < variants.size(); ++i) {
            if (!variants[i].required) continue;
            bool any = false;
            for (size_t k = 0; k < variants.size(); ++k) any |= variants[k].group == variants[i].group && (*selected)[k];
            if (!any) {
                brls::Application::notify("Pick one: " + (variants[i].groupName.empty() ? variants[i].label : variants[i].groupName));
                return true;
            }
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
    if (variants.size() > 1 && !listed) {
        common = variants[0].name.size();
        for (const auto& v : variants) {
            size_t n = 0;
            while (n < common && n < v.name.size() && v.name[n] == variants[0].name[n]) ++n;
            common = n;
        }
        auto boundary = [&](size_t n) {
            while (n > 0 && variants[0].name[n - 1] != '_' && variants[0].name[n - 1] != '/' && variants[0].name[n - 1] != ' ') --n;
            return n;
        };
        common = boundary(common);  // cut at a word boundary
        // "…(Game Version 1.2.0)" would leave just "1.2.0)": keep the word before a bare number.
        if (common > 0 && std::isdigit(static_cast<unsigned char>(variants[0].name[common])))
            common = boundary(common - 1);
    }
    // Drop a ")" whose "(" was cut off with the shared prefix.
    auto shown = [&](const std::string& name) {
        auto text = name.substr(common);
        if (!text.empty() && text.back() == ')' && std::count(text.begin(), text.end(), '(') < std::count(text.begin(), text.end(), ')'))
            text.pop_back();
        return text;
    };
    // Options of a "pick one" group show as radio buttons; picking one clears the others.
    auto boxes = std::make_shared<std::vector<brls::Label*>>(variants.size(), nullptr);
    auto icon = [](const mm::Variant& v, bool on) {
        return v.group >= 0 ? (on ? ui::icon::radioOn : ui::icon::radioOff) : (on ? ui::icon::boxOn : ui::icon::boxOff);
    };
    brls::View* first = nullptr;
    brls::View* top = nullptr;
    int lastGroup = -1;
    for (size_t i = 0; i < variants.size(); ++i) {
        const auto& v = variants[i];
        if (v.always) continue;  // the package itself goes in regardless
        if (v.group >= 0 && v.group != lastGroup && !v.groupName.empty()) {
            auto* heading = ui::label(v.groupName, 16, ui::muted(), false);
            heading->setMargins(10, 0, 4, 0);
            list->addView(heading);
        }
        lastGroup = v.group;
        auto* row = ui::focusRow();
        ui::tappable(row, ui::Tap::Act);
        auto* box = ui::label(icon(v, (*selected)[i]), 26, ui::accent());
        (*boxes)[i] = box;
        row->addView(box);
        auto* name = ui::label(v.label.empty() ? shown(v.name) : v.label, 18, ui::text());
        name->setMarginLeft(12);
        name->setGrow(1);
        name->setShrink(1);
        row->addView(name);
        // BCML options often change the game's files rather than bringing their own.
        if (!v.files.empty() || v.bcmlLogs.empty())
            row->addView(ui::label(std::to_string(v.files.size()) + " files", 15, ui::muted()));
        row->registerClickAction([prep, selected, boxes, icon, i](brls::View*) {
            const auto& variants = prep->layout.variants;
            const auto& v = variants[i];
            if (v.group < 0) {
                (*selected)[i] = !(*selected)[i];
            } else if ((*selected)[i]) {
                if (!v.required) (*selected)[i] = false;
            } else {
                for (size_t k = 0; k < variants.size(); ++k)
                    if (variants[k].group == v.group) (*selected)[k] = false;
                (*selected)[i] = true;
            }
            for (size_t k = 0; k < variants.size(); ++k)
                if ((*boxes)[k]) (*boxes)[k]->setText(icon(variants[k], (*selected)[k]));
            return true;
        });
        row->updateActionHint(brls::BUTTON_A, "Select");
        list->addView(row);
        if ((*selected)[i] && !first) first = row;  // start on the pre-selected option
        if (!top) top = row;
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
    brls::Application::giveFocus(first ? first : top);
}

void InstallFlow::finish(const ModDetails& d, const ModFile& file, std::shared_ptr<mm::PreparedArchive> prep,
                         std::vector<size_t> variants, bool sizeTablesOk) {
    auto& st = AppState::get();
    // Merging can rebuild archives of 100 MB and more, which the Album's memory may not hold.
    // Said once per session.
    static bool memoryWarned = false;
    bool merges = std::any_of(variants.begin(), variants.end(), [&](size_t i) { return !prep->layout.variants.at(i).bcmlLogs.empty(); });
    if (merges && !memoryWarned && !platform::fullMemory()) {
        memoryWarned = true;
        auto* dialog = new brls::Dialog(
            "Patchwork was opened from the Album, which leaves it only a little memory. BCML packages that change big game "
            "files (like Link's moves or the title screen) may fail with \"out of memory\".\n\n"
            "To be safe: press + to close Patchwork, then hold R while starting a game and open Patchwork from the Homebrew Menu.");
        dialog->addButton("Cancel", [this, prep] {
            mm::removeTree(prep->extractDir);  // the download stays (listed in My mods)
            leave();
        });
        dialog->addButton("Install anyway", [this, d, file, prep, variants, sizeTablesOk] { finish(d, file, prep, variants, sizeTablesOk); });
        dialog->setCancelable(false);
        dialog->open();
        return;
    }
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
    auto reinstall = reinstall_;
    auto newId = reinstall ? reinstall->id : modIdFor(d, file);
    // Other files of the same mod already installed (an older version, usually).
    std::vector<std::string> others;
    std::string otherNames;
    for (const auto* m : st.mods(st.game()))
        if (!reinstall && !d.summary.sourceKey.empty() && m->sourceKey == d.summary.sourceKey && m->id != newId) {
            others.push_back(m->id);
            otherNames += (otherNames.empty() ? "" : ", ") + m->sourceFile;
        }

    auto run = [this, d, file, prep, variants, newId, fromGameBanana, reinstall](std::vector<std::string> replace) {
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
        if (reinstall) {  // from its kept copy there's no mod page to read these from
            if (r.gameVersionsNamed.empty()) r.gameVersionsNamed = reinstall->gameVersionsNamed;
            if (r.version.empty()) r.version = reinstall->version;
        }
        for (auto i : variants) {  // as the picker named them; the package itself goes without saying
            const auto& v = prep->layout.variants.at(i);
            if (!v.always) r.variants.push_back(v.label.empty() ? v.name : v.label);
            r.chosen.push_back(v.name);
        }
        const auto& all = prep->layout.variants;
        r.hasOptions = all.size() > 1 && std::any_of(all.begin(), all.end(), [](const mm::Variant& v) { return !v.always; });
        bool enable = reinstall ? reinstall->enabled : true;
        r.resourceSizes = mm::mergeVariantSizes(prep->layout, variants);
        auto files = mm::mergeVariants(prep->layout, variants);
        std::vector<mm::fs::path> logDirs;  // BCML packages that need merging
        auto logs = mm::variantLogFiles(prep->layout, variants, r.bcmlLogs);
        // Keep the archive to change the options later without downloading it again: packages
        // and small archives (big ones can be downloaded again, or put back in the inbox).
        auto archive = st.archivePathFor(file);
        std::error_code ec;
        auto archiveSize = mm::fs::file_size(archive, ec);
        if (r.hasOptions && !ec && (mm::endsWithI(file.filename, ".bnp") || archiveSize <= (64u << 20))) {
            r.package = "package/" + mm::slugify(file.filename, 80);
            logs.push_back({archive, r.package});
        }
        for (auto i : variants)
            if (!prep->layout.variants.at(i).bcmlLogs.empty()) logDirs.push_back(prep->layout.variants.at(i).bcmlLogs);
        setBusy(true);
        cancel_.reset();  // copying can't be cancelled half-way; the library rolls back on failure
        setWorking("Installing… 0 / " + std::to_string(files.size()) + " files", 0);
        auto alive = flowAlive_;
        st.runFileJob(
            [this, alive, r, files, logs, logDirs, replace, prep, enable, fileId = file.id](mm::Library& lib) {
                size_t lastStep = 0;
                try {
                if (!lib.canMergeSizes(r)) throw std::runtime_error(kSetupMarker + "size table");
                if (!logDirs.empty()) {
                    auto missing = lib.missingGameFiles(logDirs);
                    if (!missing.empty()) throw std::runtime_error(kSetupMarker + missing.front());
                }
                auto lastPct = std::make_shared<int>(-1);
                auto lastWhat = std::make_shared<std::string>();
                lib.setMergeProgress([this, alive, lastPct, lastWhat](float f, const std::string& what) {
                    int pct = static_cast<int>(f * 100);
                    if (pct == *lastPct && what == *lastWhat) return;
                    *lastPct = pct;
                    *lastWhat = what;
                    auto text = "Merging with the game's files… " + std::to_string(pct) + "%\n" + what;
                    brls::sync([this, alive, text, f] {
                        if (*alive && workingLabel_) setWorking(text, f);
                    });
                });
                struct StopProgress {
                    mm::Library& lib;
                    ~StopProgress() { lib.setMergeProgress({}); }
                } stopProgress{lib};
                lib.add(r, files, enable, [&](size_t done, size_t total, const std::string&) {
                    size_t step = total ? done * 50 / total : 0;  // ~2% steps
                    if (step == lastStep) return;
                    lastStep = step;
                    auto msg = "Installing… " + std::to_string(done) + " / " + std::to_string(total) + " files";
                    float pct = total ? static_cast<float>(done) / total : 0;
                    brls::sync([this, alive, msg, pct] {
                        if (*alive && workingLabel_) setWorking(msg, pct);
                    });
                }, logs);
                } catch (const mm::BcmlMergeError& e) {
                    mm::removeTree(prep->extractDir);  // the download itself stays for another try
                    // "needs the game's <path>, which Patchwork hasn't read yet": pass on the path.
                    std::string what = e.what();
                    auto from = what.find("needs the game's "), to = what.find(", which Patchwork hasn't read yet");
                    if (from != std::string::npos && to != std::string::npos && to > from)
                        throw std::runtime_error(kSetupMarker + what.substr(from + 17, to - from - 17));
                    throw;
                } catch (...) {
                    mm::removeTree(prep->extractDir);  // the download itself stays for another try
                    throw;
                }
                // Only now that the new version is in place, take the old one(s) away.
                for (const auto& id : replace) lib.remove(id);
                // Installed: the download and the unpacked copy aren't needed any more.
                AppState::get().discardPending(fileId);
            },
            [this, alive, d, file, enable, reinstall](const std::string& error) {
                if (!*alive) {
                    // The page was closed meanwhile; still tell the user how it went.
                    brls::Application::notify(error.empty() ? d.summary.name + " installed" : "Install of " + d.summary.name + " failed");
                    return;
                }
                setBusy(false);
                if (error.rfind(kSetupMarker, 0) == 0) {
                    // The DLC is installed but couldn't be opened: holding L and R won't help.
                    auto dlcError = platform::lastDlcError();
                    if (error.find(mm::bcml::kDlc) != std::string::npos && !dlcError.empty() && platform::botwDlcInstalled() == 1) {
                        showMessage("Couldn't read the DLC", "This package changes the map, so it's merged with the Expansion Pass's own map files. " + dlcError +
                                    "\n\nPlease report this error. The download was kept.", true);
                        return;
                    }
                    showBcmlSetup(isLocalFile(file), error.substr(kSetupMarker.size()));
                    return;
                }
                if (!error.empty()) {
                    showMessage("Install failed", error + (isLocalFile(file) ? std::string() : "\n\nThe download was kept. Try again from here or from My mods; it won't download again."), true);
                    return;
                }
                if (reinstall)
                    showMessage("Options changed", reinstall->name + " is installed again with the options you picked" +
                                                       (enable ? ". Start the game to try it." : ". It's still turned off."), false);
                else
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
        // Files a BCML merge combines: packages' copies, and the copy of the mod highest in the
        // list that isn't a package (the merge starts from it), all end up in one file.
        std::vector<mm::fs::path> logDirs;
        for (auto i : variants)
            if (!prep->layout.variants.at(i).bcmlLogs.empty()) logDirs.push_back(prep->layout.variants.at(i).bcmlLogs);
        std::set<std::string> merging;
        if (!logDirs.empty())
            for (const auto& p : mm::bcml::gameFilesNeeded({mm::bcml::Package{newId, {}, logDirs}}))
                merging.insert(mm::toLower("atmosphere/contents/" + p));
        const auto& merged = st.snapshot().bcmlMerged;
        std::set<std::string> owned(merged.begin(), merged.end());
        std::vector<std::string> clashing;
        for (const auto* m : st.mods(st.game())) {
            bool replaced = std::find(replace.begin(), replace.end(), m->id) != replace.end();
            bool bcml = !m->bcmlLogs.empty();
            for (const auto& t : m->files) {
                auto key = mm::toLower(t);
                owned.insert(key);
                if (!m->enabled || replaced || m->id == newId || !targets.count(key)) continue;
                if (merging.count(key) || (bcml && merged.count(key))) continue;  // merged together
                if (std::find(clashing.begin(), clashing.end(), m->name) == clashing.end()) clashing.push_back(m->name);
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

// ---------------------------------------------------------------- save files

struct InstallFlow::SaveChoice {
    std::shared_ptr<mm::PreparedArchive> prep;
    std::vector<mm::SaveCandidate> saves;
    size_t index = 0;
    std::vector<platform::UserProfile> users;
    size_t user = 0;
    std::string slot;  // "" = pick a default for the user
    bool photos = false;
    const mm::SaveCandidate& save() const { return saves.at(index); }
};

namespace {

std::string saveName(const mm::SaveCandidate& c, const ModDetails& d) { return c.name.empty() ? d.summary.name : c.name; }

std::string slotLabel(mm::Game game, const std::string& folder) {
    const auto* s = mm::findSaveSlot(game, folder);
    return s ? s->label : folder;
}

}  // namespace

void InstallFlow::showSaveList(const ModDetails& d, const ModFile& file, std::shared_ptr<mm::PreparedArchive> prep,
                               std::vector<mm::SaveCandidate> saves) {
    auto choice = std::make_shared<SaveChoice>();
    choice->prep = prep;
    choice->saves = std::move(saves);
    if (choice->saves.size() == 1) {
        showSaveOptions(d, file, choice);
        return;
    }
    active_ = true;
    clear();
    auto game = AppState::get().game();
    pane_->addView(ui::label("Choose a save", 24, ui::text()));
    bool wholeSave = true;  // every slot of one player's save, rather than a pack of named saves
    for (const auto& s : choice->saves)
        if (s.photosRoot != choice->saves[0].photosRoot) wholeSave = false;
    // In a whole save, start on the latest slot; a pack's saves are in the uploader's order.
    choice->index = wholeSave ? mm::newestSave(choice->saves) : 0;
    auto* sub = ui::label(wholeSave ? "This download is a whole save with several slots, from different points of one playthrough. "
                                      "The latest one is highlighted."
                                    : "This download holds " + std::to_string(choice->saves.size()) + " saves. Pick the one to install.",
                          16, ui::muted(), false);
    sub->setMargins(6, 0, 16, 0);
    pane_->addView(sub);
    auto* list = ui::vbox();
    list->setPaddingRight(28);  // keeps rows clear of the scroll bar
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setContentView(list);
    list->registerAction("Back", brls::BUTTON_B, [this, prep](brls::View*) {
        mm::removeTree(prep->extractDir);  // the download stays (listed in My mods)
        leave();
        return true;
    });
    pane_->addView(scroll);
    brls::View* first = nullptr;
    for (size_t i = 0; i < choice->saves.size(); ++i) {
        const auto& s = choice->saves[i];
        auto* row = ui::focusRow();
        ui::tappable(row);
        row->addView(ui::label(ui::icon::file, 24, ui::muted()));
        auto* text = ui::vbox();
        text->setGrow(1);
        text->setShrink(1);
        text->setMarginLeft(12);
        text->addView(ui::label(wholeSave && !s.slot.empty() ? slotLabel(game, s.slot) : saveName(s, d), 17, ui::text()));
        std::string detail;
        if (!wholeSave && !s.slot.empty()) detail = "From " + slotLabel(game, s.slot);
        if (s.savedTime) detail += (detail.empty() ? "Saved " : " · saved ") + formatDateTime(s.savedTime, false);
        if (s.masterMode()) detail += (detail.empty() ? "" : " · ") + std::string("Master Mode");
        if (!detail.empty()) {
            auto* l = ui::label(detail, 14, ui::muted());
            l->setMarginTop(3);
            text->addView(l);
        }
        row->addView(text);
        if (wholeSave && i == choice->index && choice->saves[i].savedTime) {
            auto* tag = ui::label("Latest", 15, ui::accent());
            tag->setMarginLeft(10);
            row->addView(tag);
        }
        row->registerClickAction([this, d, file, choice, i](brls::View*) {
            choice->index = i;
            choice->slot.clear();
            showSaveOptions(d, file, choice);
            return true;
        });
        row->updateActionHint(brls::BUTTON_A, "Choose");
        list->addView(row);
        if (i == choice->index) first = row;
    }
    brls::Application::giveFocus(first);
}

void InstallFlow::showSaveOptions(const ModDetails& d, const ModFile& file, std::shared_ptr<SaveChoice> choice) {
    active_ = true;
    auto& st = AppState::get();
    auto game = st.game();
    auto gameName = std::string(mm::gameInfo(game).displayName);
    auto prep = choice->prep;
    auto back = [this, d, file, choice, prep] {
        if (choice->saves.size() > 1) {
            showSaveList(d, file, prep, choice->saves);
        } else {
            mm::removeTree(prep->extractDir);  // the download stays (listed in My mods)
            leave();
        }
    };

    if (auto blocker = platform::saveBlocker(); !blocker.empty()) {
        showMessage("Close the game first", blocker, true);
        return;
    }
    if (choice->users.empty()) {
        std::string why;
        choice->users = platform::userProfiles(why);
        if (choice->users.empty()) {
            showMessage("Can't install this save", why, true);
            return;
        }
    }
    const auto& save = choice->save();
    const auto& user = choice->users.at(choice->user);

    // Which slots hold a save for this user. Slots are kept to the save's mode: a Master
    // Mode save only works in a Master Mode slot.
    std::string why;
    auto data = platform::SaveData::open(game, user.id, why);
    std::vector<mm::SaveSlot> slots;
    std::vector<bool> inUse;
    for (const auto& s : mm::saveSlots(game))
        if (s.masterMode == save.masterMode()) {
            slots.push_back(s);
            inUse.push_back(data && mm::slotInUse(data->root(), s.folder));
        }
    data.reset();
    if (choice->slot.empty() || !mm::findSaveSlot(game, choice->slot)) {
        // Where it came from if that's free, else the first free slot, else where it came from.
        size_t pick = slots.size();
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i].folder == save.slot && !inUse[i]) pick = i;
        for (size_t i = 0; i < slots.size() && pick == slots.size(); ++i)
            if (!inUse[i]) pick = i;
        for (size_t i = 0; i < slots.size() && pick == slots.size(); ++i)
            if (slots[i].folder == save.slot) pick = i;
        choice->slot = slots[pick == slots.size() ? 0 : pick].folder;
    }
    size_t slotIndex = 0;
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].folder == choice->slot) slotIndex = i;

    clear();
    pane_->addView(ui::label("Install save: " + saveName(save, d), 24, ui::text()));
    auto* sub = ui::label("This is a save file, not a mod: it replaces one of your save slots. Patchwork first backs up all of "
                          "this profile's " + gameName + " saves to the SD card, so you can put them back from Settings.",
                          16, ui::muted(), false);
    sub->setMargins(6, 0, 12, 0);
    pane_->addView(sub);
    if (!why.empty()) {
        auto* note = ui::label(std::string(ui::icon::warning) + "  " + why, 15, ui::warn(), false);
        note->setMargins(0, 0, 12, 0);
        pane_->addView(note);
    }
    auto* list = ui::vbox();
    list->setPaddingRight(28);  // keeps rows clear of the scroll bar
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setContentView(list);
    pane_->addView(scroll);
    list->registerAction("Back", brls::BUTTON_B, [back](brls::View*) {
        back();
        return true;
    });

    std::vector<std::string> names;
    for (const auto& u : choice->users) names.push_back(u.name);
    auto* profile = new brls::SelectorCell();
    profile->init("Profile", names, static_cast<int>(choice->user), [this, d, file, choice](int i) {
        if (i < 0 || static_cast<size_t>(i) == choice->user) return;
        choice->user = static_cast<size_t>(i);
        choice->slot.clear();
        // Redraw once the dropdown has closed and handed focus back.
        brls::sync([this, d, file, choice, alive = flowAlive_] {
            if (*alive) showSaveOptions(d, file, choice);
        });
    });
    list->addView(profile);

    std::vector<std::string> slotNames;
    for (size_t i = 0; i < slots.size(); ++i)
        slotNames.push_back(slots[i].label + (!why.empty() ? "" : inUse[i] ? " · has a save" : " · empty"));
    auto* slotCell = new brls::SelectorCell();
    slotCell->init("Save into", slotNames, static_cast<int>(slotIndex), [choice, slots](int i) {
        if (i >= 0 && static_cast<size_t>(i) < slots.size()) choice->slot = slots[i].folder;
    });
    list->addView(slotCell);
    if (save.masterMode()) {
        auto* l = ui::label("This is a Master Mode save, so it goes into a Master Mode slot.", 14, ui::muted(), false);
        l->setMargins(10, 4, 4, 4);
        list->addView(l);
    }

    if (!save.photoDirs.empty()) {
        auto* photos = new brls::BooleanCell();
        photos->init("Also copy the save's photos", choice->photos, [choice](bool on) { choice->photos = on; });
        list->addView(photos);
        auto* l = ui::label(game == mm::Game::Totk
                                ? "Off: your own photos stay. On: the save's album replaces yours, for every slot; without it, "
                                  "photos in this save's compendium may be blank."
                                : "Off: your own photos stay. On: the save's album and compendium pictures replace yours, for every "
                                  "slot; without them, this save's compendium may show blank pictures.",
                            14, ui::muted(), false);
        l->setMargins(10, 4, 4, 4);
        list->addView(l);
    }

    auto* button = new brls::Button();
    button->setText("Back up and install");
    button->setMarginTop(16);
    button->registerClickAction([this, d, file, choice, slots, inUse, user, why](brls::View*) {
        if (!why.empty()) {
            brls::Application::notify(why);
            return true;
        }
        size_t i = 0;
        while (i < slots.size() && slots[i].folder != choice->slot) ++i;
        if (i < slots.size() && inUse[i]) {
            auto* dialog = new brls::Dialog(slots[i].label + " already has a save in it for " + user.name +
                                            ". It will be replaced; the backup keeps a copy.");
            dialog->addButton("Cancel", [] {});
            dialog->addButton("Replace", [this, d, file, choice] { installSave(d, file, choice); });
            dialog->open();
        } else {
            installSave(d, file, choice);
        }
        return true;
    });
    list->addView(button);
    brls::Application::giveFocus(button);
}

void InstallFlow::installSave(const ModDetails& d, const ModFile& file, std::shared_ptr<SaveChoice> choice) {
    auto& st = AppState::get();
    auto game = st.game();
    auto user = choice->users.at(choice->user);
    auto slot = choice->slot;
    auto photos = choice->photos;
    auto save = choice->save();
    auto prep = choice->prep;
    auto label = slotLabel(game, slot);
    auto reason = "Before installing " + saveName(save, d) + " into " + label;
    setBusy(true);
    cancel_.reset();  // a save half-written can't be left behind; it's restored on failure instead
    setWorking("Backing up your saves…", 0);
    auto alive = flowAlive_;
    auto update = [this, alive](std::string msg, size_t done, size_t total) {
        float pct = total ? static_cast<float>(done) / total : 0;
        brls::sync([this, alive, msg, pct] {
            if (*alive && workingLabel_) setWorking(msg, pct);
        });
    };
    auto backupsRoot = st.saveBackupsDir();
    st.runFileJob(
        [=](mm::Library&) {
            if (auto blocker = platform::saveBlocker(); !blocker.empty()) throw std::runtime_error(blocker);
            std::string why;
            auto data = platform::SaveData::open(game, user.id, why);
            if (!data) throw std::runtime_error(why);
            mm::SaveBackup backup{mm::newBackupDir(backupsRoot, game, user.id, std::time(nullptr)), game, user.id, user.name, reason,
                                  static_cast<int64_t>(std::time(nullptr))};
            try {
                mm::backupSave(data->root(), backup.dir / "save", [&](size_t done, size_t total) {
                    update("Backing up your saves… " + std::to_string(done) + " / " + std::to_string(total), done, total);
                });
                mm::writeBackupInfo(backup);
            } catch (const std::exception& e) {
                mm::removeTree(backup.dir);
                throw std::runtime_error(std::string("Couldn't back up your saves, so nothing was changed: ") + e.what());
            }
            auto commit = [&] { data->commit(); };
            try {
                mm::writeSaveSlot(save, data->root(), slot, photos, commit, [&](size_t done, size_t total) {
                    update("Installing the save… " + std::to_string(done) + " / " + std::to_string(total), done, total);
                });
            } catch (const std::exception& e) {
                std::string first = e.what();
                try {
                    update("Something went wrong. Putting your saves back…", 0, 0);
                    mm::restoreSave(backup.dir / "save", data->root(), commit);
                } catch (const std::exception& e2) {
                    throw std::runtime_error("Installing the save failed (" + first + "), and so did putting your saves back (" +
                                             e2.what() + "). Restore the backup from Settings → Save backups.");
                }
                throw std::runtime_error("Installing the save failed, so your saves were put back as they were: " + first);
            }
            mm::removeTree(prep->extractDir);
            AppState::get().discardPending(file.id);
        },
        [this, alive, d, user, label, game, prep](const std::string& error) {
            if (!*alive) {
                brls::Application::notify(error.empty() ? "Save installed" : "Installing the save failed");
                return;
            }
            setBusy(false);
            if (!error.empty()) {
                mm::removeTree(prep->extractDir);  // the download stays (listed in My mods)
                showMessage("Save not installed", error, true);
                return;
            }
            showMessage("Save installed",
                        "The save is in " + label + " for " + user.name + ". Start " + std::string(mm::gameInfo(game).displayName) +
                            " with that profile and pick it from Load.\n\nYour previous saves were backed up. To put them back, "
                            "go to Settings → Save backups.",
                        false);
        });
}

}  // namespace app
