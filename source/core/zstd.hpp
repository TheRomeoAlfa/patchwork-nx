#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace mm::zs {

// Tears of the Kingdom compresses its files (".zs") with zstd, most of them with one of the
// dictionaries in Pack/ZsDic.pack.zs. Each compressed file names the dictionary it needs.
class Dictionaries {
public:
    Dictionaries();
    ~Dictionaries();
    Dictionaries(Dictionaries&&) noexcept;
    Dictionaries& operator=(Dictionaries&&) noexcept;

    // From the game's Pack/ZsDic.pack.zs (as read from the game, still compressed).
    static Dictionaries load(std::string_view zsDicPack);
    bool has(uint32_t id) const;
    size_t size() const;

    // Throws std::runtime_error when the data is damaged or needs a dictionary we don't have.
    std::string decompress(std::string_view data) const;
    // `dictionary` 0 = none (as for the size table); level 7 as TKMM.
    std::string compress(std::string_view data, uint32_t dictionary, int level = 7) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool isZstd(std::string_view data);
// The dictionary a compressed file needs (0 = none).
uint32_t frameDictionary(std::string_view data);

}  // namespace mm::zs
