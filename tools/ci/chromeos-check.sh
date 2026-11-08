#!/usr/bin/env bash
# Inside the Chromebook container image (tools/ci/crostini-docker.sh), as root: installs a release
# .deb the way a user does (apt with the system's own repositories), then checks Gridline there:
# the CLI smoke test, the app starting under Xvfb on Mesa (as on a Chromebook without GPU support),
# with OpenGL capped at 3.0 and at 2.1 (virtual GPUs), the ChromeOS defaults of a new installation,
# and the AppImage.
#   tools/ci/chromeos-check.sh <Gridline .deb> [AppImage] [expected version]
set -euo pipefail
deb="$(readlink -f "${1:?the .deb}")"
appimage="${2:-}"
want="${3:-}"
src="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mktemp -d)"
export DEBIAN_FRONTEND=noninteractive
export GRIDLINE_NO_PLUGIN_SCAN=1

echo "== system: Debian $(cat /etc/debian_version), $(dpkg --print-architecture), $(dpkg-query -W -f '${Package} ${Version}' cros-guest-tools)"
# As in a real container: ChromeOS writes its milestone into /dev (Docker's /dev is fresh).
echo 150 > /dev/.cros_milestone

sh "$src/tools/ci/debian-archive.sh"  # (an older Debian 11 container)
apt-get update -q
apt-get install -y -q "$deb"
apt-get install -y -q --no-install-recommends xvfb xauth mesa-utils > /dev/null
test "$(command -v gridline)" = /usr/bin/gridline
ver="$(gridline --version)"
echo "== installed: $ver"
[ -z "$want" ] || test "$ver" = "Gridline $want"
SMOKE_OUT="$out/smoke" bash "$src/tools/ci/smoke.sh" /usr/bin/gridline "$want"

# app <tag> [env...]: a hidden start with its own config; stderr in $out/<tag>.err.
app() {
  local tag="$1"
  shift
  mkdir -p "$out/$tag/home"
  env "$@" GRIDLINE_CONFIG_HOME="$out/$tag" HOME="$out/$tag/home" SDL_AUDIODRIVER=dummy \
    xvfb-run -a -s '-screen 0 1280x1024x24' /usr/bin/gridline-app --hidden --no-ui-sounds --size 1024x700 --frames 8 \
    --screenshot "$out/$tag.ppm" > "$out/$tag.out" 2> "$out/$tag.err" || { cat "$out/$tag.out" "$out/$tag.err"; echo "chromeos-check: $tag failed" >&2; exit 1; }
  test -s "$out/$tag.ppm" || { echo "chromeos-check: $tag drew nothing" >&2; exit 1; }
  grep -E '^\[(system|video|graphics)\]' "$out/$tag.err" | sed "s/^/  $tag: /" || true
}
setting() { python3 -c 'import json,sys; print(int(json.load(open(sys.argv[1]))["values"][sys.argv[2]]))' "$out/$1/gridline/settings.json" "$2"; }

app default
grep -q '^\[system\] ChromeOS Linux environment (milestone 150)' "$out/default.err"
test "$(setting default audio.sampleRate)" = 48000
test "$(setting default audio.bufferFrames)" = 1024
echo "ok: ChromeOS detected, a new installation starts at 48000 Hz / 1024 frames"

app gl30 MESA_GL_VERSION_OVERRIDE=3.0 MESA_GLSL_VERSION_OVERRIDE=130
grep -q '^\[graphics\] OpenGL 3\.0.*shaders #version 130' "$out/gl30.err"
echo "ok: OpenGL 3.0"
app gl21 MESA_GL_VERSION_OVERRIDE=2.1 MESA_GLSL_VERSION_OVERRIDE=120
grep -q '^\[graphics\] OpenGL 2\.1.*shaders #version 120' "$out/gl21.err"
echo "ok: OpenGL 2.1"

if [ -n "$appimage" ]; then
  appimage="$(readlink -f "$appimage")"
  chmod +x "$appimage"
  # No FUSE 2 in the container (as on a new Chromebook): the image runs extracted.
  test "$(APPIMAGE_EXTRACT_AND_RUN=1 "$appimage" --version)" = "$ver"
  mkdir -p "$out/ai/home"
  GRIDLINE_CONFIG_HOME="$out/ai" HOME="$out/ai/home" SDL_AUDIODRIVER=dummy APPIMAGE_EXTRACT_AND_RUN=1 \
    xvfb-run -a -s '-screen 0 1280x1024x24' "$appimage" --hidden --no-ui-sounds --size 1024x700 --frames 8 --screenshot "$out/ai.ppm" 2> "$out/ai.err"
  test -s "$out/ai.ppm"
  echo "ok: AppImage"
fi

apt-get remove -y -q gridline > /dev/null
test ! -e /usr/bin/gridline
echo "chromeos-check: ok ($out)"
