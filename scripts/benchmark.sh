#!/bin/bash
# Claude Fable - 2026-11-24: unattended measurement run. Starts the game, leaves it alone for N seconds (default 120;
# it sits on the title scene, which is a real 3D scene), stops it and prints a summary of the log: frame rate, GPU
# time, time inside OpenGL, pacing, hitches.   Run: scripts/benchmark.sh [seconds]
# To measure real play instead, play normally and run:  python3 scripts/summarize_log.py logs/run_out.txt
source "$(dirname "$0")/_common.sh"
SECS="${1:-${BENCH_SECS:-120}}"
RUN_TIMEOUT="$SECS" run_game 0 >/dev/null
python3 scripts/summarize_log.py logs/run_out.txt | tee logs/benchmark_summary.txt
