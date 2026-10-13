#include "sarc.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <stdexcept>

#include "sizetable.hpp"

namespace mm {

namespace {

uint16_t rd16(const std::string& d, size_t at, bool le) {
    uint8_t a = uint8_t(d[at]), b = uint8_t(d[at + 1]);
    return le ? uint16_t(a | (b << 8)) : uint16_t((a << 8) | b);
}
uint32_t rd32(const std::string& d, size_t at, bool le) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(d[at + i])) << (le ? 8 * i : 8 * (3 - i));
    return v;
}
void put16(std::string& o, uint16_t v) {
    o += char(v & 0xff);
    o += char(v >> 8);
}
void put32(std::string& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o += char((v >> (8 * i)) & 0xff);
}
void set32(std::string& o, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) o[at + i] = char((v >> (8 * i)) & 0xff);
}

uint32_t nameHash(const std::string& name) {
    uint32_t h = 0;
    for (char c : name) h = h * 0x65 + uint32_t(int32_t(static_cast<signed char>(c)));
    return h;
}

// Nintendo binary files (BNTX, FRES, …) say in their header how they must be aligned.
size_t binaryFileAlignment(const std::string& d) {
    if (d.size() <= 0x20) return 1;
    uint16_t bom = rd16(d, 0xC, true);
    if (bom != 0xFEFF && bom != 0xFFFE) return 1;
    bool le = bom == 0xFEFF;
    if (rd32(d, 0x1C, le) != d.size()) return 1;
    unsigned shift = uint8_t(d[0xE]);
    return shift < 16 ? size_t(1) << shift : 1;
}

size_t alignmentFor(const std::string& data) {
    size_t a = 8;
    a = std::lcm(a, binaryFileAlignment(data));
    if (data.size() >= 4 && data.compare(0, 4, "SARC") == 0) a = std::lcm(a, size_t(0x2000));
    return a;
}

std::string extensionOf(const std::string& name) {
    auto dot = name.rfind('.');
    return dot == std::string::npos ? "" : name.substr(dot);
}

// BCML leaves these alone (merged by other means, or not safely merged per file).
bool isSpecial(const std::string& name) {
    static const char* names[] = {"gamedata", "savedataformat", "tera_resource.Nin_NX_NVN", "Dungeon", "Bootup_", "AocMainField"};
    for (const char* n : names)
        if (name.find(n) != std::string::npos) return true;
    return false;
}

}  // namespace

bool isSarc(const std::string& data) {
    if (data.size() >= 4 && data.compare(0, 4, "SARC") == 0) return true;
    // Yaz0: the first four bytes are always stored literally (nothing to refer back to yet),
    // right after the first flag byte.
    return isYaz0(data) && data.size() >= 21 && (uint8_t(data[16]) & 0xF0) == 0xF0 && data.compare(17, 4, "SARC") == 0;
}

Sarc Sarc::parse(const std::string& raw) {
    auto data = isYaz0(raw) ? yaz0Decompress(raw) : raw;
    if (data.size() < 0x20 || data.compare(0, 4, "SARC") != 0) throw std::runtime_error("not a SARC archive");
    Sarc s;
    s.littleEndian = uint8_t(data[6]) == 0xFF && uint8_t(data[7]) == 0xFE;
    bool le = s.littleEndian;
    uint16_t headerSize = rd16(data, 4, le);
    uint32_t dataOffset = rd32(data, 0xC, le);
    size_t sfat = headerSize;
    if (data.compare(sfat, 4, "SFAT") != 0) throw std::runtime_error("bad SARC (no SFAT)");
    uint16_t count = rd16(data, sfat + 6, le);
    size_t nodes = sfat + rd16(data, sfat + 4, le);
    size_t sfnt = nodes + size_t(count) * 16;
    if (sfnt + 8 > data.size() || data.compare(sfnt, 4, "SFNT") != 0) throw std::runtime_error("bad SARC (no SFNT)");
    size_t names = sfnt + rd16(data, sfnt + 4, le);
    for (uint16_t i = 0; i < count; ++i) {
        size_t n = nodes + size_t(i) * 16;
        uint32_t attr = rd32(data, n + 4, le), start = rd32(data, n + 8, le), end = rd32(data, n + 12, le);
        if (dataOffset + size_t(end) > data.size() || start > end) throw std::runtime_error("bad SARC (file out of range)");
        SarcFile f;
        if (attr & 0x01000000) {
            size_t at = names + size_t(attr & 0xffff) * 4;
            auto zero = data.find('\0', at);
            if (at >= data.size() || zero == std::string::npos) throw std::runtime_error("bad SARC (name)");
            f.name = data.substr(at, zero - at);
        } else {
            f.name = "0x" + std::to_string(rd32(data, n, le));  // unnamed: keyed by hash only
        }
        f.data = data.substr(dataOffset + start, end - start);
        s.files.push_back(std::move(f));
    }
    return s;
}

const SarcFile* Sarc::find(const std::string& name) const {
    for (const auto& f : files)
        if (f.name == name) return &f;
    return nullptr;
}

