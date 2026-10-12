// MD5 (RFC 1321). Used only to check downloads against GameBanana's published checksum.
#include "md5.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace mm {

namespace {

class Md5 {
public:
    void update(const unsigned char* data, size_t len) {
        size_t idx = static_cast<size_t>(bytes_ % 64);
        bytes_ += len;
        size_t i = 0;
        if (idx) {
            size_t take = std::min(len, 64 - idx);
            std::memcpy(buf_.data() + idx, data, take);
            i = take;
            if (idx + take < 64) return;
            block(buf_.data());
        }
        for (; i + 64 <= len; i += 64) block(data + i);
        std::memcpy(buf_.data(), data + i, len - i);
    }

    std::string hex() {
        uint64_t bits = bytes_ * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (bytes_ % 64 != 56) update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (8 * i));
        update(len, 8);
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (uint32_t v : state_)
            for (int i = 0; i < 4; ++i) {
                auto b = static_cast<unsigned char>(v >> (8 * i));
                out += digits[b >> 4];
                out += digits[b & 15];
            }
        return out;
    }

private:
    static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

    void block(const unsigned char* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = static_cast<uint32_t>(p[i * 4]) | static_cast<uint32_t>(p[i * 4 + 1]) << 8 |
                   static_cast<uint32_t>(p[i * 4 + 2]) << 16 | static_cast<uint32_t>(p[i * 4 + 3]) << 24;
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            uint32_t tmp = d;
            d = c;
            c = b;
            b = b + rotl(a + f + K[i] + M[g], S[i]);
            a = tmp;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
    }

    std::array<uint32_t, 4> state_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    std::array<unsigned char, 64> buf_{};
    uint64_t bytes_ = 0;
};

}  // namespace

std::string md5String(const std::string& data) {
    Md5 m;
    m.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return m.hex();
}

std::string md5File(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    Md5 m;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        auto n = in.gcount();
        if (n > 0) m.update(reinterpret_cast<const unsigned char*>(buf.data()), static_cast<size_t>(n));
    }
    return m.hex();
}

}  // namespace mm
