#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mm {

// SARC, the archive format BOTW packs most of its files in (.pack, .sbactorpack, .sblarc…).
// Archives nest: a .pack can hold .sblarc files that are themselves (Yaz0-compressed) SARCs.
struct SarcFile {
    std::string name;
    std::string data;
};

struct Sarc {
    bool littleEndian = true;
    std::vector<SarcFile> files;

    static Sarc parse(const std::string& data);  // Yaz0 or plain; throws on bad data
    // Plain SARC with the Switch's byte order and the alignment the game expects for each
    // file type.
    std::string write() const;
    const SarcFile* find(const std::string& name) const;
};

// A file of an archive read in place: `data` points into the archive's bytes.
struct SarcView {
    std::string name;
    std::string_view data;
};
// The files of a plain (not Yaz0) SARC without copying them; `data` must outlive the views.
// For big archives only read from, where Sarc::parse would copy everything.
std::vector<SarcView> sarcView(std::string_view data);

bool isSarc(std::string_view data);  // plain or Yaz0-compressed

// BCML's pack merge: start from the game's own archive (the first, when `firstIsGame`) and
// lay the mods' copies over it, lowest priority first. For each file the highest-priority
// copy that differs from the game's wins (an unchanged copy in a complete archive doesn't
// undo another mod's change). Archives nested inside are merged the same way, file by file,
// and compressed again when the game's copy was (`compress`: real Yaz0 compression, smaller;
// otherwise stored, faster).
std::string mergeSarcs(const std::vector<std::string>& archives, bool compress = true, bool firstIsGame = true);

// Lay one mod's complete archive over an already merged one: each file of `top` that differs
// from the game's copy replaces the merged one; archives inside are layered file by file
// (and compressed again when `top`'s copy was). The rest of `current` stays. All plain or
// Yaz0; returns a plain SARC. `game` and `top` are read in place.
std::string layerSarc(std::string_view game, std::string current, std::string_view top);

}  // namespace mm
