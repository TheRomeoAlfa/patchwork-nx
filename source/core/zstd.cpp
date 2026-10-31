#include "zstd.hpp"

#include <cstring>
#include <stdexcept>

#include <zstd.h>

#include "sarc.hpp"

namespace mm::zs {

namespace {

constexpr uint32_t kDictionaryMagic = 0xEC30A437;

uint32_t le32(std::string_view d, size_t at) {
    uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

}  // namespace

struct Dictionaries::Impl {
    struct Dict {
        std::string bytes;
        ZSTD_DDict* d = nullptr;
        std::map<int, ZSTD_CDict*> c;  // by level, made when first needed
    };
    std::map<uint32_t, Dict> dicts;
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    ZSTD_CCtx* cctx = ZSTD_createCCtx();

    ~Impl() {
        for (auto& [id, d] : dicts) {
            ZSTD_freeDDict(d.d);
            for (auto& [level, c] : d.c) ZSTD_freeCDict(c);
        }
        ZSTD_freeDCtx(dctx);
        ZSTD_freeCCtx(cctx);
    }
};

Dictionaries::Dictionaries() : impl_(std::make_unique<Impl>()) {}
Dictionaries::~Dictionaries() = default;
Dictionaries::Dictionaries(Dictionaries&&) noexcept = default;
Dictionaries& Dictionaries::operator=(Dictionaries&&) noexcept = default;

bool isZstd(std::string_view data) { return data.size() >= 4 && le32(data, 0) == ZSTD_MAGICNUMBER; }

uint32_t frameDictionary(std::string_view data) { return unsigned(ZSTD_getDictID_fromFrame(data.data(), data.size())); }

Dictionaries Dictionaries::load(std::string_view zsDicPack) {
    Dictionaries out;
    auto pack = out.decompress(zsDicPack);
    for (const auto& f : sarcView(pack)) {
        if (f.data.size() < 8 || le32(f.data, 0) != kDictionaryMagic) continue;
        auto& d = out.impl_->dicts[le32(f.data, 4)];
        d.bytes.assign(f.data);
        d.d = ZSTD_createDDict(d.bytes.data(), d.bytes.size());
        if (!d.d) throw std::runtime_error("couldn't read the game's compression dictionaries");
    }
    if (out.impl_->dicts.empty()) throw std::runtime_error("the game's compression dictionaries are missing");
    return out;
}

bool Dictionaries::has(uint32_t id) const { return impl_->dicts.count(id) != 0; }
size_t Dictionaries::size() const { return impl_->dicts.size(); }

std::string Dictionaries::decompress(std::string_view data) const {
    auto id = frameDictionary(data);
    const ZSTD_DDict* dict = nullptr;
    if (id) {
        auto it = impl_->dicts.find(id);
        if (it == impl_->dicts.end()) throw std::runtime_error("this file needs one of the game's compression dictionaries");
        dict = it->second.d;
    }
    auto size = ZSTD_getFrameContentSize(data.data(), data.size());
    if (size == ZSTD_CONTENTSIZE_ERROR) throw std::runtime_error("this compressed file is damaged");
    if (size != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (size > (uint64_t(1) << 31)) throw std::runtime_error("this compressed file is too big");
        std::string out(size_t(size), '\0');
        auto n = dict ? ZSTD_decompress_usingDDict(impl_->dctx, out.data(), out.size(), data.data(), data.size(), dict)
                      : ZSTD_decompressDCtx(impl_->dctx, out.data(), out.size(), data.data(), data.size());
        if (ZSTD_isError(n) || n != out.size()) throw std::runtime_error("this compressed file is damaged");
        return out;
    }
    // No size in the header: stream it.
    ZSTD_DCtx_reset(impl_->dctx, ZSTD_reset_session_only);
    ZSTD_DCtx_refDDict(impl_->dctx, dict);
    std::string out;
    ZSTD_inBuffer in{data.data(), data.size(), 0};
    size_t ret = 1;
    while (in.pos < in.size && ret != 0) {
        auto at = out.size();
        out.resize(at + ZSTD_DStreamOutSize());
        ZSTD_outBuffer o{out.data() + at, ZSTD_DStreamOutSize(), 0};
        ret = ZSTD_decompressStream(impl_->dctx, &o, &in);
        if (ZSTD_isError(ret)) throw std::runtime_error("this compressed file is damaged");
        out.resize(at + o.pos);
    }
    ZSTD_DCtx_refDDict(impl_->dctx, nullptr);
    if (ret != 0) throw std::runtime_error("this compressed file is cut short");
    return out;
}

std::string Dictionaries::compress(std::string_view data, uint32_t dictionary, int level) const {
    const ZSTD_CDict* dict = nullptr;
    if (dictionary) {
        auto it = impl_->dicts.find(dictionary);
        if (it == impl_->dicts.end()) throw std::runtime_error("the game's compression dictionary is missing");
        auto& c = it->second.c[level];
        if (!c) c = ZSTD_createCDict(it->second.bytes.data(), it->second.bytes.size(), level);
        dict = c;
    }
    std::string out(ZSTD_compressBound(data.size()), '\0');
    auto n = dict ? ZSTD_compress_usingCDict(impl_->cctx, out.data(), out.size(), data.data(), data.size(), dict)
                  : ZSTD_compressCCtx(impl_->cctx, out.data(), out.size(), data.data(), data.size(), level);
    if (ZSTD_isError(n)) throw std::runtime_error(std::string("couldn't compress a file: ") + ZSTD_getErrorName(n));
    out.resize(n);
    return out;
}

}  // namespace mm::zs
