#include "bcml.hpp"

#include <algorithm>
#include <cstdio>
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

// ---------------------------------------------------------------- AAMP helpers

std::string str3(const char* prefix, int n, int width) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s%0*d", prefix, width, n);
    return buf;
}

void removeObject(aamp::ParameterList& l, uint32_t key) {
    l.objects.erase(std::remove_if(l.objects.begin(), l.objects.end(), [&](const auto& o) { return o.first == key; }), l.objects.end());
}
void removeList(aamp::ParameterList& l, uint32_t key) {
    l.lists.erase(std::remove_if(l.lists.begin(), l.lists.end(), [&](const auto& o) { return o.first == key; }), l.lists.end());
}

// Combine "FileTable"-style logs (shop.aamp, aslist.aamp): path -> combined diff.
template <typename MergeFn>
std::map<std::string, aamp::ParameterList> fileTableLogs(Context& c, const char* logName, const char* tableName, MergeFn merge) {
    std::map<std::string, aamp::ParameterList> out;
    const uint32_t table = aamp::hash(tableName);
    for (const auto& log : c.logs(logName)) {
        auto pio = aamp::parse(readFile(log));
        const auto* files = pio.root.object(table);
        if (!files) continue;
        for (const auto& [k, p] : files->params) {
            if (!p.isString() || p.data.empty()) continue;
            if (const auto* diff = pio.root.list(aamp::hash(p.data))) merge(out[slashes(p.data)], *diff);
        }
    }
    return out;
}

// ---------------------------------------------------------------- drops

struct DropTable {
    std::string name;
    int32_t repeatMin = 0, repeatMax = 0, approach = 0, speed = 0;
    std::vector<std::pair<std::string, float>> items;  // file order
};

std::vector<DropTable> readDrops(const aamp::ParameterIO& pio) {
    std::vector<DropTable> out;
    const auto* header = pio.root.object(aamp::hash("Header"));
    if (!header) return out;
    const auto* num = header->find("TableNum");
    int n = num ? num->asInt() : 0;
    for (int i = 1; i <= n; ++i) {
        const auto* tn = header->find(str3("Table", i, 2));
        if (!tn) continue;
        const auto* obj = pio.root.object(aamp::hash(tn->data));
        if (!obj) continue;
        DropTable t;
        t.name = tn->data;
        if (auto* p = obj->find("RepeatNumMin")) t.repeatMin = p->asInt();
        if (auto* p = obj->find("RepeatNumMax")) t.repeatMax = p->asInt();
        if (auto* p = obj->find("ApproachType")) t.approach = p->asInt();
        if (auto* p = obj->find("OccurrenceSpeedType")) t.speed = p->asInt();
        int items = int(obj->params.size() > 5 ? (obj->params.size() - 5) / 2 : 0);
        for (int k = 1; k <= items; ++k) {
            const auto* name = obj->find(str3("ItemName", k, 2));
            const auto* prob = obj->find(str3("ItemProbability", k, 2));
            if (!name || !prob) continue;
            bool dup = false;
            for (auto& [in, ip] : t.items)
                if (in == name->data) {
                    ip = prob->asF32();
                    dup = true;
                }
            if (!dup) t.items.emplace_back(name->data, prob->asF32());
        }
        out.push_back(std::move(t));
    }
    return out;
}

