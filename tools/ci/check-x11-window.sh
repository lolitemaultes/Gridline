#!/usr/bin/env bash
# K-14: starts gridline-app on an X server (run under xvfb-run) and checks its window as a panel
# or window switcher sees it: WM_CLASS is the desktop entry id (src/app/AppIconImage.h) and the
# window carries its own icon (_NET_WM_ICON, 256 x 256). The window is hidden (never mapped);
# X11 properties are set either way. Needs xwininfo and xprop (x11-utils).
# Usage: xvfb-run -a tools/ci/check-x11-window.sh <gridline-app> [config dir]
set -euo pipefail
APP="${1:?gridline-app}"
CFG="${2:-$(mktemp -d)}"
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
wmclass="$(sed -n 's/.*kDesktopId = "\([^"]*\)".*/\1/p' "$SRC/src/app/AppIconImage.h")"
log="$(mktemp)"
GRIDLINE_CONFIG_HOME="$CFG" GRIDLINE_NO_PLUGIN_SCAN=1 "$APP" --hidden --size 800x600 --frames 100000 --no-ui-sounds > "$log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null || true' EXIT
win=""
for _ in $(seq 1 120); do  # up to 60 s for the first window
  win="$(xwininfo -root -tree 2> /dev/null | grep -i "(\"$wmclass\" " | head -1 | awk '{print $1}')" || true
  [ -n "$win" ] && break
  kill -0 $pid 2> /dev/null || { cat "$log"; echo "::error::gridline-app exited before its window appeared"; exit 1; }
  sleep 0.5
done
if [ -z "$win" ]; then
  xwininfo -root -tree > "$log.tree" || true; sed -n '1,40p' "$log.tree"
  echo "::error::no window with WM_CLASS \"$wmclass\""
  exit 1
fi
cls="$(xprop -id "$win" WM_CLASS)"
echo "$cls"
echo "$cls" | grep -q "= \"$wmclass\", \"$wmclass\"" || { echo "::error::WM_CLASS is not (\"$wmclass\", \"$wmclass\")"; exit 1; }
# _NET_WM_ICON: width, height, then the pixels; the first two cardinals are the size.
xprop -id "$win" -notype 32c _NET_WM_ICON > "$log.icon"
icon="$(sed -n '1,3p' "$log.icon" | cut -c1-120)"
echo "$icon"
grep -Eq "_NET_WM_ICON.*= 256, 256,|Icon \(256 ?x ?256\)" "$log.icon" || { echo "::error::the window has no 256 x 256 _NET_WM_ICON"; exit 1; }
echo "X11 window: WM_CLASS $wmclass, _NET_WM_ICON 256 x 256"
