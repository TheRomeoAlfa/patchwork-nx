#include "browse_page.hpp"

#include <algorithm>

#include "app_state.hpp"
#include "compat.hpp"
#include "ui.hpp"

namespace app {

using mm::gb::ModDetails;
using mm::gb::ModFile;

// ------------------------------------------------------------------ list cell

class ModCell : public brls::RecyclerCell {
public:
    ModCell() {
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setFocusable(true);
        setPadding(8, 12, 8, 10);
        setCornerRadius(4);
        setHighlightCornerRadius(6);

        thumb = new brls::Image();
        thumb->setDimensions(96, 54);
        thumb->setCornerRadius(4);
        thumb->setBackgroundColor(ui::thumbBg());
        thumb->setScalingType(brls::ImageScalingType::FILL);
        addView(thumb);

        // Fixed width: labels measured while the pane was re-laying out kept stale ellipses.
        auto* text = ui::vbox();
        text->setWidth(300);
        text->setMarginLeft(14);
        name = ui::label("", 18, ui::text());
        meta = ui::label("", 14, ui::muted());
        meta->setMarginTop(4);
        text->addView(name);
        text->addView(meta);
        addView(text);

        status = ui::label("", 22, ui::ok());
        status->setWidth(28);
        status->setMarginLeft(8);
        addView(status);
    }

    void onFocusGained() override {
        brls::RecyclerCell::onFocusGained();
        if (onFocus) onFocus(row);
    }

    void prepareForReuse() override {
        thumb->clear();
        modId = 0;
    }

    brls::Image* thumb;
    brls::Label* name;
    brls::Label* meta;
    brls::Label* status;
    int row = -1;
    int64_t modId = 0;
    std::function<void(int)> onFocus;
};

namespace {

class BrowseSource : public brls::RecyclerDataSource {
public:
    explicit BrowseSource(BrowsePage* page) : page_(page) {}
    int numberOfRows(brls::RecyclerFrame*, int) override { return page_->rowCount(); }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* r, brls::IndexPath i) override {
        return reinterpret_cast<brls::RecyclerCell*>(page_->bindCell(r, static_cast<int>(i.row)));
    }
    float heightForRow(brls::RecyclerFrame*, brls::IndexPath) override { return 74; }
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath i) override { page_->onRowClicked(static_cast<int>(i.row)); }

private:
    BrowsePage* page_;
};

const char* sortTitle(mm::gb::Sort s) {
    switch (s) {
        case mm::gb::Sort::MostDownloaded: return "Most downloaded";
        case mm::gb::Sort::MostLiked: return "Most liked";
        case mm::gb::Sort::Newest: return "Newest";
        case mm::gb::Sort::LatestUpdated: return "Recently updated";
    }
    return "";
}

mm::gb::Sort nextSort(mm::gb::Sort s) {
    switch (s) {
        case mm::gb::Sort::MostDownloaded: return mm::gb::Sort::MostLiked;
        case mm::gb::Sort::MostLiked: return mm::gb::Sort::Newest;
        case mm::gb::Sort::Newest: return mm::gb::Sort::LatestUpdated;
        case mm::gb::Sort::LatestUpdated: return mm::gb::Sort::MostDownloaded;
    }
    return mm::gb::Sort::MostDownloaded;
}

bool isUnsupportedPackage(const ModFile& f) {
    return mm::endsWithI(f.filename, ".bnp") || mm::endsWithI(f.filename, ".tkcl");
}

// What the file list says about each file relative to the user's game.
struct FileNote {
    std::string text;
    NVGcolor color;
    bool matches = false;
};

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

}  // namespace

// ------------------------------------------------------------------ page

