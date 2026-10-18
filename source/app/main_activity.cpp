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
    sidebar_->onOpen = [this](Page p) {
        if (p != page_) showPage(p);
        auto& views = content_->getChildren();
        if (!views.empty() && ui::tryFocus(views.front())) return;
        // Browse and Search load their list: they pick focus up from the sink once it's there.
        if (p == Page::Browse || p == Page::Search) sink_->park();
    };
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
        if (sidebar_ && v) sidebar_->setExpanded(ui::focusInside(sidebar_));
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
    brls::delay(1800, [this] { checkBcmlSetup(); });
}

// Opened from Breath of the Wild while BCML packages wait for its files: say what's next.
void MainActivity::checkBcmlSetup() {
    auto& st = AppState::get();
    // Wait for the startup check (it tells whether mods were on), any other dialog, and any
    // install or change: its screen must not be replaced underneath it.
    if (st.sizeTableProgress() >= 0 || brls::Application::getActivitiesStack().size() > 1 || st.installBusy()) {
        brls::delay(500, [this] { checkBcmlSetup(); });
        return;
    }
    using A = AppState::GameAccess;
    auto access = st.botwAccess();
    if (access == A::NotFromGame) return;
    auto pending = st.pendingBcmlNames();
    if (pending.empty() && !st.snapshot().bcmlWaiting) return;
    std::string names;
    for (size_t i = 0; i < pending.size() && i < 3; ++i) names += (i ? ", " : "") + pending[i];
    if (pending.size() > 3) names += " and " + std::to_string(pending.size() - 3) + " more";
    brls::Dialog* dialog;
    if (access == A::Ready) {
        dialog = new brls::Dialog(pending.empty()
                                      ? std::string("Some BCML packages still couldn't be merged: Patchwork couldn't read all the game "
                                                    "files they need. Turn them off and on in My mods to see which.")
                                      : "Patchwork can read Breath of the Wild's files now, so your BCML packages can be installed:\n" +
                                            names + "\n\nFinish installing them in My mods.");
        dialog->addButton("Later", [] {});
        if (!pending.empty())
            dialog->addButton("Open My mods", [this] {
                if (AppState::get().installBusy()) return;
                showPage(Page::MyMods);
                brls::sync([this] { ui::tryFocus(content_); });
            });
    } else {
        dialog = new brls::Dialog("Patchwork was opened from Breath of the Wild, but L wasn't held, so mods were on and it can't "
                                  "read the game's own files for your BCML packages" +
                                  (names.empty() ? std::string(".") : " (" + names + ").") + "\n\n" + AppState::bcmlSetupSteps(!pending.empty()));
        dialog->addButton("OK", [] {});
    }
    dialog->open();
}

// If Atmosphère's mod loader crashed since the last check (the game wouldn't start with
// mods on), explain the usual cause: Patchwork itself never sees that crash happen.
void MainActivity::checkModLoaderCrashes() {
    if (brls::Application::getActivitiesStack().size() > 1 || AppState::get().installBusy()) {
        brls::delay(500, [this] { checkModLoaderCrashes(); });
        return;
    }
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
        if (AppState::get().installBusy()) return;
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


void MainActivity::showPage(Page page) {
    page_ = page;
    sidebar_->setActive(page);
    bool focusWasInPage = ui::focusInside(content_);
    ui::parkFocusIfInside(content_, sink_);
    content_->clearViews();
    brls::View* view = nullptr;
    switch (page) {
        case Page::Browse: {
            auto* browse = new BrowsePage(false);
            browse->onSearch = [this] {
                showPage(Page::Search);
                sidebar_->setActive(Page::Search);
                if (auto* search = dynamic_cast<BrowsePage*>(content_->getChildren().front())) {
                    ui::tryFocus(search);
                    search->openSearchKeyboard();
                }
            };
            view = browse;
            break;
        }
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
