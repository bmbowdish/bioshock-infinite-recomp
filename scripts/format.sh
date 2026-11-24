#!/bin/bash
# Claude Fable - 2026-11-24: format the C / Objective-C sources with clang-format (settings in .clang-format).
# Run: scripts/format.sh [check|apply|compare]   check (default) only reports the tool; apply formats the files and records
# the compiled code of each object file; compare (after rebuilding) reports any object whose machine code changed.
source "$(dirname "$0")/_common.sh"
mkdir -p logs; exec > logs/format_out.txt 2>&1
CF=$(xcrun --find clang-format 2>/dev/null); [ -x "$CF" ] || CF=$(command -v clang-format); [ -x "$CF" ] || CF=/opt/homebrew/bin/clang-format
if [ ! -x "$CF" ]; then echo "clang-format: NOT FOUND"; ls /opt/homebrew/opt/llvm/bin/clang-format 2>&1; exit 1; fi
echo "clang-format: $CF"; "$CF" --version
FILES="game/game32.c $(ls mac32/runtime/*.c mac32/runtime/*.m mac32/runtime/*.h mac32/template/*.c 2>/dev/null)"
hashes() { for o in build/m32/*.o build/game/*.o; do [ -f "$o" ] && echo "$(otool -t "$o" | tail -n +2 | shasum | cut -c1-16) $(basename "$o")"; done; }
MODE="${1:-$(cat format_mode 2>/dev/null)}"
if [ "$MODE" = "apply" ]; then
  hashes > logs/format_hash_before.txt
  "$CF" -i --style=file $FILES; echo "formatted: $(echo $FILES | wc -w) files, exit $?"
elif [ "$MODE" = "compare" ]; then
  hashes > logs/format_hash_after.txt
  echo "object files whose machine code changed:"; diff logs/format_hash_before.txt logs/format_hash_after.txt | grep '^>' || echo "  none"
  echo "objects compared: $(wc -l < logs/format_hash_after.txt)"
else echo "check only"; fi
