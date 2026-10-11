#include "library.hpp"

#include <algorithm>
#include <ctime>
#include <set>
#include <stdexcept>

#include "nlohmann/json.hpp"

namespace mm {

using json = nlohmann::json;

namespace {

json toJson(const ModRecord& m) {
    return {
        {"id", m.id},
        {"game", gameInfo(m.game).key},
        {"name", m.name},
        {"author", m.author},
        {"version", m.version},
        {"sourceFile", m.sourceFile},
        {"profileUrl", m.profileUrl},
        {"gamebananaModId", m.gamebananaModId},
        {"gamebananaFileId", m.gamebananaFileId},
        {"installedAt", m.installedAt},
        {"variants", m.variants},
        {"gameVersionsNamed", m.gameVersionsNamed},
        {"enabled", m.enabled},
        {"priority", m.priority},
        {"files", m.files},
    };
}

ModRecord fromJson(const json& j) {
    ModRecord m;
    m.id = j.at("id").get<std::string>();
    m.game = gameFromKey(j.at("game").get<std::string>()).value_or(Game::Totk);
    m.name = j.value("name", "");
    m.author = j.value("author", "");
    m.version = j.value("version", "");
    m.sourceFile = j.value("sourceFile", "");
    m.profileUrl = j.value("profileUrl", "");
    m.gamebananaModId = j.value("gamebananaModId", int64_t{0});
    m.gamebananaFileId = j.value("gamebananaFileId", int64_t{0});
    m.installedAt = j.value("installedAt", int64_t{0});
    m.variants = j.value("variants", std::vector<std::string>{});
    m.gameVersionsNamed = j.value("gameVersionsNamed", std::vector<std::string>{});
    m.enabled = j.value("enabled", false);
    m.priority = j.value("priority", 0);
    m.files = j.value("files", std::vector<std::string>{});
    return m;
}

// Atmosphère matches exefs_patches by build id regardless of which subfolder they're in.
std::string ipsBuildId(const std::string& target) {
    const std::string prefix = "atmosphere/exefs_patches/";
    if (target.compare(0, prefix.size(), prefix) != 0 || !endsWithI(target, ".ips")) return {};
    auto slash = target.rfind('/');
    auto name = target.substr(slash + 1);
    return toUpper(name.substr(0, name.size() - 4));
}

// FAT32 is case-insensitive, so two targets differing only by case are the same file.
std::string targetKey(const std::string& target) { return toLower(target); }

}  // namespace

Library::Library(fs::path sdRoot, fs::path dataDir) : sdRoot_(std::move(sdRoot)), dataDir_(std::move(dataDir)) {}

void Library::load() {
    mods_.clear();
    deployed_.clear();
    backedUp_.clear();
    auto path = dataDir_ / "state.json";
    if (!fs::exists(path)) return;
    auto j = json::parse(readFile(path));
    for (const auto& m : j.value("mods", json::array())) mods_.push_back(fromJson(m));
    deployed_ = j.value("deployed", std::map<std::string, std::string>{});
    backedUp_ = j.value("backedUp", std::map<std::string, bool>{});
}

void Library::save() const {
    json j;
    j["version"] = 1;
    j["mods"] = json::array();
    for (const auto& m : mods_) j["mods"].push_back(toJson(m));
    j["deployed"] = deployed_;
    j["backedUp"] = backedUp_;
    writeFileAtomic(dataDir_ / "state.json", j.dump(1));
}

fs::path Library::modDir(const ModRecord& m) const {
    return dataDir_ / "library" / std::string(gameInfo(m.game).key) / m.id;
}

std::vector<const ModRecord*> Library::mods(Game game) const {
    std::vector<const ModRecord*> out;
    for (const auto& m : mods_)
        if (m.game == game) out.push_back(&m);
    std::stable_sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->priority > b->priority; });
    return out;
}

const ModRecord* Library::find(const std::string& id) const {
    for (const auto& m : mods_)
        if (m.id == id) return &m;
    return nullptr;
}

ModRecord* Library::findMut(const std::string& id) {
    for (auto& m : mods_)
        if (m.id == id) return &m;
    return nullptr;
}

