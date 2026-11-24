#!/bin/bash
# Claude Fable - 2026-11-24: one-time setup after cloning. Checks the tools this port needs, creates the Python environment
# for the translator, finds your copy of the game and checks that it is the supported build. Changes nothing outside
# this folder and never writes to the game. Safe to run again.
#   ./setup.sh                      use the game in the default Steam library
#   ./setup.sh /path/to/BioShock3.app   use a copy somewhere else
cd "$(dirname "$0")" || exit 1
fail=0
ok()   { printf '  ok      %s\n' "$*"; }
bad()  { printf '  MISSING %s\n' "$*"; fail=1; }
note() { printf '  note    %s\n' "$*"; }

echo "1. This Mac"
if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ]; then ok "Apple Silicon Mac, macOS $(sw_vers -productVersion)"
else bad "an Apple Silicon Mac is required (this is $(uname -s) $(uname -m))"; fi

echo "2. Build tools"
if xcode-select -p >/dev/null 2>&1 && command -v clang >/dev/null 2>&1; then ok "Apple command line tools ($(clang --version | head -1))"
else bad "Apple command line tools. Install with:  xcode-select --install"; fi
if command -v python3 >/dev/null 2>&1; then ok "$(python3 --version)"; else bad "python3 (comes with the Apple command line tools)"; fi
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libavcodec libavformat libavutil 2>/dev/null; then ok "FFmpeg $(pkg-config --modversion libavcodec) (plays the game's videos)"
else note "FFmpeg not found: the game still runs, but its videos stay black. To add it:  brew install ffmpeg pkg-config"; fi

echo "3. Python environment for the translator (.venv)"
if command -v python3 >/dev/null 2>&1; then
  [ -d .venv ] || python3 -m venv .venv
  if .venv/bin/python -c "import capstone, lief" 2>/dev/null; then ok "capstone and lief already installed"
  elif .venv/bin/pip install -q -r requirements.txt; then ok "installed capstone and lief"
  else bad "could not install the Python packages in requirements.txt (is the network reachable?)"; fi
fi

echo "4. Your copy of BioShock Infinite"
APP="${1:-$HOME/Library/Application Support/Steam/steamapps/common/BioShock Infinite/BioShock3.app}"
EXE="$APP/Contents/MacOS/BioShock3"
if [ -f "$EXE" ]; then
  ln -sfn "$EXE" orig.bin
  ok "found: $APP"
  want=$(grep '#define GAME_EXE_SHA256' game/game32.c | grep -o '[0-9a-f]\{64\}')
  got=$(shasum -a 256 "$EXE" | cut -d' ' -f1)
  if [ "$got" = "$want" ]; then ok "it is the supported build"
  else bad "a supported build of the game. This copy is a different version (see 'Supported game version' in README.md)"
       printf '          expected %s\n          found    %s\n' "$want" "$got"; fi
else
  bad "the Mac version of the game at: $APP"
  note "install BioShock Infinite through Steam, or pass the path:  ./setup.sh /path/to/BioShock3.app"
fi

echo
if [ "$fail" = 0 ]; then echo "Setup complete. Next:  ./build_app.sh"; else echo "Setup is not complete: fix the MISSING items above and run ./setup.sh again."; fi
exit $fail
