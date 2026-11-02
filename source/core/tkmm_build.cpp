#include "tkmm_build.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "byml.hpp"
#include "byml_merge.hpp"
#include "msbt.hpp"
#include "sarc.hpp"
#include "tkmm_data.hpp"
#include "zstd.hpp"

namespace mm::tkmm {

namespace {

using byml::Node;
using T = Node::Type;

const std::string kRemoved = "TKSCRMVD";

bool endsWith(std::string_view s, std::string_view e) { return s.size() >= e.size() && s.substr(s.size() - e.size()) == e; }

std::string_view extensionOf(std::string_view name) {
    auto slash = name.rfind('/');
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    return name.substr(dot);
}

// Which of TKMM's changelog builders a file gets (GetChangelogBuilder); Copy = none.
enum class Builder { Copy, Byml, RsdbRows, RsdbTags, GameData, Msbt, Sarc, Pack };

Builder builderFor(std::string_view canonical) {
    auto ext = extensionOf(canonical);
    if (ext == ".pack") return Builder::Pack;
    if (canonical == "GameData/GameDataList.Product.byml") return Builder::GameData;
    if (canonical == "RSDB/Tag.Product.rstbl.byml") return Builder::RsdbTags;
    if (!rsdbRowKey(canonical).empty()) return Builder::RsdbRows;
    if (ext == ".msbt") return Builder::Msbt;
    if (ext == ".bfarc" || ext == ".bkres" || ext == ".blarc" || ext == ".genvb" || ext == ".sarc" || ext == ".ta") return Builder::Sarc;
    if (ext == ".bgyml") return Builder::Byml;
    if (ext == ".byml" && canonical.substr(0, 4) != "RSDB" && canonical.substr(0, 8) != "GameData") return Builder::Byml;
    return Builder::Copy;  // (combined textures too: Patchwork can't merge them)
}

// One builder run: a file of the mod against the game's copy. Returns the changelog file, or
// nullopt when nothing changed.
std::optional<std::string> buildOne(Builder b, const std::string& canonical, const std::string& data, const std::string& vanilla);

std::optional<std::string> buildRsdbRows(const std::string& canonical, const std::string& data, const std::string& vanilla) {
    auto key = rsdbRowKey(canonical);
    auto src = byml::parse(data);
    auto game = byml::parse(vanilla);
    std::unordered_map<std::string, const Node*> byKey;
    auto keyText = [&](const Node& row, bool* hashed, uint64_t* u) -> std::optional<std::string> {
        const Node* k = row.type == T::Dict ? row.find(key) : nullptr;
        if (!k) return std::nullopt;
        if (k->type == T::String) return "$" + k->s;
        *hashed = true;
        *u = k->type == T::Int || k->type == T::Int64 ? uint64_t(uint32_t(k->i)) : k->u;
        return "#" + std::to_string(*u);
    };
    for (const auto& row : game.root.array) {
        bool h = false;
        uint64_t u = 0;
        if (auto k = keyText(row, &h, &u)) byKey[*k] = &row;
    }
    Node changelog = Node::makeDict();
    bool hashed = false;
    for (auto& row : src.root.array) {
        bool h = false;
        uint64_t u = 0;
        auto k = keyText(row, &h, &u);
        if (!k) continue;
        hashed |= h;
        Node entry = row;
        auto it = byKey.find(*k);
        if (it != byKey.end() && byml::logChanges(byml::fileType(canonical), entry, *it->second)) continue;
        if (h) {
            changelog.type = T::HashMap32;
            changelog.hashes[u] = std::move(entry);
        } else {
            changelog.dict[k->substr(1)] = std::move(entry);
        }
    }
    if (changelog.dict.empty() && changelog.hashes.empty()) return std::nullopt;
    if (hashed) changelog.type = T::HashMap32;
    byml::Document out;
    out.version = src.version;
    out.bigEndian = src.bigEndian;
    out.root = std::move(changelog);
    return byml::write(out);
}

std::optional<std::string> buildRsdbTags(const std::string& data, const std::string& vanilla) {
    auto read = [](const Node& root, std::vector<std::string>& tags) {
        std::map<std::tuple<std::string, std::string, std::string>, std::vector<std::string>> entries;
        const Node* paths = root.find("PathList");
        const Node* list = root.find("TagList");
        const Node* bits = root.find("BitTable");
        if (!paths || !list || !bits) throw std::runtime_error("RSDB: a tag table is damaged");
        for (const auto& t : list->array) tags.push_back(t.s);
        for (size_t e = 0; e * 3 + 2 < paths->array.size(); ++e) {
            auto& out = entries[{paths->array[3 * e].s, paths->array[3 * e + 1].s, paths->array[3 * e + 2].s}];
            for (size_t t = 0; t < tags.size(); ++t) {
                size_t bit = e * tags.size() + t;
                if (bit / 8 < bits->s.size() && (uint8_t(bits->s[bit / 8]) >> (bit % 8)) & 1) out.push_back(tags[t]);
            }
            std::sort(out.begin(), out.end());
        }
        return entries;
    };
    auto srcDoc = byml::parse(data);
    std::vector<std::string> srcTags, gameTags;
    auto src = read(srcDoc.root, srcTags);
    auto game = read(byml::parse(vanilla).root, gameTags);
    Node entries = Node::makeArray(), newTags = Node::makeArray();
    std::set<std::string> known(gameTags.begin(), gameTags.end());
    for (const auto& t : srcTags)
        if (!known.count(t)) newTags.array.push_back(Node::str(t));
    for (const auto& [key, tags] : src) {
        auto it = game.find(key);
        Node value;
        if (it == game.end()) {
            value = Node::makeArray();
            for (const auto& t : tags) value.array.push_back(Node::str(t));
        } else {
            // Sorted lists: tags added and removed, as TKMM's CreateChangelog.
            value.type = T::ArrayChangelog;
            std::vector<std::string> added, removed;
            std::set_difference(tags.begin(), tags.end(), it->second.begin(), it->second.end(), std::back_inserter(added));
            std::set_difference(it->second.begin(), it->second.end(), tags.begin(), tags.end(), std::back_inserter(removed));
            for (auto* list : {&removed, &added})
                for (const auto& t : *list) {
                    byml::ArrayChange c;
                    c.change = list == &added ? Node::Change::Add : Node::Change::Remove;
                    c.node = Node::str(t);
                    c.key.type = c.key2.type = T::None;
                    value.changes.push_back(std::move(c));
                }
            if (value.changes.empty()) continue;
        }
        entries.array.push_back(Node::str(std::get<0>(key)));
        entries.array.push_back(Node::str(std::get<1>(key)));
        entries.array.push_back(Node::str(std::get<2>(key)));
        entries.array.push_back(std::move(value));
    }
    if (entries.array.empty() && newTags.array.empty()) return std::nullopt;
    byml::Document out;
    out.version = srcDoc.version;
    out.bigEndian = srcDoc.bigEndian;
    out.root = Node::makeDict();
    out.root.dict["Entries"] = std::move(entries);
    out.root.dict["Tags"] = std::move(newTags);
    return byml::write(out);
}

std::optional<std::string> buildGameData(const std::string& canonical, const std::string& data, const std::string& vanilla) {
    auto src = byml::parse(data);
    auto game = byml::parse(vanilla);
    const Node* srcData = src.root.find("Data");
    const Node* gameData = game.root.find("Data");
    if (!srcData || !gameData) throw std::runtime_error("game data: a game data list is damaged");
    Node changelog = Node::makeDict();
    for (const auto& [table, rows] : srcData->dict) {
        const Node* gameRows = gameData->find(table);
        if (!gameRows) continue;
        bool wide = table == "Bool64bitKey";
        std::unordered_map<uint64_t, const Node*> byHash;
        for (const auto& r : gameRows->array)
            if (const Node* h = r.type == T::Dict ? r.find("Hash") : nullptr) byHash[h->u] = &r;
        Node changes;
        changes.type = wide ? T::HashMap64 : T::HashMap32;
        std::set<uint64_t> seen;
        for (const auto& r : rows.array) {
            const Node* h = r.type == T::Dict ? r.find("Hash") : nullptr;
            if (!h || h->type != (wide ? T::UInt64 : T::UInt)) continue;
            Node entry = r;
            auto it = byHash.find(h->u);
            if (it != byHash.end()) {
                seen.insert(h->u);
                if (byml::logChanges(table == "Struct" ? "Struct" : "", entry, *it->second, 0)) continue;
            }
            changes.hashes[h->u] = std::move(entry);
        }
        if (rows.array.size() != gameRows->array.size())
            for (const auto& [hash, row] : byHash)
                if (!seen.count(hash)) changes.hashes[hash] = Node::removed();
        if (!changes.hashes.empty()) changelog.dict[table] = std::move(changes);
    }
    (void)canonical;
    if (changelog.dict.empty()) return std::nullopt;
    byml::Document out;
    out.version = game.version;
    out.bigEndian = game.bigEndian;
    out.root = std::move(changelog);
    return byml::write(out);
}

std::optional<std::string> buildMsbt(const std::string& data, const std::string& vanilla) {
    auto src = msbt::parse(data);
    auto game = msbt::parse(vanilla);
    msbt::File out = src;
    out.messages.clear();
    for (const auto& [label, m] : src.messages) {
        const auto* g = game.find(label);
        if (g && g->text == m.text && g->attribute == m.attribute) continue;
        out.messages.emplace_back(label, m);
    }
    if (out.messages.empty()) return std::nullopt;
    return msbt::write(out);
}

std::optional<std::string> buildSarc(const std::string& canonical, const std::string& data, const std::string& vanilla) {
    auto src = Sarc::parse(data);
    auto game = Sarc::parse(vanilla);
    Sarc changelog;
    changelog.littleEndian = src.littleEndian;
    for (auto& f : src.files) {
        const auto* g = game.find(f.name);
        if (g && g->data == f.data) continue;
        auto b = builderFor(f.name);
        if (!g || b == Builder::Copy || b == Builder::Pack) {
            changelog.files.push_back(std::move(f));
            continue;
        }
        auto nested = buildOne(b, b == Builder::Sarc ? canonical + "/" + f.name : f.name, f.data, g->data);
        if (nested) changelog.files.push_back({f.name, std::move(*nested)});
    }
    if (changelog.files.empty()) return std::nullopt;
    return changelog.write();
}

std::optional<std::string> buildOne(Builder b, const std::string& canonical, const std::string& data, const std::string& vanilla) {
    switch (b) {
        case Builder::Byml: return byml::buildChangelog(canonical, data, vanilla);
        case Builder::RsdbRows: return buildRsdbRows(canonical, data, vanilla);
        case Builder::RsdbTags: return buildRsdbTags(data, vanilla);
        case Builder::GameData: return buildGameData(canonical, data, vanilla);
        case Builder::Msbt: return buildMsbt(data, vanilla);
        case Builder::Sarc: return buildSarc(canonical, data, vanilla);
        default: return data == vanilla ? std::nullopt : std::optional<std::string>(data);
    }
}

struct Collector {
    const fs::path& out;
    tkcl::Changelog changelog;
    std::map<std::string, size_t> index;  // canonical -> entry

