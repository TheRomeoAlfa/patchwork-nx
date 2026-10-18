#pragma once

#include <borealis.hpp>

#include <memory>
#include <vector>

#include "saves.hpp"

namespace app {

class SettingsPage : public brls::Box {
public:
    SettingsPage();

    ~SettingsPage() override;

private:
    void revealNsfwSetting();
    void fillBackups();
    void restoreBackup(const mm::SaveBackup& backup);
    void recopyMods(brls::View* cell);

    brls::ScrollingFrame* scroll_ = nullptr;
    brls::Box* backups_ = nullptr;        // rows of save backups, rebuilt after changes
    brls::View* backupsAnchor_ = nullptr; // stable focus target while backups_ is rebuilt
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    std::vector<brls::View*> nsfwViews_;  // hidden until L + R + minus is held
    brls::RepeatingTimer holdTimer_;
    brls::RepeatingTimer sizeTimer_;  // follows the size table check started at launch
    int heldMs_ = 0;
};

}  // namespace app
