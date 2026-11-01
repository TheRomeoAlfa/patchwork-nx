# TKMM merging on the console (work in progress)

Goal: Tears of the Kingdom mods merge on the Switch the way TKMM merges them on a PC, so
`.tkcl` packages install, and plain romfs mods that change the same files combine instead
of the higher one winning. The engine follows TkSharp (MIT, TKMM-Team/TkSharp); TKMM's BYML
library is AGPL, so Patchwork's BYML code is its own, written from the format.

## How TKMM merges

- Every mod is a **changelog**: per file, either a diff against the game's copy (BYML,
  packs, SARCs, MSBT, RSDB tables, game data, combined BNTX) or a whole file to copy.
  `.tkcl` packages carry changelogs already; plain romfs mods get one by comparing each file
  with the game's (TkChangelogBuilder).
- The merge takes the enabled mods lowest priority first and, per game file, applies every
  mod's changelog to the game's copy. Files inside `Pack/**.pack.zs` are written by
  rebuilding the whole pack. Then the AI heap size (HeapDef) and the size table (RESTBL).
- Game version: `System/RegionLangMask.txt`. Real file names (versioned, `.zs`) come from
  the address table and the FileEntry tables. Each `.zs` file names its zstd dictionary
  (from `Pack/ZsDic.pack.zs`); output is compressed with the mod file's dictionary, level 7.
- TkSharp ships an index of which pack holds each game file (0.8 MB, needed) and checksums
  of every game file (13 MB; Patchwork can compare with the game itself instead).

## Phases

1. **Done**: `.tkcl` reader (`source/core/tkcl.*`; 32 of 38 real packages read, the rest are
   TKMM 2.0 beta packages TKMM also refuses), zstd with the game's dictionaries (`zstd.*`).
2. **Done**: game version, real file names, pack index (`totk.*`), reading files inside
   packs (with a small cache), BYML v7 (hash maps, aligned binary, TKMM's changelog nodes
   0xFD/0xFE; all 4,779 BYMLs in the real packages round-trip). Settings > "Copy game files
   for testing" (TOTK, opened from the game) copies the files below to
   `switch/patchwork/totk-game-files`. Checked on the user's TOTK 1.2.1 copy: all 991 `.zs`
   files decompress, all 28,525 files in 957 actor packs are in the index (the ones indexed
   to their own pack read back identically), all 170 BYML tables round-trip. GameDataList is
   big-endian; BYML keeps each file's byte order.
3. **First installs**: `.tkcl` into My mods with its options (as BCML packages' groups);
   merge whole files, placeholders, packs, exefs patches, cheats, the size table. Many
   armor and model packages work from here. First test on the Switch.
4. **Data merges**: BYML (with TKMM's array key tables), RSDB tags and rows, game data
   (with save data sizes), AI heap.
5. **Text and textures**: Mals/MSBT, combined BNTX.
6. **Plain romfs mods**: build changelogs on the console by comparing with the game, so
   ordinary TOTK mods that change the same files merge (Linkle 3.0 + a standalone armor).
7. **Maybe**: BFRES `.mc` material cleanup (TKMM's BfresMc), memory permitting.

## Game files for tests

From the user's own game (the copy option, `totk::testFilePatterns()`): `System/`, the zstd
dictionaries, `Pack/Bootup*`, `Pack/AI.Global*`, `Pack/Actor/`, the FileEntry tables,
the effect and event flow tables, `RSDB/`, `GameData/` and `Mals/USen*`.

## Memory

GameDataList is 12.7 MB of BYML; parsing, writing and parsing it again peaks at 665 MB on
the desktop. Before merging it (phase 4): a compact node layout, and hashed dedupe in the
writer instead of a text key per node.

TkSharp keeps every rebuilt pack in memory until the end and decompresses a whole pack for
each nested file. Here: one pack at a time, spilled to the SD card, with a small cache of
decompressed packs; no parallel merge in Album mode.
