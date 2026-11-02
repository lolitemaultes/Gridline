#!/usr/bin/env bash
# Installs the Wine under test on Ubuntu 24.04, with the system tools the test needs.
# Usage: install_wine.sh distro | staging:<version> (for example staging:9.21)
set -euo pipefail
WHICH="$1"
export DEBIAN_FRONTEND=noninteractive
sudo dpkg --add-architecture i386
if [ "$WHICH" != distro ]; then
  sudo mkdir -pm755 /etc/apt/keyrings
  curl -fsSL https://dl.winehq.org/wine-builds/winehq.key | sudo gpg --batch --yes --dearmor -o /etc/apt/keyrings/winehq-archive.key
  echo "deb [arch=amd64,i386 signed-by=/etc/apt/keyrings/winehq-archive.key] https://dl.winehq.org/wine-builds/ubuntu/ noble main" | sudo tee /etc/apt/sources.list.d/winehq.list > /dev/null
fi
sudo apt-get update -qq
TOOLS=(xvfb openbox weston xwayland xdotool wmctrl x11-utils x11-xserver-utils x11-apps imagemagick libgl1-mesa-dri libglx-mesa0 xauth carla)
sudo apt-get install -y -qq --no-install-recommends "${TOOLS[@]}" > /dev/null
if [ "$WHICH" = distro ]; then
  sudo apt-get install -y -qq --no-install-recommends wine64 wine > /dev/null
else
  V="${WHICH#staging:}~noble-1"
  sudo apt-get install -y -qq --no-install-recommends --allow-downgrades \
    "winehq-staging=$V" "wine-staging=$V" "wine-staging-amd64=$V" "wine-staging-i386=$V" > /dev/null
  # Pin it: nothing later may move it.
  sudo apt-mark hold winehq-staging wine-staging wine-staging-amd64 wine-staging-i386 > /dev/null
fi
wine --version
