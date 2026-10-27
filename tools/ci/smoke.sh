#!/usr/bin/env bash
# CLI smoke test: version, help, writing the demo song, and render, FLP export and re-import of the test song.
#   tools/ci/smoke.sh <path to the gridline CLI> [expected version]
set -euo pipefail
GL="${1:?path to the gridline CLI}"
WANT="${2:-}"
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${SMOKE_OUT:-$(mktemp -d 2>/dev/null || mktemp -d -t gridline)}"
mkdir -p "$OUT"
export GRIDLINE_CONFIG_HOME="$OUT/cfg"
export GRIDLINE_NO_PLUGIN_SCAN=1

nonempty() { [ -s "$1" ] || { echo "smoke: missing or empty $1" >&2; exit 1; }; }

ver="$("$GL" --version | tr -d '\r')"
echo "version: $ver"
case "$ver" in "Gridline "[0-9]*) ;; *) echo "smoke: bad --version output" >&2; exit 1 ;; esac
if [ -n "$WANT" ] && [ "$ver" != "Gridline $WANT" ]; then
  echo "smoke: expected 'Gridline $WANT'" >&2
  exit 1
fi
"$GL" --help >/dev/null 2>&1

"$GL" demo "$OUT/written-demo.json"
nonempty "$OUT/written-demo.json"
"$GL" info "$SRC/tests/fixtures/test_song.gridline.json"
"$GL" render "$SRC/tests/fixtures/test_song.gridline.json" -o "$OUT/demo.wav" --sr 44100 --bits 16
nonempty "$OUT/demo.wav"
"$GL" render-stats "$SRC/tests/fixtures/test_song.gridline.json" --max-seconds 8
"$GL" export-flp "$SRC/tests/fixtures/test_song.gridline.json" -o "$OUT/demo.flp"
nonempty "$OUT/demo.flp"
"$GL" import-flp "$OUT/demo.flp" -o "$OUT/demo-back.json"
nonempty "$OUT/demo-back.json"
"$GL" info "$OUT/demo-back.json"
"$GL" render "$OUT/demo-back.json" -o "$OUT/demo-back.wav" --sr 44100 --bits 16
nonempty "$OUT/demo-back.wav"
"$GL" export-midi "$SRC/tests/fixtures/test_song.gridline.json" -o "$OUT/demo.mid"
nonempty "$OUT/demo.mid"
echo "smoke: ok ($OUT)"
