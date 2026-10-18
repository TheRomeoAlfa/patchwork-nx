#pragma once

#include <borealis.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "gamebanana.hpp"
#include "ui.hpp"

namespace app {

class ModRow;
class InstallFlow;

// GameBanana mod list on the left, live preview of the highlighted mod on the right.
// Pressing A on a mod turns the preview pane into the install flow.
class BrowsePage : public brls::Box {
public:
    explicit BrowsePage(bool searchMode);
    ~BrowsePage() override;

    brls::View* getDefaultFocus() override;

private:
    int rowCount() const { return static_cast<int>(items_.size()); }
    void onRowFocused(int row);
    void onRowClicked(int row);
    void appendRows(size_t from);
    void updateThumbnails(int around);
    brls::View* rowFocus();

    void reload();
    void loadMore();
    void openSearchKeyboard();
    void updateHeader();
    void refreshCells();
    void showPreview(int row);
    void backToPreview();
    void clearPane();
    void showEmptyState(const std::string& message, const std::string& action);
    std::unique_ptr<InstallFlow> makeFlow();

    enum class Source { GameBanana, Waikuteru };
    void setSource(Source s);
    void updateTabs();

    bool searchMode_;
    Source source_ = Source::GameBanana;
    brls::Box* tabs_ = nullptr;
    brls::Label* tabLabels_[2] = {nullptr, nullptr};
    std::string query_;
    mm::gb::Sort sort_ = mm::gb::Sort::MostDownloaded;
    std::vector<mm::gb::ModSummary> items_;
    int page_ = 0;
    int64_t total_ = 0;
    bool loading_ = false;
    bool reachedEnd_ = false;
    int focusedRow_ = -1;
    size_t previewDelay_ = 0;

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::vector<ModRow*> rows_;
    int generation_ = 0;  // bumped whenever rows_ is rebuilt; stale image callbacks check it

    brls::Label* title_ = nullptr;
    brls::Label* count_ = nullptr;
    brls::ScrollingFrame* scroll_ = nullptr;
    brls::Box* list_ = nullptr;
    brls::Box* pane_ = nullptr;  // right side: preview or the install flow
    brls::Box* listEmpty_ = nullptr;
    brls::Box* newSearch_ = nullptr;  // search results: a row to search again
    std::unique_ptr<InstallFlow> flow_;
    ui::FocusSink* sink_ = nullptr;
    std::shared_ptr<bool> previewToken_ = std::make_shared<bool>(false);  // the preview on screen
    int clickToken_ = 0;                                                 // latest A press
    std::chrono::steady_clock::time_point retryAfter_{};                 // after a failed page load

    brls::VoidEvent::Subscription libSub_;
};

}  // namespace app
