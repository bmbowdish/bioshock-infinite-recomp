#!/bin/bash
# Claude Fable - 2026-11-24: translate your copy of the game and build the app, in one step (run ./setup.sh first).
# Takes several minutes and about 3 GB of disk space, all inside this folder. Result:
#   build/BioShockInfiniteRecomp.app
# The app contains code translated from the game, so it is for your own use: do not redistribute it.
source "$(dirname "$0")/_common.sh"
check_game_binary || { exit 1; }
echo "1/3 translating the game's program to C (a few minutes)..."
rm -rf gen2 build/gen2
RECOMP_EXTRA_STARTS=starts.txt python3 "$FW/mac32/tools/recomp32.py" orig.bin 2cd0 13ec120 gen2 --hle hle.txt 2>&1 | grep -v "^UNSUPPORTED" | tail -2
[ -f gen2/tables.c ] || { echo "translation failed: gen2/tables.c was not produced"; exit 1; }
echo "2/3 compiling (a few minutes)..."
build || { echo "build failed: see build.log"; exit 1; }
echo "3/3 making the app..."
bash "$PROJ/bundle.sh" | tail -4
[ -d build/BioShockInfiniteRecomp.app ] || { echo "the app was not produced: see bundle.log"; exit 1; }
echo
echo "Done: $PROJ/build/BioShockInfiniteRecomp.app"
echo "Open it from Finder (the first time: right-click, then Open). In the game: fn+F12 shows the performance overlay."
