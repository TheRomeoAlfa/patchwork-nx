#include "library.hpp"

#include <algorithm>
#include <ctime>
#include <new>
#include <set>
#include <stdexcept>

#include "bcml.hpp"
#include "nlohmann/json.hpp"
#include "sarc.hpp"
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
        {"chosen", m.chosen},
        {"package", m.package},
        {"hasOptions", m.hasOptions},
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
    m.chosen = j.value("chosen", std::vector<std::string>{});
    m.package = j.value("package", "");
    // Older state files: options were recorded only when there was a choice (BCML packages
    // list just the options, without the package itself).
    m.hasOptions = j.contains("hasOptions") ? j.value("hasOptions", false)
                                            : m.variants.size() > 1 || (!m.bcmlLogs.empty() && !m.variants.empty() &&
                                                                        !(m.variants.size() == 1 && m.variants[0] == "Main"));
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
// Owner of a target recorded before its file is written (see apply()).
constexpr const char* kWriting = "*writing";
constexpr size_t kBatch = 50;  // files written between state saves

// Space a file takes on the card. FAT hands out whole clusters: 32 KB on SD cards the Switch
// formats as FAT32 (exFAT uses more; the margin below absorbs some of that).
constexpr uint64_t kCluster = 32 * 1024;
// Kept free on top of the files themselves: state saves, the merged size table, folders.
constexpr uint64_t kSpaceMargin = 32ull << 20;

uint64_t sizeOnCard(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return 0;
    auto size = fs::file_size(p, ec);
    return ec ? 0 : (size + kCluster - 1) / kCluster * kCluster;
}

}  // namespace

NotEnoughSpaceError::NotEnoughSpaceError(uint64_t needed, uint64_t free, const std::string& advice)
    : std::runtime_error("Not enough free space on the SD card: this needs about " + formatSize(needed) + " and there's " +
                         formatSize(free) + " free. " + advice),
      needed(needed),
      free(free) {}

Library::Library(fs::path sdRoot, fs::path dataDir) : sdRoot_(std::move(sdRoot)), dataDir_(std::move(dataDir)) {}

void Library::checkSpace(uint64_t needed, const std::string& advice) const {
    if (!freeSpace_) return;
    uint64_t free = freeSpace_();
    if (free && needed > free) throw NotEnoughSpaceError(needed, free, advice);
}

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
    bcmlReads_ = j.value("bcmlReads", std::vector<std::string>{});
    // Merges from before other mods' copies were merged in: merge again at the next apply().
    // Changed past the packages' part of the key, so the last merge stays usable meanwhile.
    if (!j.contains("bcmlReads") && !bcmlKey_.empty()) bcmlKey_ += " (old)";
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
    j["bcmlReads"] = bcmlReads_;
    writeFileAtomic(dataDir_ / "state.json", j.dump(1));
}

