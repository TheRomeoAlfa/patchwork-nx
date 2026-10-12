#include "sidebar.hpp"

#include "app_state.hpp"
#include "ui.hpp"

namespace app {

namespace {
constexpr float kExpandedWidth = 300;
constexpr float kCollapsedWidth = 76;
}  // namespace

brls::Box* Sidebar::makeRow(const char* icon, const std::string& text, brls::Label** iconOut, brls::Label** textOut) {
    auto* row = ui::hbox();
    row->setFocusable(true);
    row->setHeight(52);
    row->setPadding(0, 14, 0, 16);
    row->setMarginBottom(4);
    row->setCornerRadius(4);
    row->setHighlightCornerRadius(6);
    auto* i = ui::label(icon, 26, ui::muted());
    i->setWidth(30);
    auto* t = ui::label(text, 19, ui::text());
    t->setMarginLeft(14);
    t->setGrow(1);
    row->addView(i);
    row->addView(t);
    *iconOut = i;
    *textOut = t;
    return row;
}

Sidebar::Sidebar() : brls::Box(brls::Axis::COLUMN) {
    setWidth(kExpandedWidth);
    setBackgroundColor(ui::sidebarBg());
    setPadding(20, 10, 16, 10);
    setLineRight(1);
    setLineColor(ui::line());

    // Game switcher: A swaps between the two games.
    gameRow_ = makeRow(ui::icon::game, "", &gameIcon_, &gameName_);
    ui::tappable(gameRow_);  // switching game on a stray tap would be surprising
    gameRow_->setHeight(64);
    gameRow_->setMarginBottom(14);
    gameName_->setFontSize(18);
    gameIcon_->setTextColor(ui::accent());
    auto* gameText = ui::vbox();
    gameText->setGrow(1);
    gameText->setShrink(1);
    gameName_->setAutoAnimate(false);
    gameText->setMarginLeft(14);
    gameRow_->removeView(gameName_, false);
    gameName_->setMarginLeft(0);
    gameCaption_ = ui::label("", 14, ui::muted());
    gameCaption_->setAutoAnimate(false);
    gameText->addView(gameName_);
    gameText->addView(gameCaption_);
    gameRow_->addView(gameText);
    gameRow_->registerClickAction([this](brls::View*) {
        if (onSwitchGame) onSwitchGame();
        return true;
    });
    gameRow_->updateActionHint(brls::BUTTON_A, "Switch game");
    addView(gameRow_);

    const std::pair<Page, std::pair<const char*, const char*>> defs[] = {
        {Page::Browse, {ui::icon::browse, "Browse"}},
        {Page::Search, {ui::icon::search, "Search"}},
        {Page::AddMods, {ui::icon::add, "Add mods"}},
        {Page::MyMods, {ui::icon::myMods, "My mods"}},
        {Page::Settings, {ui::icon::settings, "Settings"}},
    };
    for (const auto& [page, d] : defs) {
        Item item{page, nullptr, nullptr, nullptr, nullptr};
        item.row = makeRow(d.first, d.second, &item.icon, &item.text);
        ui::tappable(item.row, ui::Tap::Act);
        item.badge = ui::label("", 15, ui::muted());
        item.badge->setWidth(36);
        item.badge->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        item.row->addView(item.badge);
        // Like the Switch's System Settings: moving onto an entry shows its page.
        item.row->getFocusEvent()->subscribe([this, page](brls::View*) {
            if (page != active_ && onNavigate) onNavigate(page);
        });
        item.row->registerClickAction([this, page](brls::View*) {
            if (onNavigate) onNavigate(page);
            return true;
        });
        addView(item.row);
        items_.push_back(item);
    }

    addView(ui::grow());
    footer_ = ui::label("", 14, ui::muted(), false);
    footer_->setMarginLeft(8);
    addView(footer_);

    refresh();
    setActive(Page::Browse);
}

brls::View* Sidebar::itemFor(Page page) {
    for (auto& i : items_)
        if (i.page == page) return i.row;
    return gameRow_;
}

void Sidebar::setActive(Page page) {
    active_ = page;
    for (auto& i : items_) {
        bool on = i.page == page;
        i.row->setBackgroundColor(on ? ui::accentDim() : nvgRGBA(0, 0, 0, 0));
        i.icon->setTextColor(on ? ui::accent() : ui::muted());
        i.text->setTextColor(on ? ui::accent() : ui::text());
    }
}

void Sidebar::setExpanded(bool expanded) {
    if (expanded == expanded_) return;
    expanded_ = expanded;
    auto vis = expanded ? brls::Visibility::VISIBLE : brls::Visibility::GONE;
    setWidth(expanded ? kExpandedWidth : kCollapsedWidth);
    gameName_->getParent()->setVisibility(vis);
    footer_->setVisibility(vis);
    for (auto& i : items_) {
        i.text->setVisibility(vis);
        i.badge->setVisibility(vis);
    }
}

void Sidebar::refresh() {
    auto& st = AppState::get();
    const auto& info = mm::gameInfo(st.game());
    gameName_->setText(std::string(info.displayName));
    auto version = st.gameVersion(st.game());
    gameCaption_->setText(version.empty() ? "Not installed · A to switch" : "Version " + version + " · A to switch");
    for (auto& i : items_)
        if (i.page == Page::MyMods) {
            auto n = st.mods(st.game()).size();
            i.badge->setText(n ? std::to_string(n) : "");
        }
    footer_->setText(formatSize(st.freeSpace()) + " free on SD card");
}

}  // namespace app
