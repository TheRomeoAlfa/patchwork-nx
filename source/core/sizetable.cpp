#include "sizetable.hpp"

#include <zlib.h>

#include <cstring>
#include <stdexcept>

namespace mm {

namespace {

uint32_t be32(const std::string& d, size_t at) {
    return (uint32_t(uint8_t(d[at])) << 24) | (uint32_t(uint8_t(d[at + 1])) << 16) | (uint32_t(uint8_t(d[at + 2])) << 8) |
           uint32_t(uint8_t(d[at + 3]));
}
uint32_t le32(const std::string& d, size_t at) {
    return uint32_t(uint8_t(d[at])) | (uint32_t(uint8_t(d[at + 1])) << 8) | (uint32_t(uint8_t(d[at + 2])) << 16) |
           (uint32_t(uint8_t(d[at + 3])) << 24);
}
void putBe32(std::string& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out += char((v >> s) & 0xff);
}
void putLe32(std::string& out, uint32_t v) {
    for (int s = 0; s <= 24; s += 8) out += char((v >> s) & 0xff);
}

constexpr size_t kNameLen = 128;

}  // namespace

bool isYaz0(const std::string& data) { return data.size() >= 16 && data.compare(0, 4, "Yaz0") == 0; }

std::string yaz0Decompress(const std::string& data) {
    if (!isYaz0(data)) throw std::runtime_error("not Yaz0 data");
    size_t size = be32(data, 4);
    std::string out;
    out.reserve(size);
    size_t src = 16;
    while (out.size() < size) {
        if (src >= data.size()) throw std::runtime_error("truncated Yaz0 data");
        uint8_t group = uint8_t(data[src++]);
        for (int bit = 7; bit >= 0 && out.size() < size; --bit) {
            if (group & (1 << bit)) {
                if (src >= data.size()) throw std::runtime_error("truncated Yaz0 data");
                out += data[src++];
                continue;
            }
            if (src + 1 >= data.size()) throw std::runtime_error("truncated Yaz0 data");
            uint8_t b1 = uint8_t(data[src]), b2 = uint8_t(data[src + 1]);
            src += 2;
            size_t dist = ((size_t(b1 & 0x0f) << 8) | b2) + 1;
            size_t len = b1 >> 4;
            if (len == 0) {
                if (src >= data.size()) throw std::runtime_error("truncated Yaz0 data");
                len = size_t(uint8_t(data[src++])) + 0x12;
            } else {
                len += 2;
            }
            if (dist > out.size()) throw std::runtime_error("bad Yaz0 back-reference");
            for (size_t i = 0; i < len && out.size() < size; ++i) out += out[out.size() - dist];
        }
    }
    return out;
}

std::string yaz0Store(const std::string& data) {
    std::string out = "Yaz0";
    putBe32(out, uint32_t(data.size()));
    out.append(8, '\0');
    out.reserve(16 + data.size() + data.size() / 8 + 1);
    for (size_t i = 0; i < data.size(); i += 8) {
        out += char(0xff);  // eight literal bytes follow
        out.append(data, i, 8);
    }
    return out;
}

uint32_t sizeTableCrc(const std::string& name) {
    return uint32_t(crc32(0, reinterpret_cast<const Bytef*>(name.data()), uInt(name.size())));
}

SizeTable SizeTable::parse(const std::string& raw) {
    auto data = isYaz0(raw) ? yaz0Decompress(raw) : raw;
    SizeTable t;
    if (data.size() < 12 || data.compare(0, 4, "RSTB") != 0) throw std::runtime_error("not a resource size table");
    uint32_t crcCount = le32(data, 4), nameCount = le32(data, 8);
    size_t at = 12;
    if (at + size_t(crcCount) * 8 + size_t(nameCount) * (kNameLen + 4) > data.size())
        throw std::runtime_error("truncated resource size table");
    for (uint32_t i = 0; i < crcCount; ++i, at += 8) t.crc[le32(data, at)] = le32(data, at + 4);
    for (uint32_t i = 0; i < nameCount; ++i, at += kNameLen + 4) {
        std::string name(data.data() + at, kNameLen);
        name.resize(std::strlen(name.c_str()));
        t.named[name] = le32(data, at + kNameLen);
    }
    return t;
}

std::string SizeTable::write() const {
    std::string out = "RSTB";
    putLe32(out, uint32_t(crc.size()));
    putLe32(out, uint32_t(named.size()));
    for (const auto& [c, size] : crc) {  // std::map keeps them sorted, as the game expects
        putLe32(out, c);
        putLe32(out, size);
    }
    for (const auto& [name, size] : named) {
        std::string padded = name.substr(0, kNameLen - 1);
        padded.resize(kNameLen, '\0');
        out += padded;
        putLe32(out, size);
    }
    return out;
}

bool SizeTable::has(const std::string& name) const { return named.count(name) || crc.count(sizeTableCrc(name)); }

void SizeTable::remove(const std::string& name) {
    if (named.erase(name)) return;
    crc.erase(sizeTableCrc(name));
}

std::string mergeSizeTables(const std::vector<std::string>& tables, const std::vector<std::map<std::string, uint32_t>>& sizes) {
    SizeTable merged;
    for (const auto& raw : tables) {
        SizeTable t;
        try {
            t = SizeTable::parse(raw);
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& [c, size] : t.crc) {
            auto& slot = merged.crc[c];
            if (size > slot) slot = size;
        }
        for (const auto& [name, size] : t.named) {
            auto& slot = merged.named[name];
            if (size > slot) slot = size;
        }
    }
    for (const auto& list : sizes) {
        for (const auto& [name, size] : list) {
            if (size == 0) {
                merged.remove(name);
                continue;
            }
            auto named = merged.named.find(name);
            auto& slot = named != merged.named.end() ? named->second : merged.crc[sizeTableCrc(name)];
            if (size > slot) slot = size;
        }
    }
    return yaz0Store(merged.write());
}

}  // namespace mm
