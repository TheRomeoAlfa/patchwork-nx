#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "zstd.hpp"

namespace mm::totk {

// Tears of the Kingdom's own files, as TKMM finds them. Mods and TKMM name files by their
// "canonical" name: no ".zs", no version ("GameData/GameDataList.Product.byml"). The game's
// real name comes from its address table and the FileEntry tables ("…Product.121.byml.zs"),
// and many files live only inside a pack (Pack/Actor/X.pack.zs), found with TKMM's index.

// The game version from System/RegionLangMask.txt (121 = 1.2.1), and the build id of its
// executable (the last line).
int parseVersion(std::string_view regionLangMask, std::string* nsoBinaryId = nullptr);

uint32_t xxHash32(std::string_view data, uint32_t seed = 0);
// TKMM hashes names as UTF-16 (little endian).
std::string utf16le(std::string_view utf8);

// Which pack holds each file that only exists inside a pack: TkSharp's
// PackFileLookup.pkcache.zs (MIT), shipped with Patchwork.
class PackIndex {
public:
    static PackIndex load(std::string_view pkcacheZs);
    // The pack's canonical name and its attributes (tkcl::Attributes), if the file is in one.
    std::optional<std::pair<std::string, uint32_t>> packFor(std::string_view canonical) const;
    size_t size() const { return lookup_.size(); }

private:
    std::unordered_map<uint64_t, uint32_t> lookup_;  // (hash, first/last letter) -> pack
    std::vector<std::pair<std::string, uint32_t>> packs_;
};

// Reads the game's own files by their path in its romfs ("Pack/ZsDic.pack.zs"), as stored.
class GameFiles {
public:
    virtual ~GameFiles() = default;
    // False when the game has no such file; throws when it can't be read.
    virtual bool read(const std::string& romfsPath, std::string& out) = 0;
};

// A copy of the game's romfs in a folder (tests, the desktop build).
class FolderGameFiles : public GameFiles {
public:
    explicit FolderGameFiles(std::string root) : root_(std::move(root)) {}
    bool read(const std::string& romfsPath, std::string& out) override;

private:
    std::string root_;
};

// The game files the merge tests need, as copyGameFiles patterns: the name tables, system
// files, data tables, US English text and the actor packs.
const std::vector<std::string>& testFilePatterns();

class Rom {
public:
    // Reads the version, the dictionaries and the name tables. Throws std::runtime_error.
    Rom(GameFiles& files, const PackIndex& packs, size_t packCacheBytes = 64u << 20);

    int version() const { return version_; }
    const std::string& nsoBinaryId() const { return nsoBinaryId_; }
    const zs::Dictionaries& dictionaries() const { return dicts_; }

    // The game's real path for a canonical name.
    std::string relativePath(const std::string& canonical, uint32_t attributes) const;
    // The game's copy of a file, decompressed; nullopt when the game has none. Files inside
    // packs are read from their pack (the last few packs stay in memory).
    std::optional<std::string> vanilla(const std::string& canonical, uint32_t attributes);
    // By real path ("…​.byml.zs"), decompressed.
    std::optional<std::string> vanillaAt(const std::string& relativePath);

private:
    std::shared_ptr<const std::string> pack(const std::string& relativePath);

    GameFiles& files_;
    const PackIndex& packs_;
    int version_ = 0;
    std::string nsoBinaryId_;
    zs::Dictionaries dicts_;
    std::unordered_map<std::string, std::string> addresses_, eventFlows_, effects_, ai_, logic_, sequences_;
    // Decompressed packs, most recently used first.
    size_t packCacheBytes_, packCacheUsed_ = 0;
    std::list<std::pair<std::string, std::shared_ptr<const std::string>>> packCache_;
};

}  // namespace mm::totk
