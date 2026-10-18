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
    bool bcmlWaiting = false;  // enabled BCML packages not merged yet (game files missing)
    bool loaded = false;       // false until the mod list has been read at startup
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
    // Hide mods GameBanana treats as NSFW. Not saved: every launch starts hidden, and the
    // setting itself only appears after holding L + R + minus on the Settings page.
    std::atomic<bool> hideNsfw{true};  // also read on the network thread
    bool nsfwSettingRevealed = false;
    // Atmosphère mod-loader crash reports already told about; null until the first check,
    // so reports from before Patchwork was installed aren't announced.
    std::optional<std::vector<std::string>> seenCrashReports;
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
    // What a background job (not started by the user) is doing, for "waiting" messages; "" if none.
    std::string backgroundTask() const;
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
    // For each mod: true if every file is a PC-only package (BCML/TKMM), so it's hidden from
    // lists. Asks GameBanana for uncached mods (several at once); remembered on the SD card.
    // Network thread only. A mod that can't be checked counts as installable.
    std::vector<bool> needsPcTool(const std::vector<mm::gb::ModSummary>& mods);
    // Whether Patchwork has BOTW's original size table (needed for BCML packages), for Settings.
    std::string sizeTableStatus() const;
    // Can Patchwork read BOTW's own files right now (for merging BCML packages)?
    enum class GameAccess {
        NotFromGame,  // opened from the Album / another title
        ModsOn,       // opened by holding R on BOTW, but without L: LayeredFS hands over mods' files
        Ready,        // opened by holding L and R on BOTW
    };
    GameAccess botwAccess() const;
    // How many of the game's files Patchwork has kept for merging.
    size_t keptGameFiles() const;
    // Downloaded BCML packages that aren't installed yet.
    std::vector<std::string> pendingBcmlNames() const;
    // BCML packages will ask for the setup: Patchwork can't read the game now and has none of
    // its files yet.
    bool bcmlSetupNeeded() const { return botwAccess() != GameAccess::Ready && keptGameFiles() == 0; }
    // The steps to let Patchwork read the game's files, for messages.
    static std::string bcmlSetupSteps(bool thenFinishInMyMods);
    // How far the check at startup is (0..1), or -1 once it's done.
    float sizeTableProgress() const;
    // Fill in which BCML packages among a mod's files install as-is. Network thread only.
    void checkBcmlFiles(std::vector<mm::gb::ModFile>& files);
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
    mm::fs::path saveBackupsDir() const { return dataDir_ / "save-backups"; }
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
    std::mutex pcMutex_;
    mutable std::mutex statusMutex_;
    std::string sizeTableStatus_ = "Checking…";
    std::string backgroundTask_;  // guarded by statusMutex_
    bool modsWereOn_ = false;     // file thread: the startup check got a mod's copy of the size table
    float sizeTableProgress_ = 0;
    void setSizeTableProgress(float p);
    // File thread. `progress` gets 0..1 while the game's file is read.
    std::string captureOriginalSizeTable(const mm::fs::path& path, const std::function<void(float)>& progress);
    bool pcLoaded_ = false;
    std::map<int64_t, std::pair<int64_t, bool>> pcOnly_;  // mod id -> (date modified, PC-only)
    std::map<int64_t, std::vector<std::string>> bnpLogs_;  // file id -> BnpCheck::summary()
    void loadPcCacheLocked();
    void savePcCacheLocked();
    bool bnpInstallable(const mm::gb::ModFile& f);  // cached; false if it can't be checked
    std::vector<mm::wk::ListEntry> wkList_;
    // Small LRU of downloaded image bytes keyed by URL.
    std::list<std::pair<std::string, std::shared_ptr<std::string>>> images_;
    std::set<std::string> imagesInFlight_;
};

// Text helpers shared by the views.
std::string formatSize(uint64_t bytes);
std::string formatFileSize(int64_t bytes);  // "size unknown" for 0 / -1
std::string formatCount(int64_t n);
// "May 24, 2023 6:50 PM". `inConsoleZone`: a real moment (now, a backup), shown in the
// console's time zone. Otherwise a time read from an archive: zip times are wall-clock
// times that libarchive converted with the C library's zone, so convert back the same way.
std::string formatDateTime(int64_t unixSeconds, bool inConsoleZone = true);
std::string stripHtml(const std::string& html, size_t maxLen = 600);
// Break a description into chunks short enough to scroll through one at a time.
std::vector<std::string> paragraphs(const std::string& text, size_t maxLen = 360);

}  // namespace app
