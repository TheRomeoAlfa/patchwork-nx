#pragma once

#include <borealis.hpp>

#include "sidebar.hpp"
#include "ui.hpp"

namespace app {

class MainActivity : public brls::Activity {
public:
    MainActivity() = default;
    ~MainActivity() override;
    brls::View* createContentView() override;
    void onContentAvailable() override;

private:
    void showPage(Page page);
    void switchGame();
    void checkModLoaderCrashes();
    void checkBcmlSetup();

    Page page_ = Page::Browse;
    Sidebar* sidebar_ = nullptr;
    brls::Box* content_ = nullptr;
    ui::FocusSink* sink_ = nullptr;
    brls::GenericEvent::Subscription focusSub_;
    brls::VoidEvent::Subscription gameSub_, libSub_;
};

}  // namespace app