aamp::ParameterIO writeDrops(const std::vector<DropTable>& tables) {
    aamp::ParameterIO pio;
    auto& header = pio.root.objectOrAdd(aamp::hash("Header"));
    header.set(aamp::hash("TableNum"), aamp::Parameter::s32(int32_t(tables.size())));
    for (size_t i = 0; i < tables.size(); ++i)
        header.set(aamp::hash(str3("Table", int(i + 1), 2)), aamp::Parameter::string(aamp::Type::String64, tables[i].name));
    for (const auto& t : tables) {
        auto& o = pio.root.objectOrAdd(aamp::hash(t.name));
        o.set(aamp::hash("RepeatNumMin"), aamp::Parameter::s32(t.repeatMin));
        o.set(aamp::hash("RepeatNumMax"), aamp::Parameter::s32(t.repeatMax));
        o.set(aamp::hash("ApproachType"), aamp::Parameter::s32(t.approach));
        o.set(aamp::hash("OccurrenceSpeedType"), aamp::Parameter::s32(t.speed));
        o.set(aamp::hash("ColumnNum"), aamp::Parameter::s32(int32_t(t.items.size())));
        for (size_t k = 0; k < t.items.size(); ++k) {
            o.set(aamp::hash(str3("ItemName", int(k + 1), 2)), aamp::Parameter::string(aamp::Type::String64, t.items[k].first));
            o.set(aamp::hash(str3("ItemProbability", int(k + 1), 2)), aamp::Parameter::f32(t.items[k].second));
        }
    }
    return pio;
}

void mergeDrops(Context& c) {
    // Per file, the highest priority package's tables replace whole tables.
    std::map<std::string, nlohmann::json> files;
    for (const auto& log : c.logs("drops.json")) {
        auto j = nlohmann::json::parse(readFile(log));
        for (const auto& [path, tables] : j.items())
            for (const auto& [name, table] : tables.items()) files[slashes(path)][name] = table;
    }
    for (const auto& [path, tables] : files) {
        auto parts = splitNested(path);
        std::vector<DropTable> stock;
        if (auto g = c.gameNested(parts)) {
            try {
                stock = readDrops(aamp::parse(*g));
            } catch (const std::exception&) {
            }
        }
        // The mod's tables, in the game's order where the game has them.
        std::vector<DropTable> out;
        auto fromJson = [&](const std::string& name, const nlohmann::json& j, const DropTable* base) {
            DropTable t;
            t.name = name;
            t.repeatMin = j.value("repeat_num_min", base ? base->repeatMin : 0);
            t.repeatMax = j.value("repeat_num_max", base ? base->repeatMax : 0);
            t.approach = j.value("approach_type", base ? base->approach : 0);
            t.speed = j.value("occurrence_speed_type", base ? base->speed : 0);
            std::map<std::string, float> probs;
            std::vector<std::string> order;
            if (base)
                for (const auto& [n, p] : base->items)
                    if (j.contains("items") && j["items"].contains(n)) order.push_back(n), probs[n] = p;
            if (j.contains("items"))
                for (const auto& [n, p] : j["items"].items()) {
                    if (!probs.count(n)) order.push_back(n);
                    probs[n] = p.is_number() ? p.get<float>() : probs[n];  // "UNDERRIDE_CONST" keeps the game's
                }
            for (const auto& n : order) t.items.emplace_back(n, probs[n]);
            return t;
        };
        for (const auto& s : stock)
            if (tables.contains(s.name)) out.push_back(fromJson(s.name, tables[s.name], &s));
        for (const auto& [name, j] : tables.items()) {
            bool known = std::any_of(stock.begin(), stock.end(), [&](const DropTable& s) { return s.name == name; });
            if (!known) out.push_back(fromJson(name, j, nullptr));
        }
        if (auto* data = c.edit(parts, true)) *data = aamp::write(writeDrops(out));
        else c.result.notes.push_back("couldn't place " + path);
    }
}

// ---------------------------------------------------------------- shops

const char* const kShopKeys[] = {"ItemName", "ItemNum", "ItemAdjustPrice", "ItemLookGetFlg", "ItemAmount"};

