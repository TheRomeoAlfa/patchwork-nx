#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mm {

// Yaz0, the compression Nintendo uses for most BOTW files (".s*" extensions).
std::string yaz0Decompress(const std::string& data);  // throws on bad data
// Valid Yaz0 that the game reads, stored without compression (nothing to tune, no bugs).
std::string yaz0Store(const std::string& data);
// Real compression (greedy matching): smaller files, slower than yaz0Store.
std::string yaz0Compress(const std::string& data);
bool isYaz0(std::string_view data);

// BOTW's resource size table (System/Resource/ResourceSizeTable.product.srsizetable): how
// much memory the game reserves for each file, keyed by the CRC32 of its path in the romfs
// ("Actor/Pack/Foo.bactorpack"), with a few colliding names stored in full. Switch layout
// (little endian).
struct SizeTable {
    std::map<uint32_t, uint32_t> crc;          // crc32(name) -> size
    std::map<std::string, uint32_t> named;     // names whose crc collides

    static SizeTable parse(const std::string& data);  // Yaz0 or plain; throws on bad data
    std::string write() const;                         // plain (wrap in yaz0Store for the SD card)

    bool has(const std::string& name) const;
    void remove(const std::string& name);
};

uint32_t sizeTableCrc(const std::string& name);

// Combine complete size tables (the game's original, the user's own file, mods' copies) by
// keeping the largest size per entry, then apply individual sizes from BCML packages in
// order: a size raises the entry (BCML never lowers one), 0 removes it. Tables that can't
// be read (e.g. a Wii U table packed by mistake) are skipped. Returns Yaz0 for the SD card.
std::string mergeSizeTables(const std::vector<std::string>& tables,
                            const std::vector<std::map<std::string, uint32_t>>& sizes = {});

}  // namespace mm
