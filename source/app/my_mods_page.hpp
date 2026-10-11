#pragma once

#include <borealis.hpp>

#include <functional>
#include <memory>
#include <string>

#include "library.hpp"

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
    void runJob(const std::string& status, std::function<void(mm::Library&)> job);
    void toggle(const std::string& id);
    void move(const std::string& id, int delta);
    void remove(const std::string& id);
    void showUnmanaged(size_t files, uint64_t bytes);
    void importUnmanaged(size_t files, uint64_t bytes);

    std::string focusedId_;
    bool busy_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    brls::Box* list_ = nullptr;
    brls::ScrollingFrame* scroll_ = nullptr;
    brls::Box* pane_ = nullptr;
    brls::Label* count_ = nullptr;
    brls::VoidEvent::Subscription libSub_;
};

}  // namespace app
