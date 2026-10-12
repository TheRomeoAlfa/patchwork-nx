#pragma once

#include <borealis.hpp>

#include <memory>
#include <string>
#include <vector>

#include "link_server.hpp"
#include "ui.hpp"
#include "util.hpp"

namespace app {

class InstallFlow;

// "Add mods": install archives copied to the SD card (switch/patchwork/inbox), or send a
// link from a phone to the Switch over the local network.
class AddModsPage : public brls::Box {
public:
    AddModsPage();
    ~AddModsPage() override;
    brls::View* getDefaultFocus() override;

private:
    void rebuild();
    void clearPane();
    bool paneFree();
    void showInboxFile(const mm::fs::path& path);
    void installInboxFile(const mm::fs::path& path);
    void deleteInboxFile(const mm::fs::path& path);
    void showLinkIntro();
    void openLinkScreen();
    void showLinkScreen();
    void closeLinkScreen();
    void openLink(const std::string& url);

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    brls::Box* list_ = nullptr;
    brls::Box* pane_ = nullptr;
    brls::View* linkRow_ = nullptr;
    ui::FocusSink* sink_ = nullptr;
    std::unique_ptr<InstallFlow> flow_;
    LinkServer server_;
    std::string receivedLink_;
    brls::Label* linkStatus_ = nullptr;
    std::string focusedPath_;
};

}  // namespace app