const ModRecord& Library::add(ModRecord record, const std::vector<FileMapping>& files, bool enable,
                              const ProgressFn& progress) {
    if (record.id.empty()) throw std::invalid_argument("mod id is empty");
    if (files.empty()) throw std::invalid_argument("mod has no files");

    // Replacing an existing copy (update/reinstall): take it off the SD card first.
    std::optional<int> keepPriority;
    if (auto* existing = findMut(record.id)) {
        keepPriority = existing->priority;
        existing->enabled = false;
        apply(progress);
        fs::remove_all(modDir(*existing));
        mods_.erase(mods_.begin() + (existing - mods_.data()));
    }

    auto dir = modDir(record);
    fs::remove_all(dir);
    record.files.clear();
    size_t done = 0;
    for (const auto& f : files) {
        if (!isSafeRelativePath(f.target)) throw std::runtime_error("unsafe target path: " + f.target);
        if (progress) progress(done, files.size(), f.target);
        copyFileCreatingDirs(f.source, dir / fs::path(f.target));
        record.files.push_back(f.target);
        ++done;
    }

    if (keepPriority) {
        record.priority = *keepPriority;
    } else {
        int maxPriority = -1;
        for (const auto& m : mods_)
            if (m.game == record.game) maxPriority = std::max(maxPriority, m.priority);
        record.priority = maxPriority + 1;  // newest install wins by default
    }
    if (!record.installedAt) record.installedAt = static_cast<int64_t>(std::time(nullptr));
    record.enabled = enable;
    mods_.push_back(std::move(record));
    auto id = mods_.back().id;
    save();
    if (enable) apply(progress);
    return *find(id);
}

void Library::setEnabled(const std::string& id, bool enabled, const ProgressFn& progress) {
    auto* m = findMut(id);
    if (!m) throw std::invalid_argument("unknown mod " + id);
    if (m->enabled == enabled) return;
    m->enabled = enabled;
    save();
    apply(progress);
}

void Library::remove(const std::string& id, const ProgressFn& progress) {
    auto* m = findMut(id);
    if (!m) return;
    m->enabled = false;
    apply(progress);
    auto dir = modDir(*m);
    std::error_code ec;
    fs::remove_all(dir, ec);
    mods_.erase(mods_.begin() + (m - mods_.data()));
    save();
}

void Library::forget(const std::string& id) {
    auto* m = findMut(id);
    if (!m) return;
    if (!m->enabled) throw std::runtime_error("turn it on first so its files are back on the SD card");
    auto dir = modDir(*m);
    for (const auto& t : m->files) {
        auto it = deployed_.find(t);
        if (it != deployed_.end() && it->second == id) {
            deployed_.erase(it);  // stays on the SD card, now unmanaged
            continue;
        }
        // Another mod's file is on top: keep ours as that mod's backup so it comes back later.
        bool covered = false;
        for (const auto& [target, owner] : deployed_)
            if (targetKey(target) == targetKey(t)) covered = true;
        if (covered) {
            copyFileCreatingDirs(dir / fs::path(t), dataDir_ / "backup" / fs::path(t));
            backedUp_[t] = true;
        }
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    mods_.erase(mods_.begin() + (m - mods_.data()));
    save();
    apply();  // nothing to copy, but keeps the deployed map consistent
}

void Library::renumber(std::vector<ModRecord*> ordered) {
    // `ordered` is highest priority first.
    int p = static_cast<int>(ordered.size()) - 1;
    for (auto* m : ordered) m->priority = p--;
}

void Library::move(const std::string& id, int delta, const ProgressFn& progress) {
    auto* target = findMut(id);
    if (!target) throw std::invalid_argument("unknown mod " + id);
    std::vector<ModRecord*> ordered;
    for (auto& m : mods_)
        if (m.game == target->game) ordered.push_back(&m);
    std::stable_sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->priority > b->priority; });
    auto it = std::find(ordered.begin(), ordered.end(), target);
    auto idx = static_cast<long>(it - ordered.begin());
    auto newIdx = std::clamp(idx - delta, 0L, static_cast<long>(ordered.size()) - 1);  // +1 = toward front
    if (newIdx == idx) return;
    ordered.erase(ordered.begin() + idx);
    ordered.insert(ordered.begin() + newIdx, target);
    renumber(ordered);
    save();
    apply(progress);
}

