#include "byml_merge.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace mm::byml {

namespace {

using T = Node::Type;
using Change = Node::Change;

bool isRemove(const Node& n) { return n.type == T::Changelog && n.i == int64_t(Change::Remove); }

// A value as text, equal exactly when the values are (to compare and look up key fields).
void keyText(const Node& n, std::string& out) {
    out += char(n.type);
    switch (n.type) {
        case T::String:
        case T::Binary:
        case T::BinaryAligned: out += std::to_string(n.s.size()) + ":" + n.s; break;
        case T::Bool: out += n.b ? '1' : '0'; break;
        case T::Int:
        case T::Int64:
        case T::Changelog: out += std::to_string(n.i); break;
        case T::UInt:
        case T::UInt64: out += std::to_string(n.u); break;
        case T::Float: {
            uint32_t bits;
            std::memcpy(&bits, &n.f, 4);
            out += std::to_string(bits);
            break;
        }
        case T::Double: {
            uint64_t bits;
            std::memcpy(&bits, &n.d, 8);
            out += std::to_string(bits);
            break;
        }
        case T::Array:
            out += "[";
            for (const auto& e : n.array) keyText(e, out);
            out += "]";
            break;
        case T::Dict:
            out += "{";
            for (const auto& [k, v] : n.dict) {
                out += std::to_string(k.size()) + ":" + k;
                keyText(v, out);
            }
            out += "}";
            break;
        case T::HashMap32:
        case T::HashMap64:
            out += "{";
            for (const auto& [h, v] : n.hashes) {
                out += std::to_string(h) + ":";
                keyText(v, out);
            }
            out += "}";
            break;
        default: break;
    }
    out += ';';
}

// A key: the primary field's value and, for pairs, the second's ("" text = no key).
struct Key {
    std::string text;
    bool empty() const { return text.empty(); }
    bool operator==(const Key& o) const { return text == o.text; }
};

Key makeKey(const Node* primary, const Node* secondary) {
    Key k;
    if (!primary) return k;
    keyText(*primary, k.text);
    k.text += '|';
    if (secondary) keyText(*secondary, k.text);
    else k.text += '-';
    return k;
}

struct KeyName {
    std::string primary, secondary;
    bool empty() const { return primary.empty(); }
};

const Node* valueAt(const Node& map, const std::string& path) {
    auto dot = path.find('.');
    auto it = map.dict.find(dot == std::string::npos ? path : path.substr(0, dot));
    if (it == map.dict.end()) return nullptr;
    if (dot == std::string::npos) return &it->second;
    return it->second.type == T::Dict ? valueAt(it->second, path.substr(dot + 1)) : nullptr;
}

Key keyOf(const Node& item, const KeyName& name) {
    if (name.empty() || item.type != T::Dict) return {};
    return makeKey(valueAt(item, name.primary), name.secondary.empty() ? nullptr : valueAt(item, name.secondary));
}

// The key fields the merger looks items up by: the builder's keyed rules, and NameHash.
KeyName mergerKeyName(std::string_view arrayName, std::string_view type, int depth) {
    auto rule = arrayRule(arrayName, type, depth);
    if (rule.kind == ArrayRule::Kind::Keyed) return {rule.primary, rule.secondary};
    if (rule.kind == ArrayRule::Kind::NameHash) return {"NameHash", ""};
    return {};
}

std::unordered_map<std::string, int> indexCache(const Node& array, const KeyName& name) {
    std::unordered_map<std::string, int> out;
    if (array.array.empty() || (array.array.size() == 1 && array.array[0].type != T::Dict)) return out;
    for (size_t i = 0; i < array.array.size(); ++i) {
        auto k = keyOf(array.array[i], name);
        if (!k.empty()) out[k.text] = int(i);
    }
    return out;
}

// ---------------------------------------------------------------- changelog builder

struct Info {
    std::string type;
    int depth = 0;
};

bool logInline(Info& info, Node& src, const Node& vanilla);

Node makeChangelog(std::vector<ArrayChange> changes) {
    Node n;
    n.type = T::ArrayChangelog;
    n.changes = std::move(changes);
    return n;
}

ArrayChange change(int index, Change c, Node node, const Node* key = nullptr, const Node* key2 = nullptr) {
    ArrayChange a;
    a.index = index;
    a.change = c;
    a.node = std::move(node);
    if (key) a.key = *key;
    else a.key.type = T::None;
    if (key2) a.key2 = *key2;
    else a.key2.type = T::None;
    return a;
}

// Items compared whole: unchanged ones found anywhere in the game's array, the rest edits
// (same place) or additions.
bool logPlainArray(Info& info, Node& root, const Node& vanilla) {
    auto src = std::move(root.array);
    std::vector<ArrayChange> out;
    std::vector<size_t> additions;  // a queue
    size_t additionsHead = 0;
    std::vector<bool> vanillaFound(vanilla.array.size()), srcIsVanilla(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        size_t j = 0;
        for (; j < vanilla.array.size(); ++j)
            if (!vanillaFound[j] && vanilla.array[j] == src[i]) break;
        if (j == vanilla.array.size()) {
            additions.push_back(i);
            continue;
        }
        srcIsVanilla[i] = true;
        vanillaFound[j] = true;
    }
    for (size_t i = 0; i < vanilla.array.size(); ++i) {
        if (vanillaFound[i]) continue;
        if (i < src.size() && !srcIsVanilla[i]) {
            Node element = src[i];
            logInline(info, element, vanilla.array[i]);
            out.push_back(change(int(i), Change::Edit, std::move(element)));
            ++additionsHead;  // as TKMM: the edit takes the place of the oldest addition
            continue;
        }
        out.push_back(change(int(i), Change::Remove, Node()));
    }
    if (additionsHead < additions.size()) {
        int at = int(additions[additionsHead]);
        for (size_t k = additionsHead; k < additions.size(); ++k) out.push_back(change(at, Change::Add, src[additions[k]]));
    }
    bool same = out.empty();
    root = makeChangelog(std::move(out));
    return same;
}

bool logKeyedArray(Info& info, Node& root, const Node& vanilla, const KeyName& name) {
    auto src = std::move(root.array);
    std::vector<ArrayChange> out;
    int added = 0;
    auto lookup = indexCache(vanilla, name);
    std::vector<bool> vanillaFound(vanilla.array.size());
    for (size_t i = 0; i < src.size(); ++i) {
        Node& node = src[i];
        auto k = keyOf(node, name);
        if (k.empty()) {
            out.push_back(change(int(i) - added, Change::Add, node));
            ++added;
            continue;
        }
        auto it = lookup.find(k.text);
        if (it == lookup.end()) {
            int relative = int(i) - added;
            out.push_back(change(int(vanilla.array.size()) > relative ? relative : int(i), Change::Add, node));
            ++added;
            continue;
        }
        int v = it->second;
        const Node* primary = valueAt(node, name.primary);
        const Node* secondary = name.secondary.empty() ? nullptr : valueAt(node, name.secondary);
        Node p = *primary, s = secondary ? *secondary : Node();
        if (!logInline(info, node, vanilla.array[size_t(v)]))
            out.push_back(change(v, Change::Edit, node, &p, secondary ? &s : nullptr));
        vanillaFound[size_t(v)] = true;
    }
    for (size_t i = 0; i < vanilla.array.size(); ++i)
        if (!vanillaFound[i]) out.push_back(change(int(i), Change::Remove, Node()));
    bool same = out.empty();
    root = makeChangelog(std::move(out));
    return same;
}

bool logNameHashArray(Info& info, Node& root, const Node& vanilla) {
    auto src = std::move(root.array);
    std::vector<ArrayChange> out;
    std::vector<bool> vanillaFound(vanilla.array.size());
    for (size_t i = 0; i < src.size(); ++i) {
        Node& node = src[i];
        const Node* hash = node.type == T::Dict ? node.find("NameHash") : nullptr;
        if (!hash || (hash->type != T::UInt && hash->type != T::Int)) throw std::runtime_error("BYML: an item has no NameHash");
        int v = -1;
        for (size_t j = 0; j < vanilla.array.size() && v < 0; ++j) {
            const Node* other = vanilla.array[j].type == T::Dict ? vanilla.array[j].find("NameHash") : nullptr;
            if (other && *other == *hash) v = int(j);
        }
        if (v < 0) {
            out.push_back(change(int(i), Change::Add, node));
            continue;
        }
        Node key = *hash;
        if (!logInline(info, node, vanilla.array[size_t(v)])) out.push_back(change(v, Change::Edit, node, &key));
        vanillaFound[size_t(v)] = true;  // (TkSharp marks the mod's index here)
    }
    for (size_t i = 0; i < vanilla.array.size(); ++i)
        if (!vanillaFound[i]) out.push_back(change(int(i), Change::Remove, Node()));
    bool same = out.empty();
    root = makeChangelog(std::move(out));
    return same;
}

bool logDirectIndexArray(Info& info, Node& root, const Node& vanilla) {
    auto src = std::move(root.array);
    std::vector<ArrayChange> out;
    size_t smaller = std::min(src.size(), vanilla.array.size()), larger = std::max(src.size(), vanilla.array.size());
    size_t i = 0;
    for (; i < smaller; ++i) {
        Node e = src[i];
        if (!logInline(info, e, vanilla.array[i])) out.push_back(change(int(i), Change::Edit, std::move(e)));
    }
    for (; i < larger; ++i) {
        if (vanilla.array.size() < src.size()) out.push_back(change(int(i), Change::Add, src[i]));
        else out.push_back(change(int(i), Change::Remove, Node()));
    }
    bool same = out.empty();
    root = makeChangelog(std::move(out));
    return same;
}

bool logArray(Info& info, Node& root, const Node& vanilla, std::string_view key) {
    auto rule = arrayRule(key, info.type, info.depth);
    switch (rule.kind) {
        case ArrayRule::Kind::Keyed: return logKeyedArray(info, root, vanilla, {rule.primary, rule.secondary});
        case ArrayRule::Kind::NameHash: return logNameHashArray(info, root, vanilla);
        case ArrayRule::Kind::DirectIndex: return logDirectIndexArray(info, root, vanilla);
        case ArrayRule::Kind::Plain: break;
    }
    return logPlainArray(info, root, vanilla);
}

template <typename K>
bool logMap(Info& info, std::map<K, Node>& src, const std::map<K, Node>& vanilla) {
    ++info.depth;
    std::vector<K> keys;
    for (const auto& [k, v] : src) keys.push_back(k);
    for (const auto& [k, v] : vanilla)
        if (!src.count(k)) keys.push_back(k);
    for (const auto& key : keys) {
        auto s = src.find(key);
        if (s == src.end()) {
            src[key] = Node::removed();
            continue;
        }
        auto v = vanilla.find(key);
        if (v == vanilla.end()) continue;
        bool same;
        if constexpr (std::is_same_v<K, std::string>) {
            if (s->second.type == T::Array && v->second.type == T::Array) same = logArray(info, s->second, v->second, key);
            else same = logInline(info, s->second, v->second);
        } else {
            same = logInline(info, s->second, v->second);
        }
        if (same) src.erase(s);
    }
    --info.depth;
    return src.empty();
}

bool logInline(Info& info, Node& src, const Node& vanilla) {
    if (src.type != vanilla.type) return false;
    switch (src.type) {
        case T::HashMap32:
        case T::HashMap64: return logMap(info, src.hashes, vanilla.hashes);
        case T::Dict: return logMap(info, src.dict, vanilla.dict);
        case T::Array:
            if (info.type == "ecocat" && info.depth == 0) return logKeyedArray(info, src, vanilla, {"AreaNumber", ""});
            return logPlainArray(info, src, vanilla);
        case T::String:
        case T::Binary:
        case T::BinaryAligned:
        case T::Bool:
        case T::Int:
        case T::Float:
        case T::UInt:
        case T::Int64:
        case T::UInt64:
        case T::Double:
        case T::Null: return src == vanilla;
        default: throw std::runtime_error("BYML: can't compare a changelog with the game's file");
    }
}

// ---------------------------------------------------------------- merger

struct ArrayTracking {
    std::string name;
    bool named = false;
    int depth = 0;
    std::vector<std::vector<std::pair<int, Node>>> additions;  // one list per changelog
    std::set<int> removals;
};

struct MapTracking {
    std::set<std::string> names;
    std::set<uint64_t> hashes;
};

struct Tracking {
    std::string canonical, type;
    std::string typeOverride;  // TKMM's BymlMergeTracking.Type (null there = empty here)
    Node* root = nullptr;
    int depth = 0;
    std::unordered_map<Node*, MapTracking> maps;
    std::unordered_map<Node*, ArrayTracking> arrays;

