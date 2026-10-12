#include "main_activity.hpp"

#include <algorithm>

#include "app_state.hpp"
#include "add_mods_page.hpp"
#include "browse_page.hpp"
#include "my_mods_page.hpp"
#include "platform.hpp"
#include "settings_page.hpp"
#include "sysmodules.hpp"
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
    sidebar_->onSwitchGame = [this] { switchGame(); };
    root->addView(sidebar_);

    content_ = ui::hbox();
    content_->setGrow(1);
    content_->setAlignItems(brls::AlignItems::STRETCH);
    root->addView(content_);
    sink_ = new ui::FocusSink();
    root->addView(sink_);

    // B inside a page goes back to the sidebar (screens with their own B handle it first);
    // B on the sidebar keeps Borealis' "exit the app?" prompt.
    content_->registerAction("Back", brls::BUTTON_B, [this](brls::View*) {
        brls::Application::giveFocus(sidebar_->itemFor(page_));
        return true;
    });

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

    showPage(Page::Browse);
    return frame;
}

void MainActivity::onContentAvailable() {
    // Actions can only be registered once the content view is set.
    for (auto button : {brls::BUTTON_LT, brls::BUTTON_RT})
        registerAction("Switch game", button, [this](brls::View*) {
            switchGame();
            return true;
        }, true);
    brls::delay(1200, [this] { checkModLoaderCrashes(); });
}

// If Atmosphère's mod loader crashed since the last check (the game wouldn't start with
// mods on), explain the usual cause: Patchwork itself never sees that crash happen.
void MainActivity::checkModLoaderCrashes() {
    auto reports = mm::modLoaderCrashReports(platform::sdRoot());
    auto& seen = AppState::get().settings().seenCrashReports;
    bool firstCheck = !seen.has_value();
    bool fresh = false;
    if (!firstCheck)
        for (const auto& r : reports) fresh |= std::find(seen->begin(), seen->end(), r) == seen->end();
    if (firstCheck || fresh || reports.size() != seen->size()) {
        seen = reports;
        AppState::get().saveSettings();
    }
    if (!fresh) return;  // the first check only records what's there already
    auto* dialog = new brls::Dialog(
        "Atmosphère crashed while loading mods (Program ID 010041544D530000), so a game couldn't start.\n\n"
        "This is usually not caused by the mod. Background add-ons (sysmodules) can use up the memory "
        "Atmosphère needs to load mods. Try turning them off in Settings, restart the Switch and launch "
        "the game again.");
    dialog->addButton("Later", [] {});
    dialog->addButton("Open Settings", [this] {
        showPage(Page::Settings);
        // The dialog hands focus back to where it was once it has closed; move it after that.
        brls::sync([this] { ui::tryFocus(content_); });
    });
    dialog->open();
}

void MainActivity::switchGame() {
    if (AppState::get().installBusy()) {
        brls::Application::notify("Wait for the current change to finish");
        return;
    }
    auto& st = AppState::get();
    st.setGame(st.game() == mm::Game::Totk ? mm::Game::Botw : mm::Game::Totk);
}

bool MainActivity::focusInside(brls::View* container) const {
    for (auto* v = brls::Application::getCurrentFocus(); v; v = v->getParent())
        if (v == container) return true;
    return false;
}

void MainActivity::showPage(Page page) {
    page_ = page;
    sidebar_->setActive(page);
    bool focusWasInPage = ui::focusInside(content_);
    ui::parkFocusIfInside(content_, sink_);
    content_->clearViews();
    brls::View* view = nullptr;
    switch (page) {
        case Page::Browse: view = new BrowsePage(false); break;
        case Page::Search: view = new BrowsePage(true); break;
        case Page::AddMods: view = new AddModsPage(); break;
        case Page::MyMods: view = new MyModsPage(); break;
        case Page::Settings: view = new SettingsPage(); break;
    }
    content_->addView(view);
    // Pages that are still loading pick focus up from the sink themselves.
    if (focusWasInPage) ui::tryFocus(view);
}

}  // namespace app