std::string Sarc::write() const {
    std::vector<const SarcFile*> sorted;
    for (const auto& f : files) sorted.push_back(&f);
    std::sort(sorted.begin(), sorted.end(), [](const SarcFile* a, const SarcFile* b) { return nameHash(a->name) < nameHash(b->name); });

    std::string names;
    std::vector<uint32_t> nameOffsets;
    for (const auto* f : sorted) {
        nameOffsets.push_back(uint32_t(names.size() / 4));
        names += f->name;
        names += '\0';
        while (names.size() % 4) names += '\0';
    }
    size_t maxAlign = 4;
    std::vector<size_t> aligns;
    for (const auto* f : sorted) {
        aligns.push_back(alignmentFor(f->data));
        maxAlign = std::max(maxAlign, aligns.back());
    }
    size_t headerEnd = 0x14 + 0xC + sorted.size() * 16 + 8 + names.size();
    size_t dataOffset = (headerEnd + maxAlign - 1) / maxAlign * maxAlign;

    // Place the files.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    size_t cursor = 0;
    for (size_t i = 0; i < sorted.size(); ++i) {
        size_t a = aligns[i];
        size_t abs = dataOffset + cursor;
        abs = (abs + a - 1) / a * a;
        cursor = abs - dataOffset;
        ranges.emplace_back(uint32_t(cursor), uint32_t(cursor + sorted[i]->data.size()));
        cursor += sorted[i]->data.size();
    }

    std::string out;
    out.reserve(dataOffset + cursor);
    out += "SARC";
    put16(out, 0x14);
    put16(out, 0xFEFF);  // written little endian: FF FE
    put32(out, 0);       // file size, set below
    put32(out, uint32_t(dataOffset));
    put16(out, 0x0100);
    put16(out, 0);
    out += "SFAT";
    put16(out, 0xC);
    put16(out, uint16_t(sorted.size()));
    put32(out, 0x65);
    for (size_t i = 0; i < sorted.size(); ++i) {
        put32(out, nameHash(sorted[i]->name));
        put32(out, 0x01000000 | nameOffsets[i]);
        put32(out, ranges[i].first);
        put32(out, ranges[i].second);
    }
    out += "SFNT";
    put16(out, 8);
    put16(out, 0);
    out += names;
    out.resize(dataOffset, '\0');
    for (size_t i = 0; i < sorted.size(); ++i) {
        out.resize(dataOffset + ranges[i].first, '\0');
        out += sorted[i]->data;
    }
    set32(out, 8, uint32_t(out.size()));
    return out;
}

std::string mergeSarcs(const std::vector<std::string>& archives, bool compress, bool firstIsGame) {
    std::vector<Sarc> opened;
    std::vector<bool> isGame;
    for (size_t i = 0; i < archives.size(); ++i) {
        try {
            opened.push_back(Sarc::parse(archives[i]));
            isGame.push_back(firstIsGame && i == 0);
        } catch (const std::exception&) {
            // like BCML: an archive that can't be read is skipped
        }
    }
    if (opened.empty()) throw std::runtime_error("no readable archive to merge");
    std::map<std::string, const SarcFile*> game;  // the game's files by name
    if (isGame[0])
        for (const auto& f : opened[0].files) game.emplace(f.name, &f);

    // The game's order of files, then new ones in the order mods add them.
    std::vector<std::string> order;
    std::map<std::string, std::vector<const std::string*>> versions;  // lowest priority first
    for (const auto& s : opened)
        for (const auto& f : s.files) {
            auto& v = versions[f.name];
            if (v.empty()) order.push_back(f.name);
            v.push_back(&f.data);
        }
    auto unpacked = [](const std::string& d) { return isYaz0(d) ? yaz0Decompress(d) : d; };

    Sarc out;
    out.littleEndian = true;
    for (const auto& name : order) {
        const auto& vs = versions[name];
        auto g = game.find(name);
        const SarcFile* original = g == game.end() ? nullptr : g->second;
        std::string originalPlain = original ? unpacked(original->data) : std::string();
        // The highest-priority copy that differs from the game's (BCML's "is modded").
        const std::string* chosen = nullptr;
        for (auto it = vs.rbegin(); it != vs.rend() && !chosen; ++it) {
            if (!original) {
                chosen = *it;
            } else if (*it != &original->data && **it != original->data && unpacked(**it) != originalPlain) {
                chosen = *it;
            }
        }
        bool modded = chosen != nullptr;
        if (!chosen) chosen = vs.front();
        auto ext = extensionOf(name);
        if (modded && vs.size() > 1 && isSarc(*chosen) && ext != ".sbeventpack" && !isSpecial(name)) {
            std::vector<std::string> nested;
            for (const auto* v : vs)
                if (isSarc(*v)) nested.push_back(*v);
            auto merged = mergeSarcs(nested, compress, original != nullptr && isSarc(original->data));
            // Compressed again when the game's (or the mod's) copy was.
            bool yaz = isYaz0(original ? original->data : *chosen);
            out.files.push_back({name, yaz ? (compress ? yaz0Compress(merged) : yaz0Store(merged)) : merged});
        } else {
            out.files.push_back({name, *chosen});
        }
    }
    return out.write();
}

}  // namespace mm
