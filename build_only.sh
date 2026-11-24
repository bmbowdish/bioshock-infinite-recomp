#!/bin/bash
# Claude Fable - 2026-11-24: compile only (no game launch). Trigger with: echo build_only > trigger
source "$(dirname "$0")/_common.sh"
build || exit 1
stage "Idle" 0
