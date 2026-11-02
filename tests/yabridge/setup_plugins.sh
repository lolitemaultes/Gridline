#!/usr/bin/env bash
# Creates the Wine prefix, installs the probes and the real plugins in it, and bridges them.
# Run on a display (the prefix's first start needs one).
# Usage: setup_plugins.sh <probe folder> <downloads folder, may be empty>
set -euo pipefail
PROBES="$1"
DL="${2:-}"
export WINEDLLOVERRIDES="mscoree=d;mshtml=d"
export WINEDEBUG="-all"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine}"
timeout 240 wineboot -i
timeout 120 wineserver -w || true
C="$WINEPREFIX/drive_c/Program Files"
V3="$C/Common Files/VST3"
CL="$C/Common Files/CLAP"
V2="$C/Steinberg/VstPlugins"
mkdir -p "$V3/GridlineProbe.vst3/Contents/x86_64-win" "$CL" "$V2"
cp "$PROBES/GridlineProbe.vst3" "$V3/GridlineProbe.vst3/Contents/x86_64-win/GridlineProbe.vst3"
cp "$PROBES/GridlineProbe.clap" "$CL/"
cp "$PROBES/GridlineProbeSynth.dll" "$PROBES/GridlineProbeFx.dll" "$V2/"

if [ -n "$DL" ] && [ -d "$DL" ]; then
  T="$(mktemp -d)"
  unzip -q "$DL/Dexed-1.1.0-win.zip" -d "$T/dexed"
  cp -r "$T/dexed/Dexed.vst3" "$V3/"
  cp "$T/dexed/Dexed.clap" "$CL/"
  unzip -q "$DL/surge-xt-win64-1.3.4-pluginsonly.zip" -d "$T/surge"
  cp -r "$T/surge/Surge XT.vst3" "$V3/"
  cp "$T/surge/Surge XT.clap" "$CL/"
  # DISTRHO's plugin set: a different toolkit (OpenGL through pugl, not JUCE).
  unzip -q "$DL/DPF-Plugins-v1.7-win64.zip" -d "$T/dpf"
  find "$T/dpf" -maxdepth 4 -iname 'Kars*' | head -20
  while IFS= read -r -d '' p; do
    case "$p" in
      *.vst3) cp -r "$p" "$V3/" ;;
      *.clap) cp "$p" "$CL/" ;;
      *.dll) cp "$p" "$V2/" ;;
    esac
  done < <(find "$T/dpf" \( -name 'Kars*.vst3' -o -name 'Kars*.clap' -o -name 'Kars*.dll' -o -name 'Nekobi*.vst3' -o -name 'Nekobi*.clap' -o -name 'Nekobi*.dll' \) -not -path '*/Contents/*' -print0)
  rm -rf "$T"
fi

YB="$HOME/.local/share/yabridge/yabridgectl"
"$YB" add "$V3"
"$YB" add "$CL"
"$YB" add "$V2"
"$YB" sync --no-verify
"$YB" status
ls -R "$HOME/.vst3/yabridge" | head -30
ls -R "$HOME/.clap/yabridge" | head -20
ls -R "$HOME/.vst/yabridge" | head -20
timeout 60 wineserver -k || true
