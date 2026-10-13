#include "bcml.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>

#include "aamp.hpp"
#include "nlohmann/json.hpp"
#include "sarc.hpp"
#include "rstbcalc.hpp"
#include "sizetable.hpp"

namespace mm::bcml {

namespace {

using byml::Node;
using T = Node::Type;

const std::string kBootup = std::string(kContent) + "Pack/Bootup.pack";
const std::string kActorInfo = std::string(kContent) + "Actor/ActorInfo.product.sbyml";

std::string unpack(const std::string& d) { return isYaz0(d) ? yaz0Decompress(d) : d; }

std::string slashes(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

// BCML's util.dict_merge on BYML trees.
void dictMerge(Node& dst, const Node& src, bool overwriteLists, bool shallow = false) {
    for (const auto& [k, v] : src.dict) {
        auto it = dst.dict.find(k);
        if (shallow || it == dst.dict.end()) {
            dst.dict[k] = v;
        } else if (it->second.type == T::Dict && v.type == T::Dict) {
            dictMerge(it->second, v, overwriteLists);
        } else if (it->second.type == T::Array && v.type == T::Array) {
            if (overwriteLists)
                it->second = v;
            else
                it->second.array.insert(it->second.array.end(), v.array.begin(), v.array.end());
        } else if (!(v.type == T::String && v.s == "UNDERRIDE_CONST")) {
            it->second = v;
        }
    }
}

struct Context {
    const std::vector<Package>& packages;
    GameFiles& game;
    Result& result;
    std::map<std::string, std::optional<std::string>> gameCache;
    // Archives being edited (the merged pack if there is one, else the game's), by path.
    struct Pack {
        Sarc sarc;
        bool yaz = false;
        bool dirty = false;
        std::map<std::string, std::unique_ptr<Pack>> nested;  // opened archives inside, by name
    };
    std::map<std::string, std::unique_ptr<Pack>> packs;
    std::map<std::string, Sarc> gameSarcs;  // parsed game archives, for "is this file changed?"

    const std::optional<std::string>& gameFileOpt(const std::string& path) {
        auto it = gameCache.find(path);
        if (it == gameCache.end()) it = gameCache.emplace(path, game.read(path)).first;
        return it->second;
    }
    // A file inside archives: {"…/Pack/Bootup.pack", "GameData/gamedata.ssarc"}. With `create`,
    // a missing last file is added (the archives must exist). nullptr when it can't be reached.
    // Marks the archives on the way as changed.
    std::string* edit(const std::vector<std::string>& path, bool create = false) {
        if (path.size() < 2) return nullptr;
        auto& top = packs[path[0]];
        if (!top) {
            auto it = result.files.find(path[0]);
            const std::string* base = it != result.files.end() ? &it->second : nullptr;
            if (!base) {
                const auto& g = gameFileOpt(path[0]);
                if (!g) {
                    packs.erase(path[0]);
                    return nullptr;
                }
                base = &*g;
            }
            top = std::make_unique<Pack>();
            top->yaz = isYaz0(*base);
            top->sarc = Sarc::parse(*base);
        }
        Pack* pack = top.get();
        std::vector<Pack*> chain{pack};
        for (size_t i = 1; i + 1 < path.size(); ++i) {
            auto& child = pack->nested[path[i]];
            if (!child) {
                const SarcFile* f = pack->sarc.find(path[i]);
                if (!f || !isSarc(f->data)) {
                    pack->nested.erase(path[i]);
                    return nullptr;
                }
                child = std::make_unique<Pack>();
                child->yaz = isYaz0(f->data);
                child->sarc = Sarc::parse(f->data);
            }
            pack = child.get();
            chain.push_back(pack);
        }
        SarcFile* target = nullptr;
        for (auto& f : pack->sarc.files)
            if (f.name == path.back()) target = &f;
        if (!target) {
            if (!create) return nullptr;
            pack->sarc.files.push_back({path.back(), {}});
            target = &pack->sarc.files.back();
        }
        for (auto* p : chain) p->dirty = true;
        return &target->data;
    }

    // Write edited archives back into their parents and into the result.
    std::string close(Pack& p) {
        for (auto& [name, child] : p.nested) {
            if (!child->dirty) continue;
            auto bytes = close(*child);
            for (auto& f : p.sarc.files)
                if (f.name == name) f.data = std::move(bytes);
        }
        auto raw = p.sarc.write();
        return p.yaz ? yaz0Compress(raw) : raw;
    }
    void closeAll() {
        for (auto& [path, p] : packs)
            if (p->dirty) result.files[path] = close(*p);
        packs.clear();
    }

    // The game's copy of a file inside archives, if there is one.
    std::optional<std::string> gameNested(const std::vector<std::string>& path) {
        const auto& top = gameFileOpt(path[0]);
        if (!top) return std::nullopt;
        if (path.size() == 1) return *top;
        auto it = gameSarcs.find(path[0]);
        if (it == gameSarcs.end()) it = gameSarcs.emplace(path[0], Sarc::parse(*top)).first;
        const Sarc* s = &it->second;
        Sarc inner;
        for (size_t i = 1; i < path.size(); ++i) {
            const auto* f = s->find(path[i]);
            if (!f) return std::nullopt;
            if (i + 1 == path.size()) return f->data;
            if (!isSarc(f->data)) return std::nullopt;
            inner = Sarc::parse(f->data);
            s = &inner;
        }
        return std::nullopt;
    }

    const std::string& gameFile(const std::string& path) {
        const auto& f = gameFileOpt(path);
        if (!f) throw std::runtime_error("needs the game's " + path.substr(path.find("romfs/") + 6) + ", which Patchwork hasn't read yet");
        return *f;
    }
    std::string gameInner(const std::string& pack, const std::string& inner) {
        auto s = Sarc::parse(gameFile(pack));
        const auto* f = s.find(inner);
        if (!f) throw std::runtime_error("the game's " + pack + " has no " + inner);
        return f->data;
    }

    // Every package's copy of a log (main logs, then options), lowest priority first.
    std::vector<fs::path> logs(const std::string& name) const {
        std::vector<fs::path> out;
        std::error_code ec;
        for (const auto& p : packages)
            for (const auto& d : p.logDirs)
                if (fs::exists(d / name, ec)) out.push_back(d / name);
        return out;
    }
    // Per package: its logs merged (options over main), lowest priority first.
    std::vector<Node> yamlLogs(const std::string& name, bool overwriteLists) const {
        std::vector<Node> out;
        std::error_code ec;
        for (const auto& p : packages) {
            Node merged;
            bool any = false;
            for (const auto& d : p.logDirs) {
                if (!fs::exists(d / name, ec)) continue;
                auto n = toByml(yaml::parse(readFile(d / name)));
                if (!any) {
                    merged = std::move(n);
                    any = true;
                } else {
                    dictMerge(merged, n, overwriteLists);
                }
            }
            if (any) out.push_back(std::move(merged));
        }
        return out;
    }
};

// ---------------------------------------------------------------- packs

void mergePacks(Context& c) {
    std::map<std::string, std::vector<fs::path>> sources;  // output path -> mods' copies, lowest first
    std::error_code ec;
    for (const auto& p : c.packages)
        for (const auto& d : p.logDirs) {
            if (!fs::exists(d / "packs.json", ec)) continue;
            auto j = nlohmann::json::parse(readFile(d / "packs.json"));
            for (const auto& [canon, path] : j.items()) {
                if (!path.is_string()) continue;
                auto rel = slashes(path.get<std::string>());
                if (rel.find("AocMainField.pack") != std::string::npos) continue;  // BCML leaves it out too
                auto file = p.filesRoot / fs::path(rel);
                if (!fs::exists(file, ec)) continue;
                auto& list = sources[rel];
                if (std::find(list.begin(), list.end(), file) == list.end()) list.push_back(file);
            }
        }
    for (const auto& [out, files] : sources) {
        std::vector<std::string> archives;
        const auto& game = c.gameFileOpt(out);
        if (game) archives.push_back(*game);
        for (const auto& f : files) archives.push_back(readFile(f));
        auto merged = mergeSarcs(archives, true, game.has_value());
        bool yaz = isYaz0(game ? *game : archives.back());
        c.result.files[out] = yaz ? yaz0Compress(merged) : merged;
    }
}

// ---------------------------------------------------------------- game data

std::string dataName(const Node& entry) {
    const auto* n = entry.find("DataName");
    return n && n->type == T::String ? n->s : std::string();
}

void mergeGameData(Context& c) {
    auto diffs = c.yamlLogs("gamedata.yml", true);
    if (diffs.empty()) return;
    Node diff = Node::makeDict();
    for (const auto& d : diffs) dictMerge(diff, d, true);

    // The game's flags, all files of a type together.
    auto stockSarc = Sarc::parse(c.gameInner(kBootup, "GameData/gamedata.ssarc"));
    std::map<std::string, std::map<std::string, Node>> types;  // type -> DataName -> entry
    for (const auto& f : stockSarc.files) {
        auto doc = byml::parse(f.data);
        for (const auto& [type, list] : doc.root.dict)
            for (const auto& e : list.array) types[type][dataName(e)] = e;
    }
    for (const auto& [type, changes] : diff.dict) {
        auto t = types.find(type);
        if (t == types.end()) continue;  // types the game doesn't have are ignored (as in BCML)
        if (const auto* add = changes.find("add"))
            for (const auto& [name, entry] : add->dict) t->second[name] = entry;
        if (const auto* del = changes.find("del"))
            for (const auto& name : del->array)
                if (name.type == T::String) t->second.erase(name.s);
    }
    Sarc out;
    for (const auto& [type, entries] : types) {
        std::vector<Node> all;
        for (const auto& [name, e] : entries) all.push_back(e);
        for (size_t i = 0, chunk = 0; i < all.size() || chunk == 0; i += 4096, ++chunk) {
            byml::Document doc;
            doc.root = Node::makeDict();
            Node arr = Node::makeArray();
            for (size_t k = i; k < std::min(all.size(), i + 4096); ++k) arr.array.push_back(all[k]);
            doc.root.dict[type] = std::move(arr);
            out.files.push_back({"/" + type + "_" + std::to_string(chunk) + ".bgdata", byml::write(doc)});
            if (all.empty()) break;
        }
    }
    auto sarc = out.write();
    *c.edit({kBootup, "GameData/gamedata.ssarc"}, true) = yaz0Compress(sarc);
}

int32_t hashValue(const Node& entry) {
    const auto* n = entry.find("HashValue");
    if (!n) return 0;
    return n->type == T::UInt ? int32_t(uint32_t(n->u)) : int32_t(n->i);
}

void mergeSaveData(Context& c) {
    auto diffs = c.yamlLogs("savedata.yml", true);
    if (diffs.empty()) return;
    // Highest priority first: the first copy of an added flag wins; deletions add up.
    std::vector<Node> adds;
    std::set<int32_t> seen, del;
    for (auto it = diffs.rbegin(); it != diffs.rend(); ++it) {
        if (const auto* a = it->find("add"))
            for (const auto& e : a->array)
                if (seen.insert(hashValue(e)).second) adds.push_back(e);
        if (const auto* d = it->find("del"))
            for (const auto& h : d->array) del.insert(h.type == T::UInt ? int32_t(uint32_t(h.u)) : int32_t(h.i));
    }
    auto stockSarc = Sarc::parse(c.gameInner(kBootup, "GameData/savedataformat.ssarc"));
    auto files = stockSarc.files;
    std::sort(files.begin(), files.end(), [](const SarcFile& a, const SarcFile& b) { return a.name < b.name; });
    if (files.size() < 3) throw std::runtime_error("unexpected save data format in the game");
    std::map<int32_t, Node> entries;
    for (size_t k = 0; k + 2 < files.size(); ++k) {
        auto doc = byml::parse(files[k].data);
        const auto* list = doc.root.find("file_list");
        if (!list || list->array.size() < 2) continue;
        for (const auto& e : list->array[1].array)
            if (!del.count(hashValue(e))) entries[hashValue(e)] = e;
    }
    for (const auto& e : adds)
        if (!del.count(hashValue(e))) entries[hashValue(e)] = e;
    std::vector<Node> all;
    for (const auto& [h, e] : entries) all.push_back(e);  // std::map: sorted as signed 32-bit
    Sarc out;
    size_t chunks = 0;
    for (size_t i = 0; i < all.size(); i += 8192, ++chunks) {
        byml::Document doc;
        doc.root = Node::makeDict();
        Node header = Node::makeDict();
        header.dict["IsCommon"] = Node::boolean(false);
        header.dict["IsCommonAtSameAccount"] = Node::boolean(false);
        header.dict["IsSaveSecureCode"] = Node::boolean(true);
        header.dict["file_name"] = Node::str("game_data.sav");
        Node list = Node::makeArray();
        list.array.push_back(header);
        Node arr = Node::makeArray();
        for (size_t k = i; k < std::min(all.size(), i + 8192); ++k) arr.array.push_back(all[k]);
        list.array.push_back(std::move(arr));
        doc.root.dict["file_list"] = std::move(list);
        Node info = Node::makeDict();
        info.dict["directory_num"] = Node::integer(8);
        info.dict["is_build_machine"] = Node::boolean(true);
        info.dict["revision"] = Node::integer(18203);
        Node infos = Node::makeArray();
        infos.array.push_back(std::move(info));
        doc.root.dict["save_info"] = std::move(infos);
        out.files.push_back({"/saveformat_" + std::to_string(chunks) + ".bgsvdata", byml::write(doc)});
    }
    out.files.push_back({"/saveformat_" + std::to_string(chunks) + ".bgsvdata", files[files.size() - 2].data});
    out.files.push_back({"/saveformat_" + std::to_string(chunks + 1) + ".bgsvdata", files[files.size() - 1].data});
    auto sarc = out.write();
    *c.edit({kBootup, "GameData/savedataformat.ssarc"}, true) = yaz0Compress(sarc);
}

// ---------------------------------------------------------------- actor info

void mergeActorInfo(Context& c) {
    auto diffs = c.yamlLogs("actorinfo.yml", true);
    if (diffs.empty()) return;
    Node diff = Node::makeDict();
    std::map<std::string, int64_t> instSize;  // the biggest any mod asks for
    for (const auto& d : diffs) {
        dictMerge(diff, d, true);
        for (const auto& [h, actor] : d.dict)
            if (const auto* s = actor.find("instSize")) {
                int64_t v = s->type == T::UInt ? int64_t(s->u) : s->i;
                instSize[h] = std::max(instSize.count(h) ? instSize[h] : v, v);
            }
    }
    for (const auto& [h, v] : instSize) diff.dict[h].dict["instSize"] = Node::integer(int32_t(v));

    auto stock = byml::parse(c.gameFile(kActorInfo));
    std::map<uint32_t, Node> actors;
    if (const auto* list = stock.root.find("Actors"))
        for (const auto& a : list->array)
            if (const auto* name = a.find("name")) actors[aamp::hash(name->s)] = a;
    for (const auto& [key, actor] : diff.dict) {
        uint32_t h = uint32_t(std::stoull(key));
        auto it = actors.find(h);
        if (it != actors.end() && it->second.type == T::Dict && actor.type == T::Dict)
            dictMerge(it->second, actor, true);
        else
            actors[h] = actor;
    }
    byml::Document out;
    out.version = stock.version;
    out.root = Node::makeDict();
    Node list = Node::makeArray(), hashes = Node::makeArray();
    for (const auto& [h, a] : actors) {
        list.array.push_back(a);
        hashes.array.push_back(h < 0x80000000u ? Node::integer(int32_t(h)) : Node::uinteger(h));
    }
    out.root.dict["Actors"] = std::move(list);
    out.root.dict["Hashes"] = std::move(hashes);
    auto raw = byml::write(out);
    c.result.files[kActorInfo] = yaz0Compress(raw);
}

// ---------------------------------------------------------------- event info

void mergeEventInfo(Context& c) {
    auto diffs = c.yamlLogs("eventinfo.yml", true);
    if (diffs.empty()) return;
    auto stock = byml::parse(c.gameInner(kBootup, "Event/EventInfo.product.sbyml"));
    for (const auto& d : diffs)
        for (const auto& [event, data] : d.dict) stock.root.dict[event] = data;  // whole events
    auto raw = byml::write(stock);
    *c.edit({kBootup, "Event/EventInfo.product.sbyml"}, true) = yaz0Compress(raw);
}

// ---------------------------------------------------------------- deep merge (AAMP)

// BCML's merge_plists: lists merge recursively, objects are added or their parameters
// overwritten.
void mergeLists(aamp::ParameterList& dst, const aamp::ParameterList& src) {
    for (const auto& [k, l] : src.lists) mergeLists(dst.listOrAdd(k), l);
    for (const auto& [k, o] : src.objects) {
        auto& target = dst.objectOrAdd(k);
        for (const auto& [pk, p] : o.params) target.set(pk, p);
    }
}

void deepMerge(Context& c) {
    const uint32_t fileTable = aamp::hash("FileTable");
    std::map<std::string, aamp::ParameterList> diffs;  // path -> combined changes
    for (const auto& log : c.logs("deepmerge.aamp")) {
        auto pio = aamp::parse(readFile(log));
        const auto* files = pio.root.object(fileTable);
        if (!files) continue;
        for (const auto& [k, p] : files->params) {
            if (!p.isString()) continue;
            const auto* diff = pio.root.list(aamp::hash(p.data));
            if (diff) mergeLists(diffs[p.data], *diff);
        }
    }
    for (const auto& [path, diff] : diffs) {
        auto parts = splitNested(slashes(path));
        if (parts.size() < 2 || !c.gameFileOpt(parts[0])) continue;  // BCML: only files the game has
        auto* data = c.edit(parts);
        if (!data) continue;
        try {
            auto pio = aamp::parse(*data);
            mergeLists(pio.root, diff);
            *data = aamp::write(pio);
        } catch (const std::exception& e) {
            c.result.notes.push_back("couldn't merge " + path + ": " + e.what());
        }
    }
}

// ---------------------------------------------------------------- size table

const std::set<std::string>& sizeExcludedExts() {
    static const std::set<std::string> e = {".pack", ".bgdata", ".txt", ".bgsvdata", ".yml", ".msbt", ".bat", ".ini", ".png", ".bfstm", ".py", ".sh"};
    return e;
}
bool sizeRecurse(const std::string& ext) {
    static const std::set<std::string> sarc = {".pack", ".bactorpack", ".bmodelsh", ".beventpack", ".stats", ".sarc", ".blarc"};
    static const std::set<std::string> no = {".sarc", ".blarc", ".bfarc"};
    return sarc.count(ext) && !no.count(ext);
}
std::string extOf(const std::string& name) {
    auto slash = name.rfind('/'), dot = name.rfind('.');
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? "" : name.substr(dot);
}

// The size table name of a package path ("01007EF00011F001/romfs/X" -> "Aoc/0010/X").
std::string tableNameOf(const std::string& path) {
    if (path.rfind(kContent, 0) == 0) return sizeTableName(path.substr(std::string(kContent).size()));
    if (path.rfind(kDlc, 0) == 0) return "Aoc/0010/" + sizeTableName(path.substr(std::string(kDlc).size()));
    return sizeTableName(path);
}

void sizeNested(Context& c, std::vector<std::string>& path, const std::string& data) {
    auto plain = unpack(data);
    for (const auto& f : Sarc::parse(plain).files) {
        path.push_back(f.name);
        auto canon = sizeTableName(f.name);
        auto ext = extOf(canon);
        auto game = c.gameNested(path);
        bool changed = !game || (*game != f.data && unpack(*game) != unpack(f.data));
        if (changed && !sizeExcludedExts().count(ext) && canon != "Actor/ActorInfo.product.byml") {
            if (auto size = resourceSize(canon, f.data)) c.result.sizes[canon] = size;
            if (sizeRecurse(ext) && isSarc(f.data)) sizeNested(c, path, f.data);
        }
        path.pop_back();
    }
}

void computeSizes(Context& c) {
    for (const auto& [path, data] : c.result.files) {
        auto canon = tableNameOf(path);
        auto ext = extOf(canon);
        if (!sizeExcludedExts().count(ext) && canon != "Actor/ActorInfo.product.byml")
            if (auto size = resourceSize(canon, data)) c.result.sizes[canon] = size;
        if (sizeRecurse(ext) && isSarc(data)) {
            std::vector<std::string> nested{path};
            sizeNested(c, nested, data);
        }
    }
}

}  // namespace

std::optional<std::string> FolderGameFiles::read(const std::string& path) {
    std::error_code ec;
    auto p = root_ / fs::path(path);
    if (!fs::exists(p, ec)) return std::nullopt;
    return readFile(p);
}

std::vector<std::string> splitNested(const std::string& path) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        auto at = path.find("//", start);
        out.push_back(path.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) break;
        start = at + 2;
    }
    return out;
}

