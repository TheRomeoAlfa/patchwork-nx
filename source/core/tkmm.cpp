#include "tkmm.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include "byml.hpp"
#include "byml_merge.hpp"
#include "msbt.hpp"
#include "restbl.hpp"
#include "sarc.hpp"
#include "tkmm_data.hpp"
#include "zstd.hpp"

namespace mm::tkmm {

namespace {

const std::string kRemoved = "TKSCRMVD";  // TKMM's mark for a file removed from an archive
const std::string kSizeOverrides = "System/Resource/ResourceSizeTable.Product.rsizetable";
const std::string kHeapDef = "System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml";
const std::string kAiGlobalPack = "Pack/AI.Global.Product.pack";

bool isRemoved(std::string_view data) { return data == kRemoved; }

enum class Kind { Copy, Byml, Sarc, Pack, RsdbRows, RsdbTags, GameData, Msbt, Unsupported };

std::string_view extensionOf(std::string_view name) {
    auto slash = name.rfind('/');
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    return name.substr(dot);
}

Kind kindOf(std::string_view canonical) {
    if (canonical == "GameData/GameDataList.Product.byml") return Kind::GameData;
    if (canonical == "RSDB/Tag.Product.rstbl.byml") return Kind::RsdbTags;
    if (canonical.substr(0, 5) == "RSDB/") return rsdbRowKey(canonical).empty() ? Kind::Copy : Kind::RsdbRows;
    auto ext = extensionOf(canonical);
    if (ext == ".pack") return Kind::Pack;
    if (ext == ".bfarc" || ext == ".bkres" || ext == ".blarc" || ext == ".genvb" || ext == ".sarc" || ext == ".ta") return Kind::Sarc;
    if (ext == ".bntx") return canonical.size() >= 15 && canonical.substr(canonical.size() - 15) == "__Combined.bntx" ? Kind::Unsupported : Kind::Copy;
    if (ext == ".byml" || ext == ".bgyml") return Kind::Byml;
    if (ext == ".msbt") return Kind::Msbt;
    return Kind::Copy;  // (TKMM's .bfres.mc material cleanup isn't done: the last copy wins)
}

// TKMM's MsbtMerger: each changed message replaces the game's whole (its attribute kept when
// the changelog has none).
std::string mergeMsbt(const std::string& vanilla, const std::vector<std::string_view>& changelogs) {
    auto base = msbt::parse(vanilla);
    for (auto c : changelogs) {
        auto changes = msbt::parse(std::string(c));
        for (auto& [label, message] : changes.messages) {
            if (!changes.hasAttributes)
                if (const auto* old = base.find(label)) message.attribute = old->attribute;
            base.set(label, std::move(message));
        }
    }
    return msbt::write(base);
}

// TKMM's choice of a mod's text archive for the game's language ("Mals/USen.Product.sarc"):
// the same language and region, else the same language, else English, else the first.
std::string bestMals(const std::vector<std::string>& files, const std::string& locale) {
    if (files.size() == 1 || locale.size() != 4) return files.front();
    const std::string* best = nullptr;
    int level = 1;
    for (const auto& f : files) {
        if (f.size() < 9) continue;
        auto region = f.substr(5, 2), lang = f.substr(7, 2);
        if (lang == locale.substr(2) && region == locale.substr(0, 2)) return f;
        if (lang == locale.substr(2)) {
            best = &f;
            level = 2;
            continue;
        }
        if (level < 2 && lang == "en") best = &f;
    }
    return best ? *best : files.front();
}

int bestVersion(int target, const std::vector<int32_t>& provided) {
    int best = provided.front();
    for (int v : provided)
        if (target >= v) best = v;
    return best;
}

struct Engine {
    totk::Rom& rom;
    fs::path out, work;
    Result result;
    restbl::Collector sizes;
    std::optional<std::string> mergedHeapDef;
    std::optional<int> aiGlobalFiles;
    size_t spillCount = 0;

    struct PackEntry {
        std::string name;
        fs::path file;
        uint32_t override = 0;
    };
    struct PackTarget {
        uint32_t attributes = 0;
        int32_t dictionary = 0;
        std::vector<PackEntry> entries;
        fs::path tracked;  // the game's pack with removals applied (PackMerger), if any
    };
    std::map<std::string, PackTarget> packs;