BrowsePage::BrowsePage(bool searchMode) : brls::Box(brls::Axis::ROW), searchMode_(searchMode) {
    setGrow(1);

    // Left: list
    auto* left = ui::vbox();
    left->setWidth(500);
    left->setShrink(0);
    left->setPadding(20, 14, 0, 16);
    left->setLineRight(1);
    left->setLineColor(ui::line());

    auto* header = ui::hbox();
    header->setMargins(0, 6, 10, 6);
    title_ = ui::label("", 22, ui::text());
    count_ = ui::label("", 15, ui::muted());
    header->addView(title_);
    header->addView(ui::grow());
    header->addView(count_);
    left->addView(header);

    listEmpty_ = ui::vbox();
    listEmpty_->setVisibility(brls::Visibility::GONE);
    left->addView(listEmpty_);

    recycler_ = new brls::RecyclerFrame();
    recycler_->setGrow(1);
    recycler_->estimatedRowHeight = 74;
    recycler_->registerCell("mod", [this] {
        auto* c = new ModCell();
        c->onFocus = [this](int row) { onRowFocused(row); };
        cells_.insert(c);
        return c;
    });
    recycler_->setDataSource(new BrowseSource(this));
    left->addView(recycler_);
    addView(left);

    // Right: preview / install pane
    pane_ = ui::vbox();
    pane_->setGrow(1);
    pane_->setPadding(24, 30, 20, 30);
    pane_->setBackgroundColor(ui::panelBg());
    addView(pane_);

    // Page-wide actions (they show in the footer while anything inside has focus).
    registerAction("Sort", brls::BUTTON_Y, [this](brls::View*) {
        if (searchMode_ || installing_) return false;
        sort_ = nextSort(sort_);
        reload();
        return true;
    });
    registerAction("Search", brls::BUTTON_X, [this](brls::View*) {
        if (installing_) return false;
        openSearchKeyboard();
        return true;
    });

    libSub_ = AppState::get().libraryChanged()->subscribe([this] { refreshCells(); });

    if (searchMode_) {
        auto* row = ui::focusRow();
        row->addView(ui::label(ui::icon::search, 24, ui::accent()));
        auto* l = ui::label("Search GameBanana", 18, ui::text());
        l->setMarginLeft(12);
        row->addView(l);
        row->registerClickAction([this](brls::View*) {
            openSearchKeyboard();
            return true;
        });
        listEmpty_->addView(row);
        listEmpty_->setVisibility(brls::Visibility::VISIBLE);
        recycler_->setVisibility(brls::Visibility::GONE);
        updateHeader();
        clearPane();
        pane_->addView(ui::label("Find mods by name, author or keyword.", 18, ui::muted(), false));
    } else {
        reload();
    }
}

BrowsePage::~BrowsePage() {
    *alive_ = false;
    if (previewDelay_) brls::cancelDelay(previewDelay_);
    if (cancel_) *cancel_ = true;
    AppState::get().libraryChanged()->unsubscribe(libSub_);
}

brls::View* BrowsePage::getDefaultFocus() {
    if (installing_) return pane_->getDefaultFocus();
    if (listEmpty_->getVisibility() == brls::Visibility::VISIBLE) return listEmpty_->getDefaultFocus();
    return recycler_->getDefaultFocus();
}

void BrowsePage::updateHeader() {
    if (searchMode_)
        title_->setText(query_.empty() ? "Search" : "Results for \"" + query_ + "\"");
    else
        title_->setText(sortTitle(sort_));
    count_->setText(total_ ? formatCount(total_) + " mods" : "");
}

void BrowsePage::reload() {
    items_.clear();
    page_ = 0;
    total_ = 0;
    reachedEnd_ = false;
    focusedRow_ = -1;
    updateHeader();
    recycler_->reloadData();
    loadMore();
}

