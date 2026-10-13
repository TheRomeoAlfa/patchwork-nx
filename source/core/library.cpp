#include "library.hpp"

#include <algorithm>
#include <ctime>
#include <set>
#include <stdexcept>

#include "bcml.hpp"
#include "nlohmann/json.hpp"
#include "sizetable.hpp"

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
        {"sourceKey", m.sourceKey},
        {"gamebananaModId", m.gamebananaModId},
        {"gamebananaFileId", m.gamebananaFileId},
        {"installedAt", m.installedAt},
        {"variants", m.variants},
        {"gameVersionsNamed", m.gameVersionsNamed},
        {"enabled", m.enabled},
        {"priority", m.priority},
        {"files", m.files},
        {"resourceSizes", m.resourceSizes},
        {"bcmlLogs", m.bcmlLogs},
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
    m.sourceKey = j.value("sourceKey", "");
    m.gamebananaModId = j.value("gamebananaModId", int64_t{0});
    m.gamebananaFileId = j.value("gamebananaFileId", int64_t{0});
    if (m.sourceKey.empty() && m.gamebananaModId) m.sourceKey = "gb:" + std::to_string(m.gamebananaModId);  // older state files
    m.installedAt = j.value("installedAt", int64_t{0});
    m.variants = j.value("variants", std::vector<std::string>{});
    m.gameVersionsNamed = j.value("gameVersionsNamed", std::vector<std::string>{});
    m.enabled = j.value("enabled", false);
    m.priority = j.value("priority", 0);
    m.files = j.value("files", std::vector<std::string>{});
    m.resourceSizes = j.value("resourceSizes", std::map<std::string, uint32_t>{});
    m.bcmlLogs = j.value("bcmlLogs", std::vector<std::string>{});
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

void replaceFile(const fs::path& from, const fs::path& to);  // below

const std::string kSizeTableKey = toLower(kBotwSizeTable);
constexpr const char* kMergedPrefix = "*merged:";
constexpr const char* kBcmlPrefix = "*bcml:";
const std::string kContentsPrefix = "atmosphere/contents/";

}  // namespace

Library::Library(fs::path sdRoot, fs::path dataDir) : sdRoot_(std::move(sdRoot)), dataDir_(std::move(dataDir)) {}

bool Library::load() {
    mods_.clear();
    deployed_.clear();
    backedUp_.clear();
    auto path = dataDir_ / "state.json";
    auto tmp = path;
    tmp += ".tmp";
    std::error_code ec;
    json j;
    bool parsed = false;
    for (const auto& candidate : {path, tmp}) {
        if (!fs::exists(candidate, ec)) continue;
        try {
            j = json::parse(readFile(candidate));
            parsed = j.is_object();
        } catch (...) {
            parsed = false;
        }
        if (parsed) break;
    }
    if (!parsed) {
        if (fs::exists(path, ec)) {
            // Keep it for inspection rather than overwriting it with an empty library.
            auto corrupt = path;
            corrupt += ".corrupt";
            fs::remove(corrupt, ec);
            fs::rename(path, corrupt, ec);
            return false;
        }
        return true;  // first run
    }
    for (const auto& m : j.value("mods", json::array())) mods_.push_back(fromJson(m));
    deployed_ = j.value("deployed", std::map<std::string, std::string>{});
    bcmlKey_ = j.value("bcmlKey", "");
    bcmlOutputs_ = j.value("bcmlOutputs", std::vector<std::string>{});
    bcmlSizes_ = j.value("bcmlSizes", std::map<std::string, uint32_t>{});
    auto backups = j.value("backedUp", json::object());
    for (const auto& [k, v] : backups.items()) {
        // Old format was {target: true}.
        auto stored = v.is_string() ? v.get<std::string>() : k;
        backedUp_[targetKey(stored)] = stored;
    }
    return true;
}

