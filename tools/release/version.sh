#!/usr/bin/env bash
# Prints the forms of Gridline's version as shell assignments (release.yml, check-version.sh):
#   numeric=0.7.0 pre=beta.1 full=0.7.0-beta.1 deb=0.7.0~beta.1 rpm_release=0.1.beta.1
# The version is set in one place, the top of CMakeLists.txt: project(gridline VERSION x.y.z) and
# set(GRIDLINE_VERSION_PRE "<label>") ("" for a release). Same rules as cmake/GridlineVersion.cmake.
#   eval "$(bash tools/release/version.sh [CMakeLists.txt])"
set -euo pipefail
file="${1:-$(cd "$(dirname "$0")/../.." && pwd)/CMakeLists.txt}"
numeric="$(sed -n 's/^project(gridline VERSION \([0-9][0-9.]*\).*/\1/p' "$file")"
echo "$numeric" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo "no project(gridline VERSION x.y.z) in $file" >&2; exit 1; }
grep -Eq '^set\(GRIDLINE_VERSION_PRE "[0-9A-Za-z.]*"\)' "$file" || { echo "no set(GRIDLINE_VERSION_PRE \"...\") in $file" >&2; exit 1; }
pre="$(sed -n 's/^set(GRIDLINE_VERSION_PRE "\([0-9A-Za-z.]*\)").*/\1/p' "$file" | head -n 1)"
if [ -z "$pre" ]; then
  full="$numeric" deb="$numeric" rpm_release=1
else
  echo "$pre" | grep -Eq '^[0-9A-Za-z]+(\.[0-9A-Za-z]+)*$' || { echo "bad prerelease label '$pre'" >&2; exit 1; }
  full="$numeric-$pre" deb="$numeric~$pre" rpm_release="0.1.$pre"
fi
echo "numeric=$numeric"
echo "pre=$pre"
echo "full=$full"
echo "deb=$deb"
echo "rpm_release=$rpm_release"
