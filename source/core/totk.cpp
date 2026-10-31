#include "totk.hpp"

#include <cstring>
#include <stdexcept>

#include "byml.hpp"
#include "sarc.hpp"
#include "tkcl.hpp"
#include "util.hpp"

namespace mm::totk {

namespace {

uint32_t rotl(uint32_t v, int r) { return (v << r) | (v >> (32 - r)); }

uint32_t le32(std::string_view d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("TKMM's pack index is damaged");
    uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

uint16_t le16(std::string_view d, size_t at) {
    if (at + 2 > d.size()) throw std::runtime_error("TKMM's pack index is damaged");
    uint16_t v;
    std::memcpy(&v, d.data() + at, 2);
    return v;
}

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

uint64_t packKey(std::string_view canonical) {
    // Its first and last letters (TKMM takes the low byte of each UTF-16 unit) and the hash.
    uint16_t section = uint16_t((uint8_t(canonical.front()) << 8) | uint8_t(canonical.back()));
    return (uint64_t(xxHash32(utf16le(canonical))) << 32) | section;
}

// A BYML table of names -> real names ("AI/X.ainb" -> "AI/X.121.ainb").
std::unordered_map<std::string, std::string> nameTable(const byml::Node& dict) {
    std::unordered_map<std::string, std::string> out;
    for (const auto& [k, v] : dict.dict)
        if (v.type == byml::Node::Type::String) out.emplace(k, v.s);
    return out;
}

}  // namespace

int parseVersion(std::string_view text, std::string* nsoBinaryId) {
    // "…\n<a range ending in the version, like 100-121>\n<build id>"
    auto last = text.rfind('\n');
    if (last == std::string_view::npos || last < 4) throw std::runtime_error("couldn't read the game version (RegionLangMask.txt)");
    if (nsoBinaryId) *nsoBinaryId = std::string(text.substr(last + 1));
    auto digits = text.substr(last - 4, 3);
    int v = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') throw std::runtime_error("couldn't read the game version (RegionLangMask.txt)");
        v = v * 10 + (c - '0');
    }
    return v;
}

uint32_t xxHash32(std::string_view data, uint32_t seed) {
    constexpr uint32_t p1 = 2654435761u, p2 = 2246822519u, p3 = 3266489917u, p4 = 668265263u, p5 = 374761393u;
    auto read = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, data.data() + at, 4);
        return v;
    };
    size_t at = 0, n = data.size();
    uint32_t h;
    if (n >= 16) {
        uint32_t v1 = seed + p1 + p2, v2 = seed + p2, v3 = seed, v4 = seed - p1;
        for (; at + 16 <= n; at += 16) {
            v1 = rotl(v1 + read(at) * p2, 13) * p1;
            v2 = rotl(v2 + read(at + 4) * p2, 13) * p1;
            v3 = rotl(v3 + read(at + 8) * p2, 13) * p1;
            v4 = rotl(v4 + read(at + 12) * p2, 13) * p1;
        }
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    } else {
        h = seed + p5;
    }
    h += uint32_t(n);
    for (; at + 4 <= n; at += 4) h = rotl(h + read(at) * p3, 17) * p4;
    for (; at < n; ++at) h = rotl(h + uint8_t(data[at]) * p5, 11) * p1;
    h ^= h >> 15;
    h *= p2;
    h ^= h >> 13;
    h *= p3;
    h ^= h >> 16;
    return h;
}

std::string utf16le(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 2);
    auto unit = [&](uint32_t u) {
        out += char(u & 0xff);
        out += char(u >> 8);
    };
    for (size_t i = 0; i < s.size();) {
        auto c = uint8_t(s[i]);
        uint32_t cp;
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (len == 1 || i + len > s.size()) {
            cp = c < 0x80 ? c : 0xFFFD;
            len = 1;
        } else {
            cp = c & (0x7F >> len);
            for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (uint8_t(s[i + k]) & 0x3F);
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 | (cp >> 10));
            unit(0xDC00 | (cp & 0x3FF));
        } else {
            unit(cp);
        }
        i += len;
    }
    return out;
}

