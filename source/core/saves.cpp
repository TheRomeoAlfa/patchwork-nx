#include "saves.hpp"

#include <algorithm>
#include <ctime>
#include <stdexcept>

#include "nlohmann/json.hpp"

namespace mm {

namespace {

// The file every slot has: TOTK's progress, BOTW's game data.
const char* markerFile(Game game) { return game == Game::Totk ? "progress.sav" : "game_data.sav"; }

// Folders shared by every slot: in-game photos, and BOTW's compendium pictures.
std::vector<std::string> photoFolders(Game game) {
    if (game == Game::Totk) return {"album"};
    return {"album", "pict_book"};
}

bool isSlotFile(const fs::path& p) { return endsWithI(p.filename().string(), ".sav") || endsWithI(p.filename().string(), ".jpg"); }

std::vector<fs::path> filesUnder(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) out.push_back(it->path());
    if (ec) throw std::runtime_error("can't read " + dir.string() + ": " + ec.message());
    std::sort(out.begin(), out.end());
    return out;
}

void removeOrThrow(const fs::path& p) {
    std::string error;
    if (!removeTree(p, &error)) throw std::runtime_error(error);
}

}  // namespace

const std::vector<SaveSlot>& saveSlots(Game game) {
    // BOTW: 0-5 are normal mode, 6-7 Master Mode. TOTK: six slots.
    static const std::vector<SaveSlot> botw = {
        {"0", "Slot 1", false}, {"1", "Slot 2", false}, {"2", "Slot 3", false},         {"3", "Slot 4", false},
        {"4", "Slot 5", false}, {"5", "Slot 6", false}, {"6", "Master Mode 1", true}, {"7", "Master Mode 2", true},
    };
    static const std::vector<SaveSlot> totk = {
        {"slot_00", "Slot 1", false}, {"slot_01", "Slot 2", false}, {"slot_02", "Slot 3", false},
        {"slot_03", "Slot 4", false}, {"slot_04", "Slot 5", false}, {"slot_05", "Slot 6", false},
    };
    return game == Game::Totk ? totk : botw;
}

const SaveSlot* findSaveSlot(Game game, const std::string& folder) {
    for (const auto& s : saveSlots(game))
        if (iequals(s.folder, folder)) return &s;
    return nullptr;
}

bool SaveCandidate::masterMode() const {
    // Only BOTW has Master Mode slots; its slot folders are digits.
    return slot == "6" || slot == "7";
}

std::vector<SaveCandidate> findSaves(const fs::path& root, Game game, const FileTimes* times) {
    std::vector<SaveCandidate> out;
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && iequals(it->path().filename().string(), markerFile(game)))
            dirs.push_back(it->path().parent_path());
    std::sort(dirs.begin(), dirs.end());
    for (const auto& dir : dirs) {
        SaveCandidate c;
        c.dir = dir;
        auto rel = dir.lexically_relative(root);
        auto parts = pathParts(rel);
        if (!parts.empty() && parts.back() != "." && findSaveSlot(game, parts.back())) {
            c.slot = findSaveSlot(game, parts.back())->folder;
            parts.pop_back();
            c.photosRoot = dir.parent_path();
        } else {
            c.photosRoot = dir;
        }
        // Title id folders ("0100F2C0115B6000") say nothing about the save.
        std::string name;
        for (const auto& p : parts) {
            if (p == "." || (p.size() == 16 && isHex(p))) continue;
            auto part = p;
            // A zip inside the download is unpacked into "<name>.zip.extracted".
            if (endsWithI(part, ".extracted")) {
                part.resize(part.size() - 10);
                auto dot = part.rfind('.');
                if (dot != std::string::npos && dot > 0) part.resize(dot);
            }
            name += (name.empty() ? "" : " / ") + part;
        }
        c.name = name;
        if (times)
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                auto t = times->find(it->path().lexically_relative(root).generic_string());
                if (t != times->end()) c.savedTime = std::max(c.savedTime, t->second);
            }
        for (const auto& pd : photoFolders(game)) {
            auto d = c.photosRoot / pd;
            if (fs::is_directory(d, ec) && !filesUnder(d).empty()) c.photoDirs.push_back(pd);
        }
        out.push_back(std::move(c));
    }
    return out;
}

size_t newestSave(const std::vector<SaveCandidate>& saves) {
    size_t best = 0;
    for (size_t i = 1; i < saves.size(); ++i)
        if (saves[i].savedTime > saves[best].savedTime) best = i;
    return best;
}

bool slotInUse(const fs::path& saveRoot, const std::string& slotFolder) {
    std::error_code ec;
    auto dir = saveRoot / slotFolder;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) return true;
    return false;
}

