#!/bin/bash
# Run the current build with no time limit (no rebuild, no scripted input) until the player quits.
source "$(dirname "$0")/_common.sh"
RUN_TIMEOUT= run_game 6
