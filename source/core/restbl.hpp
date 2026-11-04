#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace mm::restbl {

// Tears of the Kingdom's resource size table (System/Resource/ResourceSizeTable.Product.*.
// rsizetable.zs): how much memory each file needs, by the CRC32 of its name (its real path
// without ".zs"), with names spelled out where two hashes collide. The game won't load a
// changed file that needs more than its entry says.
struct Table {
    uint32_t version = 1;
    std::map<uint32_t, uint32_t> hashes;
    std::map<std::string, uint32_t> names;

    static Table parse(std::string_view data);  // throws std::runtime_error
    std::string write() const;
};

uint32_t crc32(std::string_view text);

// The table's name for a file: its real path without ".zs" or ".mc".
std::string resourceName(std::string_view path);

// The size TKMM gives a changed file: its size rounded up to 32 bytes plus what its type
// needs (worked out from the data for .ainb, .asb, .bstar and .mc files).
uint32_t resourceSize(std::string_view path, uint32_t fileSize, std::string_view data);

// Collects the merge's changed files into a copy of the game's table, as TKMM does.
class Collector {
public:
    explicit Collector(Table vanilla) : result_(vanilla), vanilla_(std::move(vanilla)) {}
    // `isVanilla` tells files that are the same as the game's (they keep their entry).
    void setVanillaCheck(std::function<bool(const std::string& path, std::string_view data)> f) { isVanilla_ = std::move(f); }
    // `path`: the real path ("Pack/Actor/X.pack.zs") or, for a file inside a pack, its name
    // there. `data`: the file (decompressed), when there is one to check. `override`: a size
    // the mod asks for (0 = work it out).
    void collect(const std::string& path, uint32_t fileSize, std::string_view data, uint32_t override = 0);
    const Table& result() const { return result_; }

private:
    Table result_, vanilla_;
    std::function<bool(const std::string&, std::string_view)> isVanilla_;
};

}  // namespace mm::restbl