    // Records an entry (TKMM's AddChangelogMetadata) and returns where its file goes.
    fs::path add(const std::string& canonical, tkcl::EntryType type, uint32_t attributes, int dict, int version,
                 const std::string* archive = nullptr) {
        auto [it, fresh] = index.try_emplace(canonical, changelog.files.size());
        if (fresh) {
            tkcl::Entry e;
            e.canonical = canonical;
            e.attributes = attributes;
            e.zsDictionaryId = dict;
            changelog.files.push_back(std::move(e));
        }
        auto& e = changelog.files[it->second];
        e.type = type;
        std::string stored = canonical;
        if (version != -1) {
            if (std::find(e.versions.begin(), e.versions.end(), version) == e.versions.end()) e.versions.push_back(version);
            stored += std::to_string(version);
        }
        if (archive && std::find(e.archiveCanonicals.begin(), e.archiveCanonicals.end(), *archive) == e.archiveCanonicals.end())
            e.archiveCanonicals.push_back(*archive);
        return out / "romfs" / fs::path(stored);
    }
};

void buildPack(Collector& c, totk::Rom& rom, const std::string& canonical, uint32_t attributes, int dict, int version,
               const std::string& data, const std::optional<std::string>& vanilla) {
    auto src = Sarc::parse(data);
    std::optional<Sarc> game;
    if (vanilla) game = Sarc::parse(*vanilla);
    Sarc removals;
    for (const auto& f : src.files) {
        if (f.data == kRemoved) throw std::runtime_error(canonical + " has a file marked as removed");
        const SarcFile* inPack = game ? game->find(f.name) : nullptr;
        std::optional<std::string> elsewhere;
        if (!inPack) elsewhere = rom.vanilla(f.name, 0);  // shared with other packs
        const std::string* v = inPack ? &inPack->data : elsewhere ? &*elsewhere : nullptr;
        if (!v) {
            writeFileAtomic(c.add(f.name, tkcl::EntryType::Copy, attributes, dict, version, &canonical), f.data);
            continue;
        }
        if (f.data == *v) {
            // Kept as the game has it: the merge puts the game's copy in (TKMM's placeholder),
            // unless it was there already.
            if (!inPack) c.add(f.name, tkcl::EntryType::Placeholder, attributes, dict, -1, &canonical);
            continue;
        }
        auto b = builderFor(f.name);
        if (b == Builder::Copy || b == Builder::Pack) {
            writeFileAtomic(c.add(f.name, tkcl::EntryType::Copy, attributes, dict, version, &canonical), f.data);
            continue;
        }
        if (auto changes = buildOne(b, f.name, f.data, *v))
            writeFileAtomic(c.add(f.name, tkcl::EntryType::Changelog, attributes, dict, version, &canonical), *changes);
    }
    if (game)
        for (const auto& g : game->files)
            if (!src.find(g.name)) removals.files.push_back({g.name, kRemoved});
    if (!removals.files.empty())
        writeFileAtomic(c.add(canonical, tkcl::EntryType::Changelog, attributes, dict, version), removals.write());
}

}  // namespace

std::string canonicalName(std::string_view p, int* versionOut, uint32_t* attributesOut) {
    int version = -1;
    uint32_t attributes = 0;
    if (endsWith(p, ".zs")) {
        attributes |= tkcl::HasZsExtension;
        p.remove_suffix(3);
    } else if (endsWith(p, ".mc")) {
        attributes |= tkcl::HasMcExtension;
        p.remove_suffix(3);
    }
    std::string lower(p);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    bool pchtxt = endsWith(lower, ".pchtxt"), txtg = endsWith(p, ".txtg");
    auto digits = [&](size_t at) { return at + 3 <= p.size() && std::isdigit(uint8_t(p[at])) && std::isdigit(uint8_t(p[at + 1])) && std::isdigit(uint8_t(p[at + 2])); };
    std::string out;
    out.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        char c = p[i];
        if (c == '.' && !pchtxt) {
            if (p.size() - i > 2 && p[i + 1] == '1' && !txtg && digits(i + 1)) {  // ".121"
                attributes |= tkcl::IsProductFile;
                version = std::stoi(std::string(p.substr(i + 1, 3)));
                i += 3;
                continue;
            }
            if (p.size() - i > 8 && p.substr(i, 8) == ".Product") {  // ".Product.121"
                attributes |= tkcl::IsProductFile;
                out += ".Product";
                i += 7;
                if (i + 1 < p.size() && p[i + 1] == '.' && digits(i + 2)) {
                    version = std::stoi(std::string(p.substr(i + 2, 3)));
                    i += 4;
                }
                continue;
            }
        }
        out += c == '\\' ? '/' : c;
    }
    if (!out.empty() && out[0] == '/') out.erase(0, 1);
    if (versionOut) *versionOut = version;
    if (attributesOut) *attributesOut = attributes;
    return out;
}

