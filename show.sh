#!/bin/bash
# Run the current build for ~3 minutes (no rebuild) so the window can be inspected.
source "$(dirname "$0")/_common.sh"
RUN_TIMEOUT=170 run_game 6