void writeSaveSlot(const SaveCandidate& save, const fs::path& saveRoot, const std::string& slotFolder, bool photos,
                   const CommitFn& commit, const SaveProgressFn& progress) {
    std::vector<std::pair<fs::path, fs::path>> copies;
    std::error_code ec;
    for (fs::directory_iterator it(save.dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && isSlotFile(it->path())) copies.emplace_back(it->path(), saveRoot / slotFolder / it->path().filename());
    if (ec) throw std::runtime_error("can't read the save: " + ec.message());
    if (copies.empty()) throw std::runtime_error("the save has no files");
    std::sort(copies.begin(), copies.end());
    if (photos)
        for (const auto& pd : save.photoDirs)
            for (const auto& f : filesUnder(save.photosRoot / pd))
                copies.emplace_back(f, saveRoot / pd / f.lexically_relative(save.photosRoot / pd));

    // Clear the slot first: a save without a file the old one had (footprint.sav, say)
    // must not end up mixed with the old one's.
    removeOrThrow(saveRoot / slotFolder);
    if (photos)
        for (const auto& pd : save.photoDirs) removeOrThrow(saveRoot / pd);
    if (commit) commit();
    for (size_t i = 0; i < copies.size(); ++i) {
        copyFileCreatingDirs(copies[i].first, copies[i].second);
        if (commit) commit();
        if (progress) progress(i + 1, copies.size());
    }
}

void backupSave(const fs::path& saveRoot, const fs::path& dest, const SaveProgressFn& progress) {
    auto files = filesUnder(saveRoot);
    if (files.empty()) throw std::runtime_error("the save is empty");
    for (size_t i = 0; i < files.size(); ++i) {
        copyFileCreatingDirs(files[i], dest / files[i].lexically_relative(saveRoot));
        if (progress) progress(i + 1, files.size());
    }
    // Check the copy before anything in the save is touched.
    std::error_code ec;
    for (const auto& f : files) {
        auto copy = dest / f.lexically_relative(saveRoot);
        if (fs::file_size(copy, ec) != fs::file_size(f, ec) || ec)
            throw std::runtime_error("the backup of " + f.lexically_relative(saveRoot).generic_string() + " doesn't match");
    }
}

void restoreSave(const fs::path& backup, const fs::path& saveRoot, const CommitFn& commit, const SaveProgressFn& progress) {
    auto files = filesUnder(backup);
    if (files.empty()) throw std::runtime_error("the backup is empty");
    std::vector<fs::path> existing;
    std::error_code ec;
    for (fs::directory_iterator it(saveRoot, ec), end; !ec && it != end; it.increment(ec)) existing.push_back(it->path());
    for (const auto& e : existing) removeOrThrow(e);
    if (commit) commit();
    for (size_t i = 0; i < files.size(); ++i) {
        copyFileCreatingDirs(files[i], saveRoot / files[i].lexically_relative(backup));
        if (commit) commit();
        if (progress) progress(i + 1, files.size());
    }
}

fs::path newBackupDir(const fs::path& backupsRoot, Game game, const std::string& userId, int64_t time) {
    std::time_t t = static_cast<std::time_t>(time);
    std::tm tm{};
    localtime_r(&t, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    auto base = backupsRoot / std::string(gameInfo(game).key) / (std::string(stamp) + "-" + slugify(userId, 16));
    auto dir = base;
    std::error_code ec;
    for (int n = 2; fs::exists(dir, ec); ++n) dir = base.string() + "-" + std::to_string(n);
    return dir;
}

void writeBackupInfo(const SaveBackup& b) {
    nlohmann::json j = {{"game", std::string(gameInfo(b.game).key)}, {"userId", b.userId}, {"userName", b.userName},
                        {"reason", b.reason}, {"time", b.time}};
    writeFileAtomic(b.dir / "info.json", j.dump(2));
}

std::vector<SaveBackup> listSaveBackups(const fs::path& backupsRoot, Game game) {
    std::vector<SaveBackup> out;
    std::error_code ec;
    for (fs::directory_iterator it(backupsRoot / std::string(gameInfo(game).key), ec), end; !ec && it != end; it.increment(ec)) {
        auto info = it->path() / "info.json";
        std::error_code e2;
        if (!fs::exists(info, e2) || !fs::is_directory(it->path() / "save", e2)) continue;
        try {
            auto j = nlohmann::json::parse(readFile(info));
            SaveBackup b;
            b.dir = it->path();
            b.game = game;
            b.userId = j.value("userId", "");
            b.userName = j.value("userName", "");
            b.reason = j.value("reason", "");
            b.time = j.value("time", int64_t(0));
            out.push_back(std::move(b));
        } catch (const std::exception&) {
        }
    }
    std::sort(out.begin(), out.end(), [](const SaveBackup& a, const SaveBackup& b) { return a.time > b.time; });
    return out;
}

}  // namespace mm