fs::path Library::keptPackage(const ModRecord& m) const {
    if (m.package.empty() || !isSafeRelativePath(m.package)) return {};
    std::error_code ec;
    auto p = modDir(m) / fs::path(m.package);
    return fs::is_regular_file(p, ec) ? p : fs::path();
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
    if (files.empty() && internal.empty()) throw std::invalid_argument("mod has no files");

    // 0. Room for the library copy and, if it's turned on, the copy in the game's folder
    //    (less what it replaces there). Checked first so a big mod doesn't fill the card.
    if (freeSpace_) {
        std::set<std::string> ours;
        for (const auto& [t, id] : deployed_) ours.insert(targetKey(t));
        uint64_t need = kSpaceMargin;
        for (const auto& f : internal) need += sizeOnCard(f.source);
        for (const auto& f : files) {
            auto size = sizeOnCard(f.source);
            need += size;
            if (!enable || !isSafeRelativePath(f.target)) continue;
            auto old = sizeOnCard(sdRoot_ / fs::path(f.target));
            auto key = targetKey(f.target);
            if (old && !ours.count(key) && !backedUp_.count(key)) need += old;  // the original's backup
            need += size > old ? size - old : 0;
        }
        checkSpace(need, "Remove some mods or other files and try again.");
    }

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
    bool replacedWasOn = false;  // replacing an enabled copy: it's turned off during the swap
    bool mergeDeferred = false;
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
            // Merges are keyed on it: a reinstall within the same second must still count as new.
            if (!record.installedAt)
                record.installedAt = std::max<int64_t>(static_cast<int64_t>(std::time(nullptr)), existing->installedAt + 1);
            replacedWasOn = existing->enabled;
            existing->enabled = false;
            // A package's own files aren't on the card (its merge is): merging without it only
            // to merge again right after would take twice as long.
            mergeDeferred = deferMerge_ = !existing->bcmlLogs.empty();
            struct Undefer {
                bool& flag;
                ~Undefer() { flag = false; }
            } undefer{deferMerge_};
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
        // The old copy is still the library's (the swap didn't happen): turn it back on.
        if (auto* existing = findMut(record.id); existing && replacedWasOn && !existing->enabled) {
            existing->enabled = true;
            try {
                apply();
            } catch (...) {
                try {
                    save();
                } catch (...) {
                }
            }
        }
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
        } catch (const NotEnoughSpaceError& e) {
            findMut(id)->enabled = false;
            save();  // apply() stopped before changing anything
            throw NotEnoughSpaceError(e.needed, e.free, "The mod is saved but turned off; free up space, then turn it on in My mods.");
        } catch (const std::exception& e) {
            findMut(id)->enabled = false;
            try {
                apply();
            } catch (...) {
            }
            save();
            std::string why = e.what();
            if (auto* fe = dynamic_cast<const fs::filesystem_error*>(&e); fe && fe->code() == std::errc::no_space_on_device)
                why = "the SD card is full";
            throw std::runtime_error("couldn't copy the files to the SD card (" + why +
                                     "). The mod is saved but turned off; turn it on in My mods to try again.");
        }
    } else if (mergeDeferred && replacedWasOn) {
        apply(progress);  // the package was on and isn't now: merge without it
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
    try {
        apply(progress);
    } catch (const NotEnoughSpaceError&) {
        findMut(id)->enabled = !enabled;  // nothing was written: put it back as it was
        save();
        throw;
    }
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
    auto before = ordered;
    ordered.erase(ordered.begin() + idx);
    ordered.insert(ordered.begin() + newIdx, target);
    renumber(ordered);
    save();
    try {
        apply(progress);
    } catch (const NotEnoughSpaceError&) {
        renumber(before);  // nothing was written: keep the old order
        save();
        throw;
    }
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

// What a merge starts from: what the packages merged before made of a file, else another
// mod's copy below every package, else the game's. Keyed by lowercase path. Notes what was read.
class BaseGameFiles : public bcml::GameFiles {
public:
    BaseGameFiles(bcml::GameFiles& game, std::map<std::string, fs::path> mods) : game_(game), mods_(std::move(mods)) {}
    std::optional<std::string> read(const std::string& path) override {
        reads.push_back(path);
        auto key = toLower(path);
        if (auto it = merged.find(key); it != merged.end()) return it->second.second;
        if (auto it = mods_.find(key); it != mods_.end()) return readFile(it->second);
        return game_.read(path);
    }
    std::vector<std::string> reads;
    std::map<std::string, std::pair<std::string, std::string>> merged;  // -> (path, data)

private:
    bcml::GameFiles& game_;
    std::map<std::string, fs::path> mods_;
};

}  // namespace

namespace {

// The packages in a merge key ("id@installedAt;…;lang=USen"), without the language.
std::set<std::string> keyPackages(const std::string& key) {
    std::set<std::string> out;
    size_t start = 0;
    for (size_t end; (end = key.find(';', start)) != std::string::npos; start = end + 1) out.insert(key.substr(start, end - start));
    return out;  // the trailing "lang=…" has no ';' after it
}

}  // namespace