void Library::save() const {
    json j;
    j["version"] = 1;
    j["mods"] = json::array();
    for (const auto& m : mods_) j["mods"].push_back(toJson(m));
    j["deployed"] = deployed_;
    j["backedUp"] = backedUp_;
    j["bcmlKey"] = bcmlKey_;
    j["bcmlOutputs"] = bcmlOutputs_;
    j["bcmlSizes"] = bcmlSizes_;
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
                              const ProgressFn& progress, const std::vector<FileMapping>& internal) {
    if (record.id.empty()) throw std::invalid_argument("mod id is empty");
    if (files.empty()) throw std::invalid_argument("mod has no files");

    // 1. Copy into a staging folder. Until this succeeds neither the library nor the SD
    //    card changes, so a failed copy (SD full, bad archive) leaves no trace.
    auto dir = modDir(record);
    auto staging = dir;
    staging += ".incoming";
    auto old = dir;
    old += ".old";
    std::string err;
    if (!removeTree(staging, &err) || !removeTree(old, &err)) throw std::runtime_error(err);
    record.files.clear();
    try {
        size_t done = 0;
        for (const auto& f : files) {
            if (!isSafeRelativePath(f.target)) throw std::runtime_error("unsafe target path: " + f.target);
            if (progress) progress(done, files.size(), f.target);
            copyFileCreatingDirs(f.source, staging / fs::path(f.target));
            record.files.push_back(f.target);
            ++done;
        }
        for (const auto& f : internal) {
            if (!isSafeRelativePath(f.target)) throw std::runtime_error("unsafe path: " + f.target);
            copyFileCreatingDirs(f.source, staging / fs::path(f.target));
        }

        // 2. Swap it in. Replacing an existing copy (update/reinstall): take that off the SD
        //    first, then move its library folder aside so it can be restored if the swap fails.
        std::optional<int> keepPriority;
        if (auto* existing = findMut(record.id)) {
            keepPriority = existing->priority;
            existing->enabled = false;
            apply(progress);
        }
        if (fs::exists(dir)) fs::rename(dir, old);
        try {
            fs::rename(staging, dir);
        } catch (...) {
            std::error_code ec;
            if (fs::exists(old, ec)) fs::rename(old, dir, ec);
            throw;
        }
        if (auto* existing = findMut(record.id)) mods_.erase(mods_.begin() + (existing - mods_.data()));
        removeTree(old);

        if (keepPriority) {
            record.priority = *keepPriority;
        } else {
            int maxPriority = -1;
            for (const auto& m : mods_)
                if (m.game == record.game) maxPriority = std::max(maxPriority, m.priority);
            record.priority = maxPriority + 1;  // newest install wins by default
        }
    } catch (...) {
        removeTree(staging);
        throw;
    }
    if (!record.installedAt) record.installedAt = static_cast<int64_t>(std::time(nullptr));
    record.enabled = enable;
    mods_.push_back(std::move(record));
    auto id = mods_.back().id;
    save();

    // 3. Copy to the SD card. If that fails part-way, take back what was copied so the game
    //    never sees half a mod; the mod stays in the library, turned off, ready to retry.
    if (enable) {
        try {
            apply(progress);
            const auto* added = find(id);
            if (added && !added->bcmlLogs.empty() && bcmlPending()) throw BcmlMergeError(bcmlError_);
        } catch (const BcmlMergeError& e) {
            findMut(id)->enabled = false;
            try {
                apply();
            } catch (...) {
            }
            save();
            throw;
        } catch (const std::exception& e) {
            findMut(id)->enabled = false;
            try {
                apply();
            } catch (...) {
            }
            save();
            throw std::runtime_error(std::string("couldn't copy the files to the SD card (") + e.what() +
                                     "). The mod is saved but turned off; free up space, then turn it on in My mods.");
        }
    }
    return *find(id);
}

void Library::setEnabled(const std::string& id, bool enabled, const ProgressFn& progress) {
    auto* m = findMut(id);
    if (!m) throw std::invalid_argument("unknown mod " + id);
    if (m->enabled == enabled) return;
    m->enabled = enabled;
    bool bcml = !m->bcmlLogs.empty();
    save();
    apply(progress);
    if (bcml && bcmlPending()) throw BcmlMergeError(bcmlError_);
}