std::map<std::string, std::string> Library::desiredDeployment() const {
    // key -> (priority, mod id, target with original casing)
    struct Owner {
        int priority;
        std::string id;
        std::string target;
    };
    std::map<std::string, Owner> owners;
    for (const auto& m : mods_) {
        if (!m.enabled) continue;
        for (const auto& t : m.files) {
            auto key = targetKey(t);
            auto it = owners.find(key);
            if (it == owners.end() || m.priority > it->second.priority) owners[key] = {m.priority, m.id, t};
        }
    }
    std::map<std::string, std::string> out;
    for (auto& [key, o] : owners) out[o.target] = o.id;
    return out;
}

void Library::apply(const ProgressFn& progress) {
    auto desired = desiredDeployment();

    std::map<std::string, std::string> desiredByKey, deployedByKey;  // key -> actual target
    for (const auto& [t, id] : desired) desiredByKey[targetKey(t)] = t;
    for (const auto& [t, id] : deployed_) deployedByKey[targetKey(t)] = t;

    std::vector<std::string> toRemove;  // deployed targets no longer wanted
    std::vector<std::string> toWrite;   // desired targets whose owner changed
    for (const auto& [key, t] : deployedByKey)
        if (!desiredByKey.count(key)) toRemove.push_back(t);
    for (const auto& [t, id] : desired) {
        auto it = deployedByKey.find(targetKey(t));
        if (it == deployedByKey.end() || deployed_.at(it->second) != id) toWrite.push_back(t);
    }

    size_t total = toRemove.size() + toWrite.size(), done = 0;
    auto backupRoot = dataDir_ / "backup";
    try {
        for (const auto& t : toRemove) {
            if (progress) progress(done, total, t);
            auto sdPath = sdRoot_ / fs::path(t);
            std::error_code ec;
            fs::remove(sdPath, ec);
            auto bk = backedUp_.find(t);
            if (bk != backedUp_.end()) {
                // Put the user's original file back.
                auto bkPath = backupRoot / fs::path(t);
                copyFileCreatingDirs(bkPath, sdPath);
                fs::remove(bkPath, ec);
                pruneEmptyDirs(bkPath.parent_path(), backupRoot);
                backedUp_.erase(bk);
            } else {
                pruneEmptyDirs(sdPath.parent_path(), sdRoot_ / "atmosphere");
            }
            deployed_.erase(t);
            ++done;
        }
        for (const auto& t : toWrite) {
            if (progress) progress(done, total, t);
            const auto& id = desired.at(t);
            const auto* m = find(id);
            auto sdPath = sdRoot_ / fs::path(t);
            auto existing = deployedByKey.find(targetKey(t));
            if (existing == deployedByKey.end() && fs::exists(sdPath)) {
                // A file we didn't put there: keep it so disabling the mod restores it.
                auto bkPath = backupRoot / fs::path(t);
                copyFileCreatingDirs(sdPath, bkPath);
                backedUp_[t] = true;
            }
            if (existing != deployedByKey.end() && existing->second != t) deployed_.erase(existing->second);
            copyFileCreatingDirs(modDir(*m) / fs::path(t), sdPath);
            deployed_[t] = id;
            ++done;
        }
    } catch (...) {
        save();  // record what actually got done so a retry continues from here
        throw;
    }
    if (progress) progress(total, total, "");
    save();
}

namespace {

// Things that live in a title's folder but aren't mod content.
bool isIgnoredFile(const std::string& name) {
    return name == "romfs_metadata.bin"  // Atmosphère's LayeredFS cache
           || name.rfind("._", 0) == 0 || name == ".DS_Store" || name == "Thumbs.db";
}

}  // namespace

