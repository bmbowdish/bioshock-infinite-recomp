#!/bin/bash
# Full loop (~3 min): regenerate the C from the game binary (after changing the translator or starts.txt), rebuild, run.
source "$(dirname "$0")/_common.sh"
check_game_binary || { exit 1; }
rm -rf gen2
RECOMP_EXTRA_STARTS=starts.txt python3 "$FW/mac32/tools/recomp32.py" orig.bin 2cd0 13ec120 gen2 --hle hle.txt 2>&1 | grep -v "^UNSUPPORTED" | tail -2
echo "gdefs entries: $(grep -c gdefs gen2/tables.c)   missing_func sites: $(cat gen2/*.c | grep -c missing_func)   (expect 2 and 0-1)"
rm -rf build/gen2
build || exit 1
run_game 4
