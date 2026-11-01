#pragma once
// A fake Tears of the Kingdom for tests: the game's zstd dictionaries and TKMM's pack index.

#include <string>
#include <utility>
#include <vector>

#include <zdict.h>

#include "sarc.hpp"
#include "test.hpp"
#include "totk.hpp"
#include "zstd.hpp"

namespace fixtures {

using namespace mm;

inline std::vector<std::string> dictionarySamples() {
    std::vector<std::string> samples;
    for (int i = 0; i < 400; ++i) samples.push_back("Actor/Weapon_Sword_" + std::to_string(i) + ".engine__actor__ActorParam.bgyml Components ModelInfo");
    return samples;
}

// A Pack/ZsDic.pack.zs like the game's: dictionaries 1 and 3 (magic, then their id), trained
// from similar files, and a file that isn't one.
inline std::string fakeZsDic() {
    auto samples = dictionarySamples();
    std::string all;
    std::vector<size_t> sizes;
    for (auto& s : samples) { all += s; sizes.push_back(s.size()); }
    auto makeDict = [&](unsigned id) {
        std::string dict(4096, '\0');
        ZDICT_params_t params{};
        params.dictID = id;
        params.compressionLevel = 7;
        auto content = all.substr(0, 2048);
        auto n = ZDICT_finalizeDictionary(dict.data(), dict.size(), content.data(), content.size(), all.data(), sizes.data(),
                                          unsigned(sizes.size()), params);
        CHECK(!ZDICT_isError(n));
        dict.resize(ZDICT_isError(n) ? 0 : n);
        return dict;
    };
    Sarc pack;
    pack.files.push_back({"zs.zsdic", makeDict(1)});
    pack.files.push_back({"pack.zsdic", makeDict(3)});
    pack.files.push_back({"readme.txt", "not a dictionary"});
    zs::Dictionaries none;
    return none.compress(pack.write(), 0);
}


// TKMM's pack index format: "PKCH", entry count, string table offset, string count, then
// entries (first/last letter, hash of the UTF-16 name, pack index) and "name\0<attributes>".
inline std::string fakePackIndex(const std::vector<std::pair<std::string, uint16_t>>& files,
                          const std::vector<std::pair<std::string, uint8_t>>& packs) {
    std::string d;
    auto put = [&](auto v) { d.append(reinterpret_cast<const char*>(&v), sizeof v); };
    put(uint32_t(0x48434B50));
    put(uint32_t(files.size()));
    put(uint32_t(0x10 + 8 * files.size()));
    put(uint32_t(packs.size()));
    for (const auto& [name, pack] : files) {
        put(uint16_t((uint8_t(name.front()) << 8) | uint8_t(name.back())));
        put(totk::xxHash32(totk::utf16le(name)));
        put(pack);
    }
    for (const auto& [name, attributes] : packs) {
        d += name;
        d += '\0';
        d += char(attributes);
    }
    zs::Dictionaries none;
    return none.compress(d, 0);
}


}  // namespace fixtures
