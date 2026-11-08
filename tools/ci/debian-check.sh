#!/usr/bin/env bash
# In a fresh Debian container (debian:11, debian:12, debian:13, ...), as root: installs a release
# .deb the way a user does (apt with the system's own repositories) and checks Gridline there:
# every program and plugin finds its libraries, the CLI smoke test, the app starting hidden under
# Xvfb with a screenshot, the Import from URL tools, removal, and the AppImage.
#   tools/ci/debian-check.sh <Gridline .deb> [AppImage] [expected version]
set -euo pipefail
deb="$(readlink -f "${1:?the .deb}")"
appimage="${2:-}"
want="${3:-}"
src="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mktemp -d)"
export DEBIAN_FRONTEND=noninteractive
export GRIDLINE_NO_PLUGIN_SCAN=1

sh "$src/tools/ci/debian-archive.sh"
. /etc/os-release
echo "== system: $PRETTY_NAME ($(cat /etc/debian_version)), $(dpkg --print-architecture), $(ldd --version | head -1)"
echo "== package: $(dpkg-deb -f "$deb" Package Version Architecture | tr '\n' ' ')"
echo "== depends: $(dpkg-deb -f "$deb" Depends)"

apt-get update -q
apt-get install -y -q "$deb"
apt-get install -y -q --no-install-recommends xvfb xauth libgl1-mesa-dri python3 > /dev/null
test "$(command -v gridline)" = /usr/bin/gridline
test -x /usr/bin/gridline-app

# Every program, plugin and helper of the package finds its libraries and symbol versions here.
missing=0
while read -r f; do
  case "$(head -c 4 "$f" | od -An -c | tr -d ' ')" in *177ELF*) ;; *) continue ;; esac
  res="$(ldd "$f" 2>&1)" || true
  case "$res" in *"not a dynamic executable"* | *"statically linked"*) continue ;; esac
  if echo "$res" | grep -q 'not found'; then
    echo "$f:"; echo "$res" | grep -E 'not found' | sed 's/^/  /'
    missing=1
  fi
done < <(dpkg -L gridline | while read -r p; do [ -f "$p" ] && [ ! -L "$p" ] && echo "$p"; done)
[ "$missing" = 0 ] || { echo "debian-check: libraries missing above" >&2; exit 1; }
echo "ok: every program and plugin finds its libraries"

ver="$(gridline --version)"
echo "== installed: $ver"
[ -z "$want" ] || test "$ver" = "Gridline $want"
SMOKE_OUT="$out/smoke" bash "$src/tools/ci/smoke.sh" /usr/bin/gridline "$want"
gridline suite status
if [ -x /usr/share/gridline/tools/ffmpeg ]; then
  /usr/share/gridline/tools/ffmpeg -hide_banner -version | head -1
  /usr/share/gridline/tools/yt-dlp --version
fi

# app <tag> <program> [env...]: a hidden start with its own config and home; output in $out/<tag>.*
app() {
  local tag="$1" prog="$2"
  shift 2
  mkdir -p "$out/$tag/home"
  env "$@" GRIDLINE_CONFIG_HOME="$out/$tag" HOME="$out/$tag/home" SDL_AUDIODRIVER=dummy \
    xvfb-run -a -s '-screen 0 1280x1024x24' "$prog" --hidden --no-ui-sounds --size 1024x700 --frames 8 \
    --screenshot "$out/$tag.ppm" > "$out/$tag.out" 2> "$out/$tag.err" || { cat "$out/$tag.out" "$out/$tag.err"; echo "debian-check: $tag failed" >&2; exit 1; }
  test -s "$out/$tag.ppm" || { echo "debian-check: $tag drew nothing" >&2; exit 1; }
  grep -E '^\[(system|video|graphics)\]' "$out/$tag.err" | sed "s/^/  $tag: /" || true
}
app deb /usr/bin/gridline-app
echo "ok: the installed app starts and draws"

if [ -n "$appimage" ]; then
  appimage="$(readlink -f "$appimage")"
  chmod +x "$appimage"
  # No FUSE 2 in a container (nor on a new installation): the image runs extracted.
  test "$(APPIMAGE_EXTRACT_AND_RUN=1 "$appimage" --version)" = "$ver"
  app appimage "$appimage" APPIMAGE_EXTRACT_AND_RUN=1
  echo "ok: AppImage"
fi

apt-get remove -y -q gridline > /dev/null
test ! -e /usr/bin/gridline
echo "debian-check: ok on $PRETTY_NAME ($out)"
