#!/bin/bash
# Claude Fable - 2026-11-24: build a double-clickable app with FFmpeg bundled. Trigger with: echo bundle > trigger
source "$(dirname "$0")/_common.sh"
stage "Bundling"
NAME=BioShockInfiniteRecomp
# Icon: copied (read-only) from the user's own installed game, the way mac32.mk's GAME_ICON intends.
ICON_NAME=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIconFile" "$GAME/Info.plist" 2>/dev/null)
ICON="$GAME/Resources/${ICON_NAME%.icns}.icns"
[ -f "$ICON" ] || ICON=$(ls "$GAME/Resources/"*.icns 2>/dev/null | head -1)
if [ -f "$ICON" ]; then echo "icon: $(basename "$ICON")"; else echo "icon: none found in the game install"; ICON=""; fi
make bundle BUNDLE_NAME=$NAME BUNDLE_ID=local.recomp.bioshockinfinite GAME_ICON="$ICON" > bundle.log 2>&1
echo "make bundle exit: $?"; tail -3 bundle.log | cut -c1-200
APP="build/$NAME.app"
[ -d "$APP" ] || { echo "no app bundle produced"; stage "Bundle failed" 1; exit 1; }
python3 bundle_dylibs.py "$APP" "$APP/Contents/MacOS/BioShock3" 2>&1 | cut -c1-600
ls -la "$APP/Contents/Resources/" | cut -c1-120; touch "$APP"
du -sh "$APP" | cut -c1-80
stage "Idle" 0
