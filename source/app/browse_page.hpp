#pragma once

#include <borealis.hpp>

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gamebanana.hpp"
#include "installer.hpp"

namespace app {

class ModCell;

// GameBanana mod list on the left, live preview of the highlighted mod on the right.
// Pressing A on a mod turns the preview pane into the install flow.
class BrowsePage : public brls::Box {
public:
    explicit BrowsePage(bool searchMode);
    ~BrowsePage() override;

    brls::View* getDefaultFocus() override;

    // Recycler plumbing (called by the data source)
    int rowCount() const { return static_cast<int>(items_.size()); }
    ModCell* bindCell(brls::RecyclerFrame* recycler, int row);
    void onRowFocused(int row);
    void onRowClicked(int row);

private:
    void reload();
    void loadMore();
    void openSearchKeyboard();
    void updateHeader();
    void refreshCells();

    // Right pane states
    void showPreview(int row);
    void showDetails(const mm::gb::ModDetails& d);
    void showFilePicker(const mm::gb::ModDetails& d);
    void beginInstall(const mm::gb::ModDetails& d, const mm::gb::ModFile& file);
    void showVariantPicker(const mm::gb::ModDetails& d, const mm::gb::ModFile& file,
                           std::shared_ptr<mm::PreparedArchive> prep);
    void finishInstall(const mm::gb::ModDetails& d, const mm::gb::ModFile& file,
                       std::shared_ptr<mm::PreparedArchive> prep, std::vector<size_t> variants);
    void showMessage(const std::string& title, const std::string& body, bool isError);
    void clearPane();
    void backToPreview();
    void setWorking(const std::string& status);

    bool searchMode_;
    std::string query_;
    mm::gb::Sort sort_ = mm::gb::Sort::MostDownloaded;
    std::vector<mm::gb::ModSummary> items_;
    int page_ = 0;
    int64_t total_ = 0;
    bool loading_ = false;
    bool reachedEnd_ = false;
    int focusedRow_ = -1;
    size_t previewDelay_ = 0;
    bool installing_ = false;
    std::shared_ptr<std::atomic<bool>> cancel_;

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::set<ModCell*> cells_;

    brls::Label* title_ = nullptr;
    brls::Label* count_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    brls::Box* pane_ = nullptr;          // right side
    brls::Label* workingLabel_ = nullptr;
    brls::Box* listEmpty_ = nullptr;

    brls::VoidEvent::Subscription libSub_;
};

}  // namespace app