tkcl::Changelog buildChangelog(const std::vector<std::pair<std::string, fs::path>>& files, totk::Rom& rom, const fs::path& outDir) {
    Collector c{outDir, {}, {}};
    c.changelog.builderVersion = 200;
    c.changelog.gameVersion = rom.version();
    for (const auto& [path, file] : files) {
        if (endsWith(path, ".rsizetable") || endsWith(path, ".rsizetable.zs") || endsWith(path, "desktop.ini") || endsWith(path, ".DS_Store")) continue;
        int version = -1;
        uint32_t attributes = 0;
        auto canonical = canonicalName(path, &version, &attributes);
        auto raw = readFile(file);
        int dict = -1;
        std::string data = raw;
        if (zs::isZstd(raw)) {
            dict = int(zs::frameDictionary(raw));
            data = rom.dictionaries().decompress(raw);
        }
        auto b = builderFor(canonical);
        bool mals = canonical.rfind("Mals/", 0) == 0;
        std::optional<std::string> vanilla = rom.vanilla(canonical, attributes);
        if (b == Builder::Copy || (!vanilla && b != Builder::Pack)) {
            if (mals) {
                // A whole text archive, made for a game this one doesn't have: kept as it is.
                c.changelog.malsFiles.push_back(canonical);
                writeFileAtomic(outDir / "romfs" / fs::path(canonical), data);
                continue;
            }
            writeFileAtomic(c.add(canonical, tkcl::EntryType::Copy, attributes, dict, version), raw);
            continue;
        }
        if (vanilla && *vanilla == data) continue;
        if (b == Builder::Pack) {
            buildPack(c, rom, canonical, attributes, dict, version, data, vanilla);
            continue;
        }
        auto changes = buildOne(b, canonical, data, *vanilla);
        if (!changes) continue;
        if (mals) {
            c.changelog.malsFiles.push_back(canonical);
            writeFileAtomic(outDir / "romfs" / fs::path(canonical), *changes);
            continue;
        }
        writeFileAtomic(c.add(canonical, tkcl::EntryType::Changelog, attributes, dict, version), *changes);
    }
    return c.changelog;
}

}  // namespace mm::tkmm