void BrowsePage::loadMore() {
    if (loading_ || reachedEnd_) return;
    if (searchMode_ && query_.empty()) return;
    loading_ = true;
    int page = page_ + 1;
    auto game = AppState::get().game();
    auto sort = sort_;
    auto query = query_;
    bool search = searchMode_;
    bool hideNsfw = AppState::get().settings().hideNsfw;
    auto alive = alive_;
    if (items_.empty()) {
        clearPane();
        pane_->addView(new brls::ProgressSpinner());
    }
    AppState::get().runNetwork([=, this] {
        mm::gb::Page result;
        std::string error;
        try {
            result = search ? AppState::get().gamebanana().search(game, query, page)
                            : AppState::get().gamebanana().list(game, sort, page, 30);
        } catch (const std::exception& e) {
            error = e.what();
        }
        brls::sync([=, this] {
            if (!*alive) return;
            loading_ = false;
            if (!error.empty()) {
                if (items_.empty()) showMessage("Couldn't reach GameBanana", "Check your internet connection, then press Y to try again.\n\n" + error, true);
                reachedEnd_ = true;
                return;
            }
            page_ = page;
            total_ = result.totalCount;
            reachedEnd_ = result.isLast || result.mods.empty();
            bool wasEmpty = items_.empty();
            for (const auto& m : result.mods)
                if (!(hideNsfw && m.nsfw)) items_.push_back(m);
            updateHeader();
            bool empty = items_.empty();
            recycler_->setVisibility(empty ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
            if (empty && search) {
                listEmpty_->setVisibility(brls::Visibility::VISIBLE);
                clearPane();
                pane_->addView(ui::label("No mods found for \"" + query + "\". Press X to try another search.", 18, ui::muted(), false));
                return;
            }
            listEmpty_->setVisibility(empty ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
            recycler_->reloadData();
            if (wasEmpty && !empty) {
                showPreview(0);
                // Only grab focus if it was already inside this page.
                for (auto* v = brls::Application::getCurrentFocus(); v; v = v->getParent())
                    if (v == this) {
                        brls::Application::giveFocus(recycler_->getDefaultFocus());
                        break;
                    }
            }
            // Short pages after filtering: keep going so the list fills up.
            if (!reachedEnd_ && items_.size() < 12) loadMore();
        });
    });
}

void BrowsePage::openSearchKeyboard() {
    auto alive = alive_;
    brls::Application::getImeManager()->openForText(
        [this, alive](std::string text) {
            if (!*alive) return;
            while (!text.empty() && text.back() == ' ') text.pop_back();
            if (text.empty()) return;
            searchMode_ = true;
            query_ = text;
            listEmpty_->setVisibility(brls::Visibility::GONE);
            reload();
        },
        "Search mods", "Name, author or keyword", 48, query_);
}

ModCell* BrowsePage::bindCell(brls::RecyclerFrame* recycler, int row) {
    auto* cell = static_cast<ModCell*>(recycler->dequeueReusableCell("mod"));
    const auto& m = items_.at(row);
    cell->row = row;
    cell->modId = m.id;
    cell->name->setText(m.name);
    cell->meta->setText(m.author + " · " + m.category);
    bool installed = AppState::get().snapshot().installedGbMods.count(m.id);
    cell->status->setText(installed ? ui::icon::check : "");
    auto alive = alive_;
    int64_t id = m.id;
    AppState::get().loadImage(cell->thumb, m.thumbnailUrl, [alive, cell, id] { return *alive && cell->modId == id; });
    if (row >= static_cast<int>(items_.size()) - 8) loadMore();
    return cell;
}

void BrowsePage::refreshCells() {
    const auto& installed = AppState::get().snapshot().installedGbMods;
    for (auto* c : cells_)
        if (c->modId) c->status->setText(installed.count(c->modId) ? ui::icon::check : "");
}

void BrowsePage::onRowFocused(int row) {
    // Moving back into the list abandons an install that hasn't started copying yet.
    if (installing_ && !workingLabel_) {
        installing_ = false;
        focusedRow_ = -1;
    }
    if (installing_ || row == focusedRow_) return;
    focusedRow_ = row;
    // Wait for scrolling to settle before fetching details.
    if (previewDelay_) brls::cancelDelay(previewDelay_);
    showPreview(row);
}

void BrowsePage::onRowClicked(int row) {
    if (installing_ || row < 0 || row >= rowCount()) return;
    auto id = items_[row].id;
    if (auto d = AppState::get().cachedDetails(id)) {
        showFilePicker(*d);
        return;
    }
    setWorking("Loading files…");
    auto alive = alive_;
    installing_ = true;
    AppState::get().runNetwork([=, this] {
        try {
            auto d = AppState::get().gamebanana().details(id);
            brls::sync([=, this] {
                if (!*alive) return;
                AppState::get().cacheDetails(d);
                showFilePicker(d);
            });
        } catch (const std::exception& e) {
            std::string err = e.what();
            brls::sync([=, this] {
                if (*alive) showMessage("Couldn't load this mod", err, true);
            });
        }
    });
}

// ------------------------------------------------------------------ right pane

void BrowsePage::clearPane() {
    pane_->clearViews();
    workingLabel_ = nullptr;
}

void BrowsePage::backToPreview() {
    installing_ = false;
    cancel_.reset();
    int row = focusedRow_;
    focusedRow_ = -1;
    if (row >= 0 && row < rowCount()) {
        showPreview(row);
        focusedRow_ = row;
    } else {
        clearPane();
    }
    brls::Application::giveFocus(recycler_->getDefaultFocus());
}

void BrowsePage::showPreview(int row) {
    if (row < 0 || row >= rowCount()) return;
    const auto summary = items_[row];
    clearPane();

    auto* hero = new brls::Image();
    hero->setHeight(300);
    hero->setCornerRadius(6);
    hero->setBackgroundColor(ui::thumbBg());
    hero->setScalingType(brls::ImageScalingType::FILL);
    hero->setClipsToBounds(true);
    pane_->addView(hero);
    auto alive = alive_;
    int64_t id = summary.id;
    auto wanted = [this, alive, id] { return *alive && !installing_ && focusedRow_ >= 0 && focusedRow_ < rowCount() && items_[focusedRow_].id == id; };
    AppState::get().loadImage(hero, summary.thumbnailUrl, wanted);

    auto* titleRow = ui::hbox();
    titleRow->setMarginTop(16);
    auto* name = ui::label(summary.name, 26, ui::text());
    name->setShrink(1);
    titleRow->addView(name);
    titleRow->addView(ui::grow());
    titleRow->addView(ui::label(std::string(ui::icon::heart) + " " + formatCount(summary.likes), 16, ui::muted()));
    pane_->addView(titleRow);
    auto* meta = ui::label("by " + summary.author + " · " + summary.category, 16, ui::muted());
    meta->setMarginTop(4);
    pane_->addView(meta);

    auto* body = ui::vbox();
    body->setMarginTop(14);
    body->addView(new brls::ProgressSpinner());
    pane_->addView(body);

    auto fill = [hero, body, wanted, alive](const ModDetails& d) {
        if (!wanted()) return;
        body->clearViews();
        auto& st = AppState::get();
        auto game = st.game();
        const auto& installed = st.gameVersion(game);
        if (!d.screenshotUrls.empty()) st.loadImage(hero, d.screenshotUrls[0], wanted);

        if (st.snapshot().installedGbMods.count(d.summary.id)) {
            auto* l = ui::label(std::string(ui::icon::check) + "  Installed", 16, ui::ok());
            l->setMarginBottom(8);
            body->addView(l);
        }
        // Compatibility line
        bool anyMatch = false, anyUnsupported = false, allUnsupported = !d.files.empty();
        for (const auto& f : d.files) {
            auto n = noteFor(f, game, installed);
            anyMatch |= n.matches;
            bool unsup = isUnsupportedPackage(f);
            anyUnsupported |= unsup;
            allUnsupported &= unsup;
        }
        if (d.files.empty()) {
            body->addView(ui::label("No downloadable files", 16, ui::warn()));
        } else if (allUnsupported) {
            body->addView(ui::label(std::string(ui::icon::pc) + "  Only available as a PC merge package (BCML/TKMM)", 16, ui::warn()));
        } else if (anyMatch) {
            body->addView(ui::label(std::string(ui::icon::check) + "  Has a file for your game (v" + installed + ")", 16, ui::ok()));
        } else {
            auto named = mm::mentionedGameVersions(d.descriptionHtml, game);
            if (!installed.empty() && mm::checkCompat(installed, named) == mm::Compat::Mismatch)
                body->addView(ui::label(std::string(ui::icon::warning) + "  Mentions game v" + named.front() + ", you have v" + installed, 16, ui::warn()));
        }
        (void)anyUnsupported;
        auto files = ui::label(std::to_string(d.files.size()) + (d.files.size() == 1 ? " file" : " files") +
                                   " · " + formatCount(d.downloads) + " downloads",
                               15, ui::muted());
        files->setMarginTop(6);
        body->addView(files);
        auto* desc = ui::label(stripHtml(d.descriptionHtml, 420), 16, nvgRGB(0xc8, 0xc8, 0xc8), false);
        desc->setMarginTop(12);
        body->addView(desc);
        (void)alive;
    };

    if (auto d = AppState::get().cachedDetails(id)) {
        fill(*d);
        return;
    }
    // Debounce: only fetch if the user stays on this row for a moment.
    previewDelay_ = brls::delay(250, [this, alive, id, fill, wanted] {
        if (!*alive) return;
        previewDelay_ = 0;
        if (!wanted()) return;
        AppState::get().runNetwork([alive, id, fill] {
            try {
                auto d = AppState::get().gamebanana().details(id);
                brls::sync([alive, d, fill] {
                    if (!*alive) return;
                    AppState::get().cacheDetails(d);
                    fill(d);
                });
            } catch (const std::exception& e) {
                brls::Logger::warning("details {} failed: {}", id, e.what());
            }
        });
    });
}

void BrowsePage::setWorking(const std::string& status) {
    if (!workingLabel_) {
        clearPane();
        auto* box = ui::vbox();
        box->setGrow(1);
        box->setJustifyContent(brls::JustifyContent::CENTER);
        box->setAlignItems(brls::AlignItems::CENTER);
        box->addView(new brls::ProgressSpinner());
        workingLabel_ = ui::label(status, 20, ui::text());
        workingLabel_->setMarginTop(16);
        box->addView(workingLabel_);
        // Something focusable so B-to-cancel works and focus doesn't stay on the list.
        auto* cancel = ui::label("Press B to cancel", 15, ui::muted());
        cancel->setMarginTop(8);
        box->addView(cancel);
        box->setFocusable(true);
        box->setHideHighlight(true);
        box->registerAction("Cancel", brls::BUTTON_B, [this](brls::View*) {
            if (cancel_) *cancel_ = true;
            if (workingLabel_) workingLabel_->setText("Cancelling…");
            return true;
        });
        pane_->addView(box);
        brls::Application::giveFocus(box);
    } else {
        workingLabel_->setText(status);
    }
}

void BrowsePage::showMessage(const std::string& title, const std::string& body, bool isError) {
    installing_ = true;  // keep focus in the pane until dismissed
    clearPane();
    auto* box = ui::vbox();
    box->setGrow(1);
    box->setFocusable(true);
    box->setHideHighlight(true);
    auto* t = ui::label(std::string(isError ? ui::icon::warning : ui::icon::check) + "  " + title, 24, isError ? ui::warn() : ui::ok());
    box->addView(t);
    auto* b = ui::label(body, 17, ui::text(), false);
    b->setMarginTop(14);
    box->addView(b);
    box->registerAction("OK", brls::BUTTON_A, [this](brls::View*) {
        backToPreview();
        return true;
    });
    box->registerAction("Back", brls::BUTTON_B, [this](brls::View*) {
        backToPreview();
        return true;
    });
    box->registerAction("Retry", brls::BUTTON_Y, [this](brls::View*) {
        installing_ = false;
        reload();
        return true;
    }, !(isError && items_.empty()));
    pane_->addView(box);
    brls::Application::giveFocus(box);
}

void BrowsePage::showFilePicker(const ModDetails& d) {
    installing_ = true;
    clearPane();
    auto& st = AppState::get();
    auto game = st.game();
    const auto& installed = st.gameVersion(game);

    pane_->addView(ui::label("Install " + d.summary.name, 24, ui::text()));
    auto* sub = ui::label("Choose a file. The highlighted one is the best match for your game.", 16, ui::muted(), false);
    sub->setMargins(6, 0, 16, 0);
    pane_->addView(sub);

    if (d.files.empty()) {
        showMessage("Nothing to install", "This mod has no downloadable files on GameBanana.", true);
        return;
    }

    auto* list = ui::vbox();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setContentView(list);
    pane_->addView(scroll);

    size_t rec = recommendedFile(d, game, installed);
    brls::View* recRow = nullptr;
    for (size_t i = 0; i < d.files.size(); ++i) {
        const auto file = d.files[i];
        auto note = noteFor(file, game, installed);
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
        auto* n = ui::label((note.matches ? std::string(ui::icon::check) + " " : "") + note.text, 15, note.color);
        n->setMarginLeft(10);
        row->addView(n);
        auto details = d;
        row->registerClickAction([this, details, file](brls::View*) {
            beginInstall(details, file);
            return true;
        });
        row->updateActionHint(brls::BUTTON_A, "Install");
        list->addView(row);
        if (i == rec) recRow = row;
    }
    pane_->registerAction("Back", brls::BUTTON_B, [this](brls::View*) {
        if (!installing_ || workingLabel_) return false;
        backToPreview();
        return true;
    });
    brls::Application::giveFocus(recRow);
}

void BrowsePage::beginInstall(const ModDetails& d, const ModFile& file) {
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
    installing_ = true;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    setWorking("Downloading " + file.filename + "…");

    auto& st = AppState::get();
    auto game = st.game();
    auto dl = st.downloadsDir() / (std::to_string(file.id) + "-" + mm::slugify(file.filename, 80));
    auto work = st.workDir();
    auto prep = std::make_shared<mm::PreparedArchive>();
    auto cancel = cancel_;
    auto alive = alive_;
    auto slug = mm::slugify(d.summary.name);
    auto url = file.downloadUrl;

    st.runFileJob(
        [=, this](mm::Library&) {
            int lastPct = -1;
            AppState::get().http().download(url, dl, [&](uint64_t done, uint64_t total) {
                int pct = total ? static_cast<int>(done * 100 / total) : -1;
                if (pct != lastPct) {
                    lastPct = pct;
                    auto msg = pct >= 0 ? "Downloading… " + std::to_string(pct) + "%" : "Downloading… " + formatSize(done);
                    brls::sync([this, alive, msg] {
                        if (*alive && workingLabel_) workingLabel_->setText(msg);
                    });
                }
                return !*cancel;
            });
            brls::sync([this, alive] {
                if (*alive && workingLabel_) workingLabel_->setText("Unpacking…");
            });
            // slugify keeps the extension, which prepareArchive checks for .bnp/.tkcl.
            std::error_code ec;
            try {
                *prep = mm::prepareArchive(dl, work, game, slug, [&](uint64_t, const std::string&) { return !*cancel; });
            } catch (...) {
                mm::fs::remove(dl, ec);
                throw;
            }
            mm::fs::remove(dl, ec);
        },
        [=, this](const std::string& error) {
            if (!*alive) return;
            if (*cancel) {
                backToPreview();
                return;
            }
            if (!error.empty()) {
                showMessage("Install failed", error, true);
                return;
            }
            const auto& layout = prep->layout;
            if (layout.kind != mm::PackageKind::Installable) {
                showMessage("Can't install this file", mm::packageKindMessage(layout.kind), true);
                return;
            }
            if (layout.variants.size() == 1)
                finishInstall(d, file, prep, {0});
            else
                showVariantPicker(d, file, prep);
        });
}

void BrowsePage::showVariantPicker(const ModDetails& d, const ModFile& file, std::shared_ptr<mm::PreparedArchive> prep) {
    clearPane();
    pane_->addView(ui::label("Choose options", 24, ui::text()));
    auto* sub = ui::label("This file contains several versions or add-ons. Pick the ones you want, then press + to install.", 16, ui::muted(), false);
    sub->setMargins(6, 0, 16, 0);
    pane_->addView(sub);

    auto selected = std::make_shared<std::vector<bool>>(prep->layout.variants.size(), false);
    (*selected)[0] = true;
    auto* list = ui::vbox();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
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
        finishInstall(d, file, prep, chosen);
        return true;
    };

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

void BrowsePage::finishInstall(const ModDetails& d, const ModFile& file, std::shared_ptr<mm::PreparedArchive> prep,
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
        setWorking("Installing " + std::to_string(files.size()) + " files…");
        auto alive = alive_;
        st.runFileJob(
            [=, this](mm::Library& lib) {
                for (const auto& id : replace) lib.remove(id);
                lib.add(r, files, true, [&](size_t done, size_t total, const std::string&) {
                    if (done % 25 != 0) return;
                    auto msg = "Installing… " + std::to_string(done) + " / " + std::to_string(total) + " files";
                    brls::sync([this, alive, msg] {
                        if (*alive && workingLabel_) workingLabel_->setText(msg);
                    });
                });
                std::error_code ec;
                mm::fs::remove_all(prep->extractDir, ec);
            },
            [=, this](const std::string& error) {
                if (!*alive) return;
                if (!error.empty()) {
                    showMessage("Install failed", error, true);
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
