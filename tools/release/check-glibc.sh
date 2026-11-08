#!/usr/bin/env bash
# Fails when a program or library in the given folders needs a newer GNU C library than <version>:
# the Linux release is built on Debian 11 so that it runs on glibc 2.31 and later. Needs objdump.
#   tools/release/check-glibc.sh <version, e.g. 2.31> <folder>...
set -euo pipefail
max="${1:?the newest glibc version allowed}"
shift
bad=0
checked=0
newest=0
while IFS= read -r -d '' f; do
  case "$(head -c 4 "$f" | od -An -c | tr -d ' ')" in *177ELF*) ;; *) continue ;; esac
  need="$(objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)"
  checked=$((checked + 1))
  [ -n "$need" ] || continue
  if [ "$(printf '%s\n%s\n' "$newest" "$need" | sort -V | tail -1)" = "$need" ]; then newest="$need"; fi
  if [ "$(printf '%s\n%s\n' "$max" "$need" | sort -V | tail -1)" != "$max" ]; then
    echo "check-glibc: $f needs glibc $need (more than $max)"
    bad=1
  fi
done < <(find "$@" -type f -print0)
[ "$checked" -gt 0 ] || { echo "check-glibc: no programs found in $*" >&2; exit 1; }
[ "$bad" = 0 ] || exit 1
echo "check-glibc: $checked programs and libraries, the newest glibc any of them needs is $newest (at most $max)"
