#!/usr/bin/env bash
# Makes a Docker image of the container a Chromebook's Linux development environment installs
# (Google's own image for this machine's architecture, newest ChromeOS milestone), tagged
# gridline-crostini. Needs docker, python3 and squashfs-tools (unsquashfs); run as a user that
# may use sudo (the root file system keeps its owners). With a Debian release name (bullseye) it
# makes the image of that release instead, as on a Chromebook whose Linux was set up years ago.
#   tools/ci/crostini-docker.sh [work folder] [Debian release]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
work="${1:-${RUNNER_TEMP:-/tmp}/crostini}"
release="${2:-}"
case "$(uname -m)" in
  x86_64) arch=amd64 ;;
  aarch64 | arm64) arch=arm64 ;;
  *) echo "crostini-docker: no ChromeOS image for $(uname -m)" >&2; exit 1 ;;
esac
mkdir -p "$work"
python3 "$here/crostini_image.py" --arch "$arch" ${release:+--release "$release"} --out "$work/rootfs.squashfs" | tee "$work/image.txt"
sudo rm -rf "$work/root"
sudo unsquashfs -q -no-xattrs -d "$work/root" "$work/rootfs.squashfs" > /dev/null
rm -f "$work/rootfs.squashfs"
# In a real Chromebook the VM mounts these; inside Docker they come from the run options.
sudo rm -f "$work/root/etc/resolv.conf"
sudo tar -C "$work/root" -c . | docker import - gridline-crostini > /dev/null
sudo rm -rf "$work/root"
docker run --rm gridline-crostini sh -c 'echo "image: Debian $(cat /etc/debian_version), $(dpkg --print-architecture)"; dpkg -s cros-guest-tools | grep -E "^(Package|Version)"'
