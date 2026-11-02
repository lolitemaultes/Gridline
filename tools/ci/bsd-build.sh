#!/bin/sh
# Build, test and smoke-test Gridline inside a BSD VM (FreeBSD, OpenBSD, NetBSD): CMake + Ninja
# with the base system's compiler. Run from the source root.
#   GRIDLINE_CMAKE_ARGS   extra configure options
#   GRIDLINE_PACKAGE=<version>   also install into a staging folder and write
#                                Gridline-<version>-<system>-<arch>.tar.gz (release.yml)
set -eu
export PATH="/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin:/usr/local/bin:$PATH"
export PKG_CONFIG_PATH="/usr/pkg/lib/pkgconfig:/usr/X11R7/lib/pkgconfig:/usr/X11R6/lib/pkgconfig:/usr/local/libdata/pkgconfig:/usr/local/lib/pkgconfig:/usr/libdata/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export GRIDLINE_NO_PLUGIN_SCAN=1
# Root in the VM may give real-time priority to audio workers; heavy tests would then starve SSH.
export GRIDLINE_NO_REALTIME=1
uname -a
c++ --version || true
echo "sdl2: $(pkg-config --modversion sdl2 2>&1 || true)"
echo "gl:   $(pkg-config --modversion gl 2>&1 || true)"
# K-X: NetBSD under KVM: the guest clock runs at its own rate per boot (seen against the runner's log
# stamps: 0.60x, 0.97x, 0.91x, 18.56x, always constant within a run; FreeBSD and OpenBSD stay at 1.00x).
# At 18x every ctest timeout and wall-clock check fires early. Guess: the TSC calibrated at boot is
# wrong, so a timer that needs no calibration is chosen when the kernel offers one. The stamps below
# let the next log show the rate: the guest's date against the runner's time on the same line.
if [ "$(uname -s)" = NetBSD ]; then
  sysctl kern.timecounter.hardware kern.timecounter.choice || true
  dmesg | grep -i -E 'timecounter|tsc' | tail -n 8 || true
  if [ "$(sysctl -n kern.timecounter.hardware 2>/dev/null || true)" = TSC ]; then
    for tc in hpet0 ACPI-Fast; do
      if sysctl -n kern.timecounter.choice 2>/dev/null | grep -q "$tc"; then
        sysctl -w kern.timecounter.hardware="$tc" || true
        break
      fi
    done
  fi
fi
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"
# The VMs stop answering SSH under memory pressure (GCC 10 on NetBSD, the large render-cache
# tests everywhere): two compilers or tests at a time.
if [ "$JOBS" -gt 2 ]; then JOBS=2; fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGRIDLINE_GUI_TESTS=OFF ${GRIDLINE_CMAKE_ARGS:-}
cmake --build build -j"$JOBS" -- -k 0
test -x build/gridline-app || { echo "gridline-app was not built (SDL2 / OpenGL missing)" >&2; exit 1; }
if [ -z "${GRIDLINE_PACKAGE:-}" ]; then
  date -u
  ctest --test-dir build -j"$JOBS" --output-on-failure --timeout 2400
  date -u
fi
bash tools/ci/smoke.sh build/gridline
build/gridline-app --version

if [ -n "${GRIDLINE_PACKAGE:-}" ]; then
  sys="$(uname -s | tr '[:upper:]' '[:lower:]')"
  arch="$(uname -m)"
  [ "$arch" = "amd64" ] && arch=x86_64
  name="Gridline-$GRIDLINE_PACKAGE-$sys-$arch"
  rm -rf "dist/$name"
  mkdir -p dist
  cmake --install build --prefix "$PWD/dist/$name"
  cmake --install build --prefix "$PWD/dist/$name" --component plugins
  rm -rf "dist/$name/share/gridline/assets/sounds/src"
  # Gridline Suite (installed with the programs: lib/vst3, lib/clap)
  test -f "dist/$name/lib/clap/Gridline Suite.clap"
  test -d "dist/$name/lib/vst3/Gridline Suite.vst3"
  deps="sndio for MIDI"
  sed -e "s/@VERSION@/$GRIDLINE_PACKAGE/g" -e "s/@SYSTEM@/$(uname -s) $(uname -r)/g" -e "s/@ARCH@/$arch/g" -e "s/@DEPS@/$deps/g" \
    tools/release/README-unix.txt > "dist/$name/README.txt"
  install -m 755 tools/release/install-unix.sh "dist/$name/install.sh"  # K-14: menus, icons, file type
  bash tools/ci/check-linux-icons.sh "dist/$name"
  "dist/$name/bin/gridline" --version
  (cd dist && tar czf "$name.tar.gz" "$name")
  ls -l "dist/$name.tar.gz"
  # The VM action copies the whole tree back to the runner; a 4 GB build there can drop the
  # connection (seen on FreeBSD after all tests had passed), and only the archive is needed.
  rm -rf build "dist/$name"
fi
