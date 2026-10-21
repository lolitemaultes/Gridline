#!/usr/bin/env bash
# The version is set in one place, project(VERSION) in CMakeLists.txt; everything that shows it
# must agree: the README badge and a CHANGELOG.md section. Prints the version.
#   tools/ci/check-version.sh [expected version]
set -euo pipefail
cd "$(dirname "$0")/../.."
ver="$(sed -n 's/^project(gridline VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)"
[ -n "$ver" ] || { echo "no project(VERSION) in CMakeLists.txt" >&2; exit 1; }
if [ -n "${1:-}" ] && [ "$1" != "$ver" ]; then echo "expected version $1, CMakeLists.txt says $ver" >&2; exit 1; fi
grep -q "alt=\"Version $ver\" src=\"https://img.shields.io/badge/version-$ver-" README.md ||
  { echo "README.md version badge does not say $ver" >&2; exit 1; }
grep -q "^## $ver\b" CHANGELOG.md || { echo "CHANGELOG.md has no section ## $ver" >&2; exit 1; }
echo "$ver"
