#!/bin/sh
# Builds what the real-device MIDI test needs (gridline-app, and on Linux the user-space port
# helper) and runs it: tools/ci/midi-real.py through ctest (.github/workflows/midi-real.yml). It
# loads kernel modules or restarts sndiod: for a throwaway CI machine only. Run from the source root.
#   GRIDLINE_MIDI_REAL_ARGS   options for midi-real.py (--require-kernel, --require-sndiod)
#   GRIDLINE_MIDI_REAL_SOFT=1 always exit 0 (the BSD VMs: the results are then copied back to the
#                             runner, which fails the job from midi-real-out/status)
# Writes midi-real-out/summary.md, the app logs, and midi-real-out/status (0 when every check passed).
set -u
export PATH="/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin:/usr/local/bin:/usr/X11R6/bin:/usr/X11R7/bin:$PATH"
export PKG_CONFIG_PATH="/usr/pkg/lib/pkgconfig:/usr/X11R7/lib/pkgconfig:/usr/X11R6/lib/pkgconfig:/usr/local/libdata/pkgconfig:/usr/local/lib/pkgconfig:/usr/libdata/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export GRIDLINE_MIDI_REAL=1 GRIDLINE_NO_PLUGIN_SCAN=1 GRIDLINE_NO_REALTIME=1
export CI="${CI:-true}"
OUT=midi-real-out
BUILD=build-midi
rm -rf "$OUT"
mkdir -p "$OUT"

sys="$(uname -s)"
if [ "$sys" = Linux ]; then
  JOBS="$(nproc 2>/dev/null || echo 2)"
  targets="gridline-app midi-seq-port"
else
  JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"
  [ "$JOBS" -gt 2 ] && JOBS=2  # the VMs stop answering SSH under memory pressure
  targets="gridline-app"
fi

# The hidden app still needs a display: Xvfb when none is given (xvfb-run on Linux).
xpid=""
if [ -z "${DISPLAY:-}" ] && command -v Xvfb >/dev/null 2>&1; then
  Xvfb :99 -screen 0 1600x1000x24 -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
  xpid=$!
  export DISPLAY=:99
  sleep 3
fi

status=1
if cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGRIDLINE_GUI_TESTS=OFF > "$OUT/configure.log" 2>&1; then
  grep -E "MIDI" "$OUT/configure.log" || true
  # shellcheck disable=SC2086  # (a list of targets)
  if cmake --build "$BUILD" -j"$JOBS" --target $targets; then
    ctest --test-dir "$BUILD" -R '^midi_real_app$' --no-tests=error -V --timeout 900
    status=$?
  else
    echo "midi-real: the build failed" >&2
    printf '## MIDI through the real back end (%s)\n\nFAIL: the build failed.\n' "$sys" > "$OUT/summary.md"
  fi
else
  cat "$OUT/configure.log" >&2
  printf '## MIDI through the real back end (%s)\n\nFAIL: CMake configure failed.\n' "$sys" > "$OUT/summary.md"
fi
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null

if [ -d "$BUILD/midi_real_app" ]; then
  cp "$BUILD/midi_real_app/summary.md" "$OUT/" 2>/dev/null || true
  for d in "$BUILD"/midi_real_app/*/; do
    [ -f "$d/out.log" ] && cp "$d/out.log" "$OUT/$(basename "$d").log"
  done
fi
[ -f "$OUT/summary.md" ] || printf '## MIDI through the real back end (%s)\n\nFAIL: the test wrote no summary (exit %s).\n' "$sys" "$status" > "$OUT/summary.md"
echo "$status" > "$OUT/status"
echo "midi-real: exit $status"

if [ "${GRIDLINE_MIDI_REAL_SOFT:-}" = 1 ]; then
  rm -rf "$BUILD"  # the VM copies the tree back: the results only
  exit 0
fi
exit "$status"
