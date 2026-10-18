#include "byml.hpp"

#include <cstring>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "sizetable.hpp"

namespace mm::byml {

namespace {

using T = Node::Type;

struct Reader {
    explicit Reader(const std::string& data) : d(data) {}
    const std::string& d;
    bool le = true;
    std::vector<std::string> keys, strings;
    // Limits against containers that point at the same children over and over (the tree
    // would grow exponentially with the nesting). The writer shares identical values, so
    // some sharing is normal; without it every node takes at least 4 bytes of the file.
    mutable size_t nodesLeft = d.size() * 4 + 65536;
    mutable size_t bytesLeft = d.size() * 64 + (1u << 20);  // strings and binary data copied out

    void charge(size_t nodes, size_t bytes) const {
        if (nodes > nodesLeft || bytes > bytesLeft) throw std::runtime_error("BYML: too many shared values");
        nodesLeft -= nodes;
        bytesLeft -= bytes;
    }

    void need(size_t at, size_t n) const {
        if (at + n > d.size()) throw std::runtime_error("BYML: out of range");
    }
    uint32_t u8(size_t at) const { need(at, 1); return uint8_t(d[at]); }
    uint32_t u24(size_t at) const {
        need(at, 3);
        uint32_t a = uint8_t(d[at]), b = uint8_t(d[at + 1]), c = uint8_t(d[at + 2]);
        return le ? (a | (b << 8) | (c << 16)) : ((a << 16) | (b << 8) | c);
    }
    uint32_t u32(size_t at) const {
        need(at, 4);
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= uint32_t(uint8_t(d[at + k])) << (le ? 8 * k : 8 * (3 - k));
        return v;
    }
    uint64_t u64(size_t at) const {
        uint64_t lo = u32(at), hi = u32(at + 4);
        return le ? (lo | (hi << 32)) : ((lo << 32) | hi);
    }

    std::vector<std::string> table(size_t at) const {
        std::vector<std::string> out;
        if (!at) return out;
        if (u8(at) != 0xC2) throw std::runtime_error("BYML: bad string table");
        uint32_t n = u24(at + 1);
        for (uint32_t k = 0; k < n; ++k) {
            size_t start = at + u32(at + 4 + 4 * k);
            size_t end = d.find('\0', start);
            if (end == std::string::npos) throw std::runtime_error("BYML: unterminated string");
            charge(0, end - start);
            out.push_back(d.substr(start, end - start));
        }
        return out;
    }

    Node value(uint8_t type, size_t slot, int depth) const {
        if (depth > 256) throw std::runtime_error("BYML: too deep");
        charge(1, 0);
        Node n;
        n.type = T(type);
        uint32_t raw = u32(slot);
        switch (n.type) {
            case T::String:
                if (raw >= strings.size()) throw std::runtime_error("BYML: bad string index");
                charge(0, strings[raw].size());
                n.s = strings[raw];
                break;
            case T::Binary: {
                uint32_t size = u32(raw);
                need(raw + 4, size);
                charge(0, size);
                n.s = d.substr(raw + 4, size);
                break;
            }
            case T::Array:
            case T::Dict: return container(raw, depth + 1);
            case T::Bool: n.b = raw != 0; break;
            case T::Int: n.i = int32_t(raw); break;
            case T::Float: std::memcpy(&n.f, &raw, 4); break;
            case T::UInt: n.u = raw; break;
            case T::Int64: n.i = int64_t(u64(raw)); break;
            case T::UInt64: n.u = u64(raw); break;
            case T::Double: {
                uint64_t bits = u64(raw);
                std::memcpy(&n.d, &bits, 8);
                break;
            }
            case T::Null: break;
            default: throw std::runtime_error("BYML: unknown node type " + std::to_string(type));
        }
        return n;
    }

