#pragma once

#include <borealis.hpp>

#include <vector>

namespace app {

class SettingsPage : public brls::Box {
public:
    SettingsPage();

private:
    void revealNsfwSetting();

    std::vector<brls::View*> nsfwViews_;  // hidden until L + R + minus is held
    brls::RepeatingTimer holdTimer_;
    int heldMs_ = 0;
};

}  // namespace app
