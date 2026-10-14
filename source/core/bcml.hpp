#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "byml.hpp"
#include "util.hpp"
#include "yaml.hpp"

namespace mm::bcml {

// Full BCML merging: combines every enabled BCML package's changes with the game's own files,
// the way BCML builds its merged "master" mod on a PC. Paths are package-style:
// "01007EF00011E000/romfs/Pack/Bootup.pack" (game) or "01007EF00011F001/romfs/…" (DLC).
inline constexpr const char* kContent = "01007EF00011E000/romfs/";
inline constexpr const char* kDlc = "01007EF00011F001/romfs/";

// The game's own files, read on demand.
class GameFiles {
public:
    virtual ~GameFiles() = default;
    virtual std::optional<std::string> read(const std::string& path) = 0;
};

// Game files copied to a folder (<root>/01007EF00011E000/romfs/…).
class FolderGameFiles : public GameFiles {
public:
    explicit FolderGameFiles(fs::path root) : root_(std::move(root)) {}
    std::optional<std::string> read(const std::string& path) override;

private:
    fs::path root_;
};

// One installed package: its files ("<filesRoot>/01007EF00011E000/romfs/…") and its log folders
// (the package's logs/ first, then those of the chosen options).
struct Package {
    std::string id;
    fs::path filesRoot;
    std::vector<fs::path> logDirs;
};

struct Result {
    std::map<std::string, std::string> files;  // merged files that replace the mods' copies
    std::map<std::string, uint32_t> sizes;     // size table entries for them (romfs names)
    std::vector<std::string> notes;             // things worth logging
};

struct Options {
    std::string language = "USen";  // the game's text language (Bootup_<language>.pack)
    std::function<void(float)> progress;  // 0..1, called between steps (optional)
};

// Packages lowest priority first. Throws if a game file it needs can't be read (the message
// names it).
Result merge(const std::vector<Package>& lowestFirst, GameFiles& game, const Options& options = {});

// The game files merging these packages reads (to copy them while they can be read).
std::set<std::string> gameFilesNeeded(const std::vector<Package>& packages, const Options& options = {});

// Log files Patchwork can merge. Size logs (rstb.json/.log) are handled separately.
bool canMerge(const std::string& logName);
// Changes whenever canMerge() learns something new (for caches of "needs a PC").
std::string mergeSupportKey();

// Conversions between BCML's YAML logs and BYML (oead's "byml text": !u = UInt, !l = Int64,
// !ul = UInt64, !f64 = Double; plain numbers are Int or Float).
byml::Node toByml(const yaml::Node& n);

// "Pack/Bootup.pack//GameData/gamedata.ssarc" style nested paths.
std::vector<std::string> splitNested(const std::string& path);

}  // namespace mm::bcml
