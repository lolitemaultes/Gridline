#!/usr/bin/env bash
# Keeps the published tree lean and free of private or internal material. Fails on:
#   * tracked files over 2 MB that are not in the allowlist below;
#   * .DS_Store, imgui.ini and editor / IDE files;
#   * personal home paths (/Users/<name>, /home/<name>, C:\Users\<name>) in tracked text files,
#     except obvious placeholders used by tests and docs;
#   * internal planning / coordination files (TASKS.md and friends) and analysis material.
#   tools/ci/check-tree.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

MAX_BYTES=$((2 * 1024 * 1024))
# Paths (relative to this tree) allowed to exceed MAX_BYTES. Keep it short.
ALLOW_LARGE=(
  "tests/param_audit_baseline.tsv"
)
# Placeholder user names that tests and docs may use in example paths.
PLACEHOLDER_USERS='me|somebody|someone|Someone|Someone Else|u|user|username|name|<name>|you|example|runner|Shared|Public'

fail=0
complain() { echo "check-tree: $*" >&2; fail=1; }

files=()
while IFS= read -r -d '' f; do files+=("$f"); done < <(git ls-files -z .)
[ "${#files[@]}" -gt 0 ] || { echo "check-tree: no tracked files" >&2; exit 1; }

for f in "${files[@]}"; do
  [ -f "$f" ] || continue
  base="${f##*/}"
  # Junk and editor files.
  case "$base" in
    .DS_Store|imgui.ini|Thumbs.db|desktop.ini|*.swp|*~|*.orig|*.rej)
      complain "$f: junk / editor file must not be tracked" ;;
  esac
  case "/$f/" in
    */.idea/*|*/.vscode/*|*/.vs/*|*/xcuserdata/*) complain "$f: IDE folder must not be tracked" ;;
  esac
  # Internal planning, agent notes and analysis material.
  case "$base" in
    TASKS.md|TASKS*.md|AGENTS.md|CLAUDE.md|REPORT.md|PLUGIN_MAP.md|MENU_MAP.md|DEMO_MAP.md|FEATURES.md|FL_STUDIO_STRUCTURE.md|*[Ll]edger*)
      complain "$f: internal planning file does not belong in the published tree" ;;
    *.flp|*.fst|*.wv|*.dylib|*.pkl|*.ppm)
      complain "$f: forbidden file type (FL data, binaries or raw captures)" ;;
  esac
  case "/$f" in
    */analysis/*|*/scratchpad/*|*/.claude/*) complain "$f: analysis / agent material does not belong in the published tree" ;;
  esac
  # Size.
  size=$(wc -c < "$f" | tr -d ' ')
  if [ "$size" -gt "$MAX_BYTES" ]; then
    allowed=0
    for a in "${ALLOW_LARGE[@]}"; do [ "$f" = "$a" ] && allowed=1; done
    [ "$allowed" = 1 ] || complain "$f: $size bytes is over 2 MB (add it to ALLOW_LARGE in tools/ci/check-tree.sh only if it is really needed)"
  fi
done

# Personal home paths in tracked text files (vendored code excluded).
homes=$(git grep -I -n -o -E '(/Users/|/home/|[A-Za-z]:(\\\\|\\|/)Users(\\\\|\\|/))[^/\\" )<>:]+' -- . ':!third_party' ':!tools/ci/check-tree.sh' || true)
if [ -n "$homes" ]; then
  bad=$(printf '%s\n' "$homes" | grep -v -E "(/Users/|/home/|Users(\\\\\\\\|\\\\|/))($PLACEHOLDER_USERS)\$" || true)
  if [ -n "$bad" ]; then
    printf '%s\n' "$bad" >&2
    complain "personal home paths above: use a placeholder (e.g. /Users/me) or an environment variable"
  fi
fi

# Test scripts run with "cmake -P" get no policies of their own: without cmake_minimum_required,
# CMake 3.x (Ubuntu 24.04's 3.28) keeps old behaviours newer CMake has dropped (CMP0054: a quoted
# "keep" compared as the variable keep), so a script could pass on one machine and fail on another.
for f in tests/*.cmake; do
  [ -f "$f" ] || continue
  grep -q '^cmake_minimum_required(' "$f" || complain "$f: a cmake -P test script needs cmake_minimum_required(VERSION 3.16) (the project's policies)"
done

if [ "$fail" != 0 ]; then exit 1; fi
echo "check-tree: ${#files[@]} tracked files OK"