    Engine(totk::Rom& r, fs::path o, restbl::Table vanilla) : rom(r), out(std::move(o)), work(out / ".work"), sizes(std::move(vanilla)) {
        sizes.setVanillaCheck([this](const std::string& path, std::string_view data) {
            auto game = rom.vanillaAt(path);
            return game && *game == data;
        });
    }

    fs::path spill(std::string_view data) {
        auto p = work / std::to_string(spillCount++);
        writeFileAtomic(p, data);
        return p;
    }

    void writeOut(const std::string& rel, std::string_view data) {
        writeFileAtomic(out / rel, data);
        result.files.push_back(rel);
    }

    void simpleOutput(const std::string& data, const std::string& relative, uint32_t attributes, int32_t dictionary, uint32_t override = 0) {
        sizes.collect(relative, uint32_t(data.size()), data, override);
        if (relative == rom.relativePath(kHeapDef, tkcl::HasZsExtension | tkcl::IsProductFile)) mergedHeapDef = data;
        if (attributes & tkcl::HasZsExtension) {
            uint32_t dict = dictionary > 0 && rom.dictionaries().has(uint32_t(dictionary)) ? uint32_t(dictionary) : 0;
            writeOut("romfs/" + relative, rom.dictionaries().compress(data, dict));
        } else {
            writeOut("romfs/" + relative, data);
        }
    }

    void collect(const tkcl::Entry& e, const std::vector<std::string>& archives, std::string_view data, uint32_t override) {
        auto file = spill(data);
        for (const auto& a : archives) {
            auto [it, fresh] = packs.try_emplace(a);
            if (fresh) {
                it->second.attributes = e.attributes;
                it->second.dictionary = e.zsDictionaryId;
            }
            it->second.entries.push_back({e.canonical, file, override});
        }
    }

    // A mod's whole file: into its packs, or written as it is.
    void copyToOutput(const std::string& raw, const std::string& relative, const tkcl::Entry& e, const std::vector<std::string>& archives,
                      uint32_t override) {
        if (!archives.empty()) {
            collect(e, archives, zs::isZstd(raw) ? rom.dictionaries().decompress(raw) : raw, override);
            return;
        }
        std::string stem = relative.size() > 3 && relative.compare(relative.size() - 3, 3, ".zs") == 0 ? relative.substr(0, relative.size() - 3) : relative;
        auto ext = extensionOf(stem);
        bool needsData = ext == ".ainb" || ext == ".asb" || ext == ".bstar" || ext == ".mc";
        if (zs::isZstd(raw)) {
            auto data = rom.dictionaries().decompress(raw);
            sizes.collect(relative, uint32_t(data.size()), needsData ? std::string_view(data) : std::string_view(), override);
        } else {
            sizes.collect(relative, uint32_t(raw.size()), needsData ? std::string_view(raw) : std::string_view(), override);
        }
        writeOut("romfs/" + relative, raw);
    }

    void mergedToOutput(const std::string& data, const std::string& relative, const tkcl::Entry& e, const std::vector<std::string>& archives) {
        if (!archives.empty()) return collect(e, archives, data, 0);
        simpleOutput(data, relative, e.attributes, e.zsDictionaryId);
    }

    std::string mergeSarc(const std::string& parent, const std::vector<std::string_view>& changelogs, const std::string& vanilla) {
        auto merged = Sarc::parse(vanilla);
        std::vector<std::pair<std::string, std::vector<std::string_view>>> groups;
        for (auto cl : changelogs)
            for (const auto& f : sarcView(cl)) {
                auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& x) { return x.first == f.name; });
                if (g == groups.end()) groups.push_back({f.name, {}}), g = groups.end() - 1;
                g->second.push_back(f.data);
            }
        for (auto& [name, buffers] : groups) {
            std::vector<std::string_view> effective;
            for (auto b : buffers)
                if (!isRemoved(b)) effective.push_back(b);
            if (effective.empty()) continue;
            auto kind = kindOf(name);
            auto it = std::find_if(merged.files.begin(), merged.files.end(), [&](const SarcFile& f) { return f.name == name; });
            if (it == merged.files.end()) {
                std::string data(effective.back());
                if (effective.size() > 1 && kind == Kind::Byml) {
                    // Added by several mods: the later copies merged onto the first.
                    std::string base(effective[0]);
                    std::vector<std::string> deltas;
                    for (size_t i = 1; i < effective.size(); ++i)
                        if (auto d = byml::buildChangelog(name, std::string(effective[i]), base)) deltas.push_back(*d);
                    std::vector<std::string_view> views(deltas.begin(), deltas.end());
                    data = views.empty() ? base : byml::mergeFiles(name, base, views);
                }
                merged.files.push_back({name, std::move(data)});
                continue;
            }
            if (kind == Kind::Byml) it->data = byml::mergeFiles(parent + "/" + name, it->data, effective);
            else if (kind == Kind::Sarc) it->data = mergeSarc(parent + "/" + name, effective, it->data);
            else if (kind == Kind::Msbt) it->data = mergeMsbt(it->data, effective);
            else it->data = std::string(effective.back());
        }
        return merged.write();
    }

