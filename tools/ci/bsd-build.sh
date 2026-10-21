#!/bin/sh
# Build, test and smoke-test Gridline inside a BSD VM (FreeBSD, OpenBSD, NetBSD): CMake + Ninja
# with the base system's compiler. Run from the source root.
set -eu
export PATH="/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin:/usr/local/bin:$PATH"
export GRIDLINE_NO_PLUGIN_SCAN=1
uname -a
c++ --version || true
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGRIDLINE_GUI_TESTS=OFF ${GRIDLINE_CMAKE_ARGS:-}
cmake --build build -j"$JOBS"
ctest --test-dir build -j"$JOBS" --output-on-failure --timeout 2400
bash tools/ci/smoke.sh build/gridline
