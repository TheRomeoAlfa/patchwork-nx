#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mm::tkcl {

// TKMM's mod package (.tkcl), for Tears of the Kingdom: a header describing the mod, its
// changelog and its options, followed by a zip of the files ("romfs/…", "exefs/…", and each
// option's under "<option id>/…"). Files are stored uncompressed under their game names
// without ".zs". Read as TkSharp (MIT) writes them, package version 0x20.

enum class EntryType : int32_t { Changelog = 0, Copy = 1, Placeholder = 2 };

// Flags of a changelog entry: what the game's own file is called.
enum Attributes : uint32_t { HasZsExtension = 1, HasMcExtension = 2, IsProductFile = 4 };

// One file of the mod: a changelog to merge with the game's copy, or a file to copy whole.
struct Entry {
    std::string canonical;  // the game's name for it, without ".zs" ("Pack/Actor/X.pack")
    EntryType type = EntryType::Copy;
    uint32_t attributes = 0;
    int32_t zsDictionaryId = 0;
    std::vector<int32_t> versions;  // game versions with their own copy ("X.byml110"); empty = one file
    std::vector<std::string> archiveCanonicals;  // the game's archives that also hold this file
};

// An exefs patch: offset -> new 32-bit value, for the game executable with this build id.
struct Patch {
    std::string nsoBinaryId;
    std::map<uint32_t, uint32_t> entries;
};

struct Cheat {
    std::string name;
    std::map<std::string, std::vector<std::vector<uint32_t>>> codes;
};

struct Changelog {
    int32_t builderVersion = 0;
    int32_t gameVersion = 0;  // the game version it was made from (121 = 1.2.1)
    std::vector<Entry> files;
    std::vector<std::string> malsFiles;  // text archives ("Mals/USen.Product.110.sarc")
    std::vector<Patch> patches;
    std::vector<Cheat> cheats;
    std::vector<std::string> subSdkFiles, exeFiles;
};

struct Dependency {
    std::string name, id;
};

struct Option {
    std::string id;  // its files are under "<id>/" in the zip
    std::string name, description;
    Changelog changelog;
    int32_t priority = 0;
};

enum class GroupType : int32_t { Multi = 0, MultiRequired = 1, Single = 2, SingleRequired = 3 };

struct OptionGroup {
    std::string name, description, iconName;
    GroupType type = GroupType::Multi;
    int32_t priority = 0;
    std::vector<Option> options;
    std::vector<size_t> defaults;  // indexes of the options picked unless the user changes them
    std::vector<Dependency> dependencies;

    bool single() const { return type == GroupType::Single || type == GroupType::SingleRequired; }
    bool required() const { return type == GroupType::MultiRequired || type == GroupType::SingleRequired; }
};

struct Package {
    std::string id, name, description, version, author;
    Changelog changelog;  // the mod's own files (under "romfs/", "exefs/"…)
    std::vector<std::pair<std::string, std::string>> contributors;  // name, what they did
    std::vector<OptionGroup> groups;
    std::vector<Dependency> dependencies;
    size_t zipOffset = 0;  // where the zip of files starts in the package
};

// Read a package's header. Throws std::runtime_error with a reason the user can read.
Package read(std::string_view data);

// A ULID's 16 bytes as its usual 26-letter text ("01M28DPHCX…").
std::string ulidText(std::string_view bytes);

}  // namespace mm::tkcl
