# Testing Patchwork

## Automated (on a Mac or Linux PC)

1. **Unit tests** for the install engine, archive handling, layout detection, version
   detection and GameBanana parsing:
   ```bash
   cmake -S . -B build && cmake --build build && ./build/mmtests
   ```
   Configure with `-DCMAKE_CXX_FLAGS=-fsanitize=address` to catch memory errors.
2. **Scripted UI runs** of the desktop build against live GameBanana, with screenshots:
   ```bash
   tools/qa-run.sh browse /tmp/fake-sd "w8000 kright w1500 kdown kdown s01 ka w3000 s02 q"
   ```
   Build the desktop app with AddressSanitizer too (`build-asan`) and run the same scripts.

### Scenarios covered before each release
| Area | Scenario |
|---|---|
| Browse | Scroll 100+ rows across page loads; every sort order; press Y twice quickly; switch game with ZL/ZR from list and sidebar |
| Browse | Read a full description (→, ↓, B back); preview survives install/back |
| Browse | No internet: error + "Try again"; WebP thumbnails |
| Install | Single-file zip; multi-option 7z (pre-selects the option for your game version); "All versions" file recommended; replace an installed version (Replace / Keep both) |
| Install | BCML `.bnp` refused with explanation; cancel a big download; navigation/ZL/ZR/+ blocked while busy |
| Install | Failed unpack keeps the download; finish it from My mods; files and temp folders cleaned up |
| My mods | On/off, move up/down, remove (dialog), import files already on SD, stop managing |
| Settings | NSFW toggle persists; delete unfinished downloads; re-copy mods |
| Restart | Library, settings and game choice survive a restart |
| Waikuteru | Source switch (L/R); BOTW list = 12 Switch mods, TOTK = 5; preview image, credit; Wii U file refused; install a small mod |
| Add mods | Inbox: install a zip, a non-mod zip (clear message), clash warning with installed mods; delete a file |
| Add mods | Link page: GET page, reject non-links, GameBanana page link, wrong-game link message, direct file link; B stops the server |

## On the Switch (can't be automated here)

Run these after any release build; they cover what the desktop build can't.

1. **Launch** from the Homebrew Menu opened by holding R over a game (full memory). Sidebar
   shows your real TOTK/BOTW versions.
2. **Secure connection:** Browse loads (the app now verifies GameBanana's certificate using
   its bundled certificate list). If you see "secure connection failed", check System
   Settings → Date and Time.
3. **Hold** ↓ on the Browse list for a few seconds: smooth scrolling, no crash, thumbnails
   appear as you go.
4. **Install** a small mod (Unbreakable Equipment, ~1.4 MB): progress bars for download and
   unpack; "Installed". Start the game: weapons don't break.
5. **Turn it off** in My mods, start the game: weapons break again.
6. **Large mod** (Wolf Link, ~116 MB): watch memory/speed; cancel once with B, then install
   fully. Remove it afterwards.
7. **Files already on SD** (BOTW randomizer): My mods shows the entry; import only if you
   have the free space it lists.
8. **Send a link from your phone:** Add mods → Send a link. Scan the QR code (or type the
   address) on a phone on the same Wi-Fi; paste a GameBanana or Waikuteru link; the Switch
   shows "Link received". Press B and check the address stops responding.
9. **Waikuteru:** Browse → R. Install No Fall Damage (BOTW) and check it in-game.
10. **Power/HOME during a copy** (optional, on a test mod): reopen the app; Settings →
   Re-copy enabled mods; nothing half-installed remains.
11. **Touch:** tap the sidebar, the GameBanana/Waikuteru tabs, and a mod (first tap selects,
   second tap installs). Turn a mod off by tapping it twice in My mods; taps elsewhere are
   ignored while files copy.
12. **Background add-ons:** Settings → Background add-ons lists your sysmodules with their real
   on/off state. Turn one off, restart: it no longer runs. Turn it back on, restart: it runs.
13. **Crash notice:** after Atmosphère shows the 010041544D530000 error on a game launch,
   reopen Patchwork: it explains the cause and opens Settings. It doesn't appear again for
   the same crash.
14. **Mod number:** Add mods → Enter a mod number → type a number from a GameBanana link for
   your game: the install screen opens. Also type the number into Search. A number for the
   other game says to switch game; a made-up number says there's no such mod.
15. **Hidden NSFW setting:** Settings shows no Browsing section. Hold L + R + − for 3 seconds:
   "Hide NSFW mods" appears. Turn it off, restart Patchwork: it's hidden and on again.
