#include "installer.hpp"

#include <map>
#include <stdexcept>

#include "bnp.hpp"

namespace mm {

PreparedArchive prepareArchive(const fs::path& archive, const fs::path& workDir, Game game, const std::string& modSlug,
                               const ExtractProgressFn& progress) {
    std::string err;
    if (!removeTree(workDir, &err)) throw std::runtime_error("couldn't clear the unpack folder: " + err);
    PreparedArchive p{workDir, {}, {}};
    if (endsWithI(archive.filename().string(), ".bnp")) {
        // A BCML package: installable when it only needs its resource sizes merged.
        extractArchive(archive, workDir, {}, progress);
        std::vector<std::string> paths;
        for (const auto& e : fs::recursive_directory_iterator(workDir))
            if (e.is_regular_file()) paths.push_back(e.path().lexically_relative(workDir).generic_string());
        auto check = checkBnp(paths);
        if (!check.installable()) {
            p.layout.kind = PackageKind::Bcml;
            p.layout.problem = bnpProblem(check);
            return p;
        }
        p.layout = detectLayout(workDir, game, modSlug);
        for (auto& v : p.layout.variants) {
            // "Main" is the package itself; options are "options/<name>".
            auto root = v.name.rfind("options/", 0) == 0 ? workDir / fs::path(v.name) : workDir;
            v.resourceSizes = readBnpSizes(root);
        }
        return p;
    }
    if (endsWithI(archive.filename().string(), ".tkcl")) {
        p.layout.kind = PackageKind::Tkmm;
        return p;
    }
    extractArchive(archive, workDir, {}, progress, &p.fileTimes);
    extractNestedArchives(workDir);
    p.layout = detectLayout(workDir, game, modSlug);
    return p;
}

std::map<std::string, uint32_t> mergeVariantSizes(const LayoutResult& layout, const std::vector<size_t>& selected) {
    std::map<std::string, uint32_t> out;
    for (auto idx : selected)
        if (idx < layout.variants.size())
            for (const auto& [name, size] : layout.variants[idx].resourceSizes) out[name] = size;
    return out;
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
