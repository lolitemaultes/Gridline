#!/bin/sh
# Debian 11 (bullseye) after the end of its long-term support: its security updates moved from the
# mirrors to archive.debian.org (deb.debian.org still lists them but no longer has the files), and
# the rest of it follows there later. As root on a Debian 11 system or container, this points apt
# at archive.debian.org for whatever the mirrors no longer serve, then runs apt-get update. Does
# nothing on other systems.
#   tools/ci/debian-archive.sh
set -eu
grep -q '^11\.' /etc/debian_version 2>/dev/null || exit 0
files="$(ls /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources 2>/dev/null || true)"
[ -n "$files" ] || exit 0
# The archive's Release files are no longer renewed; apt would call them expired.
echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99gridline-debian-archive
# shellcheck disable=SC2086 # (a list of file names without spaces)
sed -i -E 's#https?://(deb|security)\.debian\.org/debian-security/?#http://archive.debian.org/debian-security#' $files
log="$(mktemp)"
apt-get update -q > "$log" 2>&1 || true
if grep -Eq 'deb\.debian\.org/debian bullseye.*(404|Release file)' "$log"; then
  echo "debian-archive: the mirrors no longer serve bullseye; using archive.debian.org"
  # shellcheck disable=SC2086
  sed -i -E 's#https?://deb\.debian\.org/debian/?( |$)#http://archive.debian.org/debian\1#' $files
  # shellcheck disable=SC2086
  sed -i -E '/bullseye-updates/s/^/# /' $files  # (folded into bullseye in the archive)
  apt-get update -q > "$log" 2>&1
fi
rm -f "$log"
echo "debian-archive: Debian $(cat /etc/debian_version), apt sources:"
# shellcheck disable=SC2086
grep -hE '^(deb|URIs)' $files | sed 's/^/  /'
