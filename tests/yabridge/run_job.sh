#!/usr/bin/env bash
# One test job: starts the display under test, sets up the plugins, runs the harness.
# Usage: run_job.sh <gridline-app> <gridline> <probes> <downloads> <out> <wine label> <display> <scale> <run> [harness options]
set -uo pipefail
APP="$1"; CLI="$2"; PROBES="$3"; DL="$4"; OUT="$5"; WINE_LABEL="$6"; DISPLAY_KIND="$7"; SCALE="$8"; RUN="$9"
shift 9
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
# Short, as on a desktop (/run/user/1000): yabridge makes Unix sockets in here, named after the
# plugin, and a socket path may hold only 107 characters ("File name too long" otherwise).
XDG_RUNTIME_DIR="$(mktemp -d /tmp/xdgXXXX)"
export XDG_RUNTIME_DIR
chmod 700 "$XDG_RUNTIME_DIR"
pids=()
cleanup() { for p in "${pids[@]:-}"; do [ -n "$p" ] && kill "$p" 2> /dev/null; done; }
trap cleanup EXIT

start_xvfb() {
  Xvfb :77 -noreset -screen 0 1920x1200x24 +extension GLX +extension RANDR +extension XTEST > "$OUT/xvfb.log" 2>&1 &
  pids+=($!)
  export DISPLAY=:77
  for _ in $(seq 1 50); do xdpyinfo > /dev/null 2>&1 && return 0; sleep 0.2; done
  echo "Xvfb did not start"; return 1
}

start_weston() {
  weston --backend=headless --width=1920 --height=1200 --xwayland --socket=wl-gl --idle-time=0 > "$OUT/weston.log" 2>&1 &
  pids+=($!)
  export WAYLAND_DISPLAY=wl-gl
  for _ in $(seq 1 50); do [ -S "$XDG_RUNTIME_DIR/wl-gl" ] && break; sleep 0.2; done
  # Weston starts XWayland on the first connection to the socket it listens on.
  local before n
  before="$(ls /tmp/.X11-unix 2> /dev/null | tr '\n' ' ')"
  n="$(sed -n 's/.*xserver listening on display \(:[0-9]*\).*/\1/p' "$OUT/weston.log" | head -1)"
  if [ -z "$n" ]; then n=":$(ls /tmp/.X11-unix 2> /dev/null | sed 's/^X//' | sort -n | tail -1)"; fi
  export DISPLAY="$n"
  echo "weston: DISPLAY=$DISPLAY (sockets: $before)"
  for _ in $(seq 1 100); do xdpyinfo > /dev/null 2>&1 && break; sleep 0.2; done
  xdpyinfo > /dev/null 2>&1 || { echo "XWayland did not answer"; cat "$OUT/weston.log"; return 2; }
}

NO_INPUT=""
case "$DISPLAY_KIND" in
  xvfb-openbox)
    start_xvfb || exit 2
    openbox --sm-disable > "$OUT/wm.log" 2>&1 &
    pids+=($!)
    sleep 1
    ;;
  xvfb-bare)
    start_xvfb || exit 2
    ;;
  weston-x11 | weston-wayland)
    start_weston || exit 2
    if [ "$DISPLAY_KIND" = weston-x11 ]; then
      # Does XTEST work on this XWayland at all? A plain X client and a click, with neither Gridline nor Wine.
      xmessage -geometry 300x200+100+100 check > /dev/null 2>&1 &
      check_pid=$!
      sleep 2
      xdotool mousemove 150 150 click 1 >> "$OUT/xwayland-check.log" 2>&1
      sleep 2
      kill "$check_pid" 2> /dev/null
      # Weston starts a new XWayland on demand after one dies, so a display that answers now proves nothing:
      # the proof of a crash is Weston's own log line and xdotool's broken connection.
      if ! grep -q "died on signal" "$OUT/weston.log" && ! grep -q "connection to .* broken" "$OUT/xwayland-check.log"; then
        echo "XWayland takes XTEST input from a plain client" >> "$OUT/xwayland-check.log"
      else
        cp "$OUT/weston.log" "$OUT/weston-xtest-crash.log"
        echo "XWayland died on an XTEST click on a plain client (xmessage)" >> "$OUT/xwayland-check.log"
        NO_INPUT="this XWayland dies on an XTEST click on a plain X client, with neither Gridline nor Wine (see weston-xtest-crash.log)"
        for p in "${pids[@]:-}"; do [ -n "$p" ] && kill "$p" 2> /dev/null; done
        pids=()
        pkill -x Xwayland 2> /dev/null
        sleep 1
        start_weston || exit 2
      fi
    fi
    [ "$DISPLAY_KIND" = weston-wayland ] && export SDL_VIDEODRIVER=wayland
    ;;
  *)
    echo "unknown display $DISPLAY_KIND"; exit 2 ;;
esac

if [ "$SCALE" != 1 ]; then
  echo "Xft.dpi: $((96 * SCALE))" | xrdb -merge
fi

bash "$HERE/setup_plugins.sh" "$PROBES" "$DL" > "$OUT/setup.log" 2>&1
setup_rc=$?
if [ "$setup_rc" != 0 ]; then
  echo "plugin setup failed ($setup_rc)"
  tail -30 "$OUT/setup.log"
  printf '{"wine":"%s","display":"%s","scale":"%s","run":"%s","host":"gridline","format":"all","mode":"-","plugin":"setup","check":"Wine prefix and yabridge setup","status":"fail","detail":"exit %s"}\n' \
    "$WINE_LABEL" "$DISPLAY_KIND" "$SCALE" "$RUN" "$setup_rc" >> "$OUT/results.jsonl"
  exit 0
fi

export WINEDLLOVERRIDES="mscoree=d;mshtml=d"
export WINEFSYNC=0
wine --version > "$OUT/wine-version.txt" 2>&1
python3 "$HERE/harness.py" --app "$APP" --cli "$CLI" --out "$OUT" --wine "$WINE_LABEL" --display "$DISPLAY_KIND" --scale "$SCALE" --run "$RUN" --no-input "$NO_INPUT" "$@"
rc=$?
timeout 30 wineserver -k 2> /dev/null || true
if [ "$rc" != 0 ]; then
  printf '{"wine":"%s","display":"%s","scale":"%s","run":"%s","host":"gridline","format":"all","mode":"-","plugin":"harness","check":"harness finished","status":"fail","detail":"exit %s"}\n' \
    "$WINE_LABEL" "$DISPLAY_KIND" "$SCALE" "$RUN" "$rc" >> "$OUT/results.jsonl"
fi
exit 0
