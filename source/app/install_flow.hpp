#pragma once

#include <borealis.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gamebanana.hpp"
#include "installer.hpp"
#include "saves.hpp"
#include "ui.hpp"

namespace app {

// What the file list says about a file relative to the user's game.
struct FileNote {
    std::string text;
    NVGcolor color;
    bool matches = false;
};
// versionWordIsMod: from mm::versionWordIsModVersion over all of the mod's file labels.
FileNote noteFor(const mm::gb::ModFile& f, mm::Game game, const std::string& installedVersion, bool versionWordIsMod);
bool modUsesVersionWord(const mm::gb::ModDetails& d, mm::Game game);
bool isUnsupportedPackage(const mm::gb::ModFile& f);
size_t recommendedFile(const mm::gb::ModDetails& d, mm::Game game, const std::string& installedVersion);

// The install steps (pick file → download → unpack → pick options → copy), drawn into a
// pane owned by a page. Used by Browse and by My mods (to finish an earlier download).
//
// Downloads are kept until the install succeeds, so a failed or cancelled install can be
// retried without downloading again; each attempt unpacks into its own folder.
class InstallFlow {
public:
    // `sink` must outlive the pane's contents (it parks focus while the pane is redrawn).
    InstallFlow(brls::Box* pane, ui::FocusSink* sink);
    ~InstallFlow();

    std::function<void()> onExit;  // the user left the flow (B, or OK on a message)

    bool active() const { return active_; }
    bool busy() const { return busy_; }

    void showLoading(const std::string& text);
    void showFilePicker(const mm::gb::ModDetails& d);
    void install(const mm::gb::ModDetails& d, const mm::gb::ModFile& file);
    void showMessage(const std::string& title, const std::string& body, bool isError);
    // The one-time setup BCML packages need (reading the game's files), as steps.
    void showBcmlSetup(bool fromInbox);

private:
    void setWorking(const std::string& status, float progress = -1);
    void showVariantPicker(const mm::gb::ModDetails& d, const mm::gb::ModFile& file,
                           std::shared_ptr<mm::PreparedArchive> prep);
    // sizeTablesOk: the user already accepted a missing TOTK size table for their version.
    void finish(const mm::gb::ModDetails& d, const mm::gb::ModFile& file, std::shared_ptr<mm::PreparedArchive> prep,
                std::vector<size_t> variants, bool sizeTablesOk = false);
    // Save files (a different kind of install: into the game's save data, not the SD card).
    struct SaveChoice;
    void showSaveList(const mm::gb::ModDetails& d, const mm::gb::ModFile& file, std::shared_ptr<mm::PreparedArchive> prep,
                      std::vector<mm::SaveCandidate> saves);
    void showSaveOptions(const mm::gb::ModDetails& d, const mm::gb::ModFile& file, std::shared_ptr<SaveChoice> choice);
    void installSave(const mm::gb::ModDetails& d, const mm::gb::ModFile& file, std::shared_ptr<SaveChoice> choice);
    void clear();
    void leave();
    void setBusy(bool busy);

    brls::Box* pane_;
    ui::FocusSink* sink_;
    std::shared_ptr<bool> flowAlive_ = std::make_shared<bool>(true);
    bool active_ = false;
    bool busy_ = false;
    std::shared_ptr<std::atomic<bool>> cancel_;
    brls::Label* workingLabel_ = nullptr;
    ui::ProgressBar* bar_ = nullptr;
    brls::Label* hint_ = nullptr;
};

}  // namespace app
