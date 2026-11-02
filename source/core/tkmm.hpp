#pragma once

#include <functional>
#include <string>
#include <vector>

#include "tkcl.hpp"
#include "totk.hpp"
#include "util.hpp"

namespace mm::tkmm {

// TKMM's merge, after TkSharp's TkMerger (MIT): every enabled mod's changelogs applied to the
// game's files, lowest priority first. Writes, under the output folder:
//   romfs/…                     merged and copied files, rebuilt packs, the size table
//   exefs/subsdk1…, exefs/…     code modules and other exefs files
//   exefs_patches/<BUILD>.ips   the mods' code patches for this game version (with TKMM's
//                               default: room for 512 items in shops)
//   cheats/<build>.txt          cheats

// One changelog: a package's own files or one of its chosen options.
struct Input {
    std::string modId;
    fs::path root;  // its files: "romfs/…", "exefs/…"
    const tkcl::Changelog* changelog = nullptr;
};

struct Result {
    std::vector<std::string> files;     // written, relative to the output folder
    std::vector<std::string> warnings;  // changes that couldn't be applied
};

using Progress = std::function<void(float done, const std::string& what)>;

// `language`: the game's text language, as its text archives name it ("USen", "EUfr"…).
// Throws std::runtime_error (naming the file) when a changelog can't be applied.
Result merge(const std::vector<Input>& lowestFirst, totk::Rom& rom, const fs::path& outDir, const Progress& progress = {},
             const std::string& language = "USen");

// The files of a changelog Patchwork can't merge yet (combined textures): a package with any
// isn't installed.
std::vector<std::string> unsupportedFiles(const tkcl::Changelog& changelog);

// Where an entry's file is in its folder: "romfs/<name>", or for files with a copy per game
// version, the one for this game ("romfs/…Product.byml120").
std::string inputPath(const tkcl::Entry& entry, const totk::Rom& rom);

}  // namespace mm::tkmm