namespace {
const std::set<std::string>& mergedLogs() {
    static const std::set<std::string> merged = {"packs.json", "deepmerge.aamp", "gamedata.yml", "savedata.yml", "actorinfo.yml", "eventinfo.yml"};
    return merged;
}
}  // namespace

bool canMerge(const std::string& logName) {
    static const std::set<std::string> ignored = {"rstb.json", "rstb.log", ".shrink"};
    return mergedLogs().count(logName) || ignored.count(logName);
}

std::string mergeSupportKey() {
    std::string key;
    for (const auto& l : mergedLogs()) key += l + ";";
    return key;
}

byml::Node toByml(const yaml::Node& n) {
    Node out;
    switch (n.kind) {
        case yaml::Node::Kind::Null: return out;
        case yaml::Node::Kind::Seq:
            out.type = T::Array;
            for (const auto& item : n.items) out.array.push_back(toByml(item));
            return out;
        case yaml::Node::Kind::Map:
            out.type = T::Dict;
            for (const auto& [k, v] : n.map) out.dict[k] = toByml(v);
            return out;
        case yaml::Node::Kind::Scalar: break;
    }
    if (n.tag == "!u") {
        out.type = T::UInt;
        out.u = uint32_t(n.asUInt());
    } else if (n.tag == "!l") {
        out.type = T::Int64;
        out.i = n.asInt();
    } else if (n.tag == "!ul") {
        out.type = T::UInt64;
        out.u = n.asUInt();
    } else if (n.tag == "!f64") {
        out.type = T::Double;
        out.d = n.asFloat();
    } else if (n.quoted || !n.tag.empty()) {
        out.type = T::String;
        out.s = n.value;
    } else if (n.isNull()) {
        out.type = T::Null;
    } else if (n.looksBool()) {
        out.type = T::Bool;
        out.b = n.asBool();
    } else if (n.looksInt()) {
        out.type = T::Int;
        out.i = n.asInt();
    } else if (n.looksFloat()) {
        out.type = T::Float;
        out.f = float(n.asFloat());
    } else {
        out.type = T::String;
        out.s = n.value;
    }
    return out;
}

std::set<std::string> gameFilesNeeded(const std::vector<Package>& packages) {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& p : packages)
        for (const auto& d : p.logDirs) {
            auto has = [&](const char* n) { return fs::exists(d / n, ec); };
            if (has("packs.json")) {
                try {
                    auto j = nlohmann::json::parse(readFile(d / "packs.json"));
                    for (const auto& [canon, path] : j.items())
                        if (path.is_string()) out.insert(slashes(path.get<std::string>()));
                } catch (const std::exception&) {
                }
            }
            if (has("gamedata.yml") || has("savedata.yml") || has("eventinfo.yml")) out.insert(kBootup);
            if (has("actorinfo.yml")) out.insert(kActorInfo);
        }
    return out;
}

Result merge(const std::vector<Package>& lowestFirst, GameFiles& game) {
    Result result;
    Context c{lowestFirst, game, result, {}, {}, {}};
    mergePacks(c);
    deepMerge(c);
    mergeActorInfo(c);
    mergeGameData(c);
    mergeSaveData(c);
    mergeEventInfo(c);
    c.closeAll();
    computeSizes(c);
    return result;
}

}  // namespace mm::bcml
