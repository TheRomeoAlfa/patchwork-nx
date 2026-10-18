#pragma once

#include <borealis.hpp>

#include <functional>
#include <memory>
#include <string>

#include "library.hpp"
#include "ui.hpp"

namespace app {
class InstallFlow;
struct PendingInstall;
}

namespace app {

// Installed mods for the current game in load order (top wins), with on/off,
// reordering and removal. The right pane explains the highlighted mod.
class MyModsPage : public brls::Box {
public:
    MyModsPage();
    ~MyModsPage() override;
    brls::View* getDefaultFocus() override;

private:
    void rebuild();
    void showDetails(const std::string& id);
    void showModPage(const mm::ModRecord& m, brls::Box* content);
    void runJob(const std::string& status, std::function<void(mm::Library&, const mm::ProgressFn&)> job);
    void toggle(const std::string& id);
    void move(const std::string& id, int delta);
    void remove(const std::string& id);
    void showUnmanaged(size_t files, uint64_t bytes);
    void importUnmanaged(size_t files, uint64_t bytes);
    void showPending(const PendingInstall& p);
    void discardPending(const PendingInstall& p);
    void clearPane();
    void rebuildWhenSafe();
    // Called when a row gains focus; false if the pane belongs to an install in progress.
    bool paneFree();
    void onChildFocusGained(brls::View* directChild, brls::View* focusedView) override;

    std::string focusedId_;
    brls::View* focusedRow_ = nullptr;  // the row showing in the pane, for B from the pane
    std::shared_ptr<bool> detailsToken_ = std::make_shared<bool>(false);  // the details on screen
    bool busy_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    brls::Box* list_ = nullptr;
    brls::Box* pane_ = nullptr;
    brls::VoidEvent::Subscription libSub_;
    std::unique_ptr<InstallFlow> flow_;  // finishes downloads that weren't installed
    ui::FocusSink* sink_ = nullptr;
    bool dirty_ = false;  // the library changed while focus was elsewhere (e.g. a dialog)
};

}  // namespace app
