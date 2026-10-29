#include "installer.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

#include "bnp.hpp"
#include "nlohmann/json.hpp"

namespace mm {

namespace {

// A BCML package's info.json can list its options: add-ons ("multi", each with a default) and
// "pick one" groups ("single"). BCML offers only those and always installs the package itself,
// so folders the list doesn't name are left out. Without a list, every folder stays an option.
void applyBnpOptions(const fs::path& workDir, LayoutResult& layout) {
    nlohmann::json options;
    try {
        options = nlohmann::json::parse(readFile(workDir / "info.json")).at("options");
    } catch (const std::exception&) {
        return;
    }
    if (!options.is_object()) return;
    std::vector<size_t> order;
    auto take = [&](const nlohmann::json& opt) -> Variant* {
        if (!opt.is_object() || !opt.value("folder", nlohmann::json()).is_string()) return nullptr;
        auto name = "options/" + opt["folder"].get<std::string>();
        for (size_t i = 0; i < layout.variants.size(); ++i)
            if (layout.variants[i].name == name && std::find(order.begin(), order.end(), i) == order.end()) {
                order.push_back(i);
                auto& v = layout.variants[i];
                v.label = opt.value("name", nlohmann::json()).is_string() ? opt["name"].get<std::string>() : opt["folder"].get<std::string>();
                return &v;
            }
        return nullptr;
    };
    for (size_t i = 0; i < layout.variants.size(); ++i)
        if (layout.variants[i].name == "Main") {
            layout.variants[i].always = true;
            order.push_back(i);
        }
    if (options.contains("multi") && options["multi"].is_array())
        for (const auto& opt : options["multi"])
            if (auto* v = take(opt)) v->preselected = opt.value("default", nlohmann::json()).is_boolean() && opt["default"].get<bool>();
    int group = 0;
    if (options.contains("single") && options["single"].is_array())
        for (const auto& g : options["single"]) {
            if (!g.is_object() || !g.value("options", nlohmann::json()).is_array()) continue;
            bool required = g.value("required", nlohmann::json()).is_boolean() && g["required"].get<bool>();
            std::string name = g.value("name", nlohmann::json()).is_string() ? g["name"].get<std::string>() : "";
            bool first = true;
            for (const auto& opt : g["options"])
                if (auto* v = take(opt)) {
                    v->group = group;
                    v->groupName = name;
                    v->required = required;
                    v->preselected = required && first;  // as BCML: a required group starts on its first option
                    first = false;
                }
            ++group;
        }
    if (order.empty()) return;
    std::vector<Variant> kept;
    for (auto i : order) kept.push_back(std::move(layout.variants[i]));
    layout.variants = std::move(kept);
}

}  // namespace

PreparedArchive prepareArchive(const fs::path& archive, const fs::path& workDir, Game game, const std::string& modSlug,
                               const ExtractProgressFn& progress) {
    std::string err;
    if (!removeTree(workDir, &err)) throw std::runtime_error("couldn't clear the unpack folder: " + err);
    PreparedArchive p{workDir, {}, {}};
    if (endsWithI(archive.filename().string(), ".bnp")) {
        // A BCML package: installable when Patchwork can merge every log it ships.
        extractArchive(archive, workDir, {}, progress);
        std::vector<std::string> paths;
        for (const auto& e : fs::recursive_directory_iterator(workDir))
            if (e.is_regular_file()) paths.push_back(e.path().lexically_relative(workDir).generic_string());
        auto check = checkBnp(paths);
        if (game != Game::Botw) {
            p.layout.kind = PackageKind::Empty;
            p.layout.problem = "This is a BCML package, made for Breath of the Wild. Switch game with ZL/ZR, then install it again.";
            return p;
        }
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
                if (!hasVariant(name) && mergesSomething(root / "logs")) { Variant v; v.name = name; p.layout.variants.push_back(std::move(v)); }
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
        applyBnpOptions(workDir, p.layout);
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