UnmanagedFiles Library::unmanagedFiles(Game game) const {
    UnmanagedFiles out;
    std::set<std::string> managed;
    for (const auto& [t, id] : deployed_) managed.insert(targetKey(t));
    for (const auto& [t, b] : backedUp_) managed.insert(targetKey(t));  // counted from the backup below

    std::error_code ec;
    auto contents = sdRoot_ / "atmosphere" / "contents";
    for (const auto& tid : titleIdsFor(game)) {
        // The folder may use any casing on a FAT card.
        for (fs::directory_iterator dir(contents, ec), end; dir != end; dir.increment(ec)) {
            if (!iequals(dir->path().filename().string(), tid) || !dir->is_directory(ec)) continue;
            for (fs::recursive_directory_iterator it(dir->path(), ec), e; it != e; it.increment(ec)) {
                if (!it->is_regular_file(ec) || isIgnoredFile(it->path().filename().string())) continue;
                auto rel = it->path().lexically_relative(sdRoot_).generic_string();
                if (managed.count(targetKey(rel))) continue;
                out.targets.push_back(rel);
                out.bytes += it->file_size(ec);
            }
        }
    }
    // Originals we moved aside for a mod still belong to whoever installed them.
    for (const auto& [t, b] : backedUp_) {
        auto parts = pathParts(t);
        if (parts.size() < 3) continue;
        bool forGame = false;
        for (const auto& tid : titleIdsFor(game)) forGame |= iequals(parts[2], tid);
        if (!forGame) continue;
        out.targets.push_back(t);
        out.bytes += fs::file_size(dataDir_ / "backup" / fs::path(t), ec);
    }
    return out;
}

const ModRecord& Library::importUnmanaged(Game game, const std::string& name, const ProgressFn& progress) {
    auto found = unmanagedFiles(game);
    if (found.targets.empty()) throw std::runtime_error("no unmanaged files to import");

    ModRecord r;
    r.id = "local-existing-" + std::string(gameInfo(game).key) + "-" + std::to_string(std::time(nullptr));
    r.game = game;
    r.name = name;
    r.sourceFile = "Already on SD card";
    r.installedAt = static_cast<int64_t>(std::time(nullptr));
    r.enabled = true;
    int minPriority = 0;
    for (const auto& m : mods_)
        if (m.game == game) minPriority = std::min(minPriority, m.priority);
    r.priority = mods(game).empty() ? 0 : minPriority - 1;  // below everything Patchwork installed

    auto dir = modDir(r);
    size_t done = 0;
    try {
        for (const auto& t : found.targets) {
            if (progress) progress(done++, found.targets.size(), t);
            auto bk = backedUp_.find(t);
            if (bk != backedUp_.end()) {
                // A mod sits on top of this file; the original now lives in the library instead.
                auto bkPath = dataDir_ / "backup" / fs::path(t);
                copyFileCreatingDirs(bkPath, dir / fs::path(t));
                std::error_code ec;
                fs::remove(bkPath, ec);
                pruneEmptyDirs(bkPath.parent_path(), dataDir_ / "backup");
                backedUp_.erase(bk);
            } else {
                copyFileCreatingDirs(sdRoot_ / fs::path(t), dir / fs::path(t));
                deployed_[t] = r.id;  // already on the SD card, identical
            }
            r.files.push_back(t);
        }
    } catch (...) {
        // Roll back so nothing points at a half-copied library entry.
        for (auto it = deployed_.begin(); it != deployed_.end();)
            it = it->second == r.id ? deployed_.erase(it) : std::next(it);
        std::error_code ec;
        fs::remove_all(dir, ec);
        save();
        throw;
    }
    mods_.push_back(r);
    save();
    return *find(r.id);
}

std::vector<Conflict> Library::conflicts(Game game) const {
    std::map<std::string, Conflict> byKey;
    for (const auto* m : mods(game)) {  // already sorted highest priority first
        if (!m->enabled) continue;
        for (const auto& t : m->files) {
            auto& c = byKey[targetKey(t)];
            if (c.target.empty()) c.target = t;
            c.modIds.push_back(m->id);
        }
    }
    std::vector<Conflict> out;
    for (auto& [k, c] : byKey)
        if (c.modIds.size() > 1) out.push_back(std::move(c));
    return out;
}

std::vector<PatchOverlap> Library::patchOverlaps(Game game) const {
    std::map<std::string, std::set<std::string>> byBuild;
    for (const auto* m : mods(game)) {
        if (!m->enabled) continue;
        for (const auto& t : m->files) {
            auto b = ipsBuildId(t);
            if (!b.empty()) byBuild[b].insert(m->id);
        }
    }
    std::vector<PatchOverlap> out;
    for (auto& [b, ids] : byBuild)
        if (ids.size() > 1) out.push_back({b, {ids.begin(), ids.end()}});
    return out;
}

}  // namespace mm
