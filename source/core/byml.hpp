#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mm::byml {

// BYML: the game's tree format for data tables (ActorInfo, game data flags, map units,
// event info…). BOTW's files are little endian, version 2; Tears of the Kingdom's are
// version 7, which adds hash-keyed maps and aligned binary data. TKMM's changelogs add two
// more node types of their own (an array's changes, and a "removed" mark).
struct ArrayChange;

struct Node {
    enum class Type : uint8_t {
        None = 0x00,  // TKMM: no key
        HashMap32 = 0x20, HashMap64 = 0x21,
        String = 0xA0, Binary = 0xA1, BinaryAligned = 0xA2, Array = 0xC0, Dict = 0xC1, Bool = 0xD0, Int = 0xD1,
        Float = 0xD2, UInt = 0xD3, Int64 = 0xD4, UInt64 = 0xD5, Double = 0xD6,
        ArrayChangelog = 0xFD, Changelog = 0xFE,  // TKMM
        Null = 0xFF,
    };
    enum class Change : int32_t { Add = 0, Edit = 1, Remove = 2 };  // TKMM

    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;     // Int, Int64; Changelog: a Change
    uint64_t u = 0;    // UInt, UInt64; BinaryAligned: the alignment
    float f = 0;       // Float (kept as f32 so values round-trip exactly)
    double d = 0;      // Double
    std::string s;     // String, Binary, BinaryAligned
    std::vector<Node> array;
    std::map<std::string, Node> dict;  // std::string orders bytewise, as the game expects
    std::map<uint64_t, Node> hashes;   // HashMap32/64, by hash
    std::vector<ArrayChange> changes;  // ArrayChangelog

    Node() = default;
    static Node str(std::string v);
    static Node boolean(bool v);
    static Node integer(int32_t v);
    static Node uinteger(uint32_t v);
    static Node real(float v);
    static Node makeArray();
    static Node makeDict();
    static Node removed();

    bool isContainer() const {
        return type == Type::Array || type == Type::Dict || type == Type::HashMap32 || type == Type::HashMap64 ||
               type == Type::ArrayChangelog;
    }
    const Node* find(const std::string& key) const;  // Dict lookup
    bool operator==(const Node& o) const;
    bool operator!=(const Node& o) const { return !(*this == o); }
};

// One change of a TKMM array changelog: add, edit or remove the item at `index`, or the one
// whose key fields match (`key`, `key2`; Type::None when unused).
struct ArrayChange {
    int32_t index = 0;
    Node::Change change = Node::Change::Add;
    Node node, key, key2;
    bool operator==(const ArrayChange& o) const {
        return index == o.index && change == o.change && node == o.node && key == o.key && key2 == o.key2;
    }
};

inline Node Node::str(std::string v) { Node n; n.type = Type::String; n.s = std::move(v); return n; }
inline Node Node::boolean(bool v) { Node n; n.type = Type::Bool; n.b = v; return n; }
inline Node Node::integer(int32_t v) { Node n; n.type = Type::Int; n.i = v; return n; }
inline Node Node::uinteger(uint32_t v) { Node n; n.type = Type::UInt; n.u = v; return n; }
inline Node Node::real(float v) { Node n; n.type = Type::Float; n.f = v; return n; }
inline Node Node::makeArray() { Node n; n.type = Type::Array; return n; }
inline Node Node::makeDict() { Node n; n.type = Type::Dict; return n; }
inline Node Node::removed() { Node n; n.type = Type::Changelog; n.i = int64_t(Change::Remove); return n; }

struct Document {
    uint16_t version = 2;
    Node root;
};

Document parse(const std::string& data);  // Yaz0 or plain; throws on bad data
std::string write(const Document& doc);   // plain (little endian)

}  // namespace mm::byml
