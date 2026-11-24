# BioShock Infinite (2013 Mac build) on Apple Silicon — static recompilation

A port of the 32-bit Intel Mac build of BioShock Infinite to arm64 macOS by static recompilation: the original
program is translated to C, compiled natively, and linked against a runtime that stands in for the 2013 system
libraries (Carbon, CoreFoundation handles, OpenGL, Bink, FMOD, Steam).

**This repository contains no game code or data.** You need your own installed copy of the game (Steam).
Everything derived from the game binary (`orig*.bin`, `gen*/`, shaders, builds) is git-ignored.

## Layout
| Path | What it is |
|---|---|
| `mac32/` | The recompiler and runtime (from NightSightProductions' Recomp-Framework `mac32` target, MIT, see `mac32/LICENSE`), with this project's changes. `tools/recomp32.py` translates i386 to C; `runtime/` is the host side. |
| `game32.c` | Game-specific glue: engine start-up, resolution/fullscreen hooks, texture pool. |
| `starts.txt` | Extra function-start addresses for the recompiler. Bad entries here cause real bugs: see `STARTS_AUDIT.md`. |
| `hle.txt` | Functions replaced by host implementations. |
| `Makefile`, `*.sh` | Build and run scripts. `autoloop.sh` watches the `trigger` file and runs the named script. |

## Build
The scripts expect the project at `~/Downloads/bioshock-infinite-recomp` and the game installed through Steam.
`echo rebuild > trigger` builds and runs for 60 s; `build_only` compiles only; `freeplay` runs until you quit;
`bundle` makes `build/BioShockInfiniteRecomp.app` and a `.zip` of it; `regen` re-translates the game (slow).

## Controls added by the port
fn+F12 performance overlay · fn+F9 MetalFX upscaling on/off (active when the game renders below the window size).
