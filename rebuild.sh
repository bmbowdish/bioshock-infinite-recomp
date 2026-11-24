#!/bin/bash
# Fast loop (~30 s): rebuild after a runtime change (libc2.c, sys.c, cf.c, cxxstr.c, ...) and run the game.
source "$(dirname "$0")/_common.sh"
build || exit 1
run_game 4
