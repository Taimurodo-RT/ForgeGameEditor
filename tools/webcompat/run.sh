#!/bin/sh
# Draws every test page in Chromium and in the engine, then compares them.
#   tools/webcompat/run.sh BUILD_DIR OUT_DIR [PYTHON]
# Needs Node with Playwright (and its Chromium), and Python with Pillow and numpy.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=$1
out=$2
python=${3:-python3}
mkdir -p "$out/browser" "$out/engine"
node "$here/browser_shots.js" "$out/browser"
set --
for f in "$here"/cases/*.html; do set -- "$@" --page "$f"; done
(cd "$root" && "$build/apps/ui_demo/forge_ui_demo" --ui ui "$@" --out "$out/engine" > "$out/engine.log" 2>&1) || true
"$python" "$here/compare.py" "$out/browser" "$out/engine" "$out"
