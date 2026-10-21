#!/bin/sh
# Installs Gridline's build dependencies (compiler, CMake, Ninja, SDL2, OpenGL, X11, MIDI/audio
# development files, Xvfb for the window tests) on one platform. Used by the CI and release
# workflows; also a reference for building by hand.
#   tools/ci/install-deps.sh <apt|dnf|pacman|zypper|apk|brew|freebsd|openbsd|netbsd>
set -eu
case "${1:?platform}" in
  apt)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    apt-get install -y -q --no-install-recommends build-essential cmake ninja-build pkg-config git ca-certificates \
      libsdl2-dev libgl-dev libx11-dev libasound2-dev libjack-jackd2-dev libgl1-mesa-dri xvfb xauth file python3
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
