#include "browse_page.hpp"

#include <algorithm>

#include "app_state.hpp"
#include "compat.hpp"
#include "install_flow.hpp"
#include "ui.hpp"

namespace app {

using mm::gb::ModDetails;
using mm::gb::ModFile;

// ------------------------------------------------------------------ list row

class ModRow : public brls::Box {
public:
    ModRow() {
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setFocusable(true);
        setPadding(8, 10, 8, 10);
        setHeight(74);
        setCornerRadius(4);
        setHighlightCornerRadius(6);
        setLineBottom(1);
        setLineColor(ui::line());

        thumb = new brls::Image();
        thumb->setDimensions(96, 54);
        thumb->setCornerRadius(4);
        thumb->setBackgroundColor(ui::thumbBg());
        thumb->setScalingType(brls::ImageScalingType::FILL);
        addView(thumb);

        // Fixed width: labels measured while the pane was re-laying out kept stale ellipses.
        auto* text = ui::vbox();
        text->setWidth(264);
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
        brls::Box::onFocusGained();
        if (onFocus) onFocus(row);
    }

    brls::Image* thumb;
    brls::Label* name;
    brls::Label* meta;
    brls::Label* status;
    int row = -1;
    int64_t modId = 0;
    bool imageLoaded = false;
    std::function<void(int)> onFocus;
};

namespace {

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

// Break a description into chunks short enough to scroll through one at a time.
std::vector<std::string> paragraphs(const std::string& text, size_t maxLen = 360) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find("\n\n", start);
        auto para = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? text.size() : end + 2;
        while (para.size() > maxLen) {
            // Prefer to cut after a sentence or line, else at a space.
            size_t cut = para.rfind(". ", maxLen);
            if (cut == std::string::npos || cut < maxLen / 3) cut = para.rfind('\n', maxLen);
            if (cut == std::string::npos || cut < maxLen / 3) cut = para.rfind(' ', maxLen);
            if (cut == std::string::npos || cut == 0) cut = maxLen;
            out.push_back(para.substr(0, cut + 1));
            para.erase(0, cut + 1);
            while (!para.empty() && (para[0] == ' ' || para[0] == '\n')) para.erase(0, 1);
        }
        if (!para.empty() && para.find_first_not_of(" \n") != std::string::npos) out.push_back(para);
    }
    return out;
}

}  // namespace

// ------------------------------------------------------------------ page

BrowsePage::BrowsePage(bool searchMode) : brls::Box(brls::Axis::ROW), searchMode_(searchMode) {
    setGrow(1);

    // Left: list
    auto* left = ui::vbox();
    left->setWidth(500);
    left->setShrink(0);
    left->setPadding(20, 6, 0, 10);
    left->setLineRight(1);
    left->setLineColor(ui::line());

    auto* header = ui::hbox();
    header->setMargins(0, 18, 10, 12);
    // The title takes the free space itself; a separate spacer left it measured too narrow.
    // Fixed widths: a label whose text changes after layout keeps its first measurement.
    title_ = ui::label("", 22, ui::text());
    title_->setWidth(300);
    count_ = ui::label("", 15, ui::muted());
    count_->setWidth(130);
    count_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    header->addView(title_);
    header->addView(count_);
    left->addView(header);

    listEmpty_ = ui::vbox();
    listEmpty_->setVisibility(brls::Visibility::GONE);
    listEmpty_->setPadding(0, 18, 0, 6);
    left->addView(listEmpty_);

    // A plain scrolling list: rows are appended as pages arrive, so loading more never
    // moves the user's position (Borealis' RecyclerFrame can only reload everything).
    list_ = ui::vbox();
    list_->setPadding(6, 28, 6, 6);  // room for the highlight; keeps rows clear of the scroll bar
    scroll_ = new brls::ScrollingFrame();
    scroll_->setGrow(1);
    scroll_->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);  // keep the highlighted mod mid-screen
    scroll_->setContentView(list_);
    left->addView(scroll_);
    addView(left);

    // Right: preview / install pane
    pane_ = ui::vbox();
    pane_->setGrow(1);
    pane_->setPadding(24, 30, 20, 30);
    pane_->setBackgroundColor(ui::panelBg());
    addView(pane_);

    sink_ = new ui::FocusSink();
    addView(sink_);
    flow_ = makeFlow();

    // Page-wide actions (they show in the footer while anything inside has focus).
    if (!searchMode_)
        registerAction("Sort", brls::BUTTON_Y, [this](brls::View*) {
            if (searchMode_ || flow_->active()) return false;
            sort_ = nextSort(sort_);
            reload();
            return true;
        });
    registerAction("Search", brls::BUTTON_X, [this](brls::View*) {
        if (flow_->active()) return false;
        openSearchKeyboard();
        return true;
    });

    libSub_ = AppState::get().libraryChanged()->subscribe([this] { refreshCells(); });

    if (searchMode_) {
        updateHeader();
        showEmptyState("Find mods by name, author or keyword.", "Search GameBanana");
    } else {
        reload();
    }
}

