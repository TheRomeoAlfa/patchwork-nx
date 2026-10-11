#pragma once

#include <vector>

#include "archive.hpp"
#include "layout.hpp"

namespace mm {

struct PreparedArchive {
    fs::path extractDir;
    LayoutResult layout;
};

// Extract an archive into `workDir` (wiped first) and work out where its files go.
PreparedArchive prepareArchive(const fs::path& archive, const fs::path& workDir, Game game, const std::string& modSlug,
                               const ExtractProgressFn& progress = {});

// Combine the chosen variants into one file list; later selections win on overlap.
std::vector<FileMapping> mergeVariants(const LayoutResult& layout, const std::vector<size_t>& selected);

}  // namespace mm