// BCML's "shopdata": TableNames object + a list per table of items keyed by item name.
aamp::ParameterList shopData(const aamp::ParameterIO& pio) {
    aamp::ParameterList out;
    auto& names = out.objectOrAdd(aamp::hash("TableNames"));
    const auto* header = pio.root.object(aamp::hash("Header"));
    if (!header) throw std::runtime_error("shop file without a header");
    for (const auto& [k, t] : header->params) {
        if (!t.isString()) continue;
        uint32_t th = aamp::hash(t.data);
        const auto* obj = pio.root.object(th);
        if (!obj) continue;
        aamp::ParameterList items;
        for (int i = 1; i <= int(obj->params.size()); ++i) {
            const auto* name = obj->find(str3("ItemName", i, 3));
            if (!name) continue;
            aamp::ParameterObject item;
            for (const char* key : kShopKeys)
                if (const auto* p = obj->find(str3(key, i, 3))) item.set(aamp::hash(key), *p);
            items.objects.emplace_back(aamp::hash(name->data), std::move(item));
        }
        if (!items.objects.empty()) {
            names.set(th, aamp::Parameter::string(aamp::Type::String64, t.data));
            out.lists.emplace_back(th, std::move(items));
        }
    }
    return out;
}

aamp::ParameterIO writeShop(const aamp::ParameterList& data, const aamp::ParameterIO& base) {
    aamp::ParameterIO pio;
    pio.version = base.version;
    pio.type = "xml";
    auto& header = pio.root.objectOrAdd(aamp::hash("Header"));
    const auto* names = data.object(aamp::hash("TableNames"));
    std::vector<std::string> tables;
    if (names)
        for (const auto& [k, p] : names->params) tables.push_back(p.data);
    header.set(aamp::hash("TableNum"), aamp::Parameter::s32(int32_t(tables.size())));
    for (size_t i = 0; i < tables.size(); ++i)
        header.set(aamp::hash(str3("Table", int(i + 1), 2)), aamp::Parameter::string(aamp::Type::String64, tables[i]));
    for (const auto& t : tables) {
        uint32_t th = aamp::hash(t);
        const auto* items = data.list(th);
        aamp::ParameterObject obj;
        obj.set(aamp::hash("ColumnNum"), aamp::Parameter::s32(items ? int32_t(items->objects.size()) : 0));
        if (items)
            for (size_t j = 0; j < items->objects.size(); ++j) {
                int n = int(j + 1);
                obj.set(aamp::hash(str3("ItemSort", n, 3)), aamp::Parameter::s32(int32_t(j)));
                for (const char* key : kShopKeys)
                    if (const auto* p = items->objects[j].second.find(key)) obj.set(aamp::hash(str3(key, n, 3)), *p);
            }
        pio.root.objects.emplace_back(th, std::move(obj));
    }
    return pio;
}

void mergeShops(Context& c) {
    auto diffs = fileTableLogs(c, "shop.aamp", "Filenames", [](aamp::ParameterList& dst, const aamp::ParameterList& src) { mergeLists(dst, src); });
    for (const auto& [path, diff] : diffs) {
        auto parts = splitNested(path);
        if (!c.gameFileOpt(parts[0])) continue;  // BCML: only files the game has
        auto* data = c.edit(parts);
        if (!data) continue;
        try {
            auto pio = aamp::parse(*data);
            auto shop = shopData(pio);
            if (const auto* rem = diff.list(aamp::hash("Removals")))
                for (const auto& [tk, tl] : rem->lists)
                    if (auto* mine = shop.list(tk)) {
                        for (const auto& [ik, io] : tl.objects) removeObject(*mine, ik);
                        if (mine->objects.empty() && mine->lists.empty()) removeList(shop, tk);
                    }
            if (const auto* add = diff.list(aamp::hash("Additions"))) mergeLists(shop, *add);
            *data = aamp::write(writeShop(shop, pio));
        } catch (const std::exception& e) {
            c.result.notes.push_back("couldn't merge " + path + ": " + e.what());
        }
    }
}

// ---------------------------------------------------------------- animation (AS) lists

struct CFDefine {
    std::string name;
    std::vector<std::string> excepts;
    std::vector<std::pair<std::string, std::vector<std::pair<uint32_t, aamp::Parameter>>>> posts;  // name -> params