    Tracking(std::string_view c, Node* r) : canonical(c), type(fileType(c)), root(r) {}
    void apply();
};

void mergeNode(Node& base, const Node& changelog, Tracking& t);

void mergeArray(Node& base, const Node& changelog, const std::string* name, Tracking& t) {
    std::vector<std::pair<int, Node>>* additions = nullptr;
    // As TkSharp: lookups by key only for rules that don't depend on the file's type (unless
    // the merge names one, as game data's Struct table).
    KeyName keyName = name ? mergerKeyName(*name, t.typeOverride, t.depth) : KeyName{};
    std::optional<std::unordered_map<std::string, int>> lookup;
    for (const auto& c : changelog.changes) {
        switch (c.change) {
            case Change::Add: {
                auto it = t.arrays.find(&base);
                if (it == t.arrays.end()) {
                    ArrayTracking a;
                    if (name) {
                        a.name = *name;
                        a.named = true;
                    }
                    a.depth = t.depth;
                    it = t.arrays.emplace(&base, std::move(a)).first;
                }
                if (!additions) {
                    it->second.additions.emplace_back();
                    additions = &it->second.additions.back();
                }
                additions->emplace_back(c.index, c.node);
                break;
            }
            case Change::Remove:
                t.arrays[&base].removals.insert(c.index);  // (unnamed, as TkSharp, when first seen here)
                break;
            case Change::Edit: {
                if (auto it = t.arrays.find(&base); it != t.arrays.end()) it->second.removals.erase(c.index);
                Key key = makeKey(c.key.type == T::None ? nullptr : &c.key, c.key2.type == T::None ? nullptr : &c.key2);
                size_t index = size_t(c.index);
                bool atIndex = index < base.array.size() && keyOf(base.array[index], keyName) == key;
                if (!keyName.empty() && !key.empty() && !atIndex) {
                    if (!lookup) lookup = indexCache(base, keyName);
                    auto found = lookup->find(key.text);
                    if (found == lookup->end()) throw std::runtime_error("BYML: no item with the changed item's key in " + t.canonical);
                    index = size_t(found->second);
                }
                if (index >= base.array.size()) throw std::runtime_error("BYML: a changed item is past the end of a list in " + t.canonical);
                Node& target = base.array[index];
                if (c.node.isContainer()) mergeNode(target, c.node, t);
                else target = c.node;
                break;
            }
        }
    }
}

template <typename K>
void mergeMap(Node& base, std::map<K, Node>& baseMap, const std::map<K, Node>& changes, Tracking& t) {
    ++t.depth;
    for (const auto& [key, entry] : changes) {
        if (isRemove(entry)) {
            if constexpr (std::is_same_v<K, std::string>) t.maps[&base].names.insert(key);
            else t.maps[&base].hashes.insert(key);
            continue;
        }
        if (auto it = t.maps.find(&base); it != t.maps.end()) {
            if constexpr (std::is_same_v<K, std::string>) it->second.names.erase(key);
            else it->second.hashes.erase(key);
        }
        auto b = baseMap.find(key);
        if (b == baseMap.end()) {
            baseMap.emplace(key, entry);
            continue;
        }
        if constexpr (std::is_same_v<K, std::string>) {
            if (entry.type == T::ArrayChangelog && b->second.type == T::Array) {
                mergeArray(b->second, entry, &key, t);
                continue;
            }
        }
        if (entry.isContainer() && b->second.isContainer()) {
            mergeNode(b->second, entry, t);
            continue;
        }
        b->second = entry;
    }
    --t.depth;
}

void mergeNode(Node& base, const Node& changelog, Tracking& t) {
    if (base.type == T::Dict && changelog.type == T::Dict) return mergeMap(base, base.dict, changelog.dict, t);
    if ((base.type == T::HashMap32 || base.type == T::HashMap64) && changelog.type == base.type)
        return mergeMap(base, base.hashes, changelog.hashes, t);
    if (base.type == T::Array && changelog.type == T::ArrayChangelog) return mergeArray(base, changelog, nullptr, t);
    if (base.type == T::Array && changelog.type == T::Array) {
        base.array.insert(base.array.end(), changelog.array.begin(), changelog.array.end());
        return;
    }
    throw std::runtime_error("BYML: a changelog doesn't match the game's file in " + t.canonical);
}

int insertAt(int& offset, Node& base, int insertIndex, Node node) {
    int relative = insertIndex + offset;
    ++offset;
    if (int(base.array.size()) > relative) {
        base.array.insert(base.array.begin() + relative, std::move(node));
        return relative;
    }
    base.array.push_back(std::move(node));
    return int(base.array.size()) - 1;
}

// Several mods adding an item with the same key: merged into one, as changes to the first.
int mergeKeyed(const std::string& canonical, const Info& info, Node merged, std::vector<Node> entries, int& offset, Node& array,
               int insertIndex) {
    for (auto& e : entries) {
        Info i = info;
        logInline(i, e, merged);
    }
    Tracking t(canonical, &merged);
    for (const auto& e : entries) mergeNode(merged, e, t);
    t.apply();
    return insertAt(offset, array, insertIndex, std::move(merged));
}

void applyArray(Tracking& t, Node& base, ArrayTracking& a) {
    Info info{t.typeOverride.empty() ? t.type : t.typeOverride, a.depth};
    int offset = 0;
    for (int i : a.removals)
        if (i >= 0 && size_t(i) < base.array.size()) base.array[size_t(i)] = Node::removed();
    // Additions by where they go (first seen first within each place).
    std::map<int, std::vector<Node>> byIndex;
    for (auto& list : a.additions)
        for (auto& [index, node] : list) byIndex[index].push_back(std::move(node));
    KeyName keyName = a.named ? mergerKeyName(a.name, info.type, info.depth) : KeyName{};
    std::unordered_map<std::string, int> keyed;
    for (auto& [insertIndex, entries] : byIndex) {
        // Grouped by key, in the order the keys first appear.
        std::vector<std::pair<Key, std::vector<Node>>> groups;
        for (auto& e : entries) {
            auto k = keyOf(e, keyName);
            auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& x) { return x.first == k; });
            if (g == groups.end()) groups.push_back({k, {}}), g = groups.end() - 1;
            g->second.push_back(std::move(e));
        }
        for (auto& [key, list] : groups) {
            if (key.empty()) {
                for (auto& e : list) insertAt(offset, base, insertIndex, std::move(e));
                continue;
            }
            if (auto old = keyed.find(key.text); old != keyed.end()) {
                Node existing = std::move(base.array[size_t(old->second)]);
                base.array[size_t(old->second)] = Node::removed();
                old->second = mergeKeyed(t.canonical, info, std::move(existing), std::move(list), offset, base, insertIndex);
                continue;
            }
            int at;
            if (list.size() == 1) {
                at = insertAt(offset, base, insertIndex, std::move(list[0]));
            } else {
                Node first = std::move(list[0]);
                list.erase(list.begin());
                at = mergeKeyed(t.canonical, info, std::move(first), std::move(list), offset, base, insertIndex);
            }
            keyed.emplace(key.text, at);
        }
    }
    base.array.erase(std::remove_if(base.array.begin(), base.array.end(), isRemove), base.array.end());
}

