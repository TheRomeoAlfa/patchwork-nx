#pragma once

#include <borealis.hpp>

#include <functional>
#include <vector>

namespace app {

enum class Page { Browse, Search, MyMods, Settings };

// Left navigation. Shows icons + labels while it has focus and shrinks to an icon
// rail when focus moves into the page, giving the preview pane more room.
class Sidebar : public brls::Box {
public:
    Sidebar();

    std::function<void(Page)> onNavigate;
    std::function<void()> onSwitchGame;

    void setExpanded(bool expanded);
    bool expanded() const { return expanded_; }
    void setActive(Page page);
    void refresh();  // game name, version, free space, mod count
    brls::View* itemFor(Page page);

private:
    struct Item {
        Page page;
        brls::Box* row;
        brls::Label* icon;
        brls::Label* text;
        brls::Label* badge;
    };

    brls::Box* makeRow(const char* icon, const std::string& text, brls::Label** iconOut, brls::Label** textOut);

    bool expanded_ = true;
    Page active_ = Page::Browse;
    brls::Box* gameRow_ = nullptr;
    brls::Label* gameIcon_ = nullptr;
    brls::Label* gameName_ = nullptr;
    brls::Label* gameCaption_ = nullptr;
    brls::Label* footer_ = nullptr;
    std::vector<Item> items_;
};

}  // namespace app