PackIndex PackIndex::load(std::string_view pkcacheZs) {
    zs::Dictionaries none;
    auto d = none.decompress(pkcacheZs);
    if (le32(d, 0) != 0x48434B50) throw std::runtime_error("TKMM's pack index is damaged");  // "PKCH"
    uint32_t count = le32(d, 4), stringsAt = le32(d, 8), stringCount = le32(d, 12);
    PackIndex out;
    for (size_t at = stringsAt, k = 0; k < stringCount; ++k) {
        auto end = d.find('\0', at);
        if (end == std::string::npos || end + 1 >= d.size()) throw std::runtime_error("TKMM's pack index is damaged");
        out.packs_.emplace_back(d.substr(at, end - at), uint8_t(d[end + 1]));
        at = end + 2;
    }
    if (0x10 + size_t(count) * 8 > d.size()) throw std::runtime_error("TKMM's pack index is damaged");
    out.lookup_.reserve(count);
    for (size_t k = 0; k < count; ++k) {
        size_t e = 0x10 + 8 * k;
        uint16_t section = le16(d, e), index = le16(d, e + 6);
        if (index >= out.packs_.size()) throw std::runtime_error("TKMM's pack index is damaged");
        out.lookup_.emplace((uint64_t(le32(d, e + 2)) << 32) | section, index);
    }
    return out;
}

std::optional<std::pair<std::string, uint32_t>> PackIndex::packFor(std::string_view canonical) const {
    if (canonical.empty()) return std::nullopt;
    auto it = lookup_.find(packKey(canonical));
    if (it == lookup_.end()) return std::nullopt;
    return packs_[it->second];
}

bool FolderGameFiles::read(const std::string& romfsPath, std::string& out) {
    fs::path p = fs::path(root_) / fs::path(romfsPath);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    out = readFile(p);
    return true;
}

const std::vector<std::string>& testFilePatterns() {
    static const std::vector<std::string> patterns = {
        "System/",       "Pack/ZsDic.pack.zs", "Pack/Bootup*",    "Pack/AI.Global*", "Pack/Actor/",
        "AI/FileEntry/", "Logic/FileEntry/",   "Sequence/FileEntry/", "Event/EventFlow/EventFlowFileEntry*",
        "Effect/EffectFileInfo*", "RSDB/",     "GameData/",       "Mals/USen*",
    };
    return patterns;
}

Rom::Rom(GameFiles& files, const PackIndex& packs, size_t packCacheBytes)
    : files_(files), packs_(packs), packCacheBytes_(packCacheBytes) {
    std::string raw;
    if (!files_.read("System/RegionLangMask.txt", raw)) throw std::runtime_error("the game's version file (RegionLangMask.txt) is missing");
    version_ = parseVersion(raw, &nsoBinaryId_);
    if (!files_.read("Pack/ZsDic.pack.zs", raw)) throw std::runtime_error("the game's compression dictionaries (ZsDic.pack.zs) are missing");
    dicts_ = zs::Dictionaries::load(raw);

    auto table = [&](const std::string& path) {
        auto data = vanillaAt(path);
        if (!data) throw std::runtime_error("the game file " + path + " is missing");
        return byml::parse(*data).root;
    };
    addresses_ = nameTable(table("System/AddressTable/Product." + std::to_string(version_) + ".Nin_NX_NVN.atbl.byml.zs"));
    auto address = [&](const std::string& canonical) {
        auto it = addresses_.find(canonical);
        if (it == addresses_.end()) throw std::runtime_error("the game's address table has no " + canonical);
        return it->second + ".zs";
    };
    auto flows = table(address("Event/EventFlow/EventFlowFileEntry.Product.byml"));
    if (auto* v = flows.find("Versions")) eventFlows_ = nameTable(*v);
    auto effects = table(address("Effect/EffectFileInfo.Product.Nin_NX_NVN.byml"));
    if (auto* binaries = effects.find("BinaryDict"))
        for (const auto& [k, v] : binaries->dict)
            if (v.s.size() > 11 && v.s.compare(v.s.size() - 11, 7, "Product") == 0) effects_.emplace(k, v.s);
    if (auto* stat = effects.find("StaticEsetb"); stat && !stat->array.empty()) effects_["static"] = stat->array[0].s;
    ai_ = nameTable(table(address("AI/FileEntry/FileEntry.Product.byml")));
    logic_ = nameTable(table(address("Logic/FileEntry/FileEntry.Product.byml")));
    sequences_ = nameTable(table(address("Sequence/FileEntry/FileEntry.Product.byml")));
}