// The tracked containers still in the tree, deepest first (changing one moves those below it).
template <typename M>
std::vector<Node*> liveDeepestFirst(Node* root, const M& tracked) {
    std::vector<std::pair<int, Node*>> found;
    std::vector<std::pair<Node*, int>> stack{{root, 0}};
    while (!stack.empty()) {
        auto [n, level] = stack.back();
        stack.pop_back();
        if (tracked.count(n)) found.emplace_back(level, n);
        for (auto& e : n->array) stack.push_back({&e, level + 1});
        for (auto& [k, v] : n->dict) stack.push_back({&v, level + 1});
        for (auto& [k, v] : n->hashes) stack.push_back({&v, level + 1});
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Node*> out;
    for (auto& [level, n] : found) out.push_back(n);
    return out;
}

void Tracking::apply() {
    for (Node* n : liveDeepestFirst(root, maps)) {
        auto& m = maps[n];
        for (const auto& k : m.names) n->dict.erase(k);
        for (auto h : m.hashes) n->hashes.erase(h);
    }
    for (Node* n : liveDeepestFirst(root, arrays))
        if (n->type == T::Array) applyArray(*this, *n, arrays[n]);
}

}  // namespace

struct MergeSession::Impl {
    Tracking t;
};

MergeSession::MergeSession(std::string_view canonical, Node* root, std::string type) : impl_(new Impl{Tracking(canonical, root)}) {
    impl_->t.typeOverride = std::move(type);
}
MergeSession::~MergeSession() = default;