    explicit CFDefine(const aamp::ParameterList& l) {
        if (const auto* pre = l.object(aamp::hash("CFPre")))
            if (const auto* n = pre->find("Name")) name = n->data;
        if (const auto* ex = l.object(aamp::hash("CFExcepts")))
            for (const auto& [k, p] : ex->params) excepts.push_back(p.data);
        if (const auto* ps = l.list(aamp::hash("CFPosts")))
            for (const auto& [k, o] : ps->objects) {
                const auto* n = o.find("Name");
                if (!n) continue;
                std::vector<std::pair<uint32_t, aamp::Parameter>> params;
                for (const char* key : {"Frame", "StartFrameRate"})
                    if (const auto* p = o.find(key)) params.emplace_back(aamp::hash(key), *p);
                posts.emplace_back(n->data, std::move(params));
            }
    }
    void updateFrom(const CFDefine& o) {
        for (const auto& e : o.excepts)
            if (std::find(excepts.begin(), excepts.end(), e) == excepts.end()) excepts.push_back(e);
        for (const auto& [pn, pp] : o.posts) {
            auto it = std::find_if(posts.begin(), posts.end(), [&](const auto& x) { return x.first == pn; });
            if (it == posts.end()) {
                posts.emplace_back(pn, pp);
                continue;
            }
            for (const auto& [k, v] : pp) {
                auto pit = std::find_if(it->second.begin(), it->second.end(), [&](const auto& x) { return x.first == k; });
                if (pit == it->second.end()) it->second.emplace_back(k, v);
                else pit->second = v;
            }
        }
    }
    aamp::ParameterList toList() const {
        aamp::ParameterList l;
        l.objectOrAdd(aamp::hash("CFPre")).set(aamp::hash("Name"), aamp::Parameter::string(aamp::Type::String32, name));
        if (!excepts.empty()) {
            auto& ex = l.objectOrAdd(aamp::hash("CFExcepts"));
            for (size_t i = 0; i < excepts.size(); ++i)
                ex.set(aamp::hash("Name_" + std::to_string(i)), aamp::Parameter::string(aamp::Type::String32, excepts[i]));
        }
        if (!posts.empty()) {
            auto& ps = l.listOrAdd(aamp::hash("CFPosts"));
            for (size_t i = 0; i < posts.size(); ++i) {
                auto& o = ps.objectOrAdd(aamp::hash("CFPost_" + std::to_string(i)));
                o.set(aamp::hash("Name"), aamp::Parameter::string(aamp::Type::String32, posts[i].first));
                for (const auto& [k, v] : posts[i].second) o.set(k, v);
            }
        }
        return l;
    }
};