bool Library::bcmlMerged(const ModRecord& m) const {
    return keyPackages(bcmlKey_).count(m.id + "@" + std::to_string(m.installedAt)) > 0;
}

bool Library::bcmlStillUsable() const {
    auto wanted = keyPackages(bcmlKey());
    for (const auto& p : keyPackages(bcmlKey_))
        if (!wanted.count(p)) return false;
    return true;
}

std::map<std::string, std::pair<const ModRecord*, std::string>> Library::bcmlBases() const {
    std::map<std::string, std::pair<const ModRecord*, std::string>> out;
    for (const auto* m : mods(Game::Botw)) {  // highest priority first
        if (!m->enabled || !m->bcmlLogs.empty()) continue;
        for (const auto& t : m->files) {
            auto key = targetKey(t);
            if (key.rfind(kContentsPrefix, 0) == 0) out.emplace(key.substr(kContentsPrefix.size()), std::make_pair(m, t));
        }
    }
    return out;
}

std::string Library::bcmlKey() const {
    std::vector<const ModRecord*> pkgs;
    for (const auto& m : mods_)
        if (m.enabled && m.game == Game::Botw && !m.bcmlLogs.empty()) pkgs.push_back(&m);
    std::sort(pkgs.begin(), pkgs.end(), [](auto* a, auto* b) { return a->priority < b->priority; });
    std::string key;
    for (const auto* m : pkgs) key += m->id + "@" + std::to_string(m->installedAt) + ";";
    if (key.empty()) return key;
    // Other mods' copies the last merge used, and where they sit among the packages: turning
    // one on or off, moving it past a package, or a new one, means merging again.
    auto bases = bcmlBases();
    for (const auto& path : bcmlReads_) {
        auto it = bases.find(toLower(path));
        if (it == bases.end()) continue;
        const auto* m = it->second.first;
        auto below = std::count_if(pkgs.begin(), pkgs.end(), [&](auto* p) { return p->priority < m->priority; });
        key += "base:" + toLower(path) + "=" + m->id + "@" + std::to_string(m->installedAt) + "/" + std::to_string(below) + ";";
    }
    return key + "lang=" + textLanguage_;
}