std::unique_ptr<InstallFlow> BrowsePage::makeFlow() {
    auto flow = std::make_unique<InstallFlow>(pane_, sink_);
    flow->onExit = [this] { backToPreview(); };
    return flow;
}

void BrowsePage::clearPane() {
    ui::parkFocusIfInside(pane_, sink_);
    *previewToken_ = false;  // late image/details callbacks for the old preview must not touch it
    pane_->clearViews();
}

// Shown when the list is empty (nothing found, nothing left after the NSFW filter, or an
// error): a message on the right and a focusable action on the left.
void BrowsePage::showEmptyState(const std::string& message, const std::string& action) {
    ui::parkFocusIfInside(listEmpty_, sink_);
    listEmpty_->clearViews();
    auto* row = ui::focusRow();
    row->addView(ui::label(searchMode_ ? ui::icon::search : ui::icon::browse, 24, ui::accent()));
    auto* l = ui::label(action, 18, ui::text());
    l->setMarginLeft(12);
    row->addView(l);
    row->registerClickAction([this](brls::View*) {
        if (searchMode_)
            openSearchKeyboard();
        else
            reload();
        return true;
    });
    listEmpty_->addView(row);
    listEmpty_->setVisibility(brls::Visibility::VISIBLE);
    scroll_->setVisibility(brls::Visibility::GONE);
    clearPane();
    pane_->addView(ui::label(message, 18, ui::muted(), false));
    if (ui::focusInside(this) || ui::focusParked()) brls::Application::giveFocus(row);
}

BrowsePage::~BrowsePage() {
    *alive_ = false;
    if (previewDelay_) brls::cancelDelay(previewDelay_);
    AppState::get().libraryChanged()->unsubscribe(libSub_);
}

brls::View* BrowsePage::getDefaultFocus() {
    if (flow_->active()) return pane_->getDefaultFocus();
    if (listEmpty_->getVisibility() == brls::Visibility::VISIBLE) return listEmpty_->getDefaultFocus();
    return rowFocus();
}

brls::View* BrowsePage::rowFocus() {
    if (rows_.empty()) return listEmpty_->getVisibility() == brls::Visibility::VISIBLE ? listEmpty_->getDefaultFocus() : scroll_;
    int row = std::clamp(focusedRow_, 0, static_cast<int>(rows_.size()) - 1);
    return rows_[row];
}

void BrowsePage::updateHeader() {
    if (searchMode_)
        title_->setText(query_.empty() ? "Search" : "Results for \"" + query_ + "\"");
    else
        title_->setText(sortTitle(sort_));
    count_->setText(total_ ? formatCount(total_) + " mods" : "");
}

void BrowsePage::reload() {
    ui::parkFocusIfInside(this, sink_);  // the rows and the preview are about to go
    loading_ = false;                    // a reply for the old list is ignored (generation check)
    items_.clear();
    page_ = 0;
    total_ = 0;
    reachedEnd_ = false;
    focusedRow_ = -1;
    updateHeader();
    ++generation_;
    rows_.clear();
    list_->clearViews();
    scroll_->setContentOffsetY(0, false);
    loadMore();
}