    void writePacks() {
        for (auto& [archive, target] : packs) {
            auto relative = rom.relativePath(archive, target.attributes);
            Sarc sarc;
            if (!target.tracked.empty()) sarc = Sarc::parse(readFile(target.tracked));
            else if (auto game = rom.vanillaAt(relative)) sarc = Sarc::parse(*game);
            for (const auto& e : target.entries) {
                auto data = readFile(e.file);
                auto it = std::find_if(sarc.files.begin(), sarc.files.end(), [&](const SarcFile& f) { return f.name == e.name; });
                if (it != sarc.files.end() && isRemoved(it->data)) {
                    sarc.files.erase(it);
                    continue;
                }
                sizes.collect(e.name, uint32_t(data.size()), data, e.override);
                if (it != sarc.files.end()) it->data = std::move(data);
                else sarc.files.push_back({e.name, std::move(data)});
            }
            if (archive == kAiGlobalPack)
                aiGlobalFiles = int(std::count_if(sarc.files.begin(), sarc.files.end(), [](const SarcFile& f) { return f.name.rfind("AI/", 0) == 0; }));
            simpleOutput(sarc.write(), relative, target.attributes, target.dictionary);
        }
    }

    void patchAiHeap() {
        if (!aiGlobalFiles || *aiGlobalFiles <= 0) return;
        const uint32_t attributes = tkcl::HasZsExtension | tkcl::IsProductFile;
        auto relative = rom.relativePath(kHeapDef, attributes);
        std::optional<std::string> data = mergedHeapDef ? mergedHeapDef : rom.vanillaAt(relative);
        if (!data) return;
        auto doc = byml::parse(*data);
        auto* heaps = doc.root.type == byml::Node::Type::Dict ? &doc.root.dict["Heaps"] : nullptr;
        if (!heaps || heaps->type != byml::Node::Type::Dict) return;
        auto ai = heaps->dict.find("AI");
        if (ai == heaps->dict.end()) return;
        auto size = ai->second.dict.find("Size");
        if (size == ai->second.dict.end() || *aiGlobalFiles <= size->second.i) return;
        size->second = byml::Node::integer(*aiGlobalFiles);
        simpleOutput(byml::write(doc), relative, attributes, 1);
    }
};

std::map<std::string, uint32_t> readOverrides(const Input& in) {
    std::map<std::string, uint32_t> out;
    std::error_code ec;
    auto p = in.root / "romfs" / kSizeOverrides;
    if (!fs::is_regular_file(p, ec)) return out;
    try {
        out = restbl::Table::parse(readFile(p)).names;
    } catch (const std::exception&) {
    }
    return out;
}

