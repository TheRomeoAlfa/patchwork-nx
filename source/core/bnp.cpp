#include "bnp.hpp"

#include <algorithm>
#include <sstream>

#include "bcml.hpp"
#include "nlohmann/json.hpp"

namespace mm {

namespace {

// Logs that only describe resource sizes; everything else is a merge.
bool isSizeLog(const std::string& name) { return name == "rstb.json" || name == "rstb.log"; }

void readSizeLogs(const fs::path& logs, std::map<std::string, uint32_t>& out) {
    std::error_code ec;
    if (fs::exists(logs / "rstb.json", ec)) {
        auto j = nlohmann::json::parse(readFile(logs / "rstb.json"));
        for (const auto& [name, size] : j.items())
            if (size.is_number()) out[name] = size.get<uint32_t>();
    } else if (fs::exists(logs / "rstb.log", ec)) {
        // BCML 2: "name,rstb,path" lines.
        std::stringstream lines(readFile(logs / "rstb.log"));
        std::string line;
        std::getline(lines, line);  // header
        while (std::getline(lines, line)) {
            auto a = line.find(','), b = line.find(',', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            try {
                out[line.substr(0, a)] = static_cast<uint32_t>(std::stoul(line.substr(a + 1, b - a - 1)));
            } catch (...) {
            }
        }
    }
}

}  // namespace

bool BnpCheck::installable() const { return bnpInstallable(summary()); }

std::vector<std::string> BnpCheck::summary() const {
    auto out = mergeLogs;
    if (!forSwitch) out.push_back("!wiiu");
    return out;
}

bool bnpInstallable(const std::vector<std::string>& summary) {
    for (const auto& l : summary)
        if (l == "!wiiu" || !bcml::canMerge(l)) return false;
    return true;
}

BnpCheck checkBnp(const std::vector<std::string>& archivePaths) {
    BnpCheck c;
    bool switchDirs = false, wiiuDirs = false;
    for (auto path : archivePaths) {
        std::replace(path.begin(), path.end(), '\\', '/');
        auto parts = pathParts(fs::path(path));
        if (parts.empty()) continue;
        // Options repeat the layout one level down: options/<name>/...
        size_t at = (toLower(parts[0]) == "options" && parts.size() > 2) ? 2 : 0;
        auto top = toLower(parts[at]);
        if (top == "01007ef00011e000" || top == "01007ef00011f001") switchDirs = true;
        if (top == "content" || top == "aoc") wiiuDirs = true;
        if (top == "logs" && parts.size() == at + 2) {
            auto name = parts[at + 1];
            if (!isSizeLog(toLower(name)) && std::find(c.mergeLogs.begin(), c.mergeLogs.end(), name) == c.mergeLogs.end())
                c.mergeLogs.push_back(name);
        }
    }
    c.forSwitch = switchDirs || !wiiuDirs;
    return c;
}

std::map<std::string, uint32_t> readBnpSizes(const fs::path& root, const std::vector<std::string>& optionDirs) {
    std::map<std::string, uint32_t> out;
    readSizeLogs(root / "logs", out);
    for (const auto& opt : optionDirs) readSizeLogs(root / fs::path(opt) / "logs", out);  // options win
    return out;
}

std::string bnpProblem(const BnpCheck& check) {
    if (!check.forSwitch) return "This BCML package is for the Wii U version of the game.";
    std::string what;
    for (const auto& l : check.mergeLogs)
        if (!bcml::canMerge(l)) what += (what.empty() ? "" : ", ") + l;
    return "This BCML package changes parts of the game that Patchwork can't merge yet (" + what +
           "). Install it with BCML on a PC instead.";
}

}  // namespace mm
