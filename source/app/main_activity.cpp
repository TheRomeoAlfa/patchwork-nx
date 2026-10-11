#include "main_activity.hpp"

#include "app_state.hpp"
#include "browse_page.hpp"
#include "my_mods_page.hpp"
#include "settings_page.hpp"
#include "ui.hpp"

namespace app {

MainActivity::~MainActivity() {
    brls::Application::getGlobalFocusChangeEvent()->unsubscribe(focusSub_);
    AppState::get().gameChanged()->unsubscribe(gameSub_);
    AppState::get().libraryChanged()->unsubscribe(libSub_);
}

brls::View* MainActivity::createContentView() {
    auto* root = ui::hbox();
    root->setAlignItems(brls::AlignItems::STRETCH);
    root->setBackgroundColor(ui::bg());

    sidebar_ = new Sidebar();
    sidebar_->onNavigate = [this](Page p) { showPage(p); };
    sidebar_->onSwitchGame = [] {
        auto& st = AppState::get();
        st.setGame(st.game() == mm::Game::Totk ? mm::Game::Botw : mm::Game::Totk);
    };
    root->addView(sidebar_);

    content_ = ui::hbox();
    content_->setGrow(1);
    content_->setAlignItems(brls::AlignItems::STRETCH);
    root->addView(content_);

    auto* frame = new brls::AppletFrame(root);
    frame->setHeaderVisibility(brls::Visibility::GONE);

    // Collapse the sidebar to icons whenever focus leaves it.
    focusSub_ = brls::Application::getGlobalFocusChangeEvent()->subscribe([this](brls::View* v) {
        if (sidebar_ && v) sidebar_->setExpanded(focusInside(sidebar_));
    });
    gameSub_ = AppState::get().gameChanged()->subscribe([this] {
        sidebar_->refresh();
        showPage(page_);
        brls::Application::notify(std::string("Switched to ") + std::string(mm::gameInfo(AppState::get().game()).displayName));
    });
    libSub_ = AppState::get().libraryChanged()->subscribe([this] { sidebar_->refresh(); });

    // ZL / ZR flip between the games from anywhere.
    registerAction("Switch game", brls::BUTTON_LT, [](brls::View*) {
        auto& st = AppState::get();
        st.setGame(st.game() == mm::Game::Totk ? mm::Game::Botw : mm::Game::Totk);
        return true;
    }, true);
    registerAction("Switch game", brls::BUTTON_RT, [](brls::View*) {
        auto& st = AppState::get();
        st.setGame(st.game() == mm::Game::Totk ? mm::Game::Botw : mm::Game::Totk);
        return true;
    }, true);

    showPage(Page::Browse);
    return frame;
}

bool MainActivity::focusInside(brls::View* container) const {
    for (auto* v = brls::Application::getCurrentFocus(); v; v = v->getParent())
        if (v == container) return true;
    return false;
}

void MainActivity::showPage(Page page) {
    page_ = page;
    sidebar_->setActive(page);
    content_->clearViews();
    switch (page) {
        case Page::Browse: content_->addView(new BrowsePage(false)); break;
        case Page::Search: content_->addView(new BrowsePage(true)); break;
        case Page::MyMods: content_->addView(new MyModsPage()); break;
        case Page::Settings: content_->addView(new SettingsPage()); break;
    }
}

}  // namespace app