void Library::remove(const std::string& id, const ProgressFn& progress) {
    auto* m = findMut(id);
    if (!m) return;
    m->enabled = false;
    apply(progress);
    m = findMut(id);
    removeTree(modDir(*m));
    mods_.erase(mods_.begin() + (m - mods_.data()));
    save();
}

void Library::dropBackup(const std::string& key) {
    auto it = backedUp_.find(key);
    if (it == backedUp_.end()) return;
    auto path = backupPath(it->second);
    std::error_code ec;
    fs::remove(path, ec);
    pruneEmptyDirs(path.parent_path(), dataDir_ / "backup");
    backedUp_.erase(it);
}

void Library::forget(const std::string& id) {
    auto* m = findMut(id);
    if (!m) return;
    if (!m->enabled) throw std::runtime_error("turn it on first so its files are back on the SD card");
    auto dir = modDir(*m);
    for (const auto& t : m->files) {
        auto key = targetKey(t);
        auto it = deployed_.find(t);
        if (it != deployed_.end() && it->second == id) {
            // Stays on the SD card, now unmanaged; any older original it covered is superseded.
            deployed_.erase(it);
            dropBackup(key);
            continue;
        }
        // Another mod's file is on top: ours becomes the original that comes back later.
        bool covered = false;
        for (const auto& [target, owner] : deployed_)
            if (targetKey(target) == key) covered = true;
        if (covered) {
            dropBackup(key);
            copyFileCreatingDirs(dir / fs::path(t), backupPath(t));
            backedUp_[key] = t;
        }
    }
    removeTree(dir);
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
    if (!target->bcmlLogs.empty() && bcmlPending()) throw BcmlMergeError(bcmlError_);
}

std::map<std::string, std::string> Library::desiredDeployment() const {
    struct Owner {
        int priority;
        std::string id;
        std::string target;
    };
    std::map<std::string, Owner> owners;  // key -> winner
    for (const auto& m : mods_) {
        if (!m.enabled) continue;
        if (!m.bcmlLogs.empty() && !bcmlMerged(m)) continue;  // its partial packs mustn't reach the game
        for (const auto& t : m.files) {
            auto key = targetKey(t);
            auto it = owners.find(key);
            if (it == owners.end() || m.priority > it->second.priority) owners[key] = {m.priority, m.id, t};
        }
    }
    std::map<std::string, std::string> out;
    for (auto& [key, o] : owners) out[o.target] = o.id;
    // Files the BCML merge produced cover every mod's copy (partial packs, ActorInfo…).
    if (!bcmlKey_.empty()) {
        auto owner = kBcmlPrefix + std::to_string(stableId(bcmlKey_));
        for (const auto& t : bcmlOutputs_) {
            for (auto it = out.begin(); it != out.end(); ++it)
                if (targetKey(it->first) == targetKey(t)) {
                    out.erase(it);
                    break;
                }
            out[t] = owner;
        }
    }
    auto merged = sizeTableOwner();
    if (!merged.empty()) {
        for (auto it = out.begin(); it != out.end(); ++it)
            if (targetKey(it->first) == kSizeTableKey) {
                out.erase(it);
                break;
            }
        out[kBotwSizeTable] = merged;
    }
    return out;
}

bool Library::hasOriginalSizeTableOnCard() const {
    if (backedUp_.count(kSizeTableKey)) return true;
    for (const auto& [t, id] : deployed_)
        if (targetKey(t) == kSizeTableKey) return false;  // ours, not the user's
    std::error_code ec;
    return fs::exists(sdRoot_ / kBotwSizeTable, ec);
}

