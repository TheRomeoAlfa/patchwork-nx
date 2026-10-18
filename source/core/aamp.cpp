#include "aamp.hpp"

#include <zlib.h>

#include <cstring>
#include <map>
#include <stdexcept>

#include "sizetable.hpp"

namespace mm::aamp {

namespace {

constexpr uint32_t kRoot = 0xA4F6CB6C;  // crc32("param_root")

uint32_t rd32(const std::string& d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("AAMP: out of range");
    return uint32_t(uint8_t(d[at])) | (uint32_t(uint8_t(d[at + 1])) << 8) | (uint32_t(uint8_t(d[at + 2])) << 16) |
           (uint32_t(uint8_t(d[at + 3])) << 24);
}
uint16_t rd16(const std::string& d, size_t at) {
    if (at + 2 > d.size()) throw std::runtime_error("AAMP: out of range");
    return uint16_t(uint8_t(d[at]) | (uint8_t(d[at + 1]) << 8));
}
void put32(std::string& o, uint32_t v) {
    for (int k = 0; k < 4; ++k) o += char((v >> (8 * k)) & 0xff);
}
void set32(std::string& o, size_t at, uint32_t v) {
    for (int k = 0; k < 4; ++k) o[at + k] = char((v >> (8 * k)) & 0xff);
}
void set16(std::string& o, size_t at, uint16_t v) {
    o[at] = char(v & 0xff);
    o[at + 1] = char(v >> 8);
}
void align4(std::string& o) {
    while (o.size() % 4) o += '\0';
}

// Size of a fixed-size value, 0 for buffers and strings.
size_t fixedSize(Type t) {
    switch (t) {
        case Type::Bool:
        case Type::F32:
        case Type::Int:
        case Type::U32: return 4;
        case Type::Vec2: return 8;
        case Type::Vec3: return 12;
        case Type::Vec4:
        case Type::Color:
        case Type::Quat: return 16;
        case Type::Curve1: return 0x80;
        case Type::Curve2: return 0x100;
        case Type::Curve3: return 0x180;
        case Type::Curve4: return 0x200;
        default: return 0;
    }
}
bool isBuffer(Type t) { return t == Type::BufferInt || t == Type::BufferF32 || t == Type::BufferU32 || t == Type::BufferBinary; }
size_t stringCapacity(Type t) {
    switch (t) {
        case Type::String32: return 32;
        case Type::String64: return 64;
        case Type::String256: return 256;
        default: return std::string::npos;
    }
}

struct Reader {
    const std::string& d;
    int depth = 0;
    // Limits against structures that point at the same children over and over (the tree
    // would grow exponentially with the nesting). Without sharing every list, object and
    // parameter takes at least 8 bytes of the file; values may be shared (the writer does).
    size_t nodesLeft = d.size() / 2 + 64;
    size_t bytesLeft = d.size() * 64 + (1u << 20);

    void node() {
        if (nodesLeft-- == 0) throw std::runtime_error("AAMP: too many shared structures");
    }
    std::string copy(size_t at, size_t n) {
        if (n > bytesLeft) throw std::runtime_error("AAMP: too many shared values");
        bytesLeft -= n;
        return d.substr(at, n);
    }

    ParameterList list(size_t at) {
        if (++depth > 64) throw std::runtime_error("AAMP: too deep");
        node();
        ParameterList l;
        size_t lists = at + size_t(rd16(d, at + 4)) * 4, objects = at + size_t(rd16(d, at + 8)) * 4;
        uint16_t numLists = rd16(d, at + 6), numObjects = rd16(d, at + 10);
        for (uint16_t i = 0; i < numLists; ++i) {
            size_t c = lists + 12 * size_t(i);
            l.lists.emplace_back(rd32(d, c), list(c));
        }
        for (uint16_t i = 0; i < numObjects; ++i) {
            size_t c = objects + 8 * size_t(i);
            l.objects.emplace_back(rd32(d, c), object(c));
        }
        --depth;
        return l;
    }

    ParameterObject object(size_t at) {
        node();
        ParameterObject o;
        size_t params = at + size_t(rd16(d, at + 4)) * 4;
        uint16_t n = rd16(d, at + 6);
        for (uint16_t i = 0; i < n; ++i) {
            size_t p = params + 8 * size_t(i);
            o.params.emplace_back(rd32(d, p), parameter(p));
        }
        return o;
    }

