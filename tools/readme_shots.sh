#!/bin/sh
# Capture the README screenshots with gridline-app's off-screen renderer, then
# build docs/images with tools/make_readme_art.py.
#
#   tools/readme_shots.sh BUILD_DIR OUT_DIR [shot ...]
#
# BUILD_DIR holds gridline-app; OUT_DIR receives the raw captures (PNG) and the
# showcase project. With shot names, only those captures are retaken. The app
# runs with its own config and home folders inside OUT_DIR (no personal
# settings, no plugin scan, no preset libraries of other products), so the
# shots are the same on every machine.
set -e
G="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(cd "$1" && pwd)"
OUT="$2"
shift 2
mkdir -p "$OUT/config/gridline"
OUT="$(cd "$OUT" && pwd)"
# The browser lists Gridline's demo song folder instead of the home folders.
printf '%s\n' "$G/assets/demo" > "$OUT/config/gridline/browser-folders.txt"
export GRIDLINE_CONFIG_HOME="$OUT/config"
export GRIDLINE_NO_PLUGIN_SCAN=1
export GRIDLINE_HIDE_PLUGINS=all
mkdir -p "$OUT/home"
export HOME="$OUT/home"
APP="$BUILD/gridline-app"
PY="${PYTHON:-python3}"
SONG="$OUT/showcase.gridline.json"
"$PY" "$G/tools/readme_showcase.py" "$SONG" "$G"

