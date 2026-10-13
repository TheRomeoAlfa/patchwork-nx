#include "app_state.hpp"

#include "bnp.hpp"
#include "remote7z.hpp"
#include "sizetable.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <regex>

#include <webp/decode.h>

#include "nlohmann/json.hpp"
#include "platform.hpp"

namespace app {

namespace {
// Nothing in the mod can be installed here: TKMM packages, and BCML packages that need merging.
bool onlyPcPackages(const std::vector<mm::gb::ModFile>& files) {
    if (files.empty()) return false;
    for (const auto& f : files) {
        if (mm::endsWithI(f.filename, ".tkcl") || f.bcml == mm::gb::ModFile::Bcml::NeedsPc) continue;
        return false;
    }
    return true;
}
}  // namespace

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
    // Never destroyed: background downloads may still be finishing while the app exits, and
    // they must not find their locks and caches gone.
    static auto* instance = new AppState();
    return *instance;
}

AppState::AppState() = default;

void AppState::init() {
    dataDir_ = platform::sdRoot() / "switch" / "patchwork";
    mm::fs::create_directories(dataDir_);
    std::error_code ec;
    mm::fs::create_directories(inboxDir(), ec);
    for (const auto& g : mm::kGames) gameVersions_[g.game] = platform::gameVersion(g.game);

    http_ = std::make_unique<mm::CurlHttpClient>(platform::userAgent(), platform::caBundle());
    gb_ = std::make_unique<mm::gb::Client>(*http_);
    library_ = std::make_unique<mm::Library>(platform::sdRoot(), dataDir_);

    try {
        auto j = json::parse(mm::readFile(dataDir_ / "settings.json"));
        if (j.contains("seenCrashReports")) settings_.seenCrashReports = j["seenCrashReports"].get<std::vector<std::string>>();
        if (auto g = mm::gameFromKey(j.value("game", "totk"))) game_ = *g;
    } catch (...) {
        // first run or unreadable: defaults
    }

    // Start on a game the user actually has installed.
    if (gameVersions_[game_].empty())
        for (const auto& g : mm::kGames)
            if (!gameVersions_[g.game].empty()) game_ = g.game;

    auto staleWork = workDir();
    auto originalTable = dataDir_ / "original" / "botw" / "ResourceSizeTable.product.srsizetable";
    runFileJob([this, staleWork, originalTable](mm::Library& lib) {
        // Whatever happens, the size table status must stop saying "Checking…".
        struct Finish {
            AppState* st;
            std::string status = "Couldn't check";
            ~Finish() {
                std::lock_guard lock(st->statusMutex_);
                st->sizeTableStatus_ = status;
                st->sizeTableProgress_ = -1;
            }
        } finish{this};
        mm::removeTree(staleWork);  // leftovers from an install interrupted by a crash or power-off
        setSizeTableProgress(0.1f);
        lib.load();
        setSizeTableProgress(0.2f);
        try {
            finish.status = captureOriginalSizeTable(originalTable, [this](float p) { setSizeTableProgress(0.2f + 0.8f * p); });
        } catch (const std::exception& e) {
            brls::Logger::error("size table check failed: {}", e.what());
            finish.status = "Couldn't check";
        }
        lib.setOriginalSizeTable(originalTable);
    }, [](const std::string& err) {
        if (!err.empty()) brls::Application::notify("Couldn't read your mod list: " + err);
    });
}

void AppState::shutdown() { saveSettings(); }

std::string AppState::sizeTableStatus() const {
    std::lock_guard lock(statusMutex_);
    return sizeTableStatus_;
}

float AppState::sizeTableProgress() const {
    std::lock_guard lock(statusMutex_);
    return sizeTableProgress_;
}

void AppState::setSizeTableProgress(float p) {
    std::lock_guard lock(statusMutex_);
    sizeTableProgress_ = p;
}

// BOTW's own resource size table, the base BCML packages' sizes are merged into. Read from
// the game when Patchwork runs in place of it, and again after a game update.
std::string AppState::captureOriginalSizeTable(const mm::fs::path& path, const std::function<void(float)>& progress) {
    const auto& version = gameVersions_[mm::Game::Botw];
    auto versionFile = path.parent_path() / "version.txt";
    std::error_code ec;
    std::string savedVersion;
    if (mm::fs::exists(versionFile, ec)) savedVersion = mm::readFile(versionFile);
    bool have = mm::fs::exists(path, ec);
    if (have && (savedVersion == version || version.empty())) return "Read from game " + savedVersion;

    std::string data, why;
    if (!platform::readGameFile(mm::Game::Botw, "System/Resource/ResourceSizeTable.product.srsizetable", data, why,
                                [&](uint64_t done, uint64_t total) {
                                    if (total) progress(0.9f * static_cast<float>(done) / total);
                                })) {
        if (have) return "Needs update for " + version;
        return "Not read yet";
    }
    try {
        mm::SizeTable::parse(data);
    } catch (const std::exception& e) {
        return "Unreadable";
    }
    // If Atmosphère handed us the SD card's (modded) copy instead, it's not the original.
    auto onCard = platform::sdRoot() / mm::kBotwSizeTable;
    if (mm::fs::exists(onCard, ec) && mm::readFile(onCard) == data)
        return "Got a mod's copy · hold L too";
    mm::writeFileAtomic(path, data);
    mm::writeFileAtomic(versionFile, version);
    return "Read from game " + version;
}

