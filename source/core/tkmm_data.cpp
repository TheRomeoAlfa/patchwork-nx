#include "tkmm_data.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

#include "byml_merge.hpp"

namespace mm::tkmm {

namespace {

using byml::Node;
using T = Node::Type;

bool isRemove(const Node& n) { return n.type == T::Changelog && n.i == int64_t(Node::Change::Remove); }

std::vector<byml::Document> parseAll(const std::vector<std::string_view>& changelogs) {
    std::vector<byml::Document> out;
    out.reserve(changelogs.size());
    for (auto c : changelogs) out.push_back(byml::parse(std::string(c)));
    return out;
}

// TKMM's BymlRowComparer: by the key field, strings in byte order, numbers by value (rows
// whose keys can't be compared count as equal).
bool rowLess(const Node& a, const Node& b, const std::string& keyName) {
    {
        const Node* x = a.type == T::Dict ? a.find(keyName) : nullptr;
        const Node* y = b.type == T::Dict ? b.find(keyName) : nullptr;
        if (!x || !y || x->type != y->type) return false;
        switch (x->type) {
            case T::String: return x->s < y->s;
            case T::Int:
            case T::Int64: return x->i < y->i;
            case T::UInt:
            case T::UInt64: return x->u < y->u;
            default: return false;
        }
    }
}

void sortRows(Node& rows, const std::string& keyName) {
    std::stable_sort(rows.array.begin(), rows.array.end(), [&](const Node& a, const Node& b) { return rowLess(a, b, keyName); });
}

bool sortedRows(const Node& rows, const std::string& keyName) {
    for (size_t i = 1; i < rows.array.size(); ++i)
        if (rowLess(rows.array[i], rows.array[i - 1], keyName)) return false;
    return true;
}

// A row's key as the changelog names it: a string (map changelogs) or a 32-bit number
// (hash map changelogs).
bool rowKey(const Node& row, const std::string& keyName, std::string* s, uint64_t* u) {
    const Node* k = row.type == T::Dict ? row.find(keyName) : nullptr;
    if (!k) return false;
    if (k->type == T::String) {
        *s = k->s;
        return true;
    }
    if (k->type == T::UInt || k->type == T::UInt64) {
        *u = k->u;
        return true;
    }
    if (k->type == T::Int || k->type == T::Int64) {
        *u = uint64_t(uint32_t(k->i));
        return true;
    }
    return false;
}

}  // namespace

std::string rsdbRowKey(std::string_view c) {
    if (c == "RSDB/GameSafetySetting.Product.rstbl.byml") return "NameHash";
    if (c == "RSDB/RumbleCall.Product.rstbl.byml" || c == "RSDB/UIScreen.Product.rstbl.byml") return "Name";
    if (c == "RSDB/TagDef.Product.rstbl.byml") return "FullTagId";
    static const char* byRowId[] = {
        "ActorInfo", "AttachmentActorInfo", "Challenge", "EnhancementMaterialInfo", "EventPlayEnvSetting", "EventSetting",
        "GameActorInfo", "GameAnalyzedEventInfo", "GameEventBaseSetting", "GameEventMetadata", "LoadingTips", "Location",
        "LocatorData", "PouchActorInfo", "XLinkPropertyTable", "XLinkPropertyTableList",
    };
    for (const char* name : byRowId)
        if (c == "RSDB/" + std::string(name) + ".Product.rstbl.byml") return "__RowId";
    return {};
}

std::string mergeRsdbRows(std::string_view canonical, const std::string& keyName, const std::string& vanilla,
                          const std::vector<std::string_view>& changelogs) {
    auto doc = byml::parse(vanilla);
    if (doc.root.type != T::Array) throw std::runtime_error("RSDB: the game's table isn't a list");
    // TKMM sorts by key afterwards; a few tables (TagDef, GameSafetySetting) are in the game's
    // own order, which is kept (new rows go at the end).
    bool sorted = sortedRows(doc.root, keyName);
    auto parsed = parseAll(changelogs);
    // Rows are added during the merge (later mods can change them): room for all of them up
    // front, so the merge's references into the list stay valid.
    size_t extra = 0;
    for (const auto& p : parsed) extra += p.root.dict.size() + p.root.hashes.size();
    doc.root.array.reserve(doc.root.array.size() + extra);
    byml::MergeSession session(canonical, &doc.root);
    std::map<std::string, size_t> removed;  // key -> row
    for (auto& p : parsed) {
        Node& c = p.root;
        bool hashed = c.type == T::HashMap32 || c.type == T::HashMap64;
        if (!hashed && c.type != T::Dict) throw std::runtime_error("RSDB: a changelog isn't a map of rows");
        for (size_t i = 0; i < doc.root.array.size(); ++i) {
            std::string s;
            uint64_t u = 0;
            if (!rowKey(doc.root.array[i], keyName, &s, &u)) continue;
            Node change;
            std::string key;
            if (hashed) {
                auto it = c.hashes.find(u);
                if (it == c.hashes.end()) continue;
                change = std::move(it->second);
                c.hashes.erase(it);
                key = "#" + std::to_string(u);
            } else {
                auto it = c.dict.find(s);
                if (it == c.dict.end()) continue;
                change = std::move(it->second);
                c.dict.erase(it);
                key = "$" + s;
            }
            if (isRemove(change)) {
                removed[key] = i;
                continue;
            }
            removed.erase(key);  // removed by a lower mod, changed by a higher one: it stays
            session.mergeMap(doc.root.array[i], change);
        }
        for (auto& [k, row] : c.dict)
            if (!isRemove(row)) doc.root.array.push_back(std::move(row));
        for (auto& [k, row] : c.hashes)
            if (!isRemove(row)) doc.root.array.push_back(std::move(row));
    }
    session.apply();
    for (const auto& [key, i] : removed) doc.root.array[i] = Node::removed();
    doc.root.array.erase(std::remove_if(doc.root.array.begin(), doc.root.array.end(), isRemove), doc.root.array.end());
    if (sorted) sortRows(doc.root, keyName);
    return byml::write(doc);
}

std::string mergeRsdbTags(const std::string& vanilla, const std::vector<std::string_view>& changelogs) {
    auto doc = byml::parse(vanilla);
    Node& root = doc.root;
    const Node* paths = root.find("PathList");
    const Node* tagList = root.find("TagList");
    const Node* bits = root.find("BitTable");
    if (!paths || !tagList || !bits || paths->array.size() % 3) throw std::runtime_error("RSDB: the game's tag table is damaged");
    std::vector<std::string> tags;
    for (const auto& t : tagList->array) tags.push_back(t.s);
    using Key = std::tuple<std::string, std::string, std::string>;  // ordered as TKMM's key comparer
    std::map<Key, std::vector<std::string>> entries;
    size_t tagCount = tags.size();
    for (size_t e = 0; e * 3 < paths->array.size(); ++e) {
        auto& list = entries[{paths->array[3 * e].s, paths->array[3 * e + 1].s, paths->array[3 * e + 2].s}];
        for (size_t t = 0; t < tagCount; ++t) {
            size_t bit = e * tagCount + t;
            if (bit / 8 < bits->s.size() && (uint8_t(bits->s[bit / 8]) >> (bit % 8)) & 1) list.push_back(tags[t]);
        }
    }
    for (const auto& p : parseAll(changelogs)) {
        if (const Node* added = p.root.find("Tags"))
            for (const auto& t : added->array) tags.push_back(t.s);
        const Node* changed = p.root.find("Entries");
        if (!changed) continue;
        for (size_t i = 0; i + 3 < changed->array.size(); i += 4) {
            auto& list = entries[{changed->array[i].s, changed->array[i + 1].s, changed->array[i + 2].s}];
            const Node& value = changed->array[i + 3];
            if (value.type == T::Array) {
                for (const auto& t : value.array) list.push_back(t.s);
                continue;
            }
            for (const auto& c : value.changes) {
                if (c.node.type != T::String) continue;
                if (c.change == Node::Change::Remove) {
                    auto it = std::find(list.begin(), list.end(), c.node.s);
                    if (it != list.end()) list.erase(it);
                } else {
                    list.push_back(c.node.s);
                }
            }
        }
    }
    // A tag given to an entry but not listed (TkSharp would fail): listed.
    for (const auto& [key, list] : entries) tags.insert(tags.end(), list.begin(), list.end());
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    std::unordered_map<std::string, size_t> tagIndex;
    for (size_t t = 0; t < tags.size(); ++t) tagIndex[tags[t]] = t;

    Node newPaths = Node::makeArray(), newTags = Node::makeArray();
    std::string table((tags.size() * entries.size() + 7) / 8, '\0');
    size_t e = 0;
    for (auto& [key, list] : entries) {
        newPaths.array.push_back(Node::str(std::get<0>(key)));
        newPaths.array.push_back(Node::str(std::get<1>(key)));
        newPaths.array.push_back(Node::str(std::get<2>(key)));
        for (const auto& t : list) {
            size_t bit = e * tags.size() + tagIndex.at(t);
            table[bit / 8] = char(uint8_t(table[bit / 8]) | (1u << (bit % 8)));
        }
        ++e;
    }
    for (const auto& t : tags) newTags.array.push_back(Node::str(t));
    Node newBits = *bits;  // the game's node type (and alignment)
    newBits.s = std::move(table);
    root.dict["BitTable"] = std::move(newBits);
    root.dict["PathList"] = std::move(newPaths);
    root.dict["TagList"] = std::move(newTags);
    return byml::write(doc);
}

std::string mergeGameData(std::string_view canonical, const std::string& vanilla, const std::vector<std::string_view>& changelogs) {
    auto doc = byml::parse(vanilla);
    Node* data = doc.root.type == T::Dict ? &doc.root.dict["Data"] : nullptr;
    if (!data || data->type != T::Dict) throw std::runtime_error("game data: the game's file is damaged");
    auto parsed = parseAll(changelogs);
    byml::MergeSession session(canonical, &doc.root);
    struct Added {
        Node row;
        bool structTable = false;
        std::vector<Node> changes;  // from mods above the one that added it
    };
    std::map<std::string, std::map<uint64_t, Added>> added;
    std::map<std::string, std::set<uint64_t>> dropped;
    std::map<std::string, std::unordered_map<uint64_t, size_t>> rowsByHash;
    for (auto& p : parsed) {
        for (auto& [name, changes] : p.root.dict) {
            auto table = data->dict.find(name);
            if (table == data->dict.end() || table->second.type != T::Array) continue;
            auto [index, fresh] = rowsByHash.try_emplace(name);
            if (fresh)
                for (size_t i = 0; i < table->second.array.size(); ++i)
                    if (const Node* h = table->second.array[i].type == T::Dict ? table->second.array[i].find("Hash") : nullptr)
                        index->second[h->u] = i;
            bool h32 = changes.type == T::HashMap32;
            if (!h32 && changes.type != T::HashMap64) throw std::runtime_error("game data: a changelog's table isn't a hash map");
            for (auto& [hash, change] : changes.hashes) {
                auto& mine = added[name];
                if (auto a = mine.find(hash); a != mine.end()) {
                    if (isRemove(change)) {
                        dropped[name].insert(hash);
                        continue;
                    }
                    if (h32) dropped[name].erase(hash);
                    a->second.changes.push_back(std::move(change));
                    continue;
                }
                if (auto r = index->second.find(hash); r != index->second.end()) {
                    if (isRemove(change)) {
                        dropped[name].insert(hash);
                        continue;
                    }
                    if (h32) dropped[name].erase(hash);
                    session.merge(table->second.array[r->second], change);
                    continue;
                }
                if (isRemove(change)) continue;  // removing a row nobody has
                mine[hash] = {std::move(change), h32 && name == "Struct", {}};
            }
        }
    }
    session.apply();
    for (auto& [name, rows] : added)
        for (auto& [hash, a] : rows) {
            if (a.changes.empty()) continue;
            // Later mods' versions of an added row: as changes to the first one.
            for (auto& c : a.changes) byml::logChanges("", c, a.row);
            byml::MergeSession rowSession(canonical, &a.row, a.structTable ? "Struct" : "");
            for (const auto& c : a.changes) rowSession.merge(a.row, c);
            rowSession.apply();
        }
    std::set<std::string> touched;
    for (const auto& [name, rows] : rowsByHash) touched.insert(name);
    for (const auto& name : touched) {
        auto& table = data->dict[name];
        auto& drop = dropped[name];
        table.array.erase(std::remove_if(table.array.begin(), table.array.end(),
                                         [&](const Node& row) {
                                             const Node* h = row.type == T::Dict ? row.find("Hash") : nullptr;
                                             return h && drop.count(h->u);
                                         }),
                          table.array.end());
        for (auto& [hash, a] : added[name])
            if (!drop.count(hash)) table.array.push_back(std::move(a.row));
        sortRows(table, "Hash");
    }
    if (auto* meta = doc.root.type == T::Dict ? &doc.root.dict["MetaData"] : nullptr; meta && meta->type == T::Dict) saveDataMetadata(*meta, *data);
    return byml::write(doc);
}

void saveDataMetadata(Node& metaData, const Node& tables) {
    static const char* valid[] = {
        "Binary",        "BinaryArray",   "Bool",        "Bool64bitKey", "BoolArray",      "Enum",          "EnumArray",
        "Float",         "FloatArray",    "Int",         "Int64",        "Int64Array",     "IntArray",      "String16",
        "String16Array", "String32",      "String32Array", "String64",   "String64Array",  "UInt",          "UInt64",
        "UInt64Array",   "UIntArray",     "Vector2",     "Vector2Array", "Vector3",        "Vector3Array",  "WString16",
        "WString16Array", "WString32",    "WString32Array", "WString64", "WString64Array",
    };
    constexpr int kDefault = 0x130;
    int offsets[7] = {}, sizes[7] = {};
    bool sizeHasKey[7] = {};
    int allOffset = kDefault, allSize = kDefault;
    const Node* dirs = metaData.find("SaveDirectory");
    auto count = [](const std::string& table, const Node& row) -> int {
        if (const Node* n = row.find("ArraySize")) return int(n->u);
        if (const Node* n = row.find("Size")) return int(n->u);
        if (const Node* n = row.find("DefaultValue"); n && n->type == T::Array) return int(n->array.size());
        throw std::runtime_error("game data: the length of an entry in " + table + " is missing");
    };
    auto entrySize = [&](const std::string& table, const Node& row, bool array) -> int {
        int n = array ? count(table, row) : 1, size = array ? 0xC : 0x8;
        std::string type = array ? table.substr(0, table.size() - 5) : table;
        if (table == "BoolArray") {
            double bytes = std::ceil(n / 8.0);
            return size + int(std::ceil((bytes > 3 ? bytes : 4) / 4)) * 4;
        }
        if (table == "IntArray" || table == "FloatArray" || table == "UIntArray" || table == "EnumArray") return size + n * 4;
        if (table == "Binary" || table == "BinaryArray") {
            const Node* d = row.find("DefaultValue");
            return d && d->type == T::UInt ? size + n * 4 + n * int(d->u) : size + n * 4;
        }
        int each = type == "UInt64" || type == "Int64" || type == "Vector2" ? 8
                   : type == "Vector3"                                     ? 12
                   : type == "String16"                                    ? 16
                   : type == "String32" || type == "WString16"             ? 32
                   : type == "String64" || type == "WString32"             ? 64
                   : type == "WString64"                                   ? 128
                                                                           : 0;
        return size + n * each;
    };
    for (const char* name : valid) {
        const Node* table = tables.find(name);
        if (!table || table->type != T::Array) continue;
        std::string tableName = name;
        bool key64 = tableName == "Bool64bitKey";
        bool array = tableName.size() > 5 && tableName.compare(tableName.size() - 5, 5, "Array") == 0;
        for (const auto& row : table->array) {
            const Node* idx = row.type == T::Dict ? row.find("SaveFileIndex") : nullptr;
            if (!idx || idx->type != T::Int) continue;
            int file = int(idx->i);
            bool unnamed = file == -1 || !dirs || file < 0 || size_t(file) >= dirs->array.size() || dirs->array[size_t(file)].s.empty();
            if (unnamed) {
                if (key64) continue;
                allOffset += 8;
                allSize += entrySize(tableName, row, array);
                continue;
            }
            if (file >= 7) continue;
            if (!key64) {
                offsets[file] += 8;
                allOffset += 8;
            } else if (!sizeHasKey[file]) {
                sizes[file] += 8;
                allSize += 8;
                sizeHasKey[file] = true;
            }
            int s = entrySize(tableName, row, array);
            sizes[file] += s;
            allSize += s;
        }
    }
    Node offsetList = Node::makeArray(), sizeList = Node::makeArray();
    for (int i = 0; i < 7; ++i) {
        offsetList.array.push_back(Node::integer(offsets[i] > 0 ? offsets[i] + kDefault : offsets[i]));
        sizeList.array.push_back(Node::integer(sizes[i] > 0 ? sizes[i] + kDefault : sizes[i]));
    }
    metaData.dict["AllDataSaveOffset"] = Node::integer(allOffset);
    metaData.dict["AllDataSaveSize"] = Node::integer(allSize);
    metaData.dict["SaveDataOffsetPos"] = std::move(offsetList);
    metaData.dict["SaveDataSize"] = std::move(sizeList);
}

}  // namespace mm::tkmm
