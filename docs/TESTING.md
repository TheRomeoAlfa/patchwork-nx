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
