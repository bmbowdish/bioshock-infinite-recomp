#!/bin/bash
# Build, run the game ~45 s, then take three thread snapshots 2 s apart (SIGUSR1) and stop it.
source "$(dirname "$0")/_common.sh"
build || exit 1
stage "Running"
[ -s loop_env ] && export $(cat loop_env)
RT32_STRICT=1 ./build/BioShock3 orig.bin "$GAME/Resources" > run_out.txt 2>&1 &
pid=$!
sleep 45
/usr/bin/sample $pid 2 -file host_sample.txt >/dev/null 2>&1
for i in 1 2 3; do kill -USR1 $pid 2>/dev/null; sleep 2; done
kill $pid 2>/dev/null; wait $pid
stage "Idle" $?
echo sampled
