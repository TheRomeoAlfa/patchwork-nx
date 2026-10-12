#include "app_state.hpp"

#include <cstdio>
#include <regex>

#include "nlohmann/json.hpp"
#include "platform.hpp"

namespace app {

using json = nlohmann::json;

// ------------------------------------------------------------------ WorkQueue

WorkQueue::WorkQueue(int threads) {
    for (int i = 0; i < threads; ++i)
        threads_.emplace_back([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                    if (stop_ && jobs_.empty()) return;
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                    ++active_;
                }
                try {
                    job();
                } catch (const std::exception& e) {
                    brls::Logger::error("background job failed: {}", e.what());
                }
                --active_;
            }
        });
}

WorkQueue::~WorkQueue() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void WorkQueue::post(std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

// ------------------------------------------------------------------ AppState

AppState& AppState::get() {
    static AppState instance;
    return instance;
}

AppState::AppState() = default;

void AppState::init() {
    dataDir_ = platform::sdRoot() / "switch" / "patchwork";
    mm::fs::create_directories(dataDir_);
    for (const auto& g : mm::kGames) gameVersions_[g.game] = platform::gameVersion(g.game);

    http_ = std::make_unique<mm::CurlHttpClient>(platform::userAgent());
    gb_ = std::make_unique<mm::gb::Client>(*http_);
    library_ = std::make_unique<mm::Library>(platform::sdRoot(), dataDir_);

    try {
        auto j = json::parse(mm::readFile(dataDir_ / "settings.json"));
        settings_.hideNsfw = j.value("hideNsfw", true);
        if (auto g = mm::gameFromKey(j.value("game", "totk"))) game_ = *g;
    } catch (...) {
        // first run or unreadable: defaults
    }

    // Start on a game the user actually has installed.
    if (gameVersions_[game_].empty())
        for (const auto& g : mm::kGames)
            if (!gameVersions_[g.game].empty()) game_ = g.game;

    auto staleWork = workDir();
    runFileJob([staleWork](mm::Library& lib) {
        mm::removeTree(staleWork);  // leftovers from an install interrupted by a crash or power-off
        lib.load();
    }, [](const std::string& err) {
        if (!err.empty()) brls::Application::notify("Couldn't read your mod list: " + err);
    });
}

void AppState::shutdown() { saveSettings(); }

void AppState::setGame(mm::Game g) {
    if (g == game_) return;
    game_ = g;
    saveSettings();
    gameChanged_.fire();
}

void AppState::saveSettings() {
    try {
        json j{{"hideNsfw", settings_.hideNsfw}, {"game", std::string(mm::gameInfo(game_).key)}};
        mm::writeFileAtomic(dataDir_ / "settings.json", j.dump(1));
    } catch (const std::exception& e) {
        brls::Logger::error("saving settings failed: {}", e.what());
    }
}

void AppState::runFileJob(std::function<void(mm::Library&)> job, std::function<void(const std::string&)> done) {
    ++fileJobs_;
    files_.post([this, job = std::move(job), done = std::move(done)] {
        std::string error;
        try {
            job(*library_);
        } catch (const std::exception& e) {
            error = e.what();
        }
        publishSnapshot(*library_);
        brls::sync([this, done, error] {
            --fileJobs_;
            if (done) done(error);
        });
    });
}

void AppState::publishSnapshot(mm::Library& lib) {
    auto snap = std::make_shared<LibrarySnapshot>();
    for (const auto& g : mm::kGames) {
        for (const auto* m : lib.mods(g.game)) {
            snap->mods.push_back(*m);
            if (m->gamebananaModId) snap->installedGbMods.insert(m->gamebananaModId);
        }
        snap->conflicts[g.game] = lib.conflicts(g.game);
        snap->overlaps[g.game] = lib.patchOverlaps(g.game);
        auto un = lib.unmanagedFiles(g.game);
        snap->unmanaged[g.game] = {un.targets.size(), un.bytes};
    }
    brls::sync([this, snap] {
        snapshot_ = std::move(*snap);
        libraryChanged_.fire();
    });
}

std::vector<const mm::ModRecord*> AppState::mods(mm::Game g) const {
    std::vector<const mm::ModRecord*> out;
    for (const auto& m : snapshot_.mods)
        if (m.game == g) out.push_back(&m);
    return out;
}

const mm::ModRecord* AppState::findMod(const std::string& id) const {
    for (const auto& m : snapshot_.mods)
        if (m.id == id) return &m;
    return nullptr;
}

std::optional<mm::gb::ModDetails> AppState::cachedDetails(int64_t modId) const {
    auto it = details_.find(modId);
    if (it == details_.end()) return std::nullopt;
    return it->second;
}

void AppState::cacheDetails(const mm::gb::ModDetails& d) {
    if (details_.size() > 200) details_.clear();
    details_[d.summary.id] = d;
}

void AppState::loadImage(brls::Image* image, const std::string& url, std::function<bool()> stillWanted) {
    if (url.empty()) return;
    for (auto it = images_.begin(); it != images_.end(); ++it) {
        if (it->first == url) {
            images_.splice(images_.begin(), images_, it);  // mark as recently used
            auto data = it->second;
            image->setImageFromMem(reinterpret_cast<const unsigned char*>(data->data()), static_cast<int>(data->size()));
            return;
        }
    }
    runNetwork([this, image, url, stillWanted] {
        auto data = std::make_shared<std::string>();
        try {
            *data = http_->get(url);
        } catch (const std::exception& e) {
            brls::Logger::warning("image {} failed: {}", url, e.what());
            return;
        }
        brls::sync([this, image, url, stillWanted, data] {
            images_.emplace_front(url, data);
            while (images_.size() > 80) images_.pop_back();
            if (stillWanted && stillWanted())
                image->setImageFromMem(reinterpret_cast<const unsigned char*>(data->data()), static_cast<int>(data->size()));
        });
    });
}

uint64_t AppState::freeSpace() const { return platform::freeSpaceBytes(); }

// ------------------------------------------------------------------ pending installs

mm::fs::path AppState::archivePathFor(const mm::gb::ModFile& f) const {
    // slugify keeps the extension, which prepareArchive checks for .bnp/.tkcl.
    return downloadsDir() / (std::to_string(f.id) + "-" + mm::slugify(f.filename, 80));
}

bool AppState::isDownloaded(const mm::gb::ModFile& f) const {
    std::error_code ec;
    auto size = mm::fs::file_size(archivePathFor(f), ec);
    return !ec && size > 0 && (f.size <= 0 || static_cast<int64_t>(size) == f.size);
}

namespace {

json fileToJson(const mm::gb::ModFile& f) {
    return {{"id", f.id}, {"filename", f.filename}, {"size", f.size}, {"downloadUrl", f.downloadUrl},
            {"md5", f.md5}, {"version", f.version}, {"description", f.description}, {"dateAdded", f.dateAdded}};
}

mm::gb::ModFile fileFromJson(const json& j) {
    mm::gb::ModFile f;
    f.id = j.value("id", int64_t{0});
    f.filename = j.value("filename", "");
    f.size = j.value("size", int64_t{0});
    f.downloadUrl = j.value("downloadUrl", "");
    f.md5 = j.value("md5", "");
    f.version = j.value("version", "");
    f.description = j.value("description", "");
    f.dateAdded = j.value("dateAdded", int64_t{0});
    return f;
}

}  // namespace

void AppState::savePending(const PendingInstall& p) const {
    const auto& d = p.details;
    json j{{"game", std::string(mm::gameInfo(p.game).key)},
           {"modId", d.summary.id},
           {"name", d.summary.name},
           {"author", d.summary.author},
           {"category", d.summary.category},
           {"thumbnailUrl", d.summary.thumbnailUrl},
           {"profileUrl", d.summary.profileUrl},
           {"version", d.version},
           {"descriptionHtml", d.descriptionHtml.substr(0, 6000)},
           {"file", fileToJson(p.file)}};
    mm::writeFileAtomic(downloadsDir() / (std::to_string(p.file.id) + ".json"), j.dump(1));
}

std::vector<PendingInstall> AppState::pendingInstalls(mm::Game g) const {
    std::vector<PendingInstall> out;
    std::error_code ec;
    for (mm::fs::directory_iterator it(downloadsDir(), ec), end; it != end; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        try {
            auto j = json::parse(mm::readFile(it->path()));
            PendingInstall p;
            p.game = mm::gameFromKey(j.value("game", "totk")).value_or(mm::Game::Totk);
            if (p.game != g) continue;
            p.details.summary.id = j.value("modId", int64_t{0});
            p.details.summary.name = j.value("name", "");
            p.details.summary.author = j.value("author", "");
            p.details.summary.category = j.value("category", "");
            p.details.summary.thumbnailUrl = j.value("thumbnailUrl", "");
            p.details.summary.profileUrl = j.value("profileUrl", "");
            p.details.version = j.value("version", "");
            p.details.descriptionHtml = j.value("descriptionHtml", "");
            p.file = fileFromJson(j.value("file", json::object()));
            p.details.files = {p.file};
            p.archive = archivePathFor(p.file);
            if (isDownloaded(p.file)) out.push_back(std::move(p));
        } catch (...) {
            // unreadable sidecar: ignore it
        }
    }
    return out;
}

std::optional<PendingInstall> AppState::pendingFor(int64_t modId) const {
    for (auto& p : pendingInstalls(game_))
        if (p.details.summary.id == modId) return p;
    return std::nullopt;
}

void AppState::discardPending(int64_t fileId) const {
    std::error_code ec;
    for (mm::fs::directory_iterator it(downloadsDir(), ec), end; it != end; it.increment(ec)) {
        auto name = it->path().filename().string();
        auto prefix = std::to_string(fileId);
        if (name == prefix + ".json" || name.rfind(prefix + "-", 0) == 0) mm::removeTree(it->path());
    }
    mm::removeTree(workDirFor(fileId));
}

// ------------------------------------------------------------------ text helpers

std::string formatSize(uint64_t bytes) {
    char buf[32];
    if (bytes >= (1ull << 30))
        std::snprintf(buf, sizeof buf, "%.1f GB", bytes / double(1ull << 30));
    else if (bytes >= (1ull << 20))
        std::snprintf(buf, sizeof buf, "%.1f MB", bytes / double(1ull << 20));
    else
        std::snprintf(buf, sizeof buf, "%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    return buf;
}

std::string formatCount(int64_t n) {
    char buf[32];
    if (n >= 1000000)
        std::snprintf(buf, sizeof buf, "%.1fM", n / 1e6);
    else if (n >= 1000)
        std::snprintf(buf, sizeof buf, "%.1fk", n / 1e3);
    else
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
    return buf;
}

std::string stripHtml(const std::string& html, size_t maxLen) {
    static const std::regex br("<br\\s*/?>", std::regex::icase);
    static const std::regex tags("<[^>]*>");
    static const std::regex spaces("[ \\t]+");
    static const std::regex blankLines("\\n{3,}");
    auto s = std::regex_replace(html, br, "\n");
    s = std::regex_replace(s, tags, "");
    for (auto [from, to] : {std::pair{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#039;", "'"}, {"&nbsp;", " "}}) {
        size_t pos;
        while ((pos = s.find(from)) != std::string::npos) s.replace(pos, std::string(from).size(), to);
    }
    s = std::regex_replace(s, spaces, " ");
    s = std::regex_replace(s, blankLines, "\n\n");
    while (!s.empty() && (s.front() == '\n' || s.front() == ' ')) s.erase(s.begin());
    if (s.size() > maxLen) {
        s.resize(maxLen);
        // don't cut a UTF-8 sequence in half
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
        if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0x80)) s.pop_back();
        s += "…";
    }
    return s;
}

}  // namespace app
