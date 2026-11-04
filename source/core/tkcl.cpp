#include "tkcl.hpp"

#include <cstring>
#include <stdexcept>

namespace mm::tkcl {

namespace {

constexpr uint32_t kPackMagic = 0x504D4B54;       // "TKMP"
constexpr uint32_t kPackVersion = 0x20;
constexpr uint32_t kChangelogMagic = 0x4C434B54;  // "TKCL"

struct Reader {
    std::string_view data;
    size_t at = 0;

    [[noreturn]] void bad() const { throw std::runtime_error("this TKMM package is damaged or incomplete"); }
    std::string_view take(size_t n) {
        if (n > data.size() - at) bad();
        auto out = data.substr(at, n);
        at += n;
        return out;
    }
    template <typename T>
    T num() {
        T v;
        std::memcpy(&v, take(sizeof v).data(), sizeof v);  // little endian, as on the Switch and PCs
        return v;
    }
    // A count that has to fit in what's left (each item takes at least `minSize` bytes).
    size_t count(size_t minSize = 1) {
        auto n = num<int32_t>();
        if (n < 0 || size_t(n) > (data.size() - at) / minSize) bad();
        return size_t(n);
    }
    std::string str() {
        auto n = num<int32_t>();
        if (n == -1) return {};
        if (n < 0) bad();
        return std::string(take(size_t(n)));
    }
    std::vector<std::string> list() {
        std::vector<std::string> out(count(4));
        for (auto& s : out) s = str();
        return out;
    }
    std::string ulid() { return ulidText(take(16)); }
    void thumbnail() {
        if (num<uint8_t>()) str();
    }
};

Changelog readChangelog(Reader& r) {
    if (r.num<uint32_t>() != kChangelogMagic) r.bad();
    Changelog c;
    c.builderVersion = r.num<int32_t>();
    c.gameVersion = r.num<int32_t>();
    c.files.resize(r.count(17));
    for (auto& e : c.files) {
        e.canonical = r.str();
        e.type = EntryType(r.num<int32_t>());
        e.attributes = r.num<uint32_t>();
        e.zsDictionaryId = r.num<int32_t>();
        e.versions.resize(r.num<uint8_t>());
        for (auto& v : e.versions) v = r.num<int32_t>();
        e.archiveCanonicals = r.list();
    }
    c.malsFiles = r.list();
    c.patches.resize(r.count(8));
    for (auto& p : c.patches) {
        p.nsoBinaryId = r.str();
        for (auto n = r.count(8); n--;) {
            auto offset = r.num<uint32_t>();
            p.entries[offset] = r.num<uint32_t>();
        }
    }
    c.cheats.resize(r.count(8));
    for (auto& cheat : c.cheats) {
        cheat.name = r.str();
        for (auto n = r.count(8); n--;) {
            auto key = r.str();
            auto& lines = cheat.codes[key];
            lines.resize(r.count(4));
            for (auto& line : lines) {
                line.resize(r.count(4));
                for (auto& v : line) v = r.num<uint32_t>();
            }
        }
    }
    c.subSdkFiles = r.list();
    c.exeFiles = r.list();
    r.list();  // reserved
    r.list();
    return c;
}

Dependency readDependency(Reader& r) {
    Dependency d;
    d.name = r.str();
    d.id = r.ulid();
    return d;
}

}  // namespace

std::string ulidText(std::string_view bytes) {
    static const char* digits = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford's base 32
    // 128 bits read as one big-endian number, 5 bits per letter (the first letter has 3).
    std::string out(26, '0');
    for (int i = 25, bit = 0; i >= 0; --i, bit += 5) {
        unsigned v = 0;
        for (int b = 0; b < 5 && bit + b < 128; ++b) {
            int pos = bit + b;  // counted from the lowest bit
            auto byte = static_cast<unsigned char>(bytes[15 - pos / 8]);
            v |= ((byte >> (pos % 8)) & 1u) << b;
        }
        out[size_t(i)] = digits[v];
    }
    return out;
}

Package read(std::string_view data) {
    Reader r{data};
    if (data.size() < 8 || r.num<uint32_t>() != kPackMagic) throw std::runtime_error("this isn't a TKMM package");
    if (r.num<uint32_t>() != kPackVersion)
        throw std::runtime_error("this TKMM package was made with an older version of TKMM; ask the author for a new one");
    Package p;
    p.id = r.ulid();
    p.name = r.str();
    p.description = r.str();
    r.thumbnail();
    p.changelog = readChangelog(r);
    p.version = r.str();
    p.author = r.str();
    p.contributors.resize(r.count(8));
    for (auto& [name, role] : p.contributors) {
        name = r.str();
        role = r.str();
    }
    p.groups.resize(r.count(24));
    for (auto& g : p.groups) {
        g.name = r.str();
        g.description = r.str();
        r.thumbnail();
        g.type = GroupType(r.num<int32_t>());
        g.iconName = r.str();
        g.priority = r.num<int32_t>();
        g.options.resize(r.count(32));
        for (auto& o : g.options) {
            o.id = r.ulid();
            o.name = r.str();
            o.description = r.str();
            r.thumbnail();
            o.changelog = readChangelog(r);
            o.priority = r.num<int32_t>();
        }
        for (auto n = r.count(4); n--;) {
            auto index = r.num<int32_t>();
            if (index < 0 || size_t(index) >= g.options.size()) r.bad();
            g.defaults.push_back(size_t(index));
        }
        g.dependencies.resize(r.count(20));
        for (auto& d : g.dependencies) d = readDependency(r);
    }
    p.dependencies.resize(r.count(20));
    for (auto& d : p.dependencies) d = readDependency(r);
    p.zipOffset = r.at;
    return p;
}

}  // namespace mm::tkcl
