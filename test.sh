#!/bin/bash
# Scripted test run: up to 6 minutes, no rebuild (scripted input from loop_env)
source "$(dirname "$0")/_common.sh"
RUN_TIMEOUT=360 run_game 6