void MergeSession::mergeMap(Node& base, const Node& changes) {
    if (base.type == T::Dict && changes.type == T::Dict) ::mm::byml::mergeMap(base, base.dict, changes.dict, impl_->t);
    else mergeNode(base, changes, impl_->t);
}
void MergeSession::merge(Node& base, const Node& changelog) { mergeNode(base, changelog, impl_->t); }
void MergeSession::apply() { impl_->t.apply(); }

std::string fileType(std::string_view canonical) {
    auto slash = canonical.rfind('/');
    auto name = canonical.substr(slash == std::string_view::npos ? 0 : slash + 1);
    auto last = name.rfind('.');
    if (last == std::string_view::npos) return {};
    name = name.substr(0, last);
    auto dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string() : std::string(name.substr(dot + 1));
}

ArrayRule arrayRule(std::string_view key, std::string_view type, int depth) {
    using K = ArrayRule::Kind;
    auto keyed = [](std::string p, std::string s = {}) { return ArrayRule{K::Keyed, std::move(p), std::move(s)}; };
    auto in = [&](std::initializer_list<std::string_view> names) { return std::find(names.begin(), names.end(), key) != names.end(); };
    const ArrayRule plain{}, direct{K::DirectIndex, {}, {}};
    if (in({"Animal", "Enemy", "FallFloorInsect", "Fish", "GrassCut", "Insect", "NotDecayedLargeSwordList", "NotDecayedSmallSwordList",
            "NotDecayedSpearList", "RainBonusMaterial", "Seafood", "SpObjCapsuleBlockMaster", "Weapon", "bow", "bows", "shields", "weapons",
            "helmets"}))
        return keyed("name");
    if (key == "Actors") return type == "bcett" ? keyed("Hash") : type == "game__component__ArmyManagerParam" ? keyed("ActorName") : plain;
    if (key == "Table")
        return type == "game__colorvariation__ConversionActorNameToParasailPatternSetTable" ? keyed("ParasailPattern")
               : type == "game__ecosystem__DecayedWeaponMappingTable"                       ? keyed("EquipmentDeathCountGmdHash")
                                                                                            : plain;
    if (key == "BoneInfoArray")
        return type == "game__component__DragonParam" ? keyed("Hash")
               : (type == "phive__LookIKResourceHeaderParam" || type == "phive__TailBoneResourceHeaderParam") ? keyed("BoneName")
                                                                                                              : plain;
    if (key == "Elements")
        return type == "game__enemy__DrakeSubModelInfo"                ? keyed("BoneName")
               : type == "game__gamebalance__LevelSensorTargetDefine" ? keyed("ActorNameHash", "Plus")
                                                                       : plain;
    if (key == "Items") return type == "game__pouchcontent__EnhancementMaterial" ? keyed("Actor") : plain;
    if (key == "List") return type == "game__sound__ShrineSpotBgmTypeInfoList" ? keyed("DungeonIndexStr") : plain;
    if (key == "Contents") return type == "game__ui__FairyFountainGlobalSetting" ? keyed("Actor") : plain;
    if (key == "SettingTable") return type == "game__ui__LargeDungeonFloorDefaultSettingTable" ? keyed("DungeonType") : plain;
    if (key == "BrainVerbs") return keyed("ActionSeqContainer");
    if (key == "ActionVerbContainerElements") return keyed("ActionVerb");
    if (in({"ResidentActors", "Settings", "ShootableShareActorSettings", "GoodsList"})) return keyed("Actor");
    if (key == "BindActorInfo") return keyed("ActorHolderKey");
    if (in({"RegisteredActorArray", "RequirementList", "Rewards"})) return keyed("ActorName");
    if (in({"SharpInfoList", "SharpInfoBowList", "SharpInfoShieldList"})) return keyed("ActorNameHash");
    if (key == "PictureBookParamArray") return keyed("ActorNameShort");
    if (key == "NavMeshObjects") return keyed("Alias");
    if (key == "AliasEntityList") return keyed("AliasEntity");
    if (key == "AliasSensorList") return keyed("AliasSensor");
    if (key == "Anchors") return keyed("AnchorName");
    if (key == "ArmorEffect") return keyed("ArmorEffectType");
    if (key == "HornTypeAndAttachmentMapping") return keyed("AttachmentName", "HornBoneName");
    if (key == "AttackParams") return keyed("AttackType");
    if (in({"BlackboardParamBoolArray", "BlackboardParamCustomTypeArray", "BlackboardParamF32Array", "BlackboardParamMtx33fArray",
            "BlackboardParamMtx34fArray", "BlackboardParamPtrArray", "BlackboardParamQuatfArray", "BlackboardParamS32Array",
            "BlackboardParamS8Array", "BlackboardParamStringArray", "BlackboardParamU32Array", "BlackboardParamU64Array",
            "BlackboardParamU8Array", "BlackboardParamVec3fArray", "EditBBParams"}))
        return keyed("BBKey");
    if (key == "DragonInfoList") return keyed("BindPointRespawnGameDataHash");
    if (in({"OperationAngular", "OperationLinear"})) return keyed("Body");
    if (in({"BindBoneList", "BoneList", "BoneModifierSet", "Bones", "FruitOffsetTranslation", "ModelBindSettings", "StickWeaponBone"}))
        return keyed("BoneName");
    if (key == "Categories") return keyed("CallbackName");
    if (key == "CaveParams") return keyed("CaveInstanceId");
    if (key == "CheckList") return keyed("CheckType");
    if (key == "Object") return keyed("ChemicalMaterial");
    if (in({"CharacterComponentPresetCollection", "LayerHitMaskEntityCollection", "LayerHitMaskSensorCollection", "MaterialCollection",
            "MaterialPresetCollection", "MotionPropertiesCollection", "PhysicsMaterialMappingInfoCollection",
            "SubLayerHitMaskEntityCollection", "SubLayerHitMaskSensorCollection", "UserShapeTagMaskCollection"}))
        return keyed("ComponentName");
    if (in({"HingeArray", "RangeArray"})) return keyed("ConstraintName");
    if (key == "CropYieldTable") return keyed("CropName");
    if (key == "DungeonBossDifficultyGameData") return keyed("DefeatedNumGameDataHash");
    if (in({"DoCondition", "FinCondition", "PickConditions", "SuccessCondition"})) return keyed("DefineNameHash");
    if (key == "FallenActorTable") return keyed("DropActorName");
    if (key == "SmallDungeonLocationList") return keyed("DungeonIndexStr");
    if (key == "VariationListForArmorDye") return keyed("DyeColor");
    if (key == "HackEquip") return keyed("EquipUserBbKey");
    if (in({"AdventureMemorySetArray", "GlobalResidentEventList"})) return keyed("EventName");
    if (in({"AutoPlayBoneVisibilities", "AutoPlayMaterials"})) return keyed("FileName");
    if (key == "WinningRateTable") return keyed("FlintstonesNum");
    if (key == "ModelVariationAnims") return keyed("Fmab");
    if (key == "OptionParam") return keyed("FootIKMode");
    if (key == "PartialList") return keyed("GameData");
    if (key == "PlacementGroups") return keyed("GroupID");
    if (in({"EffectLimiterGroup", "HiddenMaterialGroupList"})) return keyed("GroupName");
    if (key == "Textures") return keyed("guid");
    if (key == "Points") return keyed("Hash");
    if (key == "Rails") return depth == 0 ? keyed("Hash") : plain;
    if (key == "HeadshotDamageParameters") return keyed("HeadshotBoneName");
    if (key == "TransitionParam") return keyed("Index");
    if (key == "OverwriteParam") return keyed("InstanceId");
    if (in({"Interests", "StrongInterests"})) return keyed("InterestType");
    if (in({"ConditionArray", "OverrideASEvReactVerbSettings", "SwitchParam", "TriggerParams", "Triggers"})) return keyed("Key");
    if (key == "OverrideASEventReactSettings") return keyed("Key", "Intensity");
    if (key == "OverrideReactionVerbSettings") return keyed("KeyActionVerb", "Setting.OverrideActionVerb");
    if (key == "ShootableActorSettings") return keyed("KeyHash", "Actor");
    if (in({"AttachmentGroupList", "EnemyGroupList"})) return keyed("Label");
    if (in({"ShopWeaponGroupList", "WeaponGroupList"})) return keyed("Label", "EquipmentType");
    if (key == "ActionSeqs") return keyed("LabelHash");
    if (in({"CaveEntranceNormal", "CaveEntranceSpecial", "CaveEntranceWell", "CheckPoint", "City", "District", "DragonTears", "Dungeon",
            "Ground", "ShopArmor", "ShopDye", "ShopGeneral", "ShopInn", "ShopJewelry", "Shrine", "SkyArchipelago", "SpotBig",
            "SpotBigArtifact", "SpotBigMagma", "SpotBigMountain", "SpotBigOther", "SpotBigTimber", "SpotBigWater", "SpotBigWithNameIcon",
            "SpotMiddle", "SpotMiddleArtifact", "SpotMiddleMagma", "SpotMiddleMountain", "SpotMiddleOther", "SpotMiddleTimber",
            "SpotMiddleWater", "SpotSmallArtifact", "SpotSmallMagma", "SpotSmallMountain", "SpotSmallOther", "SpotSmallTimber",
            "SpotSmallWater", "Stable", "Tower", "Underground"}))
        return type == "locationarea" ? keyed("LocationName") : plain;
    if (key == "MiasmaAreaParam") return keyed("MiasmaAreaType");
    if (in({"AnimationDrive", "CColEntityNamePathAry", "CColSensorNamePathAry", "CheckPointSetting", "Cloth", "ClothAdvandecOption",
            "ClothList", "ClothReaction", "CollectItem", "CollidableList", "ControllerEntityNamePathAry", "ControllerSensorNamePathAry",
            "ControllerSensorUnitAry", "ExternalShapeNamePathAry", "HelperBoneList", "IntData", "LookIKControllerNamePathAry",
            "LookingControllerNamePathAry", "MatterRigidBodyNamePathAry", "MeshList", "ParamTable", "RagdollReaction",
            "RagdollReactionList", "RagdollStructure", "Reaction", "RigidBodyEntityNamePathAry", "RigidBodySensorNamePathAry",
            "ShapeList", "ShapeNamePathAry", "StringData", "TailBoneControllerNamePathAry", "Node"}))
        return keyed("Name");
    if (key == "TowingHookParams") return keyed("NameHash");
    if (key == "Property") return {K::NameHash, {}, {}};
    if (in({"ExtraNewsSourceInfo", "TopNewsSourceInfo"})) return keyed("NewsKeyName");
    if (key == "PictureBookPackInfoArray") return keyed("PackActor");
    if (key == "VariationListForParasail") return keyed("Pattern");
    if (key == "PhshMesh") return keyed("PhshMeshPath");
    if (key == "PlgGdTable") return keyed("PlgGuid");
    if (in({"PropertyDefinitions", "Sources"})) return keyed("PropertyNameHash");
    if (key == "Connections") return keyed("RailHash");
    if (in({"CustomCullInfos", "SpecialCullInfos"})) return keyed("ResourceName");
    if (key == "Seats") return keyed("RidableType");
    if (key == "VisibleSageOnNonMember") return keyed("SageType");
    if (key == "SeriesArmorEffectList") return keyed("SeriesName");
    if (key == "CropActorTable") return keyed("SrcActorName");
    if (key == "SuspiciosBuffs") return keyed("SuspiciousBuffType");
    if (key == "BoostOnlyTable") return keyed("Target");
    if (key == "PartialConfigs") return keyed("TargetName");
    if (in({"OverwritePropertiesEffect", "OverwritePropertiesSound"})) return keyed("TargetTypeNameHash");
    if (key == "BGParamArray") return keyed("TexName");
    if (key == "TipsSetArray") return keyed("TipsType");
    if (key == "TmbMesh") return keyed("TmbMeshPath");
    if (in({"ActorPositionData", "EventEntry"})) return keyed("$type");
    if (in({"SB", "T", "U"})) return keyed("Umii");
    if (key == "AlreadyReadInfo") return keyed("UpdateGameDataFlag");
    if (key == "ConditionList") return keyed("WeaponEssence");
    if (key == "WeaponTypeAndSubModelMapping") return keyed("WeaponType");
    if (in({"Translate", "Rotate", "Scale", "MarginNegative", "MarginPositive", "Rot", "Trans", "Pivot", "PlayerPosOnClearEvent",
            "EnokidaCameraPos", "WeakPointActorArray", "WeakPointUser", "NearWoodStoragePos", "StaffRollSetArray"}))
        return direct;
    if (in({"ArmorCategory", "ArmorDyeColor", "ArmorSeries", "BowEffect", "FoodEffect", "MaterialBowAttachmentTag", "MaterialCategory",
            "ShieldEffect", "WeaponCategory", "WeaponEffect"}))
        return type == "game__ui__PouchSortTable" ? direct : plain;
    if (key == "RecipeArray") return type == "game__cooking__RecipeCardTable" ? direct : plain;
    if (key == "DefaultValue" && type == "Struct") return keyed("Hash");  // (the merger's table only)
    return type == "game__component__ConditionParam" ? direct : plain;
}

bool logChanges(std::string_view type, Node& src, const Node& vanilla, int depth) {
    Info info{std::string(type), depth};
    return logInline(info, src, vanilla);
}

void merge(std::string_view canonical, Node& base, const std::vector<const Node*>& changelogs) {
    Tracking t(canonical, &base);
    for (const Node* c : changelogs) mergeNode(base, *c, t);
    t.apply();
}

std::string mergeFiles(std::string_view canonical, const std::string& vanilla, const std::vector<std::string_view>& changelogs) {
    auto doc = parse(vanilla);
    std::vector<Document> parsed;
    parsed.reserve(changelogs.size());
    for (auto c : changelogs) parsed.push_back(parse(std::string(c)));
    std::vector<const Node*> roots;
    for (const auto& p : parsed) roots.push_back(&p.root);
    merge(canonical, doc.root, roots);
    return write(doc);
}

std::optional<std::string> buildChangelog(std::string_view canonical, const std::string& modFile, const std::string& vanilla) {
    auto src = parse(modFile);
    auto game = parse(vanilla);
    if (logChanges(fileType(canonical), src.root, game.root)) return std::nullopt;
    return write(src);
}

}  // namespace mm::byml
