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

    runFileJob([](mm::Library& lib) { lib.load(); }, [](const std::string& err) {
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