void AppState::setGame(mm::Game g) {
    if (g == game_) return;
    game_ = g;
    saveSettings();
    gameChanged_.fire();
}

void AppState::saveSettings() {
    try {
        json j{{"game", std::string(mm::gameInfo(game_).key)}};
        if (settings_.seenCrashReports) j["seenCrashReports"] = *settings_.seenCrashReports;
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
            if (!m->sourceKey.empty()) snap->installedKeys.insert(m->sourceKey);
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

std::optional<mm::gb::ModDetails> AppState::cachedDetails(const std::string& sourceKey) const {
    auto it = details_.find(sourceKey);
    if (it == details_.end()) return std::nullopt;
    return it->second;
}

void AppState::cacheDetails(const mm::gb::ModDetails& d) {
    if (details_.size() > 200) details_.clear();
    details_[d.summary.sourceKey] = d;
}

mm::gb::ModDetails AppState::fetchDetails(const mm::gb::ModSummary& s) {
    if (s.sourceKey.rfind("wk:", 0) == 0) {
        auto d = mm::wk::parseModPage(http_->get(s.profileUrl), s);
        for (auto& f : d.files) f.size = http_->contentLength(f.downloadUrl);  // not on the page
        if (d.files.empty()) throw std::runtime_error("No download links found on the mod's page.");
        return d;
    }
    auto d = gb_->details(s.id);
    checkBcmlFiles(d.files);
    return d;
}

mm::gb::ModDetails AppState::resolveLink(const std::string& url, mm::Game game) {
    auto otherGame = [&](mm::Game g) {
        return std::runtime_error(std::string("That's a mod for ") + std::string(mm::gameInfo(g).displayName) +
                                  ". Switch game with ZL/ZR and try again.");
    };
    // GameBanana mod page: https://gamebanana.com/mods/123456
    static const std::regex gbRe(R"(^https?://(www\.)?gamebanana\.com/mods/(\d+))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(url, m, gbRe)) {
        mm::gb::ModDetails d;
        try {
            d = gb_->details(std::stoll(m[2].str()));
        } catch (const mm::HttpStatusError& e) {
            if (e.status() == 404) throw std::runtime_error("There's no GameBanana mod with that number.");
            throw;
        }
        for (const auto& g : mm::kGames)
            if (d.summary.gameId == g.gamebananaId && g.game != game) throw otherGame(g.game);
        constexpr int64_t kBotwWiiU = 5866;  // GameBanana's separate Wii U section
        if (d.summary.gameId == kBotwWiiU)
            throw std::runtime_error("That mod is in GameBanana's Wii U section. Wii U mods use a different file format and "
                                     "don't work on the Switch. Look for a Switch version of it.");
        if (d.summary.gameId && d.summary.gameId != mm::gameInfo(game).gamebananaId)
            throw std::runtime_error("That GameBanana mod isn't for Breath of the Wild or Tears of the Kingdom.");
        if (d.summary.nsfw && settings_.hideNsfw)
            throw std::runtime_error("GameBanana marks this mod as NSFW, so it's hidden.");
        checkBcmlFiles(d.files);
        if (onlyPcPackages(d.files)) {
            std::vector<std::string> names;
            for (const auto& f : d.files) names.push_back(f.filename);
            throw std::runtime_error(mm::gb::pcToolMessage(names));
        }
        return d;
    }
    // Waikuteru mod page
    if (mm::wk::isModPageUrl(url)) {
        auto key = mm::wk::sourceKeyForUrl(url);
        mm::gb::ModSummary s;
        try {
            for (const auto& e : waikuteruMods())
                if (e.summary.sourceKey == key) s = e.summary;
        } catch (...) {
        }
        if (s.sourceKey.empty()) {
            s.sourceKey = key;
            s.name = mm::urlDecode(url.substr(url.rfind('/') + 1));
            s.author = "Waikuteru";
            s.profileUrl = url;
            s.id = mm::stableId(key);
        }
        auto g = key.rfind("wk:totk/", 0) == 0 ? mm::Game::Totk : mm::Game::Botw;
        if (g != game) throw otherGame(g);
        return fetchDetails(s);
    }
    // Anything else: a direct download. Name it after the file.
    std::string path = url.substr(0, url.find_first_of("?#"));
    std::string filename = mm::urlDecode(path.substr(path.rfind('/') + 1));
    auto dn = url.find("dn=");
    if (dn != std::string::npos) filename = mm::urlDecode(mm::urlDecode(url.substr(dn + 3, url.find('&', dn) - dn - 3)));
    if (filename.empty()) filename = "download.zip";
    mm::gb::ModDetails d;
    d.summary.sourceKey = "url:" + path;
    d.summary.id = mm::stableId(d.summary.sourceKey);
    d.summary.name = filename.substr(0, filename.rfind('.'));
    d.summary.author = url.substr(url.find("//") + 2, url.find('/', url.find("//") + 2) - url.find("//") - 2);
    d.summary.profileUrl = url;
    mm::gb::ModFile f;
    f.id = mm::stableId(url);
    f.filename = filename;
    f.downloadUrl = url;
    f.size = http_->contentLength(url);
    f.description = "From a link";
    d.files.push_back(f);
    return d;
}

void AppState::loadPcCacheLocked() {
    if (pcLoaded_) return;
    pcLoaded_ = true;
    // Seed from the list shipped with the app (popular BOTW mods, checked ahead of time), then
    // what this console has learned since. Entries for updated mods are re-checked anyway.
    try {
        auto seed = json::parse(mm::readFile(platform::resourcePath("bnp-cache.json")));
        for (const auto& [id, v] : seed["mods"].items()) pcOnly_[std::stoll(id)] = {v.at(0).get<int64_t>(), v.at(1).get<bool>()};
        for (const auto& [id, v] : seed["bnp"].items()) bnpInstallable_[std::stoll(id)] = v.get<bool>();
    } catch (...) {
    }
    try {
        auto cached = json::parse(mm::readFile(dataDir_ / "pc-only-cache.json"));  // keep alive while iterating
        if (!cached.contains("mods")) return;  // older format: start over
        for (const auto& [id, v] : cached["mods"].items()) pcOnly_[std::stoll(id)] = {v.at(0).get<int64_t>(), v.at(1).get<bool>()};
        for (const auto& [id, v] : cached["bnp"].items()) bnpInstallable_[std::stoll(id)] = v.get<bool>();
    } catch (...) {
        // none yet, or unreadable: start over
    }
}

void AppState::savePcCacheLocked() {
    try {
        json j = {{"mods", json::object()}, {"bnp", json::object()}};
        for (const auto& [id, v] : pcOnly_) j["mods"][std::to_string(id)] = {v.first, v.second};
        for (const auto& [id, ok] : bnpInstallable_) j["bnp"][std::to_string(id)] = ok;
        mm::writeFileAtomic(dataDir_ / "pc-only-cache.json", j.dump());
    } catch (const std::exception& e) {
        brls::Logger::warning("saving the file-type cache failed: {}", e.what());
    }
}

bool AppState::bnpInstallable(const mm::gb::ModFile& f) {
    {
        std::lock_guard lock(pcMutex_);
        loadPcCacheLocked();
        auto it = bnpInstallable_.find(f.id);
        if (it != bnpInstallable_.end()) return it->second;
    }
    bool ok;
    try {
        ok = mm::checkBnp(mm::remote7zListing(*http_, f.downloadUrl)).installable();
    } catch (const std::exception& e) {
        brls::Logger::warning("couldn't look inside {}: {}", f.filename, e.what());
        return false;  // not remembered: try again next time
    }
    std::lock_guard lock(pcMutex_);
    bnpInstallable_[f.id] = ok;
    savePcCacheLocked();
    return ok;
}

void AppState::checkBcmlFiles(std::vector<mm::gb::ModFile>& files) {
    for (auto& f : files)
        if (mm::endsWithI(f.filename, ".bnp") && f.id)
            f.bcml = bnpInstallable(f) ? mm::gb::ModFile::Bcml::Installable : mm::gb::ModFile::Bcml::NeedsPc;
}


std::vector<bool> AppState::needsPcTool(const std::vector<mm::gb::ModSummary>& mods) {
    std::vector<bool> out(mods.size(), false);
    std::vector<size_t> todo;
    {
        std::lock_guard lock(pcMutex_);
        loadPcCacheLocked();
        for (size_t i = 0; i < mods.size(); ++i) {
            auto it = pcOnly_.find(mods[i].id);
            if (it != pcOnly_.end() && it->second.first == mods[i].dateModified)
                out[i] = it->second.second;
            else
                todo.push_back(i);
        }
    }
    if (todo.empty()) return out;

    std::vector<int> found(todo.size(), -1);
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t k; (k = next++) < todo.size();) {
            try {
                auto files = gb_->files(mods[todo[k]].id);
                // Only packages need a closer look; anything else installs.
                bool allPackages = !files.empty() && std::all_of(files.begin(), files.end(), [](const mm::gb::ModFile& f) {
                    return mm::endsWithI(f.filename, ".bnp") || mm::endsWithI(f.filename, ".tkcl");
                });
                bool pcOnly = allPackages;
                // One installable package is enough to show the mod; stop there.
                for (const auto& f : files) {
                    if (!pcOnly) break;
                    if (mm::endsWithI(f.filename, ".bnp") && bnpInstallable(f)) pcOnly = false;
                }
                found[k] = pcOnly ? 1 : 0;
            } catch (const std::exception& e) {
                brls::Logger::warning("file list for {} failed: {}", mods[todo[k]].id, e.what());
            }
        }
    };
    auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) workers.emplace_back(worker);
    for (auto& t : workers) t.join();
    brls::Logger::info("checked {} mods for PC-only packages in {} ms", todo.size(),
                       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());

    std::lock_guard lock(pcMutex_);
    for (size_t k = 0; k < todo.size(); ++k) {
        if (found[k] < 0) continue;
        const auto& m = mods[todo[k]];
        pcOnly_[m.id] = {m.dateModified, found[k] == 1};
        out[todo[k]] = found[k] == 1;
    }
    savePcCacheLocked();
    return out;
}