void writeIps(Engine& eng, const std::vector<Input>& inputs) {
    static const std::map<std::string, uint32_t> shopParamAddress = {
        {"d5ad6ac71ef53e3e52417c1b81dbc9b4142aa3b3", 0x01ada148}, {"168dd518d925c7a327677286e72feda833314919", 0x01ad7938},
        {"9a10ed9435c06733da597d8094d9000ab5d3ee60", 0x01ace2b8}, {"6f32c68dd3bc7d77aa714b80e92a096a737cda77", 0x01ac0128},
        {"9b4e43650501a4d4489b4bbfdb740f26af3cf850", 0x01acb308}, {"6265f94d606242ce730ef721a8037dda8e4bfc63", 0x0212e60c},
        {"965eab9ceb8eb867f747da772022c95065c9b927", 0x0211e2b8}, {"5cb42b1cf25469fb0635fd046453d843c18bc8ab", 0x0211de70},
        {"277178b7dba1b6d4949a84778d4abc58b31b34f5", 0x021238fc},
    };
    auto nso = toLower(eng.rom.nsoBinaryId());
    std::map<uint32_t, uint32_t> entries;
    if (auto it = shopParamAddress.find(nso); it != shopParamAddress.end()) {
        uint32_t be = 0x52800008u | (512u << 5);  // mov w8, #512
        entries[it->second] = __builtin_bswap32(be);
    }
    for (const auto& in : inputs)
        for (const auto& p : in.changelog->patches)
            if (iequals(p.nsoBinaryId, nso))
                for (auto [address, value] : p.entries) entries[address] = value;
    std::string ips = "IPS32";
    auto be32 = [&](uint32_t v) {
        for (int k = 3; k >= 0; --k) ips += char((v >> (8 * k)) & 0xff);
    };
    for (auto [address, value] : entries) {
        be32(address + 0x100);  // the executable's header
        ips += '\0';
        ips += '\x04';
        be32(value);
    }
    ips += "EEOF";
    eng.writeOut("exefs_patches/" + toUpper(nso) + ".ips", ips);
}

void writeExefs(Engine& eng, const std::vector<Input>& inputs) {
    // Code modules: the highest mods' first, each only once, at most 9.
    std::set<std::string> seen;
    int written = 0, skipped = 0;
    for (auto it = inputs.rbegin(); it != inputs.rend(); ++it)
        for (const auto& f : it->changelog->subSdkFiles) {
            auto data = readFile(it->root / "exefs" / f);
            if (!seen.insert(data).second) continue;
            if (written >= 9) {
                ++skipped;
                continue;
            }
            eng.writeOut("exefs/subsdk" + std::to_string(++written), data);
        }
    if (skipped) eng.result.warnings.push_back(std::to_string(skipped) + " code modules (subsdk) from the lowest mods were left out: there's room for 9.");
    for (const auto& in : inputs)
        for (const auto& f : in.changelog->exeFiles) eng.writeOut("exefs/" + f, readFile(in.root / "exefs" / f));
    std::map<std::string, std::map<std::string, std::vector<std::vector<uint32_t>>>> cheats;
    for (const auto& in : inputs)
        for (const auto& c : in.changelog->cheats)
            for (const auto& [key, lines] : c.codes) cheats[c.name][key] = lines;
    for (const auto& [name, codes] : cheats) {
        std::string text;
        char hex[16];
        for (const auto& [key, lines] : codes) {
            if (!text.empty()) text += "\n";
            text += "[" + key + "]\n";
            for (const auto& line : lines) {
                for (size_t i = 0; i < line.size(); ++i) {
                    std::snprintf(hex, sizeof hex, "%08X", line[i]);
                    text += hex;
                    text += i + 1 < line.size() ? " " : "\n";
                }
            }
        }
        eng.writeOut("cheats/" + name + ".txt", text);
    }
}

}  // namespace

std::string inputPath(const tkcl::Entry& e, const totk::Rom& rom) {
    if (e.versions.empty()) return "romfs/" + e.canonical;
    auto starts = [&](std::string_view p) { return e.canonical.size() > p.size() && e.canonical.compare(0, p.size(), p) == 0; };
    int version;
    if (starts("Event/EventFlow")) {
        auto name = e.canonical.substr(e.canonical.rfind('/') + 1);
        name = name.substr(0, name.rfind('.'));
        auto* v = rom.eventFlowVersion(name);
        version = v ? bestVersion(std::atoi(v->c_str()), e.versions) : e.versions.front();
    } else if (starts("Sequence") || starts("Effect") || starts("Logic") || starts("AI")) {
        version = e.versions.front();  // (TkSharp's lookups for these never match)
    } else {
        version = bestVersion(rom.version(), e.versions);
    }
    return "romfs/" + e.canonical + std::to_string(version);
}

std::vector<std::string> unsupportedFiles(const tkcl::Changelog& c) {
    std::vector<std::string> out;
    for (const auto& e : c.files)
        if (e.type == tkcl::EntryType::Changelog && kindOf(e.canonical) == Kind::Unsupported && e.canonical != kSizeOverrides)
            out.push_back(e.canonical);
    return out;
}

