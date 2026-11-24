# BioShock Infinite (2013 Mac build) on Apple Silicon

A port of the 32-bit Intel Mac release of BioShock Infinite to Apple Silicon Macs by **static recompilation**:
the game's original program is translated to C on your Mac, compiled natively, and linked against a runtime that
stands in for the 2013 system libraries it expects (Carbon, OpenGL, Bink video, FMOD, Steam).

**This repository contains no game code and no game data.** You need your own copy of the game, and the
translation happens on your machine from that copy. This is an unofficial fan project, not affiliated with or
endorsed by the game's developers or publishers. **I cannot guarantee there is no malware or broken code. Use at your own risk.**

## What you need
- An Apple Silicon Mac.
- BioShock Infinite for Mac, installed through Steam (see *Supported game version*).
- Apple's command line tools: `xcode-select --install`
- Optional, for the in-game videos: [Homebrew](https://brew.sh) and `brew install ffmpeg pkg-config`.
  Without FFmpeg the game runs but its videos stay black.
- About 3 GB of free disk space for the build.

## Build it
    git clone https://github.com/bmbowdish/bioshock-infinite-recomp.git
    cd bioshock-infinite-recomp
    ./setup.sh        # checks your tools and your copy of the game; changes nothing outside this folder
    ./build_app.sh    # translates the game and builds the app (several minutes)

The result is `build/BioShockInfiniteRecomp.app`. The first time, open it with right-click, then **Open**, because it
is not signed by a registered developer. It finds your Steam copy of the game by itself.

The app you build contains code translated from the game. It is for your own use: **please do not redistribute it.**

## Supported game version
The port fits **one exact build** of the game: the Steam Mac release (Aspyr), executable
`BioShock3.app/Contents/MacOS/BioShock3`, 43,656,724 bytes, SHA-256
`ed65b2c936d8b4d92a851e7bb676cfc0e9f9e88617564263d62651c2f68ce74b`.

The translated code and the files in `game/` (`starts.txt`, `hle.txt`, the addresses in `game32.c`) are positions inside that file, so they
are the same for everyone who has that build and wrong for anyone who has another (a different store's version or
a different patch). `setup.sh` and `build_app.sh` check your copy, and the app checks again at launch and says why
if it will not start. To check by hand:

    shasum -a 256 ~/Library/Application\ Support/Steam/steamapps/common/BioShock\ Infinite/BioShock3.app/Contents/MacOS/BioShock3

## In the game
- **Resolution and Fullscreen** are set in the game's own graphics menu; HiDPI displays offer their full pixel sizes.
- **Upscaling:** when the game renders below the window's size, the picture is enlarged with Apple's MetalFX.
- The log is written to `~/Library/Logs/BioShock3-recomp.log`.

### Controllers
Xbox, PlayStation and Switch Pro controllers work, wired or over Bluetooth, through Apple's GameController
framework: connect the controller before or while the game is running and it is picked up automatically.
Tested with an Xbox controller. Rumble is not implemented yet.

### Keys added by the port
| Key | What it does |
|---|---|
| **F12** | Shows or hides the performance overlay in the top-left corner: frame rate and frame time, the resolution and whether MetalFX is upscaling, and memory use. |
| **F9** | Switches between MetalFX upscaling and a plain stretch, to compare them. Only has an effect while the game renders below the window's size. |

**On Mac keyboards, hold `fn` with these keys** (`fn`+`F12`, `fn`+`F9`).

## Known limitations
- Occasional short hitches, mostly while shaders compile or a level loads.
- The game's volume sliders do not affect video sound.
- Controllers do not rumble yet.
- Tested on a small number of Macs. Reports are welcome.

## Layout
| Path | What it is |
|---|---|
| `setup.sh`, `build_app.sh` | The two commands above. |
| `game/` | Everything specific to this game: `game32.c` (engine start-up, resolution and fullscreen hooks, texture pool, supported build), `starts.txt` (extra function-start addresses for the translator) and `hle.txt` (functions replaced by host implementations). |
| `mac32/` | The recompiler and runtime, from NightSightProductions' Recomp-Framework `mac32` target (MIT, see `mac32/LICENSE`), with this project's changes. `tools/recomp32.py` translates i386 to C; `runtime/` is the host side. |
| `scripts/` | Development scripts: `rebuild.sh` (compile and run), `regen.sh` (re-translate, compile, run), `build_only.sh`, `freeplay.sh` (run until you quit), `bundle.sh`, `format.sh`. |
| `docs/` | `STARTS_AUDIT.md`: how bad entries in `starts.txt` caused real crashes, and how the list was audited. |
| `Makefile`, `requirements.txt`, `.clang-format` | Build settings, the translator's Python packages, and the code style. |

Created when you build (never committed): `orig.bin` (a link to your game), `gen2/` (the translated game), `build/`
(the compiled app), `.venv/` (the translator's Python environment) and `logs/` (build and run output).

## Licence
The port's own source is under the MIT licence (`LICENSE`). `mac32/` is derived from Recomp-Framework, also MIT, and
keeps its original notice in `mac32/LICENSE`; `mac32/third_party/stb_vorbis.c` carries its own terms in the file.
The licence covers this repository only. It grants no rights to BioShock Infinite itself, and the app you build
contains code translated from the game, which is not covered and must not be redistributed.

## Credits
Built on [Recomp-Framework](https://github.com/NightSightProductions/Recomp-Framework) by NightSightProductions.
BioShock Infinite is the property of its respective owners.