    Parameter parameter(size_t at) {
        node();
        Parameter p;
        uint32_t word = rd32(d, at + 4);
        p.type = Type(word >> 24);
        if (uint8_t(p.type) > uint8_t(Type::StringRef)) throw std::runtime_error("AAMP: unknown parameter type");
        size_t data = at + size_t(word & 0xFFFFFF) * 4;
        if (p.isString()) {
            // Look no further than the type holds (shared strings would be scanned again and again).
            if (data >= d.size()) throw std::runtime_error("AAMP: unterminated string");
            size_t window = std::min(d.size() - data, stringCapacity(p.type));
            const void* end = std::memchr(d.data() + data, '\0', window);
            if (!end && window == d.size() - data) throw std::runtime_error("AAMP: unterminated string");
            p.data = copy(data, end ? size_t(static_cast<const char*>(end) - (d.data() + data)) : window);
        } else if (isBuffer(p.type)) {
            uint32_t count = rd32(d, data - 4);
            size_t bytes = p.type == Type::BufferBinary ? count : size_t(count) * 4;
            if (data + bytes > d.size()) throw std::runtime_error("AAMP: buffer out of range");
            p.data = copy(data, bytes);
        } else {
            size_t size = fixedSize(p.type);
            if (data + size > d.size()) throw std::runtime_error("AAMP: value out of range");
            p.data = copy(data, size);
        }
        return p;
    }
};

struct Writer {
    std::string out;
    std::vector<std::pair<const ParameterList*, size_t>> listPos;
    std::vector<std::pair<const ParameterObject*, size_t>> objectPos;
    std::vector<std::pair<const Parameter*, size_t>> paramPos;
    size_t numLists = 0, numObjects = 0, numParams = 0;

    void listStruct(uint32_t key) {
        put32(out, key);
        out.append(8, '\0');
        ++numLists;
    }

    // Offsets (in words, relative to the struct) and counts are 16-bit in the format.
    void set16Checked(size_t at, size_t v) {
        if (v > 0xFFFF) throw std::runtime_error("AAMP: file too large");
        set16(out, at, uint16_t(v));
    }

    size_t posOf(const ParameterList* l) const {
        for (const auto& [p, at] : listPos)
            if (p == l) return at;
        throw std::logic_error("AAMP writer: list not placed");
    }

    void childLists(const ParameterList& l, size_t at) {
        set16Checked(at + 4, (out.size() - at) / 4);
        set16Checked(at + 6, l.lists.size());
        std::vector<size_t> children;
        for (const auto& [k, c] : l.lists) {
            children.push_back(out.size());
            listPos.emplace_back(&c, out.size());
            listStruct(k);
        }
        for (size_t i = 0; i < l.lists.size(); ++i) childLists(l.lists[i].second, children[i]);
    }

    void objects(const ParameterList& l) {
        size_t at = posOf(&l);
        set16Checked(at + 8, (out.size() - at) / 4);
        set16Checked(at + 10, l.objects.size());
        for (const auto& [k, o] : l.objects) {
            objectPos.emplace_back(&o, out.size());
            put32(out, k);
            out.append(4, '\0');
            ++numObjects;
        }
        for (const auto& [k, c] : l.lists) objects(c);
    }

    size_t objectAt(const ParameterObject* o) const {
        for (const auto& [p, at] : objectPos)
            if (p == o) return at;
        throw std::logic_error("AAMP writer: object not placed");
    }

    void params(const ParameterList& l) {
        for (const auto& [k, c] : l.lists) params(c);
        for (const auto& [k, o] : l.objects) {
            size_t at = objectAt(&o);
            set16Checked(at + 4, (out.size() - at) / 4);
            set16Checked(at + 6, o.params.size());
            for (const auto& [pk, p] : o.params) {
                paramPos.emplace_back(&p, out.size());
                put32(out, pk);
                out.append(4, '\0');
                ++numParams;
            }
        }
    }

