#!/usr/bin/env bash
# Builds the macOS disk image from a build folder (release.yml; works on any build):
#   tools/release/package-macos.sh <build dir> <version> <out dir> [arch label]
# <version> is the full version (0.7.0-beta.1): the file name, the volume name and the README.
# The bundles' Info.plist versions are the numbers only (0.7.0), written by the build.
# The image holds Gridline.app (the app, the command line tool and the assets inside), the
# plugins (Gridline.vst3 / .clap / .component), the command line tool and a README. Everything
# is signed ad hoc (no Developer ID: users open it with right-click > Open the first time).
set -euo pipefail
BUILD="$(cd "${1:?build dir}" && pwd)"
VER="${2:?version}"
OUT="${3:?out dir}"
LABEL="${4:-universal}"
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
ROOT="$OUT/dmgroot"
rm -rf "$ROOT"
mkdir -p "$ROOT/Plugins"

app="$ROOT/Gridline.app"
ditto "$BUILD/Gridline.app" "$app"
# The build tree's bundle holds a hard link to the program; the image gets a real copy, the
# command line tool next to it and the assets inside (the app finds Contents/Resources/assets).
rm -f "$app/Contents/MacOS/gridline-app"
cp "$BUILD/gridline-app" "$app/Contents/MacOS/gridline-app"
cp "$BUILD/gridline" "$app/Contents/MacOS/gridline"
rm -rf "$app/Contents/Resources/assets"
ditto "$SRC/assets" "$app/Contents/Resources/assets"
rm -rf "$app/Contents/Resources/assets/sounds/src"
cp "$SRC/assets/icons/Gridline.icns" "$app/Contents/Resources/Gridline.icns"

# The Gridline Suite (Gridline's instruments and effects as plugins for other music programs)
# travels inside the app: Gridline installs it into ~/Library/Audio/Plug-Ins on first launch
# (with the user's consent), and `gridline suite install` does the same from a terminal.
SUITE_BUNDLES=("Gridline Suite.vst3" "Gridline Suite.clap" "Gridline Suite.component"
               "Gridline Suite Mini.vst3" "Gridline Suite Mini.clap" "Gridline Suite Mini.component")
rm -rf "$app/Contents/Resources/Suite"
mkdir -p "$app/Contents/Resources/Suite"
for p in "${SUITE_BUNDLES[@]}"; do
  test -d "$BUILD/plugins/$p" || { echo "missing $BUILD/plugins/$p (build targets gridline_suite_gridline, gridline_suite_mini)" >&2; exit 1; }
  ditto "$BUILD/plugins/$p" "$app/Contents/Resources/Suite/$p"
  rm -rf "$app/Contents/Resources/Suite/$p/Contents/Resources/assets/sounds/src"
done

# J-192: yt-dlp and ffmpeg for Import from URL (GRIDLINE_MEDIA_TOOLS: a folder made by
# tools/release/fetch-media-tools.py --target macos-universal; without it Gridline installs them
# on first use).
rm -rf "$app/Contents/Resources/tools"
if [ -n "${GRIDLINE_MEDIA_TOOLS:-}" ]; then
  test -x "$GRIDLINE_MEDIA_TOOLS/ffmpeg" -a -x "$GRIDLINE_MEDIA_TOOLS/yt-dlp" || { echo "no ffmpeg / yt-dlp in $GRIDLINE_MEDIA_TOOLS" >&2; exit 1; }
  ditto "$GRIDLINE_MEDIA_TOOLS" "$app/Contents/Resources/tools"
fi

for p in Gridline.vst3 Gridline.clap Gridline.component; do
  ditto "$BUILD/plugins/$p" "$ROOT/Plugins/$p"
  rm -rf "$ROOT/Plugins/$p/Contents/Resources/assets/sounds/src"
done
cp "$BUILD/gridline" "$ROOT/gridline"
sed "s/@VERSION@/$VER/g" "$SRC/tools/release/README-macos.txt" > "$ROOT/README.txt"
ln -s /Applications "$ROOT/Applications"

# Ad hoc signatures (inside out: nested code first).
codesign --force --sign - --timestamp=none "$app/Contents/MacOS/gridline"
for t in yt-dlp ffmpeg ffprobe; do
  if [ -f "$app/Contents/Resources/tools/$t" ]; then codesign --force --sign - --timestamp=none "$app/Contents/Resources/tools/$t"; fi
done
for p in "${SUITE_BUNDLES[@]}"; do codesign --force --sign - --timestamp=none "$app/Contents/Resources/Suite/$p"; done
codesign --force --sign - --timestamp=none "$app"  # no hardened runtime: it would refuse unsigned plugins
for p in Gridline.vst3 Gridline.clap Gridline.component; do codesign --force --sign - --timestamp=none "$ROOT/Plugins/$p"; done
codesign --force --sign - --timestamp=none "$ROOT/gridline"
codesign --verify --deep --strict "$app"

dmg="$OUT/Gridline-$VER-macos-$LABEL.dmg"
rm -f "$dmg"
# The window design (background, icon layout, volume icon; tools/release/dmg, make_dmg.py) needs
# dmgbuild (GRIDLINE_PYTHON: a python3 that has it). Without it the image is plain, or an error
# with GRIDLINE_DMG_REQUIRE_LAYOUT=1 (release.yml).
PY="${GRIDLINE_PYTHON:-python3}"
if "$PY" -c "import dmgbuild" 2>/dev/null; then
  "$PY" "$SRC/tools/release/make_dmg.py" "$ROOT" "$dmg" "Gridline $VER"
elif [ "${GRIDLINE_DMG_REQUIRE_LAYOUT:-0}" = "1" ]; then
  echo "dmgbuild is missing (pip install dmgbuild): no disk image window design" >&2; exit 1
else
  echo "warning: dmgbuild is missing (pip install dmgbuild): plain disk image without the window design" >&2
  for attempt in 1 2 3 4 5; do  # hdiutil is flaky on CI machines ("resource busy")
    if hdiutil create -volname "Gridline $VER" -srcfolder "$ROOT" -ov -format UDZO -fs HFS+ "$dmg"; then break; fi
    sleep 5
  done
fi
test -s "$dmg"
echo "$dmg"
