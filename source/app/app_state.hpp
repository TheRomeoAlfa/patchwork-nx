#pragma once

#include <borealis.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gamebanana.hpp"
#include "library.hpp"
#include "waikuteru.hpp"

namespace app {

// Fixed set of threads draining a queue of jobs.
class WorkQueue {
public:
    explicit WorkQueue(int threads);
    ~WorkQueue();
    void post(std::function<void()> job);
    bool idle() const { return active_ == 0; }

private:
    std::vector<std::thread> threads_;
    std::deque<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<int> active_{0};
    bool stop_ = false;
};

// Copy of the library state for the UI thread; the real Library is only touched by
// the file worker so long installs never block drawing.
struct LibrarySnapshot {
    std::vector<mm::ModRecord> mods;  // all games, highest priority first within a game
    std::map<mm::Game, std::vector<mm::Conflict>> conflicts;
    std::map<mm::Game, std::vector<mm::PatchOverlap>> overlaps;
    std::set<std::string> installedKeys;  // sourceKey of every installed mod
    struct Unmanaged {
        size_t files = 0;
        uint64_t bytes = 0;
    };
    std::map<mm::Game, Unmanaged> unmanaged;  // files on the SD card Patchwork didn't install
};

// A mod file that finished downloading but isn't installed yet (failed or abandoned
// install). Kept so trying again doesn't download it again.
struct PendingInstall {
    mm::Game game = mm::Game::Totk;
    mm::gb::ModDetails details;
    mm::gb::ModFile file;
    mm::fs::path archive;
};

struct Settings {
    bool hideNsfw = true;  // hide mods GameBanana treats as NSFW
};

class AppState {
public:
    static AppState& get();

    void init();
    void shutdown();

    // ---- current game
    mm::Game game() const { return game_; }
    void setGame(mm::Game g);
    brls::VoidEvent* gameChanged() { return &gameChanged_; }
    const std::string& gameVersion(mm::Game g) const { return gameVersions_.at(g); }

    // ---- network (runs on a small thread pool; callbacks must hop back with brls::sync)
    mm::gb::Client& gamebanana() { return *gb_; }
    mm::HttpClient& http() { return *http_; }
    void runNetwork(std::function<void()> job) { net_.post(std::move(job)); }

    // ---- library: mutate only inside runFileJob; read the snapshot on the UI thread
    void runFileJob(std::function<void(mm::Library&)> job, std::function<void(const std::string& error)> done);
    bool fileJobRunning() const { return fileJobs_ > 0; }
    // An install flow is downloading/unpacking/copying: page and game switches wait for it.
    void setInstallBusy(bool busy) { installBusy_ += busy ? 1 : -1; }
    bool installBusy() const { return installBusy_ > 0; }
    const LibrarySnapshot& snapshot() const { return snapshot_; }
    brls::VoidEvent* libraryChanged() { return &libraryChanged_; }
    std::vector<const mm::ModRecord*> mods(mm::Game g) const;
    const mm::ModRecord* findMod(const std::string& id) const;

    // ---- caches (UI thread only)
    std::optional<mm::gb::ModDetails> cachedDetails(const std::string& sourceKey) const;
    void cacheDetails(const mm::gb::ModDetails& d);

    // ---- sources (network; call from runNetwork, not the UI thread)
    // Details for a mod from any source, by its summary (GameBanana id or page URL).
    mm::gb::ModDetails fetchDetails(const mm::gb::ModSummary& s);
    // Waikuteru's mod list (fetched once per session).
    std::vector<mm::wk::ListEntry> waikuteruMods();
    // Turn a pasted link into something installable: a GameBanana or Waikuteru mod page, or
    // a direct link to an archive. Throws with a message for the user (e.g. wrong game).
    mm::gb::ModDetails resolveLink(const std::string& url, mm::Game game);
    // Load an image URL into `image` unless `stillWanted` says the view moved on.
    void loadImage(brls::Image* image, const std::string& url, std::function<bool()> stillWanted);

    // ---- settings
    Settings& settings() { return settings_; }
    void saveSettings();

    const mm::fs::path& dataDir() const { return dataDir_; }
    mm::fs::path downloadsDir() const { return dataDir_ / "downloads"; }
    mm::fs::path workDir() const { return dataDir_ / "work"; }
    mm::fs::path inboxDir() const { return dataDir_ / "inbox"; }  // archives copied here by the user
    // Each install unpacks into its own folder so two attempts can never mix files.
    mm::fs::path workDirFor(int64_t fileId) const { return workDir() / std::to_string(fileId); }
    mm::fs::path archivePathFor(const mm::gb::ModFile& f) const;
    bool isDownloaded(const mm::gb::ModFile& f) const;

    // Downloads waiting to be installed. The sidecar is written before downloading starts.
    std::vector<PendingInstall> pendingInstalls(mm::Game g) const;
    std::optional<PendingInstall> pendingFor(const std::string& sourceKey) const;
    void savePending(const PendingInstall& p) const;
    // Thread-safe (filesystem only): delete the archive, its sidecar and its unpack folder.
    void discardPending(int64_t fileId) const;
    uint64_t freeSpace() const;

private:
    AppState();
    void publishSnapshot(mm::Library& lib);

    mm::Game game_ = mm::Game::Totk;
    std::map<mm::Game, std::string> gameVersions_;
    mm::fs::path dataDir_;
    std::unique_ptr<mm::HttpClient> http_;
    std::unique_ptr<mm::gb::Client> gb_;
    std::unique_ptr<mm::Library> library_;
    LibrarySnapshot snapshot_;
    Settings settings_;

    WorkQueue net_{3};
    WorkQueue files_{1};
    std::atomic<int> fileJobs_{0};
    int installBusy_ = 0;

    brls::VoidEvent gameChanged_;
    brls::VoidEvent libraryChanged_;

    std::map<std::string, mm::gb::ModDetails> details_;
    std::mutex wkMutex_;
    std::vector<mm::wk::ListEntry> wkList_;
    // Small LRU of downloaded image bytes keyed by URL.
    std::list<std::pair<std::string, std::shared_ptr<std::string>>> images_;
    std::set<std::string> imagesInFlight_;
};

// Text helpers shared by the views.
std::string formatSize(uint64_t bytes);
std::string formatFileSize(int64_t bytes);  // "size unknown" for 0 / -1
std::string formatCount(int64_t n);
std::string stripHtml(const std::string& html, size_t maxLen = 600);

}  // namespace app
