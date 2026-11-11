#!/bin/sh
# Installs Gridline's build dependencies (compiler, CMake, Ninja, SDL2, OpenGL, X11, MIDI/audio
# development files, Xvfb for the window tests; on apt also xprop / xwininfo and
# desktop-file-validate for the K-14 window class and icon checks) on one platform. Used by the CI and release
# workflows; also a reference for building by hand.
#   tools/ci/install-deps.sh <apt|dnf|pacman|zypper|apk|brew|freebsd|openbsd|netbsd>
set -eu
# apt: a runner whose package mirrors are briefly unreachable (a name that does not resolve, a dropped
# connection) gets a few more tries, a little apart, with the indexes read again before each.
apt_retry() {
  n=1
  until "$@"; do
    [ "$n" -ge 4 ] && return 1
    echo "install-deps: '$*' failed; trying again in $((n * 30)) s" >&2
    sleep $((n * 30))
    n=$((n + 1))
    apt-get update -q -o Acquire::Retries=3 || true
  done
}
case "${1:?platform}" in
  apt)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q -o Acquire::Retries=3 || true
    apt_retry apt-get install -y -q -o Acquire::Retries=3 --no-install-recommends build-essential cmake ninja-build pkg-config git ca-certificates \
      libsdl2-dev libgl-dev libx11-dev libasound2-dev libjack-jackd2-dev libgl1-mesa-dri xvfb xauth x11-utils desktop-file-utils file python3
    # Debian 11's CMake (3.18) builds Gridline, but the app tests read JSON with CMake 3.19 or later:
    # there Kitware's official CMake goes to /usr/local (checked against its SHA-256).
    if ! cmake --version | head -1 | grep -Eq ' (3\.(19|[2-9][0-9])|[4-9])\.'; then
      v=3.31.9
      case "$(uname -m)" in
        x86_64) a=x86_64 sum=312d78e0b7c5c9b4a97a87a46c3e525aa84895fe8135c238c4616bba73dc9518 ;;
        aarch64) a=aarch64 sum=09b7127b09e3d3ed5fa894f4e59f6a01112f267da6acc4249e46dba97b14afcb ;;
        *) echo "install-deps: no CMake $v binary for $(uname -m)" >&2; exit 1 ;;
      esac
      apt-get install -y -q --no-install-recommends curl > /dev/null
      curl -fsSL -o /tmp/cmake.tar.gz "https://github.com/Kitware/CMake/releases/download/v$v/cmake-$v-linux-$a.tar.gz"
      echo "$sum  /tmp/cmake.tar.gz" | sha256sum -c -
      rm -rf /opt/cmake && mkdir -p /opt/cmake && tar -C /opt/cmake --strip-components=1 -xzf /tmp/cmake.tar.gz && rm -f /tmp/cmake.tar.gz
      for t in cmake ctest cpack; do ln -sf "/opt/cmake/bin/$t" "/usr/local/bin/$t"; done
      hash -r 2>/dev/null || true
      /usr/local/bin/cmake --version | head -1
    fi
    ;;
  dnf)
    dnf install -y -q gcc-c++ cmake ninja-build pkgconf-pkg-config git SDL2-devel mesa-libGL-devel libX11-devel \
      alsa-lib-devel mesa-dri-drivers xorg-x11-server-Xvfb xorg-x11-xauth which file python3 rpm-build
    ;;
  pacman)
    pacman -Syu --noconfirm --needed base-devel cmake ninja pkgconf git sdl2 mesa libx11 alsa-lib \
      xorg-server-xvfb xorg-xauth python
    ;;
  zypper)
    zypper --non-interactive refresh
    zypper --non-interactive install -y gcc-c++ cmake ninja pkgconf git SDL2-devel Mesa-libGL-devel libX11-devel \
      alsa-devel Mesa-dri xvfb-run xauth which file python3 rpm-build gzip tar
    ;;
  apk)
    apk add --no-cache build-base cmake samurai pkgconf git sdl2-dev mesa-dev libx11-dev alsa-lib-dev \
      linux-headers xvfb-run xauth mesa-dri-gallium bash python3 file
    ;;
  brew)
    brew install --quiet sdl2 ninja || true
    ;;
  freebsd)
    pkg install -y cmake ninja pkgconf git sdl2 libX11 mesa-libs sndio xorg-vfbserver xauth bash python3
    ;;
  openbsd)
    # X11 (Xlib, Mesa's libGL, Xvfb) and sndio are part of the base system.
    pkg_add -I cmake ninja sdl2 git bash python3 || pkg_add -I cmake ninja sdl2 git bash python%3
    ;;
  netbsd)
    # X11 (Xlib, Mesa's libGL, Xvfb) is part of the base sets; sndio from pkgsrc.
    export PATH="/usr/pkg/sbin:/usr/pkg/bin:$PATH"
    # The X11 sets (Xlib, Mesa's libGL, headers) when the system was installed without them.
    if [ ! -f /usr/X11R7/include/X11/Xlib.h ]; then
      for set in xbase xcomp; do
        # Current releases on the CDN, older ones in the archive.
        for base in https://cdn.netbsd.org/pub/NetBSD https://archive.netbsd.org/pub/NetBSD-archive; do
          if ftp -o "/tmp/$set.tar.xz" "$base/NetBSD-$(uname -r)/$(uname -m)/binary/sets/$set.tar.xz"; then
            tar -xJpf "/tmp/$set.tar.xz" -C / && rm -f "/tmp/$set.tar.xz" && break
          fi
        done
      done
    fi
    if command -v pkgin >/dev/null 2>&1; then
      pkgin -y install cmake ninja-build pkgconf SDL2 sndio bash
    else
      for p in cmake ninja-build pkgconf SDL2 sndio bash; do  # mirrors are flaky: three tries each
        pkg_info -e "$p" >/dev/null 2>&1 || pkg_add "$p" || { sleep 10; pkg_add "$p"; } || { sleep 30; pkg_add "$p"; } || echo "pkg_add $p failed" >&2
      done
    fi
    ls /usr/X11R7/include/X11/Xlib.h /usr/X11R7/lib/libGL.so
    ;;
  *)
    echo "unknown platform: $1" >&2
    exit 2
    ;;
esac
