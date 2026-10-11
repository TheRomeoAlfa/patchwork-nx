#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "util.hpp"

namespace mm {

struct ExtractLimits {
    uint64_t maxTotalBytes = 8ull << 30;  // refuse archives that unpack to more than this
    uint32_t maxEntries = 200000;
};

// Called with bytes written so far; return false to cancel.
using ExtractProgressFn = std::function<bool(uint64_t bytesWritten, const std::string& current)>;

// Extract a zip / 7z / rar (any format libarchive reads) into `destDir`.
// Entries with absolute paths, "..", or link types are skipped. Throws on error.
void extractArchive(const fs::path& archive, const fs::path& destDir, const ExtractLimits& limits = {},
                    const ExtractProgressFn& progress = {});

// Archives nested inside an extracted mod (a zip of zips) are unpacked one level,
// each into "<name>.extracted/" next to it. BCML/TKMM packages are left alone.
void extractNestedArchives(const fs::path& dir, const ExtractLimits& limits = {});

bool isSupportedArchiveName(const std::string& filename);

}  // namespace mm