    void setDataOffset(size_t paramAt, size_t dataAt, Type type) {
        size_t rel = (dataAt - paramAt) / 4;
        if (rel >= (1u << 24)) throw std::runtime_error("AAMP: file too large");
        set32(out, paramAt + 4, uint32_t(rel) | (uint32_t(type) << 24));
    }
};

}  // namespace

uint32_t hash(const std::string& name) { return uint32_t(crc32(0, reinterpret_cast<const Bytef*>(name.data()), uInt(name.size()))); }

Parameter Parameter::boolean(bool v) {
    Parameter p{Type::Bool, {}};
    put32(p.data, v ? 1 : 0);
    return p;
}
Parameter Parameter::f32(float v) {
    Parameter p{Type::F32, std::string(4, '\0')};
    std::memcpy(p.data.data(), &v, 4);
    return p;
}
Parameter Parameter::s32(int32_t v) {
    Parameter p{Type::Int, {}};
    put32(p.data, uint32_t(v));
    return p;
}
Parameter Parameter::u32(uint32_t v) {
    Parameter p{Type::U32, {}};
    put32(p.data, v);
    return p;
}
Parameter Parameter::string(Type type, std::string v) { return Parameter{type, std::move(v)}; }

bool Parameter::asBool() const { return data.size() >= 4 && rd32(data, 0) != 0; }
float Parameter::asF32() const {
    float f = 0;
    if (data.size() >= 4) std::memcpy(&f, data.data(), 4);
    return f;
}
int32_t Parameter::asInt() const { return data.size() >= 4 ? int32_t(rd32(data, 0)) : 0; }

const Parameter* ParameterObject::find(uint32_t key) const {
    for (const auto& [k, p] : params)
        if (k == key) return &p;
    return nullptr;
}
void ParameterObject::set(uint32_t key, Parameter p) {
    for (auto& [k, v] : params)
        if (k == key) {
            v = std::move(p);
            return;
        }
    params.emplace_back(key, std::move(p));
}

const ParameterList* ParameterList::list(uint32_t key) const {
    for (const auto& [k, l] : lists)
        if (k == key) return &l;
    return nullptr;
}
ParameterList* ParameterList::list(uint32_t key) {
    for (auto& [k, l] : lists)
        if (k == key) return &l;
    return nullptr;
}
const ParameterObject* ParameterList::object(uint32_t key) const {
    for (const auto& [k, o] : objects)
        if (k == key) return &o;
    return nullptr;
}
ParameterObject* ParameterList::object(uint32_t key) {
    for (auto& [k, o] : objects)
        if (k == key) return &o;
    return nullptr;
}
ParameterList& ParameterList::listOrAdd(uint32_t key) {
    if (auto* l = list(key)) return *l;
    lists.emplace_back(key, ParameterList{});
    return lists.back().second;
}
ParameterObject& ParameterList::objectOrAdd(uint32_t key) {
    if (auto* o = object(key)) return *o;
    objects.emplace_back(key, ParameterObject{});
    return objects.back().second;
}

ParameterIO parse(const std::string& raw) {
    std::string d = isYaz0(raw) ? yaz0Decompress(raw) : raw;
    if (d.size() < 0x34 || d.compare(0, 4, "AAMP") != 0) throw std::runtime_error("not an AAMP file");
    if (rd32(d, 4) != 2) throw std::runtime_error("AAMP: unsupported version");
    if ((rd32(d, 8) & 1) == 0) throw std::runtime_error("AAMP: big-endian files aren't supported");
    ParameterIO pio;
    pio.version = rd32(d, 0x10);
    size_t typeEnd = d.find('\0', 0x30);
    if (typeEnd == std::string::npos) throw std::runtime_error("AAMP: bad type string");
    pio.type = d.substr(0x30, typeEnd - 0x30);
    size_t root = 0x30 + rd32(d, 0x14);
    if (rd32(d, root) != kRoot) throw std::runtime_error("AAMP: root isn't param_root");
    Reader r{d};
    pio.root = r.list(root);
    return pio;
}

std::string write(const ParameterIO& pio) {
    Writer w;
    w.out = "AAMP";
    put32(w.out, 2);
    put32(w.out, 3);
    w.out.append(0x30 - w.out.size(), '\0');
    w.out += pio.type;
    w.out += '\0';
    align4(w.out);
    set32(w.out, 0x10, pio.version);
    set32(w.out, 0x14, uint32_t(w.out.size() - 0x30));

    w.listPos.emplace_back(&pio.root, w.out.size());
    w.listStruct(kRoot);
    w.childLists(pio.root, w.listPos[0].second);
    w.objects(pio.root);
    w.params(pio.root);

    // Data section: values in parameter order, identical values written once.
    size_t dataStart = w.out.size();
    std::map<std::string, size_t> seen;
    std::vector<std::pair<const Parameter*, size_t>> strings;
    for (const auto& [p, at] : w.paramPos) {
        if (p->isString()) {
            strings.emplace_back(p, at);
            continue;
        }
        std::string bytes;
        if (isBuffer(p->type)) put32(bytes, uint32_t(p->type == Type::BufferBinary ? p->data.size() : p->data.size() / 4));
        bytes += p->data;
        auto it = seen.find(bytes);
        size_t pos;
        if (it != seen.end()) {
            pos = it->second;
        } else {
            pos = w.out.size();
            w.out += bytes;
            align4(w.out);
            seen.emplace(bytes, pos);
        }
        w.setDataOffset(at, isBuffer(p->type) ? pos + 4 : pos, p->type);
    }
    size_t stringStart = w.out.size();
    std::map<std::string, size_t> seenStrings;
    for (const auto& [p, at] : strings) {
        auto it = seenStrings.find(p->data);
        size_t pos;
        if (it != seenStrings.end()) {
            pos = it->second;
        } else {
            pos = w.out.size();
            w.out += p->data;
            w.out += '\0';
            align4(w.out);
            seenStrings.emplace(p->data, pos);
        }
        w.setDataOffset(at, pos, p->type);
    }
    set32(w.out, 0x0C, uint32_t(w.out.size()));
    set32(w.out, 0x18, uint32_t(w.numLists));
    set32(w.out, 0x1C, uint32_t(w.numObjects));
    set32(w.out, 0x20, uint32_t(w.numParams));
    set32(w.out, 0x24, uint32_t(stringStart - dataStart));
    set32(w.out, 0x28, uint32_t(w.out.size() - stringStart));
    set32(w.out, 0x2C, 0);
    return w.out;
}

}  // namespace mm::aamp
