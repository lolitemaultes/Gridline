#!/usr/bin/env bash
# K-14: checks that an installed Gridline tree (a DESTDIR / AppDir, the tarball, an unpacked .deb or
# .rpm) ties the window to its launcher and carries every icon:
#   the desktop entry id = StartupWMClass = the window class the program sets (src/app/AppIconImage.h),
#   Icon= names an icon installed in hicolor at every size plus scalable, AppStream's launchable and
#   icon agree with the entry, the MIME type's icon exists.
# Usage: tools/ci/check-linux-icons.sh <prefix dir, e.g. AppDir/usr or stage/Gridline-...> [AppDir root]
# With an AppDir root it also checks the AppImage's own root icon, .DirIcon and desktop entry.
set -euo pipefail
P="${1:?installed prefix (the folder with bin/ and share/)}"
APPDIR="${2:-}"
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
fail=0
bad() { echo "::error::$*"; fail=1; }

wmclass="$(sed -n 's/.*kDesktopId = "\([^"]*\)".*/\1/p' "$SRC/src/app/AppIconImage.h")"
iconname="$(sed -n 's/.*kIconName = "\([^"]*\)".*/\1/p' "$SRC/src/app/AppIconImage.h")"
[ -n "$wmclass" ] && [ -n "$iconname" ] || bad "cannot read kDesktopId / kIconName from src/app/AppIconImage.h"

desktop="$P/share/applications/$wmclass.desktop"
[ -f "$desktop" ] || bad "missing $desktop (the desktop entry id must be the window class '$wmclass')"
key() { sed -n "s/^$1=//p" "$2" | head -1; }
if [ -f "$desktop" ]; then
  [ "$(key StartupWMClass "$desktop")" = "$wmclass" ] || bad "StartupWMClass is '$(key StartupWMClass "$desktop")', not '$wmclass'"
  [ "$(key Icon "$desktop")" = "$iconname" ] || bad "Icon is '$(key Icon "$desktop")', not '$iconname'"
  exe="$(key Exec "$desktop" | cut -d' ' -f1)"
  [ -x "$P/bin/$(basename "$exe")" ] || bad "Exec '$exe' is not in $P/bin"
  if command -v desktop-file-validate > /dev/null; then desktop-file-validate "$desktop" || bad "desktop-file-validate failed"; fi
fi

for sz in 16 24 32 48 64 96 128 256 512; do
  f="$P/share/icons/hicolor/${sz}x${sz}/apps/$iconname.png"
  if [ ! -f "$f" ]; then bad "missing $f"; continue; fi
  # PNG IHDR width / height (bytes 16..23)
  dim="$(od -An -tu1 -j16 -N8 "$f" | tr -s ' ' | sed -E 's/^ //; s/(^| )0+([0-9])/\1\2/g')"  # the BSDs' od pads with zeros
  set -- $dim
  w=$(( ($1 << 24) | ($2 << 16) | ($3 << 8) | $4 )); h=$(( ($5 << 24) | ($6 << 16) | ($7 << 8) | $8 ))
  [ "$w" = "$sz" ] && [ "$h" = "$sz" ] || bad "$f is ${w}x${h}, not ${sz}x${sz}"
done
svg="$P/share/icons/hicolor/scalable/apps/$iconname.svg"
[ -f "$svg" ] && grep -q "<svg" "$svg" || bad "missing or broken $svg"

meta="$P/share/metainfo/com.lolitemaultes.Gridline.metainfo.xml"
if [ -f "$meta" ]; then
  grep -q "<launchable type=\"desktop-id\">$wmclass.desktop</launchable>" "$meta" || bad "AppStream launchable is not $wmclass.desktop"
  ! grep -q "<icon type=\"stock\">" "$meta" || bad "AppStream names a stock icon (appstream-util rejects it; the desktop entry's Icon= is used)"
else
  bad "missing $meta"
fi
mime="$P/share/mime/packages/gridline.xml"
[ -f "$mime" ] && grep -q "<icon name=\"$iconname\"/>" "$mime" || bad "MIME type file missing or its icon is not $iconname"

if [ -n "$APPDIR" ]; then  # AppImage root: the entry, the icon and .DirIcon (file managers, appimaged)
  [ -f "$APPDIR/$wmclass.desktop" ] || bad "AppImage root has no $wmclass.desktop"
  [ -e "$APPDIR/$iconname.png" ] || [ -e "$APPDIR/$iconname.svg" ] || bad "AppImage root has no $iconname icon"
  if [ -e "$APPDIR/.DirIcon" ]; then
    dim="$(od -An -tu1 -N24 "$APPDIR/.DirIcon" | tr -s ' \n' '  ' | sed -E 's/^ //; s/(^| )0+([0-9])/\1\2/g')"
    set -- $dim
    [ "$2" = 80 ] && [ "$3" = 78 ] && [ "$4" = 71 ] || bad ".DirIcon is not a PNG (thumbnailers read a PNG)"
    w=$(( (${17} << 24) | (${18} << 16) | (${19} << 8) | ${20} ))
    [ "$w" -ge 256 ] || bad ".DirIcon is ${w} px wide, less than 256"
  else
    bad "AppImage root has no .DirIcon"
  fi
fi

if [ "$fail" = 0 ]; then echo "Linux icons and window class agree ($wmclass, icon $iconname): $P"; fi
exit "$fail"
