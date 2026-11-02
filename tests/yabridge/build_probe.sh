#!/usr/bin/env bash
# Cross-compiles the Windows probe plugins with mingw-w64 from original code and the SDK headers
# in third_party. Usage: build_probe.sh <gridline source folder> <out folder>
set -euo pipefail
SRC="$(cd "$1" && pwd)"
OUT="$2"
mkdir -p "$OUT"
CXX="$(command -v x86_64-w64-mingw32-g++-posix || command -v x86_64-w64-mingw32-g++)"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -shared -static -static-libgcc -static-libstdc++ -DNOMINMAX -I "$SRC/tests/yabridge/probe")
LIBS=(-luser32 -lgdi32 -lkernel32)

"$CXX" "${FLAGS[@]}" -isystem "$SRC/third_party/vst3" \
  "$SRC/tests/yabridge/probe/probe_vst3.cpp" \
  "$SRC/third_party/vst3/pluginterfaces/base/funknown.cpp" \
  "$SRC/third_party/vst3/pluginterfaces/base/coreiids.cpp" \
  -lole32 "${LIBS[@]}" -o "$OUT/GridlineProbe.vst3"

"$CXX" "${FLAGS[@]}" -isystem "$SRC/third_party/clap/include" \
  "$SRC/tests/yabridge/probe/probe_clap.cpp" "${LIBS[@]}" -o "$OUT/GridlineProbe.clap"

for kind in Synth:1 Fx:0; do
  "$CXX" "${FLAGS[@]}" -I "$SRC/src" -DPROBE_SYNTH="${kind#*:}" \
    "$SRC/tests/yabridge/probe/probe_vst2.cpp" "${LIBS[@]}" -o "$OUT/GridlineProbe${kind%:*}.dll"
done
ls -l "$OUT"
