# BioShock Infinite (2013 Mac build) on Apple Silicon

A port of the 32-bit Intel Mac release of BioShock Infinite to Apple Silicon Macs by **static recompilation**:
the game's original program is translated to C on your Mac, compiled natively, and linked against a runtime that
stands in for the 2013 system libraries it expects (Carbon, OpenGL, Bink video, FMOD, Steam).

**This repository contains no game code and no game data.** You need your own copy of the game, and the
translation happens on your machine from that copy. This is an unofficial fan project, not affiliated with or
endorsed by the game's developers or publishers.

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

The translated code, `starts.txt`, `hle.txt` and the addresses in `game32.c` are positions inside that file, so they
are the same for everyone who has that build and wrong for anyone who has another (a different store's version or
a different patch). `setup.sh` and `build_app.sh` check your copy, and the app checks again at launch and says why
if it will not start. To check by hand:

    shasum -a 256 ~/Library/Application\ Support/Steam/steamapps/common/BioShock\ Infinite/BioShock3.app/Contents/MacOS/BioShock3

## In the game
- **Resolution and Fullscreen** are set in the game's own graphics menu; HiDPI displays offer their full pixel sizes.
- **Upscaling:** when the game renders below the window's size, the picture is enlarged with Apple's MetalFX.
  **fn+F9** switches to a plain stretch for comparison.
- **fn+F12** shows a performance overlay (frame rate, resolution, memory).
- The log is written to `~/Library/Logs/BioShock3-recomp.log`.

## Known limitations
- Occasional short hitches, mostly while shaders compile or a level loads.
- The game's volume sliders do not affect video sound.
- Tested on a small number of Macs. Reports are welcome.

## Layout
| Path | What it is |
|---|---|
| `mac32/` | The recompiler and runtime, from NightSightProductions' Recomp-Framework `mac32` target (MIT, see `mac32/LICENSE`), with this project's changes. `tools/recomp32.py` translates i386 to C; `runtime/` is the host side. |
| `game32.c` | Game-specific glue: engine start-up, resolution and fullscreen hooks, texture pool, supported build. |
| `starts.txt` | Extra function-start addresses for the translator. Bad entries cause real bugs: see `STARTS_AUDIT.md`. |
| `hle.txt` | Functions replaced by host implementations. |
| `setup.sh`, `build_app.sh` | The two commands above. |
| other `*.sh` | Development loop: `autoloop.sh` watches a `trigger` file and runs the named script (`rebuild`, `regen`, `build_only`, `freeplay`, `bundle`, `format`). |

## Licence
The port's own source is under the MIT licence (`LICENSE`). `mac32/` is derived from Recomp-Framework, also MIT, and
keeps its original notice in `mac32/LICENSE`; `mac32/third_party/stb_vorbis.c` carries its own terms in the file.
The licence covers this repository only. It grants no rights to BioShock Infinite itself, and the app you build
contains code translated from the game, which is not covered and must not be redistributed.

## Credits
Built on [Recomp-Framework](https://github.com/NightSightProductions/Recomp-Framework) by NightSightProductions.
BioShock Infinite is the property of its respective owners.
