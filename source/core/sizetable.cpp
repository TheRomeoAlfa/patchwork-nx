#include "sizetable.hpp"

#include <zlib.h>

#include <algorithm>
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

std::string yaz0Compress(const std::string& data) {
    std::string out = "Yaz0";
    putBe32(out, uint32_t(data.size()));
    out.append(8, '\0');
    out.reserve(16 + data.size() / 2);
    constexpr size_t kWindow = 0x1000, kMaxLen = 0x111, kMinLen = 3, kChain = 16;
    constexpr size_t kHashBits = 15;
    std::vector<int64_t> head(size_t(1) << kHashBits, -1);
    std::vector<int64_t> prev(data.size(), -1);
    auto hashAt = [&](size_t i) {
        uint32_t v = (uint32_t(uint8_t(data[i])) << 16) | (uint32_t(uint8_t(data[i + 1])) << 8) | uint8_t(data[i + 2]);
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    auto insert = [&](size_t i) {
        if (i + 2 >= data.size()) return;
        auto h = hashAt(i);
        prev[i] = head[h];
        head[h] = int64_t(i);
    };
    size_t pos = 0;
    while (pos < data.size()) {
        size_t flagAt = out.size();
        out += '\0';
        uint8_t flags = 0;
        for (int bit = 7; bit >= 0 && pos < data.size(); --bit) {
            size_t bestLen = 0, bestDist = 0;
            if (pos + kMinLen <= data.size()) {
                size_t limit = std::min(kMaxLen, data.size() - pos);
                int64_t cand = head[hashAt(pos)];
                for (size_t n = 0; cand >= 0 && n < kChain; ++n, cand = prev[size_t(cand)]) {
                    size_t c = size_t(cand), dist = pos - c;
                    if (dist > kWindow) break;
                    size_t len = 0;
                    while (len < limit && data[c + len] == data[pos + len]) ++len;
                    if (len > bestLen) {
                        bestLen = len;
                        bestDist = dist;
                        if (len == limit) break;
                    }
                }
            }
            if (bestLen >= kMinLen) {
                size_t d = bestDist - 1;
                if (bestLen < 0x12) {
                    out += char(((bestLen - 2) << 4) | (d >> 8));
                    out += char(d & 0xff);
                } else {
                    out += char(d >> 8);
                    out += char(d & 0xff);
                    out += char(bestLen - 0x12);
                }
                for (size_t k = 0; k < bestLen; ++k) insert(pos + k);
                pos += bestLen;
            } else {
                flags |= uint8_t(1 << bit);
                out += data[pos];
                insert(pos);
                ++pos;
            }
        }
        out[flagAt] = char(flags);
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