std::string Library::sizeTableOwner() const {
    // Inputs: enabled BOTW mods that ship a table or list sizes, the user's own table, the
    // game's original. Merge when sizes need a base, or when two tables would compete.
    std::string inputs;
    int tables = 0;
    bool sizes = false;
    std::vector<const ModRecord*> contributing;
    for (const auto& m : mods_) {
        if (!m.enabled || m.game != Game::Botw) continue;
        bool shipsTable = std::any_of(m.files.begin(), m.files.end(), [](const std::string& t) { return targetKey(t) == kSizeTableKey; });
        if (!shipsTable && m.resourceSizes.empty()) continue;
        tables += shipsTable;
        sizes |= !m.resourceSizes.empty();
        contributing.push_back(&m);
    }
    bool original = hasOriginalSizeTableOnCard();
    tables += original;
    if (!bcmlSizes_.empty()) sizes = true;
    if (!sizes && tables < 2) return "";
    std::sort(contributing.begin(), contributing.end(), [](auto* a, auto* b) { return a->priority < b->priority; });
    for (const auto* m : contributing) inputs += m->id + "@" + std::to_string(m->installedAt) + ";";
    std::error_code ec;
    if (original) inputs += "card;";
    if (!bcmlSizes_.empty()) inputs += "bcml:" + bcmlKey_ + ";";
    if (!originalSizeTable_.empty() && fs::exists(originalSizeTable_, ec))
        inputs += "game@" + std::to_string(fs::file_size(originalSizeTable_, ec)) + ";";
    return kMergedPrefix + std::to_string(stableId(inputs));
}

bool Library::canMergeSizes(const ModRecord& mod) const {
    // Packages that need merging get size entries for their merged files too.
    if (mod.resourceSizes.empty() && mod.bcmlLogs.empty()) return true;
    std::error_code ec;
    if (!originalSizeTable_.empty() && fs::exists(originalSizeTable_, ec)) return true;
    if (hasOriginalSizeTableOnCard()) return true;
    for (const auto& m : mods_)
        if (m.enabled && m.game == Game::Botw)
            for (const auto& t : m.files)
                if (targetKey(t) == kSizeTableKey) return true;
    return std::any_of(mod.files.begin(), mod.files.end(), [](const std::string& t) { return targetKey(t) == kSizeTableKey; });
}

void Library::writeMergedSizeTable(const fs::path& sdPath) const {
    std::vector<std::string> tables;
    std::error_code ec;
    if (!originalSizeTable_.empty() && fs::exists(originalSizeTable_, ec)) tables.push_back(readFile(originalSizeTable_));
    auto bk = backedUp_.find(kSizeTableKey);
    if (bk != backedUp_.end() && fs::exists(backupPath(bk->second), ec)) tables.push_back(readFile(backupPath(bk->second)));
    std::vector<const ModRecord*> contributing;
    for (const auto& m : mods_)
        if (m.enabled && m.game == Game::Botw) contributing.push_back(&m);
    std::sort(contributing.begin(), contributing.end(), [](auto* a, auto* b) { return a->priority < b->priority; });
    std::vector<std::map<std::string, uint32_t>> sizes;
    for (const auto* m : contributing) {
        for (const auto& t : m->files)
            if (targetKey(t) == kSizeTableKey) tables.push_back(readFile(modDir(*m) / fs::path(t)));
        if (!m->resourceSizes.empty()) sizes.push_back(m->resourceSizes);
    }
    if (!bcmlSizes_.empty()) sizes.push_back(bcmlSizes_);  // the merged files' sizes, last
    if (tables.empty())
        throw std::runtime_error("BOTW's size table is needed to install this mod. Open Patchwork once by holding L and R "
                                 "while starting Breath of the Wild so it can read it.");
    auto merged = mergeSizeTables(tables, sizes);
    auto staged = dataDir_ / "merged" / fs::path(kBotwSizeTable);
    writeFileAtomic(staged, merged);
    replaceFile(staged, sdPath);
}

namespace {

class ReaderGameFiles : public bcml::GameFiles {
public:
    explicit ReaderGameFiles(const Library::GameFileReader& r) : r_(r) {}
    std::optional<std::string> read(const std::string& path) override {
        if (!r_) return std::nullopt;
        auto f = r_(path);
        if (f.state != Library::GameFile::State::Found) return std::nullopt;
        return std::move(f.data);
    }

private:
    const Library::GameFileReader& r_;
};

}  // namespace

bool Library::bcmlMerged(const ModRecord& m) const {
    return bcmlKey_.find(m.id + "@" + std::to_string(m.installedAt) + ";") != std::string::npos;
}