// BCML's AS list merge_plists, with its special lists.
void mergeAsList(aamp::ParameterList& dst, const aamp::ParameterList& src) {
    const uint32_t addReses = 2777926231u, asDefines = 3752287078u, cfDefines = 3305786543u;
    for (const auto& [key, sub] : src.lists) {
        auto& mine = dst.listOrAdd(key);
        if (key == addReses) {
            std::vector<std::string> anims;
            for (const aamp::ParameterList* l : {static_cast<const aamp::ParameterList*>(&mine), &sub})
                for (const auto& [k, o] : l->objects)
                    if (const auto* a = o.find("Anim"))
                        if (std::find(anims.begin(), anims.end(), a->data) == anims.end()) anims.push_back(a->data);
            for (size_t i = 0; i < anims.size(); ++i)
                mine.objectOrAdd(aamp::hash("AddRes_" + std::to_string(i))).set(aamp::hash("Anim"), aamp::Parameter::string(aamp::Type::String64, anims[i]));
        } else if (key == asDefines) {
            std::map<std::string, size_t> listing;
            for (size_t i = 0; i < mine.objects.size(); ++i)
                if (const auto* n = mine.objects[i].second.find("Name")) listing[n->data] = i;
            std::vector<std::pair<std::string, std::string>> defs;
            for (const auto& [k, o] : sub.objects) {
                const auto* n = o.find("Name");
                const auto* f = o.find("Filename");
                if (!n || !f) continue;
                auto it = std::find_if(defs.begin(), defs.end(), [&](const auto& d) { return d.first == n->data; });
                if (it == defs.end()) defs.emplace_back(n->data, f->data);
                else it->second = f->data;
            }
            size_t next = listing.size();
            for (const auto& [name, file] : defs) {
                std::string objKey;
                if (listing.count(name)) {
                    objKey = "ASDefine_" + std::to_string(listing[name]);
                } else {
                    objKey = "ASDefine_" + std::to_string(next++);
                    mine.objectOrAdd(aamp::hash(objKey)).set(aamp::hash("Name"), aamp::Parameter::string(aamp::Type::String64, name));
                }
                mine.objectOrAdd(aamp::hash(objKey)).set(aamp::hash("Filename"), aamp::Parameter::string(aamp::Type::String64, file));
            }
        } else if (key == cfDefines) {
            std::map<std::string, size_t> listing;
            for (size_t i = 0; i < mine.lists.size(); ++i) listing[CFDefine(mine.lists[i].second).name] = i;
            size_t next = listing.size();
            for (const auto& [k, l] : sub.lists) {
                CFDefine def(l);
                if (listing.count(def.name)) {
                    auto defKey = aamp::hash("CFDefine_" + std::to_string(listing[def.name]));
                    auto* existing = mine.list(defKey);
                    if (!existing) continue;
                    CFDefine merged(*existing);
                    merged.updateFrom(def);
                    *existing = merged.toList();
                } else {
                    mine.lists.emplace_back(aamp::hash("CFDefine_" + std::to_string(next++)), def.toList());
                }
            }
        } else {
            mergeAsList(mine, sub);
        }
    }
    for (const auto& [key, obj] : src.objects) {
        auto& mine = dst.objectOrAdd(key);
        for (const auto& [pk, p] : obj.params) mine.set(pk, p);
    }
}

void mergeAsLists(Context& c) {
    auto diffs = fileTableLogs(c, "aslist.aamp", "FileTable", [](aamp::ParameterList& dst, const aamp::ParameterList& src) { mergeAsList(dst, src); });
    for (const auto& [path, diff] : diffs) {
        auto parts = splitNested(path);
        auto* data = c.edit(parts);
        if (!data) continue;
        try {
            auto pio = aamp::parse(*data);
            mergeAsList(pio.root, diff);
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
    static const std::set<std::string> merged = {"packs.json", "deepmerge.aamp", "drops.json", "shop.aamp", "aslist.aamp",
                                                 "gamedata.yml", "savedata.yml", "actorinfo.yml", "eventinfo.yml"};
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
            try {
                if (has("drops.json"))
                    for (const auto& [path, t] : nlohmann::json::parse(readFile(d / "drops.json")).items())
                        out.insert(splitNested(slashes(path))[0]);
                for (const char* log : {"shop.aamp", "aslist.aamp", "deepmerge.aamp"}) {
                    if (!has(log)) continue;
                    auto pio = aamp::parse(readFile(d / log));
                    for (const char* table : {"Filenames", "FileTable"})
                        if (const auto* files = pio.root.object(aamp::hash(table)))
                            for (const auto& [k, p] : files->params)
                                if (p.isString() && !p.data.empty()) out.insert(splitNested(slashes(p.data))[0]);
                }
            } catch (const std::exception&) {
            }
            if (has("actorinfo.yml")) out.insert(kActorInfo);
        }
    return out;
}

Result merge(const std::vector<Package>& lowestFirst, GameFiles& game) {
    Result result;
    Context c{lowestFirst, game, result, {}, {}, {}};
    mergePacks(c);
    deepMerge(c);
    mergeDrops(c);
    mergeShops(c);
    mergeAsLists(c);
    mergeActorInfo(c);
    mergeGameData(c);
    mergeSaveData(c);
    mergeEventInfo(c);
    c.closeAll();
    computeSizes(c);
    return result;
}

}  // namespace mm::bcml