std::vector<mm::wk::ListEntry> AppState::waikuteruMods() {
    {
        std::lock_guard lock(wkMutex_);
        if (!wkList_.empty()) return wkList_;
    }
    auto list = mm::wk::parseDownloadsPage(http_->get(mm::wk::kDownloadsUrl));
    if (list.empty()) throw std::runtime_error("Couldn't read Waikuteru's Downloads page (the site may have changed).");
    std::lock_guard lock(wkMutex_);
    wkList_ = list;
    return wkList_;
}

namespace {

bool isWebp(const std::string& data) {
    return data.size() > 12 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0;
}

// Borealis decodes JPEG/PNG itself; many GameBanana previews are WebP, which we decode.
void setImageData(brls::Image* image, const std::string& data) {
    if (!isWebp(data)) {
        image->setImageFromMem(reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()));
        return;
    }
    int w = 0, h = 0;
    uint8_t* rgba = WebPDecodeRGBA(reinterpret_cast<const uint8_t*>(data.data()), data.size(), &w, &h);
    if (!rgba) return;
    int tex = nvgCreateImageRGBA(brls::Application::getNVGContext(), w, h, 0, rgba);
    WebPFree(rgba);
    if (tex) image->innerSetImage(tex);
}

}  // namespace

void AppState::loadImage(brls::Image* image, const std::string& url, std::function<bool()> stillWanted) {
    if (url.empty()) return;
    for (auto it = images_.begin(); it != images_.end(); ++it) {
        if (it->first == url) {
            images_.splice(images_.begin(), images_, it);  // mark as recently used
            setImageData(image, *it->second);
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
            if (stillWanted && stillWanted()) setImageData(image, *data);
        });
    });
}