    Node container(size_t at, int depth) const {
        Node n;
        uint32_t type = u8(at), count = u24(at + 1);
        if (type == 0xC0) {
            n.type = T::Array;
            size_t values = at + 4 + ((count + 3) & ~3u);
            need(values, 4 * size_t(count));  // before reserving room for them
            n.array.reserve(count);
            for (uint32_t k = 0; k < count; ++k) n.array.push_back(value(uint8_t(u8(at + 4 + k)), values + 4 * k, depth));
        } else if (type == 0xC1) {
            n.type = T::Dict;
            need(at + 4, 8 * size_t(count));
            for (uint32_t k = 0; k < count; ++k) {
                size_t e = at + 4 + 8 * k;
                uint32_t keyIndex = u24(e);
                if (keyIndex >= keys.size()) throw std::runtime_error("BYML: bad key index");
                n.dict.emplace(keys[keyIndex], value(uint8_t(u8(e + 3)), e + 4, depth));
            }
        } else {
            throw std::runtime_error("BYML: expected a container");
        }
        return n;
    }
};

bool inlineType(T t) { return t == T::String || t == T::Bool || t == T::Int || t == T::Float || t == T::UInt || t == T::Null; }

struct Writer {
    std::string out;
    std::map<std::string, uint32_t> keyIndex, stringIndex;
    std::unordered_map<std::string, uint32_t> written;  // canonical form -> offset (dedupe)

    void put8(uint8_t v) { out += char(v); }
    void put24(uint32_t v) {
        for (int k = 0; k < 3; ++k) out += char((v >> (8 * k)) & 0xff);
    }
    void put32(uint32_t v) {
        for (int k = 0; k < 4; ++k) out += char((v >> (8 * k)) & 0xff);
    }
    void put64(uint64_t v) {
        put32(uint32_t(v));
        put32(uint32_t(v >> 32));
    }
    void set32(size_t at, uint32_t v) {
        for (int k = 0; k < 4; ++k) out[at + k] = char((v >> (8 * k)) & 0xff);
    }
    void align4() {
        while (out.size() % 4) out += '\0';
    }

    void collect(const Node& n, std::set<std::string>& keys, std::set<std::string>& strings) {
        if (n.type == T::String) strings.insert(n.s);
        for (const auto& c : n.array) collect(c, keys, strings);
        for (const auto& [k, v] : n.dict) {
            keys.insert(k);
            collect(v, keys, strings);
        }
    }

    void writeTable(const std::set<std::string>& set, std::map<std::string, uint32_t>& index) {
        size_t start = out.size();
        put8(0xC2);
        put24(uint32_t(set.size()));
        size_t offs = out.size();
        out.append(4 * (set.size() + 1), '\0');
        uint32_t i = 0;
        for (const auto& s : set) {
            set32(offs + 4 * i, uint32_t(out.size() - start));
            index[s] = i++;
            out += s;
            out += '\0';
        }
        set32(offs + 4 * i, uint32_t(out.size() - start));
        align4();
    }

    // A form of the value that's equal exactly when the values are (for dedupe).
    static void canonical(const Node& n, std::string& c) {
        c += char(n.type);
        switch (n.type) {
            case T::String:
            case T::Binary:
                c += std::to_string(n.s.size()) + ":" + n.s;
                break;
            case T::Array:
                c += std::to_string(n.array.size()) + "[";
                for (const auto& e : n.array) canonical(e, c);
                c += "]";
                break;
            case T::Dict:
                c += std::to_string(n.dict.size()) + "{";
                for (const auto& [k, v] : n.dict) {
                    c += std::to_string(k.size()) + ":" + k;
                    canonical(v, c);
                }
                c += "}";
                break;
            case T::Bool: c += n.b ? '1' : '0'; break;
            case T::Int:
            case T::Int64: c += std::to_string(n.i); break;
            case T::UInt:
            case T::UInt64: c += std::to_string(n.u); break;
            case T::Float: {
                uint32_t bits;
                std::memcpy(&bits, &n.f, 4);
                c += std::to_string(bits);
                break;
            }
            case T::Double: {
                uint64_t bits;
                std::memcpy(&bits, &n.d, 8);
                c += std::to_string(bits);
                break;
            }
            case T::Null: break;
        }
        c += ';';
    }

    uint32_t inlineValue(const Node& n) {
        switch (n.type) {
            case T::String: return stringIndex.at(n.s);
            case T::Bool: return n.b ? 1 : 0;
            case T::Int: return uint32_t(int32_t(n.i));
            case T::Float: {
                uint32_t bits;
                std::memcpy(&bits, &n.f, 4);
                return bits;
            }
            case T::UInt: return uint32_t(n.u);
            default: return 0;
        }
    }

