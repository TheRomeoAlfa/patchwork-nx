# TKMM merging on the console (experimental)

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
3. **Done** (BYML moved here from phase 4, since nearly every package has BYML changes):
   - BYML merging and changelogs (`byml_merge.*`). Random edits on the game's own files,
     made into changelogs and merged back, match 99%; the rest are lists TKMM itself can't
     tell apart.
   - The size table (`restbl.*`; the game's table writes back byte for byte).
   - The merge engine (`tkmm.*`): whole files, SARC and pack merging (TOTK's 959 packs
     rewrite byte for byte), code patches with TKMM's shop default, cheats, the AI heap.
   - Library integration: enabled packages merge into `tkmm-merged`, deployed like BCML's.
   - Installer and picker: a `.tkcl`, or an archive with one inside, with its options.
   - Packages needing phase 4 or 5 are refused with the reason.
   - Desktop QA: Frostbite Set Linkle Edition installs, merges, turns off cleanly.
   - Not yet tried on a Switch.
4. **Done**: RSDB row tables (by key), the RSDB tag table (bit table rebuilt), game data
   (rows by hash, with the save data layout worked out again) in `tkmm_data.*`. Merging the
   game's own files with no changes gives them back exactly. Unlike TKMM, tables the game
   keeps in its own order (TagDef, GameSafetySetting) aren't re-sorted.
5. **Done (text)**: Mals text archives, in the console's language (TKMM's pick: same
   language and region, else same language, else English); messages replaced whole. TOTK's
   1,510 US English message files round-trip exactly. **Not done: combined textures**
   (`*__Combined.bntx`, BNTX rebuilt with changed textures): none of the 13 packages tried
   needs it (their `.bntx` files are whole icons, copied), and there's no game file to check a
   BNTX writer against; packages that need it are still refused with the reason.
6. **Done (plain romfs mods)**: ordinary TOTK mods merge too (Linkle 3.0 + a standalone
   armor), in `tkmm_build.*` (TKMM's names for files, and changelogs made by comparing a mod's
   files with the game's: BYML, RSDB tables, game data, text, SARCs, the files in packs).
   - Only files two or more enabled mods change are merged (a size table counts, and so do
     files TKMM packages change); the rest go on the SD card as they are, with their sizes
     (and those of the files in their packs) put in the merged size table.
   - When the game's files can't be read, the higher mod wins as before, and My mods says why.
   - Released in 0.4.0 behind Settings → "Merge mods that change the same files" (off by
     default: not yet tried in the game on a Switch); packages always merge.
   - Checked on the user's game: two mods changing an armor pack, two RSDB tables, game data
     and text in different places merge to both changes and nothing else; the app's merge on
     the card is identical. Comparing long lists was quadratic: 7.7 s to 0.4 s per mod.
7. **Maybe**: BFRES `.mc` material cleanup (TKMM's BfresMc), memory permitting.

## Game files for tests

From the user's own game (the copy option, `totk::testFilePatterns()`): `System/`, the zstd
dictionaries, `Pack/Bootup*`, `Pack/AI.Global*`, the armor, weapon, horse and Lynel actor
packs, the FileEntry tables,
the effect and event flow tables, `RSDB/`, `GameData/` and `Mals/USen*`.

## Memory

GameDataList is 12.7 MB of BYML; parsing, writing and parsing it again peaks at 665 MB on
the desktop. Before merging it (phase 4): a compact node layout, and hashed dedupe in the
writer instead of a text key per node.

TkSharp keeps every rebuilt pack in memory until the end and decompresses a whole pack for
each nested file. Here: one pack at a time, spilled to the SD card, with a small cache of
decompressed packs; no parallel merge in Album mode.
