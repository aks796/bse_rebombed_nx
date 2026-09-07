<div align="center">

<img src="resources/icon.jpg" alt="Explodinary" width="160">

# bse_rebombed_nx

**BombSquad Explodinary on Nintendo Switch**

An unofficial Nintendo Switch wrapper for the Android version of
**BombSquad Explodinary**.

[![Switch](https://img.shields.io/badge/Nintendo_Switch-Homebrew-E60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)](#)
[![Version](https://img.shields.io/badge/Version-1.0.0-4C8BF5?style=for-the-badge)](#)
[![Ballistica](https://img.shields.io/badge/Ballistica-Engine-F5A623?style=for-the-badge)](#)

</div>

---

## About

`bse_rebombed_nx` is a native wrapper that runs the ARM64 Android build of
**BombSquad Explodinary** on Nintendo Switch. It recreates the Android,
Bionic, audio, input, networking and graphics services expected by the game
under Horizon OS.

Explodinary ships its own engine build rather than the stock one. It reports
the same 1.7.62 version string as stock BombSquad but a lower build number,
22824 against 22837, and that number is what tells the two apart. The loader
checks it before it maps anything executable, so a stock BombSquad library
stops with a clear message instead of misbehaving later. Use `bombsquad_nx`
for stock BombSquad; the two ports do not share a game folder.

The repository does not include the game, APK, libraries or assets. You must
provide your own legally obtained compatible copy.

---

## Controls

The four face buttons change meaning with the screen you are on. In menus, on
the pause screen and on the join screen they act as the letters printed on
them. Once a round is being played they switch to the positional layout other
gamepads use, so jump sits under the thumb it normally does.

| Input | Menus, pause and join | In a match |
| --- | --- | --- |
| **Left Stick** | Navigate | Move |
| **A** | Confirm | Bomb |
| **B** | Back | Jump |
| **X** | — | Pick up |
| **Y** | — | Punch |
| **L / R / ZL / ZR** | — | Run |
| **+ / –** | Back out | Pause |
| **L + R + – + +** | Quit to the Homebrew Menu | Quit to the Homebrew Menu |

Both shoulder rows run, and so do SL and SR on a single Joy-Con: everything
under an index finger reports as the same pair of buttons the game binds
running to.

A Joy-Con pair splits into two players by default, and each half is read held
sideways with SL and SR under the index fingers. The four buttons on a single
Joy-Con are read by their position rather than the letter printed on them, so
both halves put the same action under the same thumb.

Set `face_buttons` in `bse_rebombed_nx.cfg` to `labels`, `default` or `xbox` to
pin one layout instead. Do not use the game's own controller configuration
screen while the automatic layout is on, since that screen is a menu and would
record the wrong bindings.

---

## Build

### Requirements

* devkitPro
* devkitA64 and libnx
* Switch Mesa and libdrm_nouveau
* Switch SDL2, FreeType, HarfBuzz, libpng, bzip2 and zlib
* GNU Make
* Python 3 for the import-table generator

Install the required devkitPro packages:

```bash
pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-sdl2 switch-freetype switch-harfbuzz switch-libpng switch-bzip2 switch-zlib
```

Compile the wrapper:

```bash
cd bse_rebombed_nx
make -j
```

For a clean rebuild:

```bash
make clean
make -j
```

The game library imports 636 symbols, the same set stock BombSquad imports.
`tools/verify_imports.py` checks the binding table against a copy of
`libmain.so` and fails if any of them is unbound:

```bash
python3 tools/verify_imports.py /path/to/libmain.so source/imports.c
```

---

## Running

Explodinary is released by the Square Hair Team on Game Jolt:

[gamejolt.com/games/bse-rebombed/1001581](https://gamejolt.com/games/bse-rebombed/1001581)

Take the Android build from there. Create this folder on the SD card and drop
the APK into it:

```text
sd:/switch/bse_rebombed_nx/
├── bse_rebombed_nx.nro
└── BSE_Rebombed.apk
```

The first launch unpacks the library and the game data out of the APK, checks
every file against the checksum the archive recorded for it, and then deletes
the APK. Explodinary carries about four thousand asset files, so give it a
couple of minutes; it shows its progress on screen. Set `keep_apk = 1` in
`bse_rebombed_nx.cfg` to hold on to the archive instead.

Afterwards the folder looks like this:

```text
sd:/switch/bse_rebombed_nx/
├── bse_rebombed_nx.nro
├── bse_rebombed_nx.cfg
├── libmain.so
├── trace.txt
└── no_backup/
    └── ballistica_files/
```

The file name does not matter as long as it ends in `.apk`. An APK you
extracted yourself is also accepted: put it in the same folder so that
`lib/arm64-v8a/libmain.so` and `assets/` are present, and the first launch
will move them into place.

Launch the NRO through title override for full application memory: hold **R**
while opening an installed game, then start **BombSquad: Explodinary
Rebombed** from the Homebrew Menu. Starting the Homebrew Menu from the Album
applet will not work, and the port says so rather than failing silently.

Settings live in `bse_rebombed_nx.cfg`, which is written on the first launch and
explains each option in place.

---

## Status

Gameplay, audio, account login, online play, LAN play, BombSquad Remote,
touchscreen and controller input are working. A Pro Controller, a Joy-Con pair
and single Joy-Cons are all supported, and the console waking from sleep
rebuilds the sockets the network stack loses.

The first launch after an install is the slowest one, since the game
byte-compiles its Python library and fills its caches as it goes. Later
launches skip that work.

Explodinary's engine does not export the entry point stock BombSquad uses to
be told the network came back. Nothing depends on it here, so it is looked up
and skipped when absent rather than treated as a missing requirement.

The port also corrects one bug in Explodinary's own scripts on every launch,
before the engine reads them. Both shop buttons close the selector's window
and then ask the UI system to navigate away from it a tenth of a second
later, which leaves the menu with no window in it at all: every widget still
lights up and none of them do anything. The corrected lines are named in
`trace.txt`, and re-unpacking the APK does not bring the bug back.

The wrapper is built specifically for the Explodinary release carrying engine
build **22824**. Libraries and assets from another release, stock BombSquad
included, are not supported.

---

## Credits

**Explodinary Nintendo Switch port** — aks796

**BombSquad Explodinary** — Square Hair Team

**BombSquad PS Vita port** — SpliffCurryBeats,
[gitlab.com/sexcurrybeats/bombsquad-vita](https://gitlab.com/sexcurrybeats/bombsquad-vita)

The loader and compatibility layer derive from the open-source Switch `.so`
loader work by Andy Nguyen and fgsfds, building on TheOfficialFloW's Vita and
Switch loader work. The inherited wrapper code is MIT-licensed. JNI interface
declarations come from the Android Open Source Project under the Apache
License 2.0. See `THIRD_PARTY_NOTICES.md` for the full list.

**BombSquad** was created by Eric Froemling.

---

## Contributing

Bug reports and tested improvements are welcome. Include the build version,
steps to reproduce and the relevant `trace.txt` when reporting an issue. Set
`log_input = 1` in `bse_rebombed_nx.cfg` first if the problem is a controller
mapping.

---

## Disclaimer

This is an unofficial fan project and is not affiliated with, sponsored by or
endorsed by Eric Froemling or the Square Hair Team. BombSquad and all related
artwork, audio, trademarks and game assets belong to their respective owners.

This repository contains only the compatibility code required by the Nintendo
Switch port and does not distribute proprietary game files.