want() { [ -z "$ONLY" ] || case " $ONLY " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
ONLY="$*"

shot() {  # shot NAME "VAR=value ..." args...
  name=$1
  envs=$2
  shift 2
  want "$name" || return 0
  (cd "$G" && env $envs "$APP" --hidden "$@" --fixed-dt 16 --screenshot "$OUT/$name.ppm" >/dev/null 2>&1) || true
  sips -s format png "$OUT/$name.ppm" --out "$OUT/$name.png" >/dev/null 2>&1 ||
    "$PY" -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$OUT/$name.ppm" "$OUT/$name.png"
  rm -f "$OUT/$name.ppm"
  echo "captured $name"
}

# Views: 1600x1000 window at UI scale 2 (3200x2000 captures). Patterns of the
# showcase: 0 Beat, 1 Fill, 2 Bassline, 3 Chords, 4 Lead, 5 Pad, 6 Arp;
# channels: 0-7 demo kit, 8 Lead, 9 Pad, 10 Arp, 11 Texture.
S="--size 1600x1000 --scale 2"
shot main "" "$SONG" --hide rack --play $S --frames 330
shot pr "" "$SONG" --hide rack --edit 4 8 $S --frames 400
shot chords "" "$SONG" --hide rack --edit 5 9 --show chords $S --frames 400
shot edison "GRIDLINE_EDISON_DEMO=1 GRIDLINE_EDISON_VIEW=spectral" "$SONG" --hide rack --plugin gl.edison $S --frames 400
shot autoedit "" "$SONG" --hide rack --show autoedit $S --frames 60
if want picker && [ ! -d "$OUT/config/gridline/thumbs" ]; then  # editor previews for the picker
  (cd "$G" && "$APP" --hidden --make-thumbs --size 1440x900 --frames 100000 --screenshot "$OUT/thumbs.ppm" >/dev/null 2>&1) || true
  rm -f "$OUT/thumbs.ppm"
fi
printf '20 move 54 260\n21 down 0\n22 up 0\n23 move 1500 40\n' > "$OUT/picker-synth.txt"  # sidebar: Synth
shot picker "" "$SONG" --picker $S --frames 400 --dev-input "$OUT/picker-synth.txt"
# Plasma with a factory preset; a collaborative session with a simulated
# collaborator (the panel, their cursor over the playlist).
export GL_PLASMA_PRESET="Reese Growl"  # a name with a space: exported, not passed through shot's env list
shot plasma "" "$SONG" --hide rack --plugin gl.plasma --size 1600x1100 --scale 2 --frames 90
unset GL_PLASMA_PRESET
printf '200 move 640 203\n202 down 0\n204 move 700 203\n206 move 820 203\n208 move 880 203\n210 up 0\n212 move 1590 990\n' > "$OUT/collab-panel.txt"  # the panel to the right
shot collab "" "$SONG" --hide rack --show collab-demo-panel $S --frames 400 --dev-input "$OUT/collab-panel.txt"
# Plugin editors at UI scale 2: instruments (channel window) 1600x1100, effects 1440x900.
for t in matrixfm harmor wavetable; do
  shot "ed_$t" "" "$SONG" --hide rack --plugin "gl.$t" --size 1600x1100 --scale 2 --frames 60
done
for t in eq hall amprack; do
  shot "ed_$t" "" "$SONG" --hide rack --plugin "gl.$t" --preset 2 --size 1440x900 --scale 2 --frames 60
done

# Platter with a track on each deck (the showcase song on A, the demo song's drop on B),
# both playing, hot cues set.
if want platter; then
  "$BUILD/gridline" demo "$OUT/demo.gridline.json" >/dev/null
  "$BUILD/gridline" render "$SONG" -o "$OUT/Showcase.wav" >/dev/null 2>&1
  "$BUILD/gridline" render "$OUT/demo.gridline.json" -o "$OUT/Gridline Drop.wav" --scope range:65-96 >/dev/null 2>&1
fi
export GRIDLINE_PLATTER_DEMO="$OUT/Showcase.wav" GRIDLINE_PLATTER_DEMO_B="$OUT/Gridline Drop.wav"
shot platter "GRIDLINE_PLATTER_PROFILE=none GRIDLINE_PLATTER_PLAY=AB GRIDLINE_PLATTER_HOTS=1" "$SONG" --hide rack --plugin gl.platter $S --frames 600
unset GRIDLINE_PLATTER_DEMO GRIDLINE_PLATTER_DEMO_B

# The welcome screen with a few recent projects (Gridline's own songs and templates, the
# demo song a favourite) in a config folder of its own.
WEL="$OUT/welcome"
if want welcome; then
  rm -rf "$WEL" && mkdir -p "$WEL/config/gridline" "$WEL/Projects"
  "$BUILD/gridline" demo "$WEL/Projects/Gridline.gridline.json" >/dev/null
  cp "$SONG" "$WEL/Projects/Showcase.gridline.json"
  cp "$G/tests/fixtures/test_song.gridline.json" "$WEL/Projects/First steps.gridline.json"
  cp "$G/assets/templates/Basics/Beat starter.gridline.json" "$WEL/Projects/Beat starter.gridline.json"
  cp "$G/assets/templates/Mixing/Mixing desk.gridline.json" "$WEL/Projects/Mixing desk.gridline.json"
  now=$(date +%s)
  : > "$WEL/config/gridline/recent.txt"
  : > "$WEL/config/gridline/recent-times.tsv"
  age=600
  for p in Gridline Showcase "First steps" "Beat starter" "Mixing desk"; do
    printf '%s\n' "$WEL/Projects/$p.gridline.json" >> "$WEL/config/gridline/recent.txt"
    printf '%s\t%s\n' "$((now - age))" "$WEL/Projects/$p.gridline.json" >> "$WEL/config/gridline/recent-times.tsv"
    age=$((age * 9))
  done
  printf '%s\n' "$WEL/Projects/Gridline.gridline.json" > "$WEL/config/gridline/favourite-projects.txt"
fi
export GRIDLINE_CONFIG_HOME="$WEL/config"
shot welcome "" --show launcher --size 1280x800 --scale 2 --frames 240
export GRIDLINE_CONFIG_HOME="$OUT/config"
# The welcome screen's Collaborative view with simulated collaborators.
shot collabview "" --show launcher-collab-demo $S --frames 240

# The plugin manager over a plugin cache that holds only Gridline's own builds (the
# in-tree test plugins in VST3, CLAP, VST2 and LV2, Gridline as a plugin, the Gridline
# Suite), copied into the plug-in folders of the shots' own home folder (macOS layout).
PLUG="$HOME/Library/Audio/Plug-Ins"
if want plugins; then
  rm -rf "$OUT/scan" "$PLUG" && mkdir -p "$OUT/scan" "$PLUG/VST3" "$PLUG/CLAP" "$PLUG/VST" "$PLUG/LV2"
  for e in "CLAP test-clap/GridlineTest.clap" "VST3 test-vst3/GridlineTest.vst3" "VST test-vst2/GridlineTestVst2.vst" \
           "LV2 test-lv2/GridlineTest.lv2" "CLAP plugins/Gridline.clap" "VST3 plugins/Gridline.vst3" \
           "CLAP plugins/Gridline Suite.clap" "VST3 plugins/Gridline Suite.vst3" \
           "CLAP plugins/Gridline Suite Mini.clap" "VST3 plugins/Gridline Suite Mini.vst3"; do
    [ -e "$BUILD/${e#* }" ] && cp -R "$BUILD/${e#* }" "$PLUG/${e%% *}/"
  done
  n=0
  for d in "$PLUG"/*/*; do
    case "$d" in */VST3/*) f=vst3 ;; */CLAP/*) f=clap ;; */VST/*) f=vst2 ;; */LV2/*) f=lv2 ;; esac
    n=$((n + 1))
    "$BUILD/gridline" --scan-plugin "$f" "$d" --out "$OUT/scan/$n.json" >/dev/null 2>&1 || true
  done
  "$PY" "$G/tools/readme_scan_cache.py" "$OUT/scan" "$OUT/config/gridline/plugin-cache.json"
fi
shot plugins "GRIDLINE_HIDE_PLUGINS= GRIDLINE_NO_SUITE_INSTALL=1" "$SONG" --hide rack --show plugins $S --frames 120
if want plugins; then rm -rf "$OUT/config/gridline/plugin-cache.json" "$PLUG"; fi

# Seltmann with two bars of the showcase song's beat chopped, the sequence lane under the waveform.
if want seltmann; then
  "$BUILD/gridline" render "$SONG" -o "$OUT/Beat.wav" --scope pattern:1 --loops 2 --tail cut >/dev/null 2>&1
fi
export GRIDLINE_SELTMANN_DEMO="$OUT/Beat.wav"
shot seltmann "" "$SONG" --hide rack --plugin gl.seltmann $S --frames 400
unset GRIDLINE_SELTMANN_DEMO
