#include "msbt.hpp"

#include <stdexcept>

namespace mm::msbt {

namespace {

uint32_t rd32(const std::string& d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("MSBT: out of range");
    return uint32_t(uint8_t(d[at])) | (uint32_t(uint8_t(d[at + 1])) << 8) | (uint32_t(uint8_t(d[at + 2])) << 16) |
           (uint32_t(uint8_t(d[at + 3])) << 24);
}
uint16_t rd16(const std::string& d, size_t at) {
    if (at + 2 > d.size()) throw std::runtime_error("MSBT: out of range");
    return uint16_t(uint8_t(d[at]) | (uint8_t(d[at + 1]) << 8));
}
void put32(std::string& o, uint32_t v) {
    for (int k = 0; k < 4; ++k) o += char((v >> (8 * k)) & 0xff);
}
void put16(std::string& o, uint16_t v) {
    o += char(v & 0xff);
    o += char(v >> 8);
}

uint32_t labelHash(const std::string& label, uint32_t buckets) {
    uint32_t h = 0;
    for (unsigned char c : label) h = h * 0x492 + c;
    return h % buckets;
}

}  // namespace

Message* File::find(const std::string& label) {
    for (auto& [l, m] : messages)
        if (l == label) return &m;
    return nullptr;
}
const Message* File::find(const std::string& label) const {
    for (const auto& [l, m] : messages)
        if (l == label) return &m;
    return nullptr;
}
void File::set(const std::string& label, Message m) {
    if (auto* old = find(label)) {
        *old = std::move(m);
        return;
    }
    if (!attrStrings && m.attribute.size() != attrSize) m.attribute.resize(attrSize, '\0');
    if (attrStrings && m.attribute.empty()) m.attribute = std::string(2, '\0');
    messages.emplace_back(label, std::move(m));
}

File parse(const std::string& d) {
    if (d.size() < 0x20 || d.compare(0, 8, "MsgStdBn") != 0) throw std::runtime_error("not an MSBT file");
    if (uint8_t(d[8]) != 0xFF || uint8_t(d[9]) != 0xFE) throw std::runtime_error("MSBT: big-endian files aren't supported");
    if (uint8_t(d[0x0C]) != 1) throw std::runtime_error("MSBT: only UTF-16 files are supported");
    File f;
    uint16_t blocks = rd16(d, 0x0E);
    std::vector<std::pair<std::string, uint32_t>> labels;  // label -> index
    std::vector<std::string> texts;
    std::vector<std::string> attrs;
    std::vector<uint32_t> styles;
    size_t at = 0x20;
    for (uint16_t b = 0; b < blocks; ++b) {
        if (at + 0x10 > d.size()) throw std::runtime_error("MSBT: truncated");
        std::string magic = d.substr(at, 4);
        uint32_t size = rd32(d, at + 4);
        size_t data = at + 0x10;
        if (data + size > d.size()) throw std::runtime_error("MSBT: block out of range");
        std::string block = d.substr(data, size);
        if (magic == "LBL1") {
            f.buckets = rd32(block, 0);
            for (uint32_t k = 0; k < f.buckets; ++k) {
                uint32_t count = rd32(block, 4 + 8 * k), off = rd32(block, 8 + 8 * k);
                for (uint32_t n = 0; n < count; ++n) {
                    if (off >= block.size()) throw std::runtime_error("MSBT: bad label");
                    uint8_t len = uint8_t(block[off]);
                    std::string name = block.substr(off + 1, len);
                    labels.emplace_back(name, rd32(block, off + 1 + len));
                    // Each label takes at least 5 bytes; more means buckets point at the same ones.
                    if (labels.size() > block.size() / 5) throw std::runtime_error("MSBT: bad label table");
                    off += 1 + len + 4;
                }
            }
        } else if (magic == "ATR1") {
            f.hasAttributes = true;
            uint32_t count = rd32(block, 0);
            f.attrSize = rd32(block, 4);
            // Rows must fit in the block; attribute-less rows (size 0) still need a message each,
            // and every message takes at least a 4-byte text offset.
            if (f.attrSize ? uint64_t(count) * f.attrSize > block.size() : count > d.size() / 4)
                throw std::runtime_error("MSBT: bad attribute table");
            size_t end = 8 + size_t(count) * f.attrSize;
            // BOTW: each row is the offset of a UTF-16 string stored after the rows.
            bool strings = f.attrSize == 4 && count > 0 && end < block.size();
            for (uint32_t k = 0; strings && k < count; ++k) {
                uint32_t off = rd32(block, 8 + 4 * k);
                if (off < end || off > block.size()) strings = false;
            }
            f.attrStrings = strings;
            size_t attrBytes = 0;
            for (uint32_t k = 0; k < count; ++k) {
                if (strings) {
                    uint32_t off = rd32(block, 8 + 4 * k);
                    uint32_t next = k + 1 < count ? rd32(block, 8 + 4 * (k + 1)) : uint32_t(block.size());
                    if (next < off) next = off;
                    if ((attrBytes += next - off) > block.size()) throw std::runtime_error("MSBT: bad attribute strings");
                    attrs.push_back(block.substr(off, next - off));
                } else {
                    attrs.push_back(block.substr(8 + size_t(k) * f.attrSize, f.attrSize));
                }
            }
            if (!strings && end < block.size()) f.attrExtra = block.substr(end);
        } else if (magic == "TSY1") {
            f.hasStyles = true;
            for (size_t k = 0; k + 4 <= block.size(); k += 4) styles.push_back(rd32(block, k));
        } else if (magic == "TXT2") {
            uint32_t count = rd32(block, 0);
            for (uint32_t k = 0; k < count; ++k) {
                uint32_t start = rd32(block, 4 + 4 * k);
                uint32_t end = k + 1 < count ? rd32(block, 8 + 4 * k) : uint32_t(block.size());
                if (start > end || end > block.size()) throw std::runtime_error("MSBT: bad text offset");
                std::string t = block.substr(start, end - start);
                if (t.size() >= 2 && t[t.size() - 1] == 0 && t[t.size() - 2] == 0) t.resize(t.size() - 2);  // terminator
                texts.push_back(std::move(t));
            }
        } else {
            f.otherBlocks.emplace_back(magic, block);
        }
        at = (data + size + 15) & ~size_t(15);
    }
    // Messages in TXT2 order, named by their labels.
    std::vector<std::string> names(texts.size());
    for (const auto& [name, index] : labels)
        if (index < names.size()) names[index] = name;
    for (size_t k = 0; k < texts.size(); ++k) {
        Message m;
        m.text = texts[k];
        if (k < attrs.size()) m.attribute = attrs[k];
        if (k < styles.size()) m.style = styles[k];
        f.messages.emplace_back(names[k], std::move(m));
    }
    return f;
}

std::string write(const File& f) {
    std::vector<std::pair<std::string, std::string>> blocks;
    // LBL1
    {
        std::vector<std::vector<std::pair<std::string, uint32_t>>> buckets(f.buckets);
        for (size_t k = 0; k < f.messages.size(); ++k)
            buckets[labelHash(f.messages[k].first, f.buckets)].emplace_back(f.messages[k].first, uint32_t(k));
        std::string b;
        put32(b, f.buckets);
        std::string labels;
        uint32_t off = 4 + 8 * f.buckets;
        for (const auto& bucket : buckets) {
            put32(b, uint32_t(bucket.size()));
            put32(b, off + uint32_t(labels.size()));
            for (const auto& [name, index] : bucket) {
                labels += char(uint8_t(name.size()));
                labels += name;
                put32(labels, index);
            }
        }
        blocks.emplace_back("LBL1", b + labels);
    }
    if (f.hasAttributes) {
        std::string b;
        put32(b, uint32_t(f.messages.size()));
        put32(b, f.attrSize);
        if (f.attrStrings) {
            uint32_t off = 8 + 4 * uint32_t(f.messages.size());
            std::string pool;
            for (const auto& [l, m] : f.messages) {
                put32(b, off + uint32_t(pool.size()));
                pool += m.attribute.empty() ? std::string(2, '\0') : m.attribute;
            }
            b += pool;
        } else {
            for (const auto& [l, m] : f.messages) {
                std::string row = m.attribute;
                row.resize(f.attrSize, '\0');
                b += row;
            }
            b += f.attrExtra;
        }
        blocks.emplace_back("ATR1", b);
    }
    if (f.hasStyles) {
        std::string b;
        for (const auto& [l, m] : f.messages) put32(b, m.style);
        blocks.emplace_back("TSY1", b);
    }
    {
        std::string b;
        put32(b, uint32_t(f.messages.size()));
        uint32_t off = 4 + 4 * uint32_t(f.messages.size());
        std::string strings;
        for (const auto& [l, m] : f.messages) {
            put32(b, off + uint32_t(strings.size()));
            strings += m.text;
            strings += std::string(2, '\0');
        }
        blocks.emplace_back("TXT2", b + strings);
    }
    for (const auto& ob : f.otherBlocks) blocks.push_back(ob);

    std::string out = "MsgStdBn";
    out += char(0xFF);
    out += char(0xFE);
    put16(out, 0);
    out += char(1);  // UTF-16
    out += char(3);  // version
    put16(out, uint16_t(blocks.size()));
    put16(out, 0);
    put32(out, 0);  // file size, below
    out.append(0x20 - out.size(), '\0');
    for (const auto& [magic, data] : blocks) {
        out += magic;
        put32(out, uint32_t(data.size()));
        out.append(8, '\0');
        out += data;
        while (out.size() % 16) out += char(0xAB);
    }
    for (int k = 0; k < 4; ++k) out[0x12 + k] = char((out.size() >> (8 * k)) & 0xff);
    return out;
}

std::string toUtf16(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = uint8_t(s[i]);
        size_t len = cp < 0x80 ? 1 : cp < 0xE0 ? 2 : cp < 0xF0 ? 3 : 4;
        if (len > 1) {
            cp &= len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07;
            for (size_t k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (uint8_t(s[i + k]) & 0x3F);
        }
        i += len;
        auto unit = [&](uint32_t u) { put16(out, uint16_t(u)); };
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 | (cp >> 10));
            unit(0xDC00 | (cp & 0x3FF));
        } else {
            unit(cp);
        }
    }
    return out;
}

std::string toUtf8(const std::string& s) {
    std::string out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        uint32_t cp = rd16(s, i);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < s.size()) {
            uint32_t lo = rd16(s, i + 2);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

}  // namespace mm::msbt