std::string Library::bcmlKey() const {
    std::vector<const ModRecord*> pkgs;
    for (const auto& m : mods_)
        if (m.enabled && m.game == Game::Botw && !m.bcmlLogs.empty()) pkgs.push_back(&m);
    std::sort(pkgs.begin(), pkgs.end(), [](auto* a, auto* b) { return a->priority < b->priority; });
    std::string key;
    for (const auto* m : pkgs) key += m->id + "@" + std::to_string(m->installedAt) + ";";
    if (!key.empty()) key += "lang=" + textLanguage_;
    return key;
}

void Library::refreshBcml() {
    auto key = bcmlKey();
    if (key == bcmlKey_) return;
    auto staging = dataDir_ / "bcml-merged";
    if (key.empty()) {
        bcmlKey_.clear();
        bcmlOutputs_.clear();
        bcmlSizes_.clear();
        removeTree(staging);
        return;
    }
    std::vector<bcml::Package> pkgs;
    std::vector<const ModRecord*> sorted;
    for (const auto& m : mods_)
        if (m.enabled && m.game == Game::Botw && !m.bcmlLogs.empty()) sorted.push_back(&m);
    std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->priority < b->priority; });
    for (const auto* m : sorted) {
        bcml::Package p{m->id, modDir(*m) / "atmosphere" / "contents", {}};
        for (const auto& d : m->bcmlLogs) p.logDirs.push_back(modDir(*m) / "bcml" / d);
        pkgs.push_back(std::move(p));
    }
    ReaderGameFiles game(gameFiles_);
    bcml::Options options;
    options.language = textLanguage_;
    auto result = bcml::merge(pkgs, game, options);
    std::string err;
    if (!removeTree(staging, &err)) throw std::runtime_error(err);
    std::vector<std::string> outputs;
    for (const auto& [path, data] : result.files) {
        writeFileAtomic(staging / fs::path(path), data);
        outputs.push_back(kContentsPrefix + path);
    }
    bcmlKey_ = key;
    bcmlOutputs_ = std::move(outputs);
    bcmlSizes_ = std::move(result.sizes);
    save();
}

std::vector<std::string> Library::missingGameFiles(const std::vector<fs::path>& logDirs) const {
    std::vector<std::string> out;
    bcml::Options options;
    options.language = textLanguage_;
    for (const auto& path : bcml::gameFilesNeeded({{"new", {}, logDirs}}, options)) {
        // A new pack the game doesn't have is fine; everything else must be readable.
        if (gameFiles_ && gameFiles_(path).state != GameFile::State::Unavailable) continue;
        out.push_back(path);
    }
    return out;
}

