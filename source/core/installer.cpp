#include "installer.hpp"

#include <algorithm>
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
        if (!check.mergeLogs.empty()) {
            // Packages that only change merged data (gamedata.yml, texts.json…) have no files
            // to copy, so the layout found nothing for them: they're still installable.
            auto mergesSomething = [](const fs::path& logs) {
                std::error_code ec;
                for (fs::directory_iterator it(logs, ec), end; it != end; it.increment(ec))
                    if (it->is_regular_file(ec) && !iequals(it->path().filename().string(), "rstb.json")) return true;
                return false;
            };
            auto hasVariant = [&](const std::string& name) {
                return std::any_of(p.layout.variants.begin(), p.layout.variants.end(),
                                   [&](const Variant& v) { return v.name == name; });
            };
            std::vector<std::string> names{"Main"};
            std::error_code ec;
            for (fs::directory_iterator it(workDir / "options", ec), end; it != end; it.increment(ec))
                if (it->is_directory(ec)) names.push_back("options/" + it->path().filename().string());
            std::sort(names.begin() + 1, names.end());
            for (const auto& name : names) {
                auto root = name == "Main" ? workDir : workDir / fs::path(name);
                if (!hasVariant(name) && mergesSomething(root / "logs")) p.layout.variants.push_back({name, {}, {}, {}});
            }
            if (!p.layout.variants.empty()) p.layout.kind = PackageKind::Installable;
        }
        for (auto& v : p.layout.variants) {
            // "Main" is the package itself; options are "options/<name>".
            auto root = v.name.rfind("options/", 0) == 0 ? workDir / fs::path(v.name) : workDir;
            v.resourceSizes = readBnpSizes(root);
            std::error_code ec;
            if (!check.mergeLogs.empty() && fs::is_directory(root / "logs", ec)) v.bcmlLogs = root / "logs";
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

std::vector<FileMapping> variantLogFiles(const LayoutResult& layout, const std::vector<size_t>& selected,
                                         std::vector<std::string>& folders) {
    std::vector<FileMapping> out;
    folders.clear();
    std::vector<size_t> order = selected;
    // The main package's logs first (its variant isn't under options/), then the options'.
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        bool ao = layout.variants.at(a).name.rfind("options/", 0) == 0, bo = layout.variants.at(b).name.rfind("options/", 0) == 0;
        return !ao && bo;
    });
    for (auto idx : order) {
        const auto& logs = layout.variants.at(idx).bcmlLogs;
        if (logs.empty()) continue;
        auto folder = std::to_string(folders.size());
        folders.push_back(folder);
        std::error_code ec;
        for (fs::recursive_directory_iterator it(logs, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec))
                out.push_back({it->path(), "bcml/" + folder + "/" + it->path().lexically_relative(logs).generic_string()});
    }
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