void Library::refreshBcml() {
    auto key = bcmlKey();
    if (key == bcmlKey_) return;
    auto staging = dataDir_ / "bcml-merged";
    if (key.empty()) {
        bcmlKey_.clear();
        bcmlOutputs_.clear();
        bcmlSizes_.clear();
        bcmlReads_.clear();
        removeTree(staging);
        removeTree(fs::path(staging) += ".new");
        removeTree(fs::path(staging) += ".old");
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
    bcml::Options options;
    options.language = textLanguage_;
    // Other mods' copies of files the merge reads. One below every package is where the merge
    // starts (BCML on a PC puts such mods under the packages). One higher up keeps its place:
    // the packages below it are merged first, its changes are laid over that, and the packages
    // above it are merged on top.
    std::set<std::string> needed;
    for (const auto& p : bcml::gameFilesNeeded(pkgs, options)) needed.insert(toLower(p));
    std::map<std::string, fs::path> startFrom;
    std::map<size_t, std::vector<std::pair<std::string, fs::path>>> layers;  // packages below -> (path, copy)
    for (const auto& [key, owner] : bcmlBases()) {
        auto file = modDir(*owner.first) / fs::path(owner.second);
        auto below = size_t(std::count_if(sorted.begin(), sorted.end(), [&](auto* p) { return p->priority < owner.first->priority; }));
        if (below == 0)
            startFrom[key] = file;
        else if (needed.count(key))
            layers[below].push_back({owner.second.substr(kContentsPrefix.size()), file});
    }
    ReaderGameFiles reader(gameFiles_);
    BaseGameFiles game(reader, std::move(startFrom));
    bcml::Result result;
    size_t runs = 1 + layers.size(), run = 0, done = 0;
    auto mergeUpTo = [&](size_t end) {
        if (end > done) {
            if (mergeProgress_)
                options.progress = [this, run, runs](float f, const std::string& what) { mergeProgress_(0.9f * (run + f) / runs, what); };
            std::vector<bcml::Package> part(pkgs.begin() + done, pkgs.begin() + end);
            auto r = bcml::merge(part, game, options);
            for (auto& [path, data] : r.files) game.merged[toLower(path)] = {path, std::move(data)};
            for (const auto& [name, size] : r.sizes) result.sizes[name] = size;
            done = end;
        }
        ++run;
    };
    for (const auto& [below, copies] : layers) {
        mergeUpTo(below);
        for (const auto& [path, file] : copies) {
            if (mergeProgress_) mergeProgress_(0.9f * run / runs, "Adding " + fs::path(path).filename().string());
            auto top = readFile(file);
            auto original = reader.read(path);
            auto it = game.merged.find(toLower(path));
            std::string current = it != game.merged.end() ? std::move(it->second.second) : original ? *original : std::string();
            std::string layered;
            if (original && isSarc(*original) && isSarc(current) && isSarc(top)) {
                layered = layerSarc(*original, std::move(current), top);
                if (isYaz0(top)) layered = yaz0Compress(layered);
            } else {
                layered = std::move(top);  // not an archive: the higher copy wins whole
            }
            game.merged[toLower(path)] = {path, std::move(layered)};
        }
    }
    mergeUpTo(pkgs.size());
    for (auto& [key, file] : game.merged) result.files[file.first] = std::move(file.second);
    game.merged.clear();
    // Written next to the last merge's files and swapped in once all are there, so a failed
    // write leaves the last merge as it was.
    auto incoming = staging, previous = staging;
    incoming += ".new";
    previous += ".old";
    std::string err;
    if (!removeTree(incoming, &err) || !removeTree(previous, &err)) throw std::runtime_error(err);
    std::vector<std::string> outputs;
    for (const auto& [path, data] : result.files) {
        // merge() only produces these; they become paths on the SD card.
        if (!bcml::isPackagePath(path)) throw std::logic_error("BCML merge produced a path outside the game: " + path);
        writeFileAtomic(incoming / fs::path(path), data);
        outputs.push_back(kContentsPrefix + path);
        if (mergeProgress_) mergeProgress_(0.9f + 0.1f * outputs.size() / result.files.size(), "Saving the merged files");
    }
    // Forget the last merge before its files go: if this is interrupted the next apply()
    // merges again instead of deploying files that don't match the state.
    bcmlKey_.clear();
    save();
    if (fs::exists(staging)) fs::rename(staging, previous);
    if (!result.files.empty()) fs::rename(incoming, staging);
    removeTree(previous);
    removeTree(incoming);
    bcmlOutputs_ = std::move(outputs);
    bcmlReads_ = std::move(game.reads);
    std::sort(bcmlReads_.begin(), bcmlReads_.end());
    bcmlReads_.erase(std::unique(bcmlReads_.begin(), bcmlReads_.end()), bcmlReads_.end());
    bcmlKey_ = bcmlKey();  // with the other mods' copies this merge started from
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
// - files are written in batches; before each, the originals they cover are backed up and
//   the targets are recorded and saved as ours (owner kWriting), so after an interrupted
//   write the next apply rewrites or removes them, and never backs up our own file as the
//   user's;
// - an existing backup is never overwritten (it's the real original from an earlier,
//   interrupted run);
// - a backup is only deleted after it has been copied back, and a missing backup means it
//   was already restored.
// State is also saved every kBatch removals and at the end.
void Library::apply(const ProgressFn& progress) {
    // A merge that can't run (game files missing) doesn't stop the rest: the packages it
    // concerns just stay off the SD card for now.
    try {
        if (!deferMerge_) refreshBcml();
        bcmlError_.clear();
    } catch (const std::exception& e) {
        bcmlError_ = dynamic_cast<const std::bad_alloc*>(&e) ? kOutOfMemory : e.what();
        // The last merge stays on the card while it only lacks packages turned on since (they
        // wait). If it includes one that's now off, removed or reinstalled, it would keep that
        // package's changes in the game: take every merged file off until a merge succeeds.
        if (!bcmlStillUsable()) {
            bcmlKey_.clear();
            bcmlOutputs_.clear();
            bcmlSizes_.clear();
        }
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

    // Where each target's content comes from (none for the merged size table, built in place).
    auto sourceOf = [&](const std::string& t, const std::string& id) -> fs::path {
        if (id.rfind(kMergedPrefix, 0) == 0) return {};
        if (id.rfind(kBcmlPrefix, 0) == 0) return dataDir_ / "bcml-merged" / fs::path(t.substr(kContentsPrefix.size()));
        return modDir(*find(id)) / fs::path(t);
    };

    // Check there's room before changing anything, so a full card can't stop this half-way.
    // Removals run first and free space; originals we haven't backed up yet get copied; a
    // replaced file briefly exists twice (see replaceFile).
    if (freeSpace_) {
        uint64_t need = 0, freed = 0, largest = 0;
        for (const auto& t : toRemove)
            if (!backedUp_.count(targetKey(t))) freed += sizeOnCard(sdRoot_ / fs::path(t));
        for (const auto& t : toWrite) {
            const auto& id = desired.at(t);
            auto sdPath = sdRoot_ / fs::path(t);
            auto key = targetKey(t);
            auto old = sizeOnCard(sdPath);
            auto src = sourceOf(t, id);
            auto size = src.empty() ? std::max(old, sizeOnCard(originalSizeTable_)) : sizeOnCard(src);
            if (old && !deployedByKey.count(key) && !backedUp_.count(key) && !fs::exists(backupPath(t))) need += old;
            need += size > old ? size - old : 0;
            largest = std::max(largest, std::min(size, old));
        }
        need += largest;
        // Turning mods off or rearranging them without needing more room is never refused.
        if (need > freed) checkSpace(need - freed + kSpaceMargin, "Remove some mods or other files and try again.");
    }

    size_t total = toRemove.size() + toWrite.size(), done = 0, sinceSave = 0;
    auto checkpoint = [&] {
        if (++sinceSave >= kBatch) {
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
        for (size_t batch = 0; batch < toWrite.size(); batch += kBatch) {
            size_t end = std::min(toWrite.size(), batch + kBatch);
            // Before writing a batch: back up the user's originals, then record the targets as
            // ours with no owner yet (kWriting), and save. If writing is interrupted, the next
            // apply() rewrites or removes them and never mistakes our file for an original.
            for (size_t i = batch; i < end; ++i) {
                const auto& t = toWrite[i];
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
                deployed_[t] = kWriting;
            }
            save();
            for (size_t i = batch; i < end; ++i) {
                const auto& t = toWrite[i];
                if (progress) progress(done, total, t);
                const auto& id = desired.at(t);
                auto sdPath = sdRoot_ / fs::path(t);
                if (id.rfind(kMergedPrefix, 0) == 0)
                    writeMergedSizeTable(sdPath);
                else
                    replaceFile(sourceOf(t, id), sdPath);
                deployed_[t] = id;
                ++done;
            }
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

std::set<std::string> Library::bcmlMergedTargets() const {
    std::set<std::string> out;
    if (!bcmlKey_.empty())
        for (const auto& t : bcmlOutputs_) out.insert(targetKey(t));
    return out;
}

std::vector<Conflict> Library::conflicts(Game game) const {
    auto merged = game == Game::Botw ? bcmlMergedTargets() : std::set<std::string>{};
    std::map<std::string, Conflict> byKey;
    for (const auto* m : mods(game)) {  // already sorted highest priority first
        if (!m->enabled) continue;
        for (const auto& t : m->files) {
            auto key = targetKey(t);
            if (key == kSizeTableKey && m->game == Game::Botw) continue;  // merged, not a conflict
            if (merged.count(key) && !m->bcmlLogs.empty()) continue;  // part of the merged file
            auto& c = byKey[key];
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
