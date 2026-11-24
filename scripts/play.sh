#!/bin/bash
# Run the current build for up to 30 minutes (no rebuild, no scripted input) so a person can play.
source "$(dirname "$0")/_common.sh"
RUN_TIMEOUT=1800 run_game 6
