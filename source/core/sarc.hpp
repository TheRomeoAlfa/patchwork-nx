#pragma once

#include <cstdint>
#include <string>
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

bool isSarc(const std::string& data);  // plain or Yaz0-compressed

// BCML's pack merge: start from the first archive (the game's own) and lay each later one
// (mods, lowest priority first) over it. A file a mod has wins; archives nested inside are
// merged the same way, file by file, and Yaz0-compressed again when their name starts with
// ".s" (".sblarc", ".sbactorpack"; not ".sarc"). `compress` picks real compression for those
// (smaller) or stored (faster).
std::string mergeSarcs(const std::vector<std::string>& archives, bool compress = true);

}  // namespace mm
