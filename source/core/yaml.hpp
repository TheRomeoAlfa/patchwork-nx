#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mm::yaml {

// A reader for the YAML BCML writes its logs in (oead's flavour): block and flow mappings
// and sequences, plain and quoted scalars, block scalars, and tags such as !u, !obj, !list,
// !str64 that say which game type a value has. Anchors, aliases and multiple documents
// aren't used there and aren't supported.
struct Node {
    enum class Kind { Null, Scalar, Seq, Map };
    Kind kind = Kind::Null;
    std::string tag;     // "!u", "!obj"… ("" when untagged)
    std::string value;   // scalar text (quotes removed, escapes applied)
    bool quoted = false; // a quoted scalar is always a string
    std::vector<Node> items;                       // Seq
    std::vector<std::pair<std::string, Node>> map; // Map, in file order

    bool isNull() const { return kind == Kind::Null || (kind == Kind::Scalar && !quoted && (value == "~" || value == "null")); }
    const Node* find(const std::string& key) const;
    const Node& operator[](const std::string& key) const;  // a Null node when missing

    // Scalar conversions (throw on mismatch).
    bool asBool() const;
    int64_t asInt() const;   // decimal or 0x hex
    uint64_t asUInt() const;
    double asFloat() const;
    const std::string& asString() const { return value; }
    // Untagged plain scalars that look like numbers.
    bool looksInt() const;
    bool looksFloat() const;
    bool looksBool() const;
};

Node parse(const std::string& text);  // throws std::runtime_error with a line number

}  // namespace mm::yaml
