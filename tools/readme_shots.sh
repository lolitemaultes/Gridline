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
# The browser lists Gridline's demo folder instead of the home folders.
printf '%s\n' "$G/demo" > "$OUT/config/gridline/browser-folders.txt"
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

# Release shots (docs/images/release-<version>/): the playlist with the song loop
# marker, clip volume lines and crossfades; the channel rack at 64 steps; effect
# editors with their live graphics while the song plays; the audio clip menu
# with its time stretch modes.
shot rel_playlist "" "$SONG" --show playlist --play $S --frames 330
shot rel_rack "" "$SONG" --rack steps64 --hide browser $S --frames 120
for t in reverb delay limiter; do
  shot "rel_$t" "" "$SONG" --hide rack --plugin "gl.$t" --play --size 1440x900 --scale 2 --frames 700
done
printf '30 move 150 106\n50 move 330 106\n60 move 362 140\n70 move 362 166\n' > "$OUT/stretch-hover.txt"  # open "Time stretch", hover Transient
shot rel_stretch "" "$SONG" --show playlist --menu audioclip $S --frames 150 --dev-input "$OUT/stretch-hover.txt"
