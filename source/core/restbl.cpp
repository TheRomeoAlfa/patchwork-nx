#include "restbl.hpp"

#include <cstring>
#include <stdexcept>

#include <zlib.h>

namespace mm::restbl {

namespace {

constexpr size_t kNameSize = 0xA0;

uint32_t le32(std::string_view d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("the size table is damaged");
    uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

void put32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }

bool endsWith(std::string_view s, std::string_view end) { return s.size() >= end.size() && s.substr(s.size() - end.size()) == end; }

// Path.GetExtension: from the last dot of the file name.
std::string_view extension(std::string_view path) {
    auto slash = path.find_last_of("/\\");
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    return path.substr(dot);
}

std::string_view resourceExtension(std::string_view path) {
    if (endsWith(path, ".casset.byml.zs")) return ".casset.byml";
    if (endsWith(path, ".ta.zs")) return extension(path);  // ".zs", as TKMM
    if (endsWith(path, ".zs")) return extension(path.substr(0, path.size() - 3));
    return extension(path);
}

// Reads a little-endian u32 of the file, 0 when it's too short (a damaged file then gets the
// base size instead of crashing the merge).
uint32_t at32(std::string_view d, size_t at) {
    if (at + 4 > d.size()) return 0;
    uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

// The signature count of an AI file's EXB section (0 = none).
uint32_t signatureCount(std::string_view d, uint32_t exb) {
    if (!exb) return 0;
    return at32(d, size_t(exb) + at32(d, size_t(exb) + 0x20));
}

uint32_t typeSize(uint32_t size, std::string_view name, std::string_view ext, std::string_view data) {
    if (name == "Event/EventFlow/Dm_ED_0004.bfevfl") return size + 0x1E0;
    if (name == "Effect/static.Nin_NX_NVN.esetb.byml" || name == "Effect/static.Product.110.Nin_NX_NVN.esetb.byml" ||
        name == "Lib/agl/agl_resource.Nin_NX_NVN.release.sarc" || name == "Lib/gsys/gsys_resource.Nin_NX_NVN.release.sarc" ||
        name == "Lib/Terrain/tera_resource.Nin_NX_NVN.release.sarc" || name == "Shader/ApplicationPackage.Nin_NX_NVN.release.sarc")
        return size + 0x1000;
    if (ext == ".ainb") return size + 392 + 16 + (signatureCount(data, at32(data, 0x44)) + 1) / 2 * 8;
    if (ext == ".asb") {
        uint32_t exb = at32(data, 0x60);
        return size + 552 + 40 * at32(data, 0x10) + (exb ? 16 + (signatureCount(data, exb) + 1) / 2 * 8 : 0);
    }
    if (ext == ".bstar") return size + 0x120 + at32(data, 0x8) * 8;
    if (ext == ".mc") {
        int32_t flags = int32_t(at32(data, 0x8));
        int64_t s = int64_t(flags >> 5) << (flags & 0xF);
        return uint32_t(double(int32_t(s)) * 2.55);
    }
    if (ext == ".bgyml") return (size + 2000) * 8;
    struct Extra {
        const char* ext;
        uint32_t add;
    };
    static const Extra extras[] = {
        {".baatarc", 0x100}, {".baev", 0x120}, {".bagst", 0x100}, {".bars", 0x240}, {".bcul", 0x100},  {".beco", 0x100},
        {".belnk", 0x100},   {".bfarc", 0x100}, {".bfevfl", 0x120}, {".bfsha", 0x100}, {".bhtmp", 0x100}, {".blal", 0x100},
        {".blarc", 0x100},   {".blwp", 0x100},  {".bnsh", 0x100},   {".bntx", 0x100},  {".bphcl", 0x100}, {".bphhb", 0x100},
        {".bphnm", 0x120},   {".bphsh", 0x170}, {".bslnk", 0x100},  {".byml", 0x100},  {".cai", 0x100},   {".casset.byml", 0x1C0},
        {".chunk", 0x100},   {".crbin", 0x100}, {".cutinfo", 0x100}, {".dpi", 0x100},  {".genvb", 0x180}, {".jpg", 0x100},
        {".pack", 0x180},    {".png", 0x100},   {".quad", 0x100},   {".sarc", 0x180},  {".tscb", 0x100},  {".txtg", 0x100},
        {".txt", 0x100},     {".vsts", 0x100},  {".wbr", 0x100},    {".zs", 0x100},
    };
    for (const auto& e : extras)
        if (ext == e.ext) return size + e.add;
    return (size + 1500) * 4;
}

}  // namespace

Table Table::parse(std::string_view d) {
    if (d.size() < 22 || d.substr(0, 6) != "RESTBL") throw std::runtime_error("the size table is damaged");
    Table t;
    t.version = le32(d, 6);
    uint32_t nameSize = le32(d, 10), hashCount = le32(d, 14), nameCount = le32(d, 18);
    if (nameSize != kNameSize || 22 + uint64_t(hashCount) * 8 + uint64_t(nameCount) * (nameSize + 4) > d.size())
        throw std::runtime_error("the size table is damaged");
    size_t at = 22;
    for (uint32_t i = 0; i < hashCount; ++i, at += 8) t.hashes[le32(d, at)] = le32(d, at + 4);
    for (uint32_t i = 0; i < nameCount; ++i, at += nameSize + 4) {
        auto raw = d.substr(at, nameSize);
        t.names[std::string(raw.substr(0, raw.find('\0')))] = le32(d, at + nameSize);
    }
    return t;
}

std::string Table::write() const {
    std::string out = "RESTBL";
    put32(out, version);
    put32(out, uint32_t(kNameSize));
    put32(out, uint32_t(hashes.size()));
    put32(out, uint32_t(names.size()));
    for (const auto& [h, s] : hashes) {
        put32(out, h);
        put32(out, s);
    }
    for (const auto& [n, s] : names) {
        std::string name = n.substr(0, kNameSize - 1);
        name.resize(kNameSize, '\0');
        out += name;
        put32(out, s);
    }
    return out;
}

uint32_t crc32(std::string_view text) {
    return uint32_t(::crc32(0, reinterpret_cast<const Bytef*>(text.data()), uInt(text.size())));
}

std::string resourceName(std::string_view path) {
    if (endsWith(path, ".zs") || endsWith(path, ".mc")) path = path.substr(0, path.size() - 3);
    std::string out(path);
    for (auto& c : out)
        if (c == '\\') c = '/';
    return out;
}

uint32_t resourceSize(std::string_view path, uint32_t fileSize, std::string_view data) {
    uint32_t size = (fileSize + 0x1F) & ~0x1Fu;
    return typeSize(size, resourceName(path), resourceExtension(path), data);
}

void Collector::collect(const std::string& path, uint32_t fileSize, std::string_view data, uint32_t override) {
    auto name = resourceName(path);
    auto ext = resourceExtension(path);
    if (name == "Pack/ZsDic.pack" || ext == ".rsizetable" || ext == ".bwav" || ext == ".webm") return;
    if (!data.empty() && isVanilla_ && isVanilla_(path, data)) return;
    uint32_t size = override ? override : resourceSize(path, fileSize, data);
    if (auto it = result_.names.find(name); it != result_.names.end()) {
        it->second = size;
        return;
    }
    auto hash = crc32(name);
    if (result_.hashes.emplace(hash, size).second) return;
    // Already there: the game's own entry (replace it), or another changed file with the same
    // hash (spell this one out).
    if (!vanilla_.hashes.count(hash)) result_.names[name] = size;
    else result_.hashes[hash] = size;
}

}  // namespace mm::restbl
