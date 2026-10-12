#pragma once

#include <vector>

#include "archive.hpp"
#include "layout.hpp"

namespace mm {

struct PreparedArchive {
    fs::path extractDir;
    LayoutResult layout;
    FileTimes fileTimes;  // of the outer archive's files, relative to extractDir
};

// Extract an archive into `workDir` (wiped first) and work out where its files go.
PreparedArchive prepareArchive(const fs::path& archive, const fs::path& workDir, Game game, const std::string& modSlug,
                               const ExtractProgressFn& progress = {});

// Combine the chosen variants into one file list; later selections win on overlap.
std::vector<FileMapping> mergeVariants(const LayoutResult& layout, const std::vector<size_t>& selected);
// The resource sizes (BCML packages) of the chosen variants.
std::map<std::string, uint32_t> mergeVariantSizes(const LayoutResult& layout, const std::vector<size_t>& selected);

}  // namespace mm