std::string Rom::relativePath(const std::string& canonical, uint32_t attributes) const {
    auto lookup = [](const std::unordered_map<std::string, std::string>& m, const std::string& k) -> const std::string* {
        auto it = m.find(k);
        return it == m.end() ? nullptr : &it->second;
    };
    std::string result = canonical;
    if (auto* a = lookup(addresses_, canonical)) result = *a;
    const std::string* found = nullptr;
    if (result.size() > 3 && startsWith(result, "AI/") && (found = lookup(ai_, result))) {
        result = *found;
    } else if (result.size() > 26 && startsWith(result, "Event/EventFlow")) {
        // "Event/EventFlow/<name>.bfevfl" -> "Event/EventFlow/<name>.<version>.bfevfl"
        auto name = result.substr(16, result.size() - 16 - 7);
        if ((found = lookup(eventFlows_, name))) result = "Event/EventFlow/" + name + "." + *found + result.substr(result.rfind('.'));
    } else if ((attributes & tkcl::IsProductFile) && result.size() > 37 && startsWith(result, "Effect")) {
        auto name = result.substr(7, result.size() - 7 - 30);
        if ((found = lookup(effects_, name))) result = "Effect/" + *found + ".Product.Nin_NX_NVN.esetb.byml";
    } else if (result.size() > 9 && startsWith(result, "Sequence/") && (found = lookup(sequences_, result))) {
        result = *found;
    } else if (result.size() > 6 && startsWith(result, "Logic/") && (found = lookup(logic_, result))) {
        result = *found;
    }
    if (attributes & tkcl::HasZsExtension) result += ".zs";
    if (attributes & tkcl::HasMcExtension) result += ".mc";
    return result;
}

std::optional<std::string> Rom::vanilla(const std::string& canonical, uint32_t attributes) {
    return vanillaAt(relativePath(canonical, attributes));
}

std::shared_ptr<const std::string> Rom::pack(const std::string& relativePath) {
    for (auto it = packCache_.begin(); it != packCache_.end(); ++it)
        if (it->first == relativePath) {
            packCache_.splice(packCache_.begin(), packCache_, it);
            return it->second;
        }
    std::string raw;
    if (!files_.read(relativePath, raw)) return nullptr;
    auto data = std::make_shared<const std::string>(zs::isZstd(raw) ? dicts_.decompress(raw) : std::move(raw));
    packCache_.emplace_front(relativePath, data);
    packCacheUsed_ += data->size();
    while (packCacheUsed_ > packCacheBytes_ && packCache_.size() > 1) {
        packCacheUsed_ -= packCache_.back().second->size();
        packCache_.pop_back();
    }
    return data;
}

std::optional<std::string> Rom::vanillaAt(const std::string& relativePath) {
    std::string raw;
    if (files_.read(relativePath, raw)) {
        if (zs::isZstd(raw)) return dicts_.decompress(raw);
        return raw;
    }
    // Only inside a pack: there its name is the canonical one.
    auto where = packs_.packFor(relativePath);
    if (!where) return std::nullopt;
    auto data = pack(this->relativePath(where->first, where->second));
    if (!data) return std::nullopt;
    for (const auto& f : sarcView(*data))
        if (f.name == relativePath) return std::string(f.data);
    return std::nullopt;
}

}  // namespace mm::totk
