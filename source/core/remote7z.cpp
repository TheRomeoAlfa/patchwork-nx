#include "remote7z.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace mm {

namespace {

const char kSignature[6] = {'7', 'z', char(0xBC), char(0xAF), 0x27, 0x1C};

uint64_t le64(const std::string& d, size_t at) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | uint8_t(d[at + i]);
    return v;
}

// 7z's variable-length numbers: the first byte's leading 1 bits say how many bytes follow.
uint64_t readNumber(const std::string& d, size_t& at) {
    if (at >= d.size()) throw std::runtime_error("bad 7z header");
    uint8_t first = uint8_t(d[at++]);
    uint8_t mask = 0x80;
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        if (!(first & mask)) return value | (uint64_t(first & (mask - 1)) << (8 * i));
        if (at >= d.size()) throw std::runtime_error("bad 7z header");
        value |= uint64_t(uint8_t(d[at++])) << (8 * i);
        mask >>= 1;
    }
    return value;
}

// A read-only view of a file where only some byte ranges are known; the rest reads as zeros
// (the table of contents never points there).
struct Sparse {
    const std::vector<std::pair<uint64_t, std::string>>* pieces;
    uint64_t size = 0, pos = 0;
    std::vector<char> buf = std::vector<char>(64 * 1024);
};

la_ssize_t sparseRead(struct archive*, void* ud, const void** out) {
    auto* s = static_cast<Sparse*>(ud);
    if (s->pos >= s->size) return 0;
    size_t n = size_t(std::min<uint64_t>(s->buf.size(), s->size - s->pos));
    std::fill(s->buf.begin(), s->buf.begin() + n, 0);
    for (const auto& [off, data] : *s->pieces) {
        uint64_t a = std::max(off, s->pos), b = std::min(off + data.size(), s->pos + n);
        if (a < b) std::memcpy(s->buf.data() + (a - s->pos), data.data() + (a - off), size_t(b - a));
    }
    s->pos += n;
    *out = s->buf.data();
    return la_ssize_t(n);
}

la_int64_t sparseSeek(struct archive*, void* ud, la_int64_t offset, int whence) {
    auto* s = static_cast<Sparse*>(ud);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? int64_t(s->pos) : int64_t(s->size);
    int64_t to = base + offset;
    if (to < 0) return ARCHIVE_FATAL;
    s->pos = uint64_t(to);
    return to;
}

la_int64_t sparseSkip(struct archive*, void* ud, la_int64_t request) {
    auto* s = static_cast<Sparse*>(ud);
    auto n = std::min<uint64_t>(uint64_t(request), s->size - std::min(s->pos, s->size));
    s->pos += n;
    return la_int64_t(n);
}

}  // namespace

std::vector<std::string> list7zFromPieces(const std::vector<std::pair<uint64_t, std::string>>& pieces, uint64_t totalSize) {
    Sparse sparse{&pieces, totalSize};
    auto* a = archive_read_new();
    archive_read_support_format_7zip(a);
    archive_read_support_filter_all(a);
    archive_read_set_read_callback(a, sparseRead);
    archive_read_set_seek_callback(a, sparseSeek);
    archive_read_set_skip_callback(a, sparseSkip);
    archive_read_set_callback_data(a, &sparse);
    std::vector<std::string> names;
    if (archive_read_open1(a) != ARCHIVE_OK) {
        std::string err = archive_error_string(a) ? archive_error_string(a) : "unreadable";
        archive_read_free(a);
        throw std::runtime_error("couldn't read the archive's contents: " + err);
    }
    archive_entry* e = nullptr;
    int rc;
    while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
        if (archive_entry_filetype(e) != AE_IFDIR && archive_entry_pathname(e)) names.push_back(archive_entry_pathname(e));
    }
    bool failed = rc != ARCHIVE_EOF;
    std::string err = archive_error_string(a) ? archive_error_string(a) : "";
    archive_read_free(a);
    if (failed && names.empty()) throw std::runtime_error("couldn't read the archive's contents: " + err);
    return names;
}

std::vector<std::string> remote7zListing(HttpClient& http, const std::string& url) {
    std::vector<std::pair<uint64_t, std::string>> pieces;
    std::string direct;  // the file server GameBanana's link redirects to
    auto head = http.getRange(url, 0, 31, &direct);
    if (direct.rfind("https://", 0) != 0) direct = url;
    if (head.size() < 32 || std::memcmp(head.data(), kSignature, 6) != 0) throw std::runtime_error("not a 7z archive");
    uint64_t start = 32 + le64(head, 12), size = le64(head, 20);
    if (size == 0 || size > 16 * 1024 * 1024) throw std::runtime_error("unexpected 7z header size");
    pieces.emplace_back(0, head);
    auto header = http.getRange(direct, start, start + size - 1);
    pieces.emplace_back(start, header);
    if (!header.empty() && uint8_t(header[0]) == 0x17) {
        // Compressed header: it's stored as a pack stream, described here.
        size_t at = 1;
        if (header.size() < 2 || uint8_t(header[at++]) != 0x06) throw std::runtime_error("unexpected 7z header");
        uint64_t packPos = readNumber(header, at), count = readNumber(header, at), packed = 0;
        if (at < header.size() && uint8_t(header[at]) == 0x09) {
            ++at;
            for (uint64_t i = 0; i < count; ++i) packed += readNumber(header, at);
        }
        if (packed == 0 || packed > 16 * 1024 * 1024) throw std::runtime_error("unexpected 7z header size");
        pieces.emplace_back(32 + packPos, http.getRange(direct, 32 + packPos, 32 + packPos + packed - 1));
    }
    return list7zFromPieces(pieces, start + size);
}

}  // namespace mm