void BrowsePage::loadMore() {
    if (loading_ || reachedEnd_ || std::chrono::steady_clock::now() < retryAfter_) return;
    if (searchMode_ && query_.empty()) return;
    loading_ = true;
    int page = page_ + 1;
    auto game = AppState::get().game();
    auto sort = sort_;
    auto query = query_;
    bool search = searchMode_;
    bool hideNsfw = AppState::get().settings().hideNsfw;
    auto alive = alive_;
    int gen = generation_;
    if (items_.empty() && !flow_->active()) {
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
            if (!*alive || gen != generation_) return;  // the list was reloaded meanwhile
            loading_ = false;
            if (!error.empty()) {
                if (items_.empty())
                    showEmptyState("Couldn't reach GameBanana. Check your internet connection.\n\n" + error, "Try again");
                else
                    retryAfter_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);  // retried on scroll
                return;
            }
            page_ = page;
            total_ = result.totalCount;
            reachedEnd_ = result.isLast || result.mods.empty();
            bool wasEmpty = items_.empty();
            size_t firstNew = items_.size();
            for (const auto& m : result.mods)
                if (!(hideNsfw && m.nsfw)) items_.push_back(m);
            updateHeader();
            bool empty = items_.empty();
            if (empty && reachedEnd_) {
                showEmptyState(search ? "No mods found for \"" + query + "\"." : "No mods to show here.",
                               search ? "Search again" : "Reload");
                return;
            }
            scroll_->setVisibility(empty ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
            listEmpty_->setVisibility(brls::Visibility::GONE);
            appendRows(firstNew);
            if (wasEmpty && !empty) {
                focusedRow_ = 0;
                showPreview(0);
                // Take focus only if it was in this page (or parked while it reloaded).
                if (ui::focusInside(this) || ui::focusParked()) brls::Application::giveFocus(rows_[0]);
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

void BrowsePage::appendRows(size_t from) {
    for (size_t i = from; i < items_.size(); ++i) {
        const auto& m = items_[i];
        auto* row = new ModRow();
        row->row = static_cast<int>(i);
        row->modId = m.id;
        row->name->setText(m.name);
        row->meta->setText(m.author + " · " + m.category);
        row->status->setText(AppState::get().snapshot().installedGbMods.count(m.id) ? ui::icon::check : "");
        row->onFocus = [this](int r) { onRowFocused(r); };
        int index = static_cast<int>(i);
        row->registerClickAction([this, index](brls::View*) {
            onRowClicked(index);
            return true;
        });
        row->updateActionHint(brls::BUTTON_A, "Install");
        list_->addView(row);
        rows_.push_back(row);
    }
    updateThumbnails(std::max(focusedRow_, 0));
}

void BrowsePage::updateThumbnails(int around) {
    // Keep textures only for rows near the highlighted one; long lists would otherwise
    // fill the Switch's limited applet memory with thumbnails.
    auto alive = alive_;
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        auto* row = rows_[i];
        bool near = i >= around - 12 && i <= around + 20;
        bool far = i < around - 50 || i > around + 60;
        if (near && !row->imageLoaded) {
            row->imageLoaded = true;
            int64_t id = row->modId;
            int gen = generation_;
            AppState::get().loadImage(row->thumb, items_[i].thumbnailUrl, [this, alive, gen, row, id] {
                return *alive && gen == generation_ && row->imageLoaded && row->modId == id;
            });
        } else if (far && row->imageLoaded) {
            row->imageLoaded = false;
            row->thumb->clear();
        }
    }
}

void BrowsePage::refreshCells() {
    const auto& installed = AppState::get().snapshot().installedGbMods;
    for (auto* r : rows_) r->status->setText(installed.count(r->modId) ? ui::icon::check : "");
}

void BrowsePage::onRowFocused(int row) {
    // Moving back into the list abandons an install that hasn't started working yet.
    if (flow_->active() && !flow_->busy()) {
        flow_->onExit = nullptr;
        flow_ = makeFlow();
        focusedRow_ = -1;
    }
    if (flow_->active() || row == focusedRow_) return;
    focusedRow_ = row;
    updateThumbnails(row);
    if (row >= static_cast<int>(items_.size()) - 8) loadMore();
    if (previewDelay_) brls::cancelDelay(previewDelay_);
    showPreview(row);
}

void BrowsePage::onRowClicked(int row) {
    if (flow_->active() || row < 0 || row >= rowCount()) return;
    auto id = items_[row].id;
    *previewToken_ = false;
    if (auto d = AppState::get().cachedDetails(id)) {
        flow_->showFilePicker(*d);
        return;
    }
    flow_->showLoading("Loading files…");
    auto alive = alive_;
    // Ignore the reply if the user moved on (another A press, or left this flow).
    int token = ++clickToken_;
    InstallFlow* flow = flow_.get();
    auto current = [this, alive, token, flow] { return *alive && token == clickToken_ && flow_.get() == flow && flow_->active(); };
    AppState::get().runNetwork([=, this] {
        try {
            auto d = AppState::get().gamebanana().details(id);
            brls::sync([=, this] {
                if (!current()) return;
                AppState::get().cacheDetails(d);
                flow_->showFilePicker(d);
            });
        } catch (const std::exception& e) {
            std::string err = e.what();
            brls::sync([=, this] {
                if (current()) flow_->showMessage("Couldn't load this mod", err, true);
            });
        }
    });
}

// ------------------------------------------------------------------ preview pane

void BrowsePage::backToPreview() {
    int row = focusedRow_;
    if (row >= 0 && row < rowCount()) {
        focusedRow_ = row;  // set before showPreview: the preview only fills in for the focused row
        showPreview(row);
    } else {
        clearPane();
    }
    ui::tryFocus(rowFocus());
}

void BrowsePage::showPreview(int row) {
    if (row < 0 || row >= rowCount()) return;
    const auto summary = items_[row];
    clearPane();
    auto token = std::make_shared<bool>(true);
    previewToken_ = token;

    // Everything scrolls together: move right into the pane to read the whole description.
    auto* content = ui::vbox();
    content->setPadding(0, 22, 20, 0);  // right: keep text clear of the scroll bar
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setContentView(content);
    pane_->addView(scroll);
    content->registerAction("Back to list", brls::BUTTON_B, [this](brls::View*) {
        ui::tryFocus(rowFocus());
        return true;
    });
    content->registerAction("Install", brls::BUTTON_A, [this](brls::View*) {
        onRowClicked(focusedRow_);
        return true;
    });

    auto* hero = new brls::Image();
    hero->setHeight(300);
    hero->setCornerRadius(6);
    hero->setBackgroundColor(ui::thumbBg());
    hero->setScalingType(brls::ImageScalingType::FILL);
    hero->setClipsToBounds(true);
    content->addView(hero);
    auto alive = alive_;
    int64_t id = summary.id;
    // Only for this preview, while it's on screen (it may be rebuilt for the same mod).
    auto wanted = [alive, token] { return *alive && *token; };
    AppState::get().loadImage(hero, summary.thumbnailUrl, wanted);

    auto* titleRow = ui::hbox();
    titleRow->setMarginTop(16);
    titleRow->setMinHeight(38);
    auto* name = ui::label(summary.name, 26, ui::text());
    name->setShrink(1);
    titleRow->addView(name);
    titleRow->addView(ui::grow());
    auto* likes = ui::label(std::string(ui::icon::heart) + " " + formatCount(summary.likes), 16, ui::muted());
    likes->setShrink(0);
    likes->setMarginLeft(12);
    titleRow->addView(likes);
    content->addView(titleRow);
    auto* meta = ui::label("by " + summary.author + " · " + summary.category, 16, ui::muted());
    meta->setMarginTop(6);
    content->addView(meta);

    auto* body = ui::vbox();
    body->setMarginTop(14);
    body->addView(new brls::ProgressSpinner());
    content->addView(body);

    auto fill = [body, hero, wanted, id](const ModDetails& d) {
        if (!wanted()) return;
        body->clearViews();
        auto& st = AppState::get();
        auto game = st.game();
        const auto& installed = st.gameVersion(game);
        if (!d.screenshotUrls.empty()) st.loadImage(hero, d.screenshotUrls[0], wanted);

        auto line = [body](const std::string& text, NVGcolor color) {
            auto* l = ui::label(text, 16, color);
            l->setMarginBottom(6);
            body->addView(l);
        };
        if (st.snapshot().installedGbMods.count(id)) line(std::string(ui::icon::check) + "  Installed", ui::ok());
        if (st.pendingFor(id)) line(std::string(ui::icon::file) + "  Downloaded, not installed yet: press A to finish", ui::accent());

        bool anyMatch = false, allUnsupported = !d.files.empty();
        bool versionWordIsMod = modUsesVersionWord(d, game);
        for (const auto& f : d.files) {
            anyMatch |= noteFor(f, game, installed, versionWordIsMod).matches;
            allUnsupported &= isUnsupportedPackage(f);
        }
        if (d.files.empty()) {
            line("No downloadable files", ui::warn());
        } else if (allUnsupported) {
            line(std::string(ui::icon::pc) + "  Only available as a PC merge package (BCML/TKMM)", ui::warn());
        } else if (anyMatch) {
            line(std::string(ui::icon::check) + "  Has a file for your game" + (installed.empty() ? std::string() : " (v" + installed + ")"), ui::ok());
        } else {
            auto named = mm::mentionedGameVersions(d.descriptionHtml, game, true);
            if (!installed.empty() && mm::checkCompat(installed, named) == mm::Compat::Mismatch)
                line(std::string(ui::icon::warning) + "  Mentions game v" + named.front() + ", you have v" + installed, ui::warn());
        }
        line(std::to_string(d.files.size()) + (d.files.size() == 1 ? " file" : " files") + " · " +
                 formatCount(d.downloads) + " downloads",
             ui::muted());

        // Full description, one focusable chunk per paragraph so it can be scrolled through.
        auto paras = paragraphs(stripHtml(d.descriptionHtml, 12000));
        if (paras.size() > 1) line("Press → to read the full description", ui::muted());
        for (size_t i = 0; i < paras.size(); ++i) {
            auto* chunk = ui::vbox();
            chunk->setFocusable(true);
            chunk->setHideHighlightBackground(true);
            chunk->setHighlightPadding(4);
            chunk->setMarginTop(i == 0 ? 10 : 8);
            chunk->addView(ui::label(paras[i], 16, nvgRGB(0xc8, 0xc8, 0xc8), false));
            body->addView(chunk);
        }
    };

    if (auto d = AppState::get().cachedDetails(id)) {
        fill(*d);
        return;
    }
    // Debounce: only fetch if the user stays on this row for a moment.
    previewDelay_ = brls::delay(250, [this, alive, id, fill, wanted] {
        // (the debounce: only fetch if the preview is still showing this mod)
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

}  // namespace app
