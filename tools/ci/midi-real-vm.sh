#!/bin/sh
# The real-device MIDI test (tools/ci/midi-real.py) in a virtual machine, for Linux runners whose own
# kernel has no sound modules: GitHub's runners use Ubuntu's azure kernel, which is built without
# sound, so neither the ALSA sequencer (snd-seq) nor the virtual MIDI device (snd-virmidi) exists
# there, and no package provides them. This builds gridline-app and the user-space port helper here,
# boots the cloud image of the same Ubuntu release (its generic kernel has the sound modules) under
# KVM, copies the binaries, the assets and the test in, and runs the test there with real kernel
# modules loaded and unloaded. Run from the source root (.github/workflows/midi-real.yml).
#   GRIDLINE_MIDI_REAL_ARGS   options for midi-real.py (--require-kernel)
# Writes midi-real-out/summary.md, the app logs, the machine's console log and midi-real-out/status.
# Exit: 0 every check passed, 1 a check failed or the build failed, 3 the virtual machine could not
# be used here (no KVM, no image, no network): nothing of real ALSA was exercised; the summary says why.
set -u
OUT=midi-real-out
BUILD=build-midi
VM=vm-work
PORT=2222
ARGS="${GRIDLINE_MIDI_REAL_ARGS:-}"
rm -rf "$OUT" "$VM"
mkdir -p "$OUT" "$VM"
HOST_KERNEL="$(uname -r)"
TITLE="## MIDI through the real back end (ALSA, Linux, in a virtual machine)"

# Exit 3 with a summary saying what could not be done and that nothing was exercised.
unavailable() {
  {
    printf '%s\n\n' "$TITLE"
    printf 'NOT TESTED: %s\n\n' "$1"
    printf "This runner's kernel (%s) has no sound modules, and the virtual machine with Ubuntu's generic kernel could not be used, " "$HOST_KERNEL"
    printf 'so nothing of real ALSA MIDI was exercised on this run: no sequencer, no kernel MIDI device plugged in or out, no user-space port.\n'
  } > "$OUT/summary.md"
  echo 3 > "$OUT/status"
  echo "midi-real-vm: $1" >&2
  stop_vm
  exit 3
}

stop_vm() {
  if [ -f "$VM/qemu.pid" ]; then
    sudo kill "$(cat "$VM/qemu.pid")" 2> /dev/null || true
    rm -f "$VM/qemu.pid"
  fi
}

# 1. The binaries, built on this runner (the machine runs the same Ubuntu release, so they run there).
if ! cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGRIDLINE_GUI_TESTS=OFF > "$OUT/configure.log" 2>&1; then
  cat "$OUT/configure.log" >&2
  printf '%s\n\nFAIL: CMake configure failed.\n' "$TITLE" > "$OUT/summary.md"
  echo 1 > "$OUT/status"
  exit 1
fi
if ! cmake --build "$BUILD" -j"$(nproc 2> /dev/null || echo 2)" --target gridline-app midi-seq-port; then
  printf '%s\n\nFAIL: the build failed.\n' "$TITLE" > "$OUT/summary.md"
  echo 1 > "$OUT/status"
  exit 1
fi

# 2. The virtual machine: KVM, QEMU, the cloud image of this runner's Ubuntu release.
[ -e /dev/kvm ] || unavailable "this runner has no KVM (/dev/kvm)"
sudo chmod a+rw /dev/kvm || unavailable "/dev/kvm cannot be opened"
# shellcheck disable=SC2024  # (the log is the runner user's)
if ! sudo apt-get install -y -q --no-install-recommends qemu-system-x86 qemu-utils cloud-image-utils openssh-client > "$OUT/vm-packages.log" 2>&1; then
  unavailable "QEMU could not be installed (see vm-packages.log)"
fi
# shellcheck disable=SC1091  # (the runner's own file)
CODENAME="$(. /etc/os-release && echo "${VERSION_CODENAME:-}")"
[ -n "$CODENAME" ] || unavailable "the runner's Ubuntu release is not known (/etc/os-release)"
IMAGE_URL="https://cloud-images.ubuntu.com/$CODENAME/current/$CODENAME-server-cloudimg-amd64.img"
ok=0
for _ in 1 2 3; do
  if curl -fsSL --retry 3 -o "$VM/base.img" "$IMAGE_URL"; then ok=1 && break; fi
  sleep 10
done
[ "$ok" = 1 ] || unavailable "the cloud image could not be downloaded ($IMAGE_URL)"
# The image's published checksum, from the same place.
if curl -fsSL -o "$VM/SHA256SUMS" "https://cloud-images.ubuntu.com/$CODENAME/current/SHA256SUMS"; then
  want="$(grep " \*\?$CODENAME-server-cloudimg-amd64.img\$" "$VM/SHA256SUMS" | cut -d' ' -f1)"
  have="$(sha256sum "$VM/base.img" | cut -d' ' -f1)"
  [ -n "$want" ] || unavailable "the cloud image's checksum is not in its SHA256SUMS"
  [ "$want" = "$have" ] || unavailable "the cloud image does not match its published checksum"
else
  unavailable "the cloud image's checksums could not be downloaded"
