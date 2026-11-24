#!/bin/bash
# Shared helpers for regen.sh / rebuild.sh (sourced, not run directly).
PROJ="$HOME/Downloads/bioshock-infinite-recomp"
FW="$PROJ"   # Claude Fable - 2026-11-24: the mac32 framework is now ./mac32 in this repo (was ~/Downloads/Recomp-Framework-main)
GAME="$HOME/Library/Application Support/Steam/steamapps/common/BioShock Infinite/BioShock3.app/Contents"
cd "$PROJ" || { echo "project folder not found: $PROJ"; exit 1; }
source .venv/bin/activate

# Tell the progress widget what we're doing: stage "Building"   or   stage "Idle" 134
stage() {
  if [ -n "$2" ]; then echo "{\"stage\":\"$1\",\"t\":$(date +%s),\"exit\":$2}" > progress_stage.json
  else echo "{\"stage\":\"$1\",\"t\":$(date +%s)}" > progress_stage.json; fi
}

# Start the progress widget in the background if it isn't already running.
ensure_widget() {
  if ! curl -s -m 1 http://localhost:8765/status >/dev/null 2>&1; then
    nohup python3 progress_server.py >/dev/null 2>&1 &
    sleep 1
    echo "Progress widget started: http://localhost:8765"
  fi
}

build() {
  stage "Building"
  make -k -j"$(sysctl -n hw.ncpu)" app > build.log 2>&1
  local rc=$?
  local errs
  errs=$(grep -c "error:" build.log)
  echo "build exit: $rc, errors: $errs"
  if [ "$errs" -gt 0 ] || [ ! -x build/BioShock3 ]; then
    grep "error:" build.log | head -5 | cut -c1-220
    stage "Build failed" 1
    return 1
  fi
}

run_game() {
  stage "Running"
  [ -s loop_env ] && export $(cat loop_env)   # optional debug env vars, one per line/space-separated (written by Claude)
  RT32_STRICT=1 ./build/BioShock3 orig.bin "$GAME/Resources" > run_out.txt 2>&1 &
  local pid=$!
  if [ -n "$RUN_TIMEOUT" ]; then   # autoloop: stop a still-running game after N seconds
    ( sleep "$RUN_TIMEOUT"; kill "$pid" 2>/dev/null ) &
  fi
  # Claude can profile the running game: it creates `sample_now`; this takes a 5 s sample into host_sample.txt
  ( while kill -0 "$pid" 2>/dev/null; do
      if [ -f sample_now ]; then : > sample_now; mv sample_now sample_now.done 2>/dev/null
        /usr/bin/sample "$pid" 5 -file host_sample.txt >/dev/null 2>&1; echo "sampled $(date +%T)" > sample_status.txt; fi
      sleep 1; done ) &
  wait "$pid"
  local rc=$?
  mkdir -p logs && cp run_out.txt "logs/run_$(date +%Y%m%d_%H%M%S)_$(basename "$0" .sh).txt" 2>/dev/null   # keep every run's log
  stage "Idle" "$rc"
  echo "run exit: $rc"
  tail -"${1:-4}" run_out.txt | cut -c1-250
}
