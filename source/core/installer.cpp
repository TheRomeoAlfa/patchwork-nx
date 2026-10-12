#include "installer.hpp"

#include <map>
#include <stdexcept>

namespace mm {

PreparedArchive prepareArchive(const fs::path& archive, const fs::path& workDir, Game game, const std::string& modSlug,
                               const ExtractProgressFn& progress) {
    std::string err;
    if (!removeTree(workDir, &err)) throw std::runtime_error("couldn't clear the unpack folder: " + err);
    PreparedArchive p{workDir, {}};
    // A .bnp is a 7z with BCML's own layout; don't bother extracting it.
    if (endsWithI(archive.filename().string(), ".bnp")) {
        p.layout.kind = PackageKind::Bcml;
        return p;
    }
    if (endsWithI(archive.filename().string(), ".tkcl")) {
        p.layout.kind = PackageKind::Tkmm;
        return p;
    }
    extractArchive(archive, workDir, {}, progress);
    extractNestedArchives(workDir);
    p.layout = detectLayout(workDir, game, modSlug);
    return p;
}

std::vector<FileMapping> mergeVariants(const LayoutResult& layout, const std::vector<size_t>& selected) {
    std::map<std::string, fs::path> byTarget;
    for (auto idx : selected) {
        if (idx >= layout.variants.size()) continue;
        for (const auto& f : layout.variants[idx].files) byTarget[toLower(f.target)] = f.source;
    }
    // Keep the original casing of the last variant that provided each target.
    std::vector<FileMapping> out;
    for (auto idx : selected) {
        if (idx >= layout.variants.size()) continue;
        for (const auto& f : layout.variants[idx].files) {
            auto it = byTarget.find(toLower(f.target));
            if (it != byTarget.end() && it->second == f.source) {
                out.push_back(f);
                byTarget.erase(it);
            }
        }
    }
    return out;
}

}  // namespace mm
