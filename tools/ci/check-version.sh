#!/usr/bin/env bash
# The version is set in one place, the top of CMakeLists.txt: project(VERSION x.y.z) and
# GRIDLINE_VERSION_PRE (a prerelease label such as beta.1, "" for a release; read by
# tools/release/version.sh). Everything that shows it must agree on the full version
# (0.7.0-beta.1 / 0.7.0): the README badge, a CHANGELOG.md section and the AppStream metadata. Prints the full version.
#   tools/ci/check-version.sh [expected full version]
set -euo pipefail
cd "$(dirname "$0")/../.."
eval "$(bash tools/release/version.sh CMakeLists.txt)"
ver="$full"
if [ -n "${1:-}" ] && [ "$1" != "$ver" ]; then echo "expected version $1, CMakeLists.txt says $ver" >&2; exit 1; fi
# shields.io static badges: a '-' in the text is written '--' (a single '-' separates the fields).
badge="${ver//-/--}"
grep -qF "alt=\"Version $ver\" src=\"https://img.shields.io/badge/version-$badge-" README.md ||
  { echo "README.md version badge does not say $ver (expected .../badge/version-$badge-...)" >&2; exit 1; }
# The section heading is exactly "## <version>" (so 0.7.0 does not match "## 0.7.0-beta.1").
grep -qxF "## $ver" CHANGELOG.md || { echo "CHANGELOG.md has no section ## $ver" >&2; exit 1; }
# Linux software centres, and the release builds refuse to run without it.
grep -qF "<release version=\"$ver\"" assets/linux/com.lolitemaultes.Gridline.metainfo.xml ||
  { echo "assets/linux/com.lolitemaultes.Gridline.metainfo.xml has no <release version=\"$ver\">" >&2; exit 1; }
echo "$ver"
