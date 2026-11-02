#!/bin/sh
# Gridline: installs this folder (the Linux / BSD .tar.gz) so the desktop shows Gridline with its
# icon in menus, launchers, docks and file managers (K-14). Shipped as install.sh in the tarball.
#
#   ./install.sh                  install for this user (~/.local) or, as root, for everyone (/usr/local)
#   ./install.sh --prefix DIR     install under DIR (DIR/bin, DIR/share)
#   ./install.sh --uninstall      remove what an earlier ./install.sh put there (same --prefix)
#
# Installs bin/ and share/ (the programs, their assets, the desktop entry, the icons at every size,
# the project file type and the AppStream metadata) and refreshes the desktop's caches. The plugins
# in lib/ are not copied: Gridline offers to install the Gridline Suite itself (gridline suite install).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
if [ "$(id -u)" = 0 ]; then prefix=/usr/local; else prefix="$HOME/.local"; fi
uninstall=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) prefix="${2:?--prefix needs a folder}"; shift 2 ;;
    --prefix=*) prefix="${1#--prefix=}"; shift ;;
    --uninstall) uninstall=1; shift ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
done
manifest="$prefix/share/gridline/install-manifest.txt"

refresh() {
  if command -v gtk-update-icon-cache > /dev/null 2>&1 && [ -f "$prefix/share/icons/hicolor/index.theme" ]; then
    gtk-update-icon-cache -q -t -f "$prefix/share/icons/hicolor" || true
  fi
  if command -v update-desktop-database > /dev/null 2>&1; then update-desktop-database -q "$prefix/share/applications" || true; fi
  if command -v update-mime-database > /dev/null 2>&1; then update-mime-database "$prefix/share/mime" > /dev/null 2>&1 || true; fi
  if command -v xdg-desktop-menu > /dev/null 2>&1; then xdg-desktop-menu forceupdate > /dev/null 2>&1 || true; fi
  # KDE Plasma rebuilds its menu from the desktop entries.
  for k in kbuildsycoca6 kbuildsycoca5; do
    if command -v "$k" > /dev/null 2>&1; then "$k" > /dev/null 2>&1 || true; break; fi
  done
}

if [ "$uninstall" = 1 ]; then
  [ -f "$manifest" ] || { echo "No Gridline installed by install.sh under $prefix ($manifest is missing)." >&2; exit 1; }
  # Files first, then the folders that became empty (deepest first).
  grep -v '/$' "$manifest" | while IFS= read -r f; do rm -f "$prefix/$f"; done
  rm -f "$manifest"
  grep '/$' "$manifest.dirs" 2> /dev/null | sort -r | while IFS= read -r d; do rmdir "$prefix/$d" 2> /dev/null || true; done
  rm -f "$manifest.dirs"
  rmdir "$prefix/share/gridline" 2> /dev/null || true
  refresh
  echo "Gridline removed from $prefix."
  exit 0
fi

[ -x "$here/bin/gridline-app" ] || { echo "Run install.sh from the unpacked Gridline folder (bin/gridline-app is missing)." >&2; exit 1; }
mkdir -p "$prefix/share/gridline"
: > "$manifest"
: > "$manifest.dirs"
(cd "$here" && find bin share -type d) | while IFS= read -r d; do
  [ -d "$prefix/$d" ] || { mkdir -p "$prefix/$d"; echo "$d/" >> "$manifest.dirs"; }
done
(cd "$here" && find bin share ! -type d) | while IFS= read -r f; do
  rm -f "$prefix/$f"
  cp -P "$here/$f" "$prefix/$f"
  echo "$f" >> "$manifest"
done
# Menus start Exec=gridline-app through PATH: write the full path when the folder is not on it.
case ":$PATH:" in
  *":$prefix/bin:"*) ;;
  *) sed -i.bak "s|^Exec=gridline-app|Exec=\"$prefix/bin/gridline-app\"|" "$prefix/share/applications/gridline.desktop" &&
       rm -f "$prefix/share/applications/gridline.desktop.bak" ;;
esac
# A user prefix has no icon theme index of its own: the caches are made for the system theme then.
refresh
echo "Gridline is installed in $prefix ($prefix/bin/gridline-app). Remove it with: $0 --prefix \"$prefix\" --uninstall"
