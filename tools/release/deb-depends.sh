#!/usr/bin/env bash
# The Linux .deb is built on Debian 11, so its Depends (from dpkg-shlibdeps) name Debian 11's
# library packages. Debian 13 and Ubuntu 24.04 renamed some of them for the 64-bit time change
# (libasound2 is libasound2t64 there). This adds the new name as an alternative to each, with the
# same version bound, so the .deb installs on old and new systems without leaning on a "Provides"
# that a later release may drop. Rewrites the .deb in place; needs dpkg-deb and perl.
#   tools/release/deb-depends.sh <.deb>...
set -euo pipefail
# old name -> name in Debian 13 / Ubuntu 24.04
renamed=(
  "libasound2 libasound2t64"
  "libcurl4 libcurl4t64"
  "libcurl3-gnutls libcurl3t64-gnutls"
)
for deb in "$@"; do
  deb="$(readlink -f "$deb")"
  tmp="$(mktemp -d)"
  dpkg-deb -R "$deb" "$tmp/p"
  ctl="$tmp/p/DEBIAN/control"
  for pair in "${renamed[@]}"; do
    read -r old new <<< "$pair"
    # "old (>= v)" or "old" as a whole alternative, unless the new name is already there.
    OLD="$old" NEW="$new" perl -i -pe '
      if (/^(Pre-)?Depends:/ && !/\Q$ENV{NEW}\E/) {
        s/(?<=[:,|]\s)\Q$ENV{OLD}\E(\s*\([^)]*\))?(?=\s*(,|\||$))/"$ENV{OLD}$1 | $ENV{NEW}$1"/ge;
      }' "$ctl"
  done
  case "$(ar t "$deb" | grep '^data\.tar')" in
    *.xz) z=xz ;; *.zst) z=zstd ;; *.bz2) z=bzip2 ;; *.tar) z=none ;; *) z=gzip ;;
  esac
  dpkg-deb --root-owner-group -Z"$z" -b "$tmp/p" "$deb" > /dev/null
  rm -rf "$tmp"
  echo "deb-depends: $(basename "$deb"): $(dpkg-deb -f "$deb" Depends)"
done