namespace {

// Replace `to` with a copy of `from` without ever leaving a half-written `to`: copy to a
// temp name first, then swap. A failed copy leaves `to` untouched.
void replaceFile(const fs::path& from, const fs::path& to) {
    auto tmp = to;
    tmp += ".pwtmp";
    try {
        copyFileCreatingDirs(from, tmp);
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
    std::error_code ec;
    fs::rename(tmp, to, ec);  // replaces on most systems
    if (ec) {
        // FAT on the Switch won't rename over an existing file.
        fs::remove(to, ec);
        fs::rename(tmp, to);
    }
}

}  // namespace

// Make the SD card match the enabled mods. Written so an interruption at any point (power
// loss, crash, full card) leaves something a later apply() can finish:
// - a target is recorded as deployed *before* its file is written, so a failed write is
//   cleaned up by the next apply;
// - an original is backed up before being overwritten, and an existing backup is never
//   overwritten (it's the real original from an earlier, interrupted run);
// - a backup is only deleted after it has been copied back, and a missing backup means it
//   was already restored.
// State is saved every 50 files and at the end.
void Library::apply(const ProgressFn& progress) {
    // A merge that can't run (game files missing) doesn't stop the rest: the packages it
    // concerns just stay off the SD card for now.
    try {
        refreshBcml();
        bcmlError_.clear();
    } catch (const std::exception& e) {
        bcmlError_ = e.what();
    }
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

    size_t total = toRemove.size() + toWrite.size(), done = 0, sinceSave = 0;
    auto checkpoint = [&] {
        if (++sinceSave >= 50) {
            save();
            sinceSave = 0;
        }
    };
    try {
        for (const auto& t : toRemove) {
            if (progress) progress(done, total, t);
            auto sdPath = sdRoot_ / fs::path(t);
            std::error_code ec;
            auto bk = backedUp_.find(targetKey(t));
            if (bk != backedUp_.end()) {
                // Put the user's original file back (unless an earlier run already did).
                auto bkPath = backupPath(bk->second);
                if (fs::exists(bkPath, ec)) replaceFile(bkPath, sdPath);
                dropBackup(bk->first);
            } else {
                fs::remove(sdPath, ec);
                pruneEmptyDirs(sdPath.parent_path(), sdRoot_ / "atmosphere");
            }
            deployed_.erase(t);
            ++done;
            checkpoint();
        }
        for (const auto& t : toWrite) {
            if (progress) progress(done, total, t);
            const auto& id = desired.at(t);
            const auto* m = find(id);
            auto sdPath = sdRoot_ / fs::path(t);
            auto key = targetKey(t);
            auto existing = deployedByKey.find(key);
            std::error_code ec;
            if (existing == deployedByKey.end() && fs::exists(sdPath, ec) && !backedUp_.count(key)) {
                // A file we didn't put there: keep it so disabling the mod restores it.
                auto bkPath = backupPath(t);
                if (!fs::exists(bkPath, ec)) copyFileCreatingDirs(sdPath, bkPath);
                backedUp_[key] = t;
            }
            if (existing != deployedByKey.end() && existing->second != t) deployed_.erase(existing->second);
            if (id.rfind(kMergedPrefix, 0) == 0)
                writeMergedSizeTable(sdPath);
            else if (id.rfind(kBcmlPrefix, 0) == 0)
                replaceFile(dataDir_ / "bcml-merged" / fs::path(t.substr(kContentsPrefix.size())), sdPath);
            else
                replaceFile(modDir(*m) / fs::path(t), sdPath);
            deployed_[t] = id;
            ++done;
            checkpoint();
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
           || name.rfind("._", 0) == 0 || name == ".DS_Store" || name == "Thumbs.db" || endsWithI(name, ".pwtmp");
}

}  // namespace

UnmanagedFiles Library::unmanagedFiles(Game game) const {
    UnmanagedFiles out;
    std::set<std::string> managed;
    for (const auto& [t, id] : deployed_) managed.insert(targetKey(t));
    for (const auto& [key, stored] : backedUp_) managed.insert(key);  // counted from the backup below

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
    for (const auto& [key, stored] : backedUp_) {
        auto parts = pathParts(stored);
        if (parts.size() < 3) continue;
        bool forGame = false;
        for (const auto& tid : titleIdsFor(game)) forGame |= iequals(parts[2], tid);
        if (!forGame) continue;
        out.targets.push_back(stored);
        out.bytes += fs::file_size(backupPath(stored), ec);
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

    // Pass 1: copy everything into the library. Nothing else changes, so a failure here
    // (SD full) just removes the partial copy; the originals are untouched.
    auto dir = modDir(r);
    try {
        size_t done = 0;
        for (const auto& t : found.targets) {
            if (progress) progress(done++, found.targets.size(), t);
            auto bk = backedUp_.find(targetKey(t));
            auto from = bk != backedUp_.end() ? backupPath(bk->second) : sdRoot_ / fs::path(t);
            copyFileCreatingDirs(from, dir / fs::path(t));
            r.files.push_back(t);
        }
    } catch (...) {
        removeTree(dir);
        throw;
    }
    // Pass 2: switch ownership over, then save.
    for (const auto& t : r.files) {
        auto key = targetKey(t);
        if (backedUp_.count(key))
            dropBackup(key);  // a mod sits on top; the original now lives in the library instead
        else
            deployed_[t] = r.id;  // already on the SD card, identical
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
            if (targetKey(t) == kSizeTableKey && m->game == Game::Botw) continue;  // merged, not a conflict
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
