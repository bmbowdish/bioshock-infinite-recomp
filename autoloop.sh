#!/bin/bash
# Leave this running in one Terminal window. Claude writes "rebuild" or "regen" into the file `trigger`;
# this script runs that, then writes the result to `loop_status.txt`. Ctrl+C to stop.
cd "$(dirname "$0")" || exit 1
echo "autoloop running. Waiting for triggers... (Ctrl+C to stop)"
while true; do
  if [ -f trigger ]; then
    mode=$(cat trigger); rm -f trigger
    echo "[$(date +%T)] running: $mode"
    echo "running $mode" > loop_status.txt
    # a game that is still alive after 60 s is a good sign; it is stopped so the loop can continue
    RUN_TIMEOUT=${RUN_TIMEOUT:-120} bash "./$mode.sh" > loop_log.txt 2>&1
    echo "finished $mode exit=$? at $(date +%T)" > loop_status.txt
    echo "[$(date +%T)] finished: $mode"
  fi
  sleep 1
done