    // Write a non-inline value at the current position (or reuse an identical one).
    uint32_t place(const Node& n) {
        std::string c;
        canonical(n, c);
        if (auto it = written.find(c); it != written.end()) return it->second;
        align4();
        uint32_t at = uint32_t(out.size());
        written.emplace(std::move(c), at);
        switch (n.type) {
            case T::Array:
            case T::Dict: container(n); break;
            case T::Binary:
                put32(uint32_t(n.s.size()));
                out += n.s;
                break;
            case T::Int64: put64(uint64_t(n.i)); break;
            case T::UInt64: put64(n.u); break;
            case T::Double: {
                uint64_t bits;
                std::memcpy(&bits, &n.d, 8);
                put64(bits);
                break;
            }
            default: break;
        }
        return at;
    }

    void container(const Node& n) {
        std::vector<std::pair<size_t, const Node*>> later;
        if (n.type == T::Array) {
            put8(0xC0);
            put24(uint32_t(n.array.size()));
            for (const auto& e : n.array) put8(uint8_t(e.type));
            align4();
            for (const auto& e : n.array) {
                if (!inlineType(e.type)) later.emplace_back(out.size(), &e);
                put32(inlineValue(e));
            }
        } else {
            put8(0xC1);
            put24(uint32_t(n.dict.size()));
            for (const auto& [k, v] : n.dict) {  // std::map: sorted by key, so by key index
                put24(keyIndex.at(k));
                put8(uint8_t(v.type));
                if (!inlineType(v.type)) later.emplace_back(out.size(), &v);
                put32(inlineValue(v));
            }
        }
        for (const auto& [slot, child] : later) set32(slot, place(*child));
    }
};

}  // namespace

const Node* Node::find(const std::string& key) const {
    auto it = dict.find(key);
    return it == dict.end() ? nullptr : &it->second;
}

bool Node::operator==(const Node& o) const {
    if (type != o.type) return false;
    switch (type) {
        case T::String:
        case T::Binary: return s == o.s;
        case T::Array: return array == o.array;
        case T::Dict: return dict == o.dict;
        case T::Bool: return b == o.b;
        case T::Int:
        case T::Int64: return i == o.i;
        case T::UInt:
        case T::UInt64: return u == o.u;
        case T::Float: return std::memcmp(&f, &o.f, 4) == 0;
        case T::Double: return std::memcmp(&d, &o.d, 8) == 0;
        case T::Null: return true;
    }
    return false;
}

Document parse(const std::string& raw) {
    std::string data = isYaz0(raw) ? yaz0Decompress(raw) : raw;
    if (data.size() < 0x10) throw std::runtime_error("BYML: too short");
    Reader r(data);
    if (data.compare(0, 2, "YB") == 0)
        r.le = true;
    else if (data.compare(0, 2, "BY") == 0)
        r.le = false;
    else
        throw std::runtime_error("not a BYML file");
    Document doc;
    doc.version = uint16_t(r.le ? (uint8_t(data[2]) | (uint8_t(data[3]) << 8)) : ((uint8_t(data[2]) << 8) | uint8_t(data[3])));
    r.keys = r.table(r.u32(4));
    r.strings = r.table(r.u32(8));
    uint32_t root = r.u32(12);
    if (root) doc.root = r.container(root, 0);
    return doc;
}

std::string write(const Document& doc) {
    Writer w;
    w.out = "YB";
    w.out += char(doc.version & 0xff);
    w.out += char(doc.version >> 8);
    w.out.append(12, '\0');
    if (!doc.root.isContainer()) return w.out;
    std::set<std::string> keys, strings;
    w.collect(doc.root, keys, strings);
    if (!keys.empty()) {
        w.set32(4, uint32_t(w.out.size()));
        w.writeTable(keys, w.keyIndex);
    }
    if (!strings.empty()) {
        w.set32(8, uint32_t(w.out.size()));
        w.writeTable(strings, w.stringIndex);
    }
    w.align4();
    w.set32(12, w.place(doc.root));
    w.align4();
    return w.out;
}

}  // namespace mm::byml