fi
qemu-img create -q -f qcow2 -F qcow2 -b "$PWD/$VM/base.img" "$VM/disk.qcow2" 20G || unavailable "qemu-img could not make the disk"
ssh-keygen -q -t ed25519 -N "" -f "$VM/key" || unavailable "ssh-keygen failed"
cat > "$VM/user-data" << EOF
#cloud-config
ssh_authorized_keys:
  - $(cat "$VM/key.pub")
EOF
printf 'instance-id: gridline-midi\nlocal-hostname: gridline-midi\n' > "$VM/meta-data"
cloud-localds "$VM/seed.img" "$VM/user-data" "$VM/meta-data" || unavailable "cloud-localds could not make the seed"
sudo qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 4096 -display none -monitor none \
  -drive "file=$VM/disk.qcow2,if=virtio,format=qcow2" -drive "file=$VM/seed.img,if=virtio,format=raw" \
  -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:22" -device virtio-net-pci,netdev=n0 \
  -serial "file:$OUT/vm-console.log" -daemonize -pidfile "$VM/qemu.pid" || unavailable "QEMU did not start"
sudo chmod a+r "$VM/qemu.pid" "$OUT/vm-console.log" 2> /dev/null || true

vm() {
  ssh -q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o ServerAliveInterval=15 \
    -i "$VM/key" -p "$PORT" ubuntu@127.0.0.1 "$@"
}
up=0
for _ in $(seq 1 60); do
  if vm true 2> /dev/null; then up=1 && break; fi
  sleep 5
done
[ "$up" = 1 ] || unavailable "the virtual machine did not answer on SSH within 5 minutes (see vm-console.log)"
vm cloud-init status --wait > /dev/null 2>&1 || true
VM_KERNEL="$(vm uname -r)"
echo "midi-real-vm: the virtual machine runs kernel $VM_KERNEL"

# 3. What the test needs, copied in at the same places relative to the source root.
tar -cf - "$BUILD/gridline-app" "$BUILD/midi-seq-port" assets tools/ci tests/fixtures/test_song.gridline.json |
  vm 'mkdir -p g && tar -xf - -C g' || unavailable "the files could not be copied into the virtual machine"
# Run-time libraries, Xvfb, the ALSA tools, and the kernel's sound modules (a separate package on some
# releases). The image's own package timers stay out of the way, and apt waits for a lock held anyway.
vm 'sudo systemctl stop unattended-upgrades.service apt-daily.timer apt-daily-upgrade.timer apt-daily.service apt-daily-upgrade.service; echo "DPkg::Lock::Timeout \"600\";" | sudo tee /etc/apt/apt.conf.d/90gridline-wait' > "$OUT/vm-deps.log" 2>&1 || true
APT_ENV='sudo env DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=a'
if ! vm "cd g && $APT_ENV sh tools/ci/install-deps.sh apt && $APT_ENV apt-get install -y -q --no-install-recommends alsa-utils kmod python3 xvfb xauth" >> "$OUT/vm-deps.log" 2>&1; then
  unavailable "the packages could not be installed in the virtual machine (see vm-deps.log)"
fi
vm "$APT_ENV apt-get install -y -q --no-install-recommends \"linux-modules-extra-\$(uname -r)\"" >> "$OUT/vm-deps.log" 2>&1 || true
if ! vm 'sudo modprobe snd-seq && test -e /dev/snd/seq' > "$OUT/vm-modules.log" 2>&1; then
  unavailable "the virtual machine's kernel ($VM_KERNEL) has no ALSA sequencer either (see vm-modules.log)"
fi

# 4. The test, in the machine. From here on a failure is the test's.
vm "cd g && GRIDLINE_MIDI_REAL=1 GRIDLINE_NO_PLUGIN_SCAN=1 GRIDLINE_NO_REALTIME=1 CI=true GITHUB_ACTIONS=true \
  xvfb-run -a -s '-screen 0 1600x1000x24' python3 tools/ci/midi-real.py --app $BUILD/gridline-app \
  --demo tests/fixtures/test_song.gridline.json --out midi-real-out --helper $BUILD/midi-seq-port $ARGS"
status=$?
mkdir -p "$VM/back"
vm 'tar -cf - -C g midi-real-out' | tar -xf - -C "$VM/back" 2> /dev/null || true
cp -R "$VM/back/midi-real-out/." "$OUT/" 2> /dev/null || true
if [ -f "$OUT/summary.md" ]; then
  note="Ran in a virtual machine with Ubuntu's generic kernel $VM_KERNEL: this runner's own kernel ($HOST_KERNEL) has no sound modules."
  { sed -n '1p' "$OUT/summary.md"; printf '\n%s\n' "$note"; sed '1d' "$OUT/summary.md"; } > "$OUT/summary.tmp" && mv "$OUT/summary.tmp" "$OUT/summary.md"
else
  printf '%s\n\nFAIL: the test wrote no summary (exit %s).\n' "$TITLE" "$status" > "$OUT/summary.md"
  [ "$status" = 0 ] && status=1
fi
echo "$status" > "$OUT/status"
stop_vm
echo "midi-real-vm: exit $status"
exit "$status"