uint64_t AppState::freeSpace() const { return platform::freeSpaceBytes(); }

// ------------------------------------------------------------------ pending installs

mm::fs::path AppState::archivePathFor(const mm::gb::ModFile& f) const {
    if (f.downloadUrl.rfind("file://", 0) == 0) return f.downloadUrl.substr(7);  // already on the SD card
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
           {"sourceKey", d.summary.sourceKey},
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
            p.details.summary.sourceKey = j.value("sourceKey", "gb:" + std::to_string(p.details.summary.id));
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

std::optional<PendingInstall> AppState::pendingFor(const std::string& sourceKey) const {
    for (auto& p : pendingInstalls(game_))
        if (p.details.summary.sourceKey == sourceKey) return p;
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

std::string formatFileSize(int64_t bytes) { return bytes > 0 ? formatSize(static_cast<uint64_t>(bytes)) : "size unknown"; }

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

std::string formatDateTime(int64_t unixSeconds, bool inConsoleZone) {
    std::tm tm{};
    if (inConsoleZone) {
        tm = platform::localTime(unixSeconds);
    } else {
        std::time_t t = static_cast<std::time_t>(unixSeconds);
        localtime_r(&t, &tm);
    }
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int hour = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s %d, %d %d:%02d %s", months[(tm.tm_mon % 12 + 12) % 12], tm.tm_mday, tm.tm_year + 1900, hour,
                  tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    return buf;
}

std::string stripHtml(const std::string& html, size_t maxLen) {
    static const std::regex br("<br\\s*/?>|</?(p|div|li|ul|ol|h[1-6]|blockquote|tr)\\b[^>]*>", std::regex::icase);
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
