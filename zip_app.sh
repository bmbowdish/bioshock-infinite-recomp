#!/bin/bash
# Claude Fable - 2026-11-24: zip the app bundle for sending to another Mac. Trigger with: echo zip_app > trigger
source "$(dirname "$0")/_common.sh"
ditto -c -k --sequesterRsrc --keepParent build/BioShockInfiniteRecomp.app build/BioShockInfiniteRecomp.zip; echo "zip exit: $?"; ls -la build/BioShockInfiniteRecomp.zip | cut -c1-120
stage "Idle" 0