Result merge(const std::vector<Input>& inputs, totk::Rom& rom, const fs::path& outDir, const Progress& progress,
             const std::string& language) {
    removeTree(outDir);
    auto sizePath = rom.version() >= 140 ? "System/Resource/ResourceSizeTable.Product." + std::to_string(rom.version()) + ".Nin_NX_NVN.rsizetable.zs"
                                         : "System/Resource/ResourceSizeTable.Product." + std::to_string(rom.version()) + ".rsizetable.zs";
    auto vanillaSizes = rom.vanillaAt(sizePath);
    if (!vanillaSizes) throw std::runtime_error("the game's size table is missing");
    Engine eng(rom, outDir, restbl::Table::parse(*vanillaSizes));
    if (progress) progress(0, "Reading the mods");

    std::vector<std::map<std::string, uint32_t>> overrides;
    for (const auto& in : inputs) overrides.push_back(readOverrides(in));
    writeIps(eng, inputs);
    writeExefs(eng, inputs);

    // Text: each mod's archive for the game's language merged into the game's.
    std::vector<std::string> mals;
    for (const auto& in : inputs)
        if (!in.changelog->malsFiles.empty()) mals.push_back(readFile(in.root / "romfs" / bestMals(in.changelog->malsFiles, language)));
    if (!mals.empty()) {
        if (progress) progress(0, "Merging text");
        const std::string canonical = "Mals/" + language + ".Product.sarc";
        const uint32_t attributes = tkcl::HasZsExtension | tkcl::IsProductFile;
        auto relative = rom.relativePath(canonical, attributes);
        auto game = rom.vanillaAt(relative);
        if (!game) {
            eng.result.warnings.push_back("The game's text for " + language + " couldn't be found, so the mods' text was left out.");
        } else {
            for (auto& m : mals)
                if (zs::isZstd(m)) m = rom.dictionaries().decompress(m);
            std::vector<std::string_view> views(mals.begin(), mals.end());
            try {
                eng.simpleOutput(eng.mergeSarc(canonical, views, *game), relative, attributes, 1);
            } catch (const std::exception& e) {
                throw std::runtime_error("couldn't merge the text (" + language + "): " + e.what());
            }
        }
    }

    // Every mod's entries for each file, in order.
    struct Use {
        size_t input;
        const tkcl::Entry* entry;
    };
    std::vector<std::pair<std::string, std::vector<Use>>> targets;
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < inputs.size(); ++i)
        for (const auto& e : inputs[i].changelog->files) {
            if (e.canonical == kSizeOverrides) continue;
            auto [it, fresh] = index.try_emplace(e.canonical, targets.size());
            if (fresh) targets.push_back({e.canonical, {}});
            targets[it->second].second.push_back({i, &e});
        }

    for (size_t t = 0; t < targets.size(); ++t) {
        const auto& [canonical, uses] = targets[t];
        auto base = canonical.substr(canonical.rfind('/') + 1);
        if (base == ".DS_Store" || base == "Thumbs.db" || base == "desktop.ini") continue;  // left by the author's computer
        if (progress && t % 16 == 0) progress(0.9f * float(t) / float(targets.size()), "Merging " + canonical);
        const tkcl::Entry& key = *uses.front().entry;
        std::vector<std::string> archives;
        for (const auto& u : uses)
            for (const auto& a : u.entry->archiveCanonicals)
                if (std::find(archives.begin(), archives.end(), a) == archives.end()) archives.push_back(a);
        std::vector<const Use*> real;
        for (const auto& u : uses)
            if (u.entry->type != tkcl::EntryType::Placeholder) real.push_back(&u);
        uint32_t override = 0;
        if (!real.empty() && real.back()->entry->type == tkcl::EntryType::Copy) {
            auto& table = overrides[real.back()->input];
            if (auto it = table.find(canonical); it != table.end()) override = it->second;
        }
        auto relative = rom.relativePath(canonical, archives.empty() ? key.attributes : 0);
        auto kind = kindOf(canonical);
        auto readInput = [&](const Use* u) { return readFile(inputs[u->input].root / inputPath(*u->entry, rom)); };
        try {
            if (real.empty()) {
                // Only placeholders: the game's copy, into the packs that list it.
                if (auto game = rom.vanillaAt(relative)) {
                    if (!archives.empty()) eng.collect(key, archives, *game, 0);
                } else {
                    eng.result.warnings.push_back("The game has no " + canonical + " to put in a pack.");
                }
                continue;
            }
            if (kind == Kind::Copy || kind == Kind::Unsupported) {
                bool anyChangelog = std::any_of(real.begin(), real.end(), [](const Use* u) { return u->entry->type == tkcl::EntryType::Changelog; });
                if (kind == Kind::Unsupported && anyChangelog) {
                    eng.result.warnings.push_back("Can't merge " + canonical + " yet; its changes were left out.");
                    continue;
                }
                eng.copyToOutput(readInput(real.back()), relative, key, archives, override);
                continue;
            }
            auto game = rom.vanillaAt(relative);
            std::vector<std::string> data;
            for (const auto* u : real) {
                auto d = readInput(u);
                data.push_back(zs::isZstd(d) ? rom.dictionaries().decompress(d) : std::move(d));
            }
            if (!game) {
                // A changelog needs the game's copy to apply to (TkSharp would use the changelog
                // itself as the file).
                if (real.front()->entry->type == tkcl::EntryType::Changelog) {
                    eng.result.warnings.push_back("The game's " + canonical + " couldn't be found, so changes to it were left out.");
                    continue;
                }
                // A mod's own new file: the first copy is the base.
                std::string merged = data.front();
                if (data.size() > 1 && kind == Kind::Byml) {
                    std::vector<std::string> deltas;
                    for (size_t i = 1; i < data.size(); ++i) {
                        if (real[i]->entry->type == tkcl::EntryType::Changelog) deltas.push_back(data[i]);
                        else if (auto d = byml::buildChangelog(canonical, data[i], merged)) deltas.push_back(*d);
                    }
                    std::vector<std::string_view> views(deltas.begin(), deltas.end());
                    if (!views.empty()) merged = byml::mergeFiles(canonical, merged, views);
                } else if (data.size() > 1) {
                    merged = data.back();
                }
                eng.mergedToOutput(merged, relative, key, archives);
                continue;
            }
            std::vector<std::string_view> views(data.begin(), data.end());
            if (kind == Kind::Pack) {
                // Only removals: the files themselves come as their own entries.
                auto sarc = Sarc::parse(*game);
                std::map<std::string, std::string_view> last;
                for (auto v : views)
                    for (const auto& f : sarcView(v)) last[f.name] = f.data;
                for (const auto& [name, d] : last)
                    if (isRemoved(d))
                        sarc.files.erase(std::remove_if(sarc.files.begin(), sarc.files.end(), [&](const SarcFile& f) { return f.name == name; }),
                                         sarc.files.end());
                auto& target = eng.packs[canonical];
                if (target.entries.empty() && target.tracked.empty()) {
                    target.attributes = key.attributes;
                    target.dictionary = key.zsDictionaryId;
                }
                target.tracked = eng.spill(sarc.write());
                continue;
            }
            std::string merged;
            switch (kind) {
                case Kind::Byml: merged = byml::mergeFiles(canonical, *game, views); break;
                case Kind::RsdbRows: merged = mergeRsdbRows(canonical, rsdbRowKey(canonical), *game, views); break;
                case Kind::RsdbTags: merged = mergeRsdbTags(*game, views); break;
                case Kind::GameData: merged = mergeGameData(canonical, *game, views); break;
                case Kind::Msbt: merged = mergeMsbt(*game, views); break;
                default: merged = eng.mergeSarc(canonical, views, *game); break;
            }
            eng.mergedToOutput(merged, relative, key, archives);
        } catch (const std::exception& e) {
            throw std::runtime_error("couldn't merge " + canonical + " (" + inputs[real.empty() ? uses.front().input : real.back()->input].modId +
                                     "): " + e.what());
        }
    }

    if (progress) progress(0.9f, "Rebuilding packs");
    eng.writePacks();
    eng.patchAiHeap();
    eng.writeOut("romfs/" + sizePath, rom.dictionaries().compress(eng.sizes.result().write(), 0));
    removeTree(eng.work);
    auto& files = eng.result.files;  // (the AI heap can be written twice)
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    if (progress) progress(1, "Done");
    return eng.result;
}

}  // namespace mm::tkmm
