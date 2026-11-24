#!/bin/bash
# Shared helpers for the build and run scripts (sourced, not run directly).
# Claude Fable - 2026-11-24: everything is relative to where this repository was cloned (it used to assume ~/Downloads).
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$PROJ"   # the mac32 recompiler + runtime live in ./mac32
cd "$PROJ" || { echo "project folder not found: $PROJ"; exit 1; }
# The user's own copy of the game. setup.sh makes orig.bin a link to its executable; everything else is found from
# that link, so a copy outside the default Steam folder works too.
GAME="$HOME/Library/Application Support/Steam/steamapps/common/BioShock Infinite/BioShock3.app/Contents"
if [ -L orig.bin ]; then
  _exe="$(readlink orig.bin)"; [ -d "$(dirname "$_exe")/../Resources" ] && GAME="$(cd "$(dirname "$_exe")/.." && pwd)"
fi
[ -f .venv/bin/activate ] || { echo "Python environment missing: run ./setup.sh first"; exit 1; }
source .venv/bin/activate

# Claude Fable - 2026-11-24: the port only fits one exact build of the game (see GAME_EXE_SHA256 in game32.c). Refuse to
# translate any other executable: it would produce a program that starts and then fails in confusing ways.
check_game_binary() {
  local want got
  want=$(grep '#define GAME_EXE_SHA256' game32.c | grep -o '[0-9a-f]\{64\}')
  [ -n "$want" ] || { echo "version check: GAME_EXE_SHA256 not found in game32.c"; return 1; }
  [ -r orig.bin ] || { echo "version check: orig.bin is missing (it should point at the game's executable, Contents/MacOS/BioShock3)"; return 1; }
  got=$(shasum -a 256 orig.bin | cut -d' ' -f1)
  if [ "$got" = "$want" ]; then echo "version check: game executable matches ($(echo "$got" | cut -c1-16)...)"; return 0; fi
  echo "version check FAILED: orig.bin is not the build this port was made for"
  echo "  expected sha256 $want"
  echo "  found    sha256 $got"
  return 1
}

build() {
  make -k -j"$(sysctl -n hw.ncpu)" app > build.log 2>&1
  local rc=$?
  local errs
  errs=$(grep -c "error:" build.log)
  echo "build exit: $rc, errors: $errs"
  if [ "$errs" -gt 0 ] || [ ! -x build/BioShock3 ]; then
    grep "error:" build.log | head -5 | cut -c1-220
    return 1
  fi
}

run_game() {
  [ -s loop_env ] && export $(cat loop_env)   # optional: a file named loop_env with VAR=value settings for this run (RT32_HUD=1 ...)
  RT32_STRICT=1 ./build/BioShock3 orig.bin "$GAME/Resources" > run_out.txt 2>&1 &
  local pid=$!
  if [ -n "$RUN_TIMEOUT" ]; then   # RUN_TIMEOUT=N stops the game after N seconds (for unattended test runs)
    ( sleep "$RUN_TIMEOUT"; kill "$pid" 2>/dev/null ) &
  fi
  # Profiling: create a file named sample_now while the game runs to take a 5 s CPU sample into host_sample.txt
  ( while kill -0 "$pid" 2>/dev/null; do
      if [ -f sample_now ]; then : > sample_now; mv sample_now sample_now.done 2>/dev/null
        /usr/bin/sample "$pid" 5 -file host_sample.txt >/dev/null 2>&1; echo "sampled $(date +%T)" > sample_status.txt; fi
      sleep 1; done ) &
  wait "$pid"
  local rc=$?
  mkdir -p logs && cp run_out.txt "logs/run_$(date +%Y%m%d_%H%M%S)_$(basename "$0" .sh).txt" 2>/dev/null   # keep every run's log
  echo "run exit: $rc"
  tail -"${1:-4}" run_out.txt | cut -c1-250
}
