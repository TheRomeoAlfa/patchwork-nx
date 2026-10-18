#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mm::byml {

// BYML: the game's tree format for data tables (ActorInfo, game data flags, map units,
// event info…). Switch files are little endian, version 2.
struct Node {
    enum class Type : uint8_t {
        String = 0xA0, Binary = 0xA1, Array = 0xC0, Dict = 0xC1, Bool = 0xD0, Int = 0xD1, Float = 0xD2, UInt = 0xD3,
        Int64 = 0xD4, UInt64 = 0xD5, Double = 0xD6, Null = 0xFF,
    };
    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;     // Int, Int64
    uint64_t u = 0;    // UInt, UInt64
    float f = 0;       // Float (kept as f32 so values round-trip exactly)
    double d = 0;      // Double
    std::string s;     // String, Binary
    std::vector<Node> array;
    std::map<std::string, Node> dict;  // std::string orders bytewise, as the game expects

    Node() = default;
    static Node str(std::string v) { Node n; n.type = Type::String; n.s = std::move(v); return n; }
    static Node boolean(bool v) { Node n; n.type = Type::Bool; n.b = v; return n; }
    static Node integer(int32_t v) { Node n; n.type = Type::Int; n.i = v; return n; }
    static Node uinteger(uint32_t v) { Node n; n.type = Type::UInt; n.u = v; return n; }
    static Node real(float v) { Node n; n.type = Type::Float; n.f = v; return n; }
    static Node makeArray() { Node n; n.type = Type::Array; return n; }
    static Node makeDict() { Node n; n.type = Type::Dict; return n; }

    bool isContainer() const { return type == Type::Array || type == Type::Dict; }
    const Node* find(const std::string& key) const;  // Dict lookup
    bool operator==(const Node& o) const;
    bool operator!=(const Node& o) const { return !(*this == o); }
};

struct Document {
    uint16_t version = 2;
    Node root;
};

Document parse(const std::string& data);  // Yaz0 or plain; throws on bad data
std::string write(const Document& doc);   // plain (little endian)

}  // namespace mm::byml
