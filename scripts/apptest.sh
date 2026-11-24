#!/bin/bash
# Claude Fable - 2026-11-24: start the bundled app the way a double-click would (no arguments), stop it after 45 s, show its log.
source "$(dirname "$0")/_common.sh"
APP="build/BioShockInfiniteRecomp.app"
LOGDIR="$HOME/Library/Logs"
before=$(ls -t "$LOGDIR"/*.log 2>/dev/null | head -1)
"$APP/Contents/MacOS/BioShock3" > logs/apptest_stdout.txt 2>&1 &
pid=$!
sleep 45
if kill -0 $pid 2>/dev/null; then echo "app still running after 45 s (good)"; kill $pid; else echo "app exited early"; fi
wait $pid 2>/dev/null; echo "exit code: $?"
newest=$(ls -t "$LOGDIR"/*.log 2>/dev/null | head -1)
echo "log file: $newest"
cp "$newest" logs/apptest_log.txt 2>/dev/null
grep -c . logs/apptest_log.txt; grep -m3 'bink\|Library not loaded\|dyld' logs/apptest_log.txt logs/apptest_stdout.txt | cut -c1-200
grep '\[perf\] present' logs/apptest_log.txt | tail -1 | cut -c1-120