16. **Size table warning (TOTK):** install a mod whose files include
   `ResourceSizeTable.Product.<version>.rsizetable.zs` but not your version's (e.g. mod
   455219 on 1.2.1): a warning names the versions it supports; Cancel is the default.
17. **PC-only mods hidden:** BOTW Browse doesn't list mods whose only files are .bnp (e.g.
   FastSwim, 372295). The first visit to a page takes a moment longer; later visits are
   instant (switch/patchwork/pc-only-cache.json). Entering 372295 as a mod number explains
   that it needs BCML on a PC.
18. **BOTW size table (needed for BCML packages):** hold **L and R** while starting Breath of
   the Wild, then start Patchwork from the Homebrew Menu. Settings → Storage → "BOTW size
   table" should say "Read from game 1.6.0" (a bar shows while it reads). Holding only R gives
   "Got a mod's copy · hold L too" when a mod (e.g. the Randomizer) has its own table. If it
   says "Unreadable" or "Couldn't check", report it.
   Opening Patchwork from the Album afterwards keeps the saved copy.
19. **BCML package that installs as-is:** enter mod number 586246 (Unofficial Linkle 3.0
   Armor Icons). Pick the **.bnp** file (not the UKMM zip): it shows "BCML package · installs
   here". Install, start the game, open the inventory's Armor tab: the armor icons show
   Linkle instead of Link. Turn the mod off: the icons are back to normal.
20. **BCML package that needs merging** (e.g. FastSwim, 372295): not listed in Browse; the
   number says it needs BCML on a PC.
21. **Size table merging with the Randomizer:** with your BOTW Randomizer files on the card,
   install a BCML package: the game still starts and the Randomizer works. Turn the package
   off: the Randomizer's own size table is restored.
22. **Save file (TOTK):** close TOTK first (HOME, X). Install Shrines Unlocked Save File
   (number 445614). Pick your profile; it suggests the first empty slot. Leave photos off,
   press "Back up and install". Start TOTK, Load: the save is there with the shrines unlocked,
   and your own saves and photos are untouched.
23. **Save backups:** Settings → Save backups lists the backup from step 22. Restore it: the
   installed save is gone and your saves are as before; a second backup ("Before restoring…")
   appears. X on a backup deletes it.
24. **Game open:** with TOTK suspended in the background, try installing a save: Patchwork asks
   you to close the game first and changes nothing.
25. **Photos toggle:** install a save with "Also copy the save's photos" on (e.g. 445614 again,
   into another slot): the in-game album shows the save's photos. Restore the backup to get
   yours back.
26. **Whole-save downloads:** a download with all six slots (e.g. 447106) lists them with their
   dates and highlights the latest. A BOTW Master Mode save (e.g. 582966's master mode file)
   only offers Master Mode slots; check it loads in Master Mode.

27. **BCML packages with nothing to copy:** packages that only change merged data install:
   99 slots for all weapons (305388, game data) and Early Travel Medallion (448012, map). An
   earlier test build's Settings → For testing section is gone; opening the new build removes
   its merged Linkle title screen from the game if it was still there.
28. **BCML setup, as a new user would meet it:** open Patchwork from the Album (no buttons
   held). Settings starts with "BCML packages": Game files "Not read yet". Enter mod number
   423509 ([LINKLE] Title Screen NX): its .bnp says "BCML package · one-time setup". Install it:
   a blue "One-time setup for BCML packages" screen lists the steps. Follow them (hold L and R
   starting BOTW, open Patchwork): a message offers "Open My mods"; choose Finish installing.
   Start BOTW normally: the title screen shows Linkle. Turn it off: the normal one is back.
29. **Game files kept:** after step 28, open Patchwork from the Album (no buttons held) and
   install another merging package (e.g. 321704 Early Master Cycle): it installs without asking
   for L and R if it only needs files already read; otherwise it says which files and to
   relaunch with L and R.
30. **Text, actor info, shops (armor mods):** install a Linkle armor package that changes
   texts and shops (e.g. 340011 LL Upgradeable Armor Rebalance). In the game, check an armor
   name/description and a shop that sells it. If the game crashes at load, report it with the
   mod list.
31. **Maps (DLC or not):** install a package with map changes (e.g. 358261 Crafting Project).
   Check that the game loads into the overworld, and note whether you have the DLC.
32. **Order and turning off:** with two merging packages on, move one up/down in My mods and
   turn one off: each change re-merges (the progress screen shows it), and the game still loads.

