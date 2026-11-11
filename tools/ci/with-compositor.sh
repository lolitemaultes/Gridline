#!/usr/bin/env bash
# Runs a command on a headless Wayland compositor of its own (CI and local checks).
#
#   tools/ci/with-compositor.sh <weston|mutter|kwin> <xwayland|native> [--scale N] [--outputs N] [--size WxH] -- command...
#
#   xwayland  the compositor runs XWayland and DISPLAY points at it: Gridline's default on a Wayland
#             desktop (X11 through XWayland). SDL_VIDEODRIVER=wayland is set as well, as desktops set
#             it for games: Gridline must still pick X11.
#   native    no XWayland and no DISPLAY: Gridline on Wayland itself, and anything that reaches for
#             X11 fails.
#
# The compositor gets a runtime folder of its own (never the session's), and it stops with the command.
# Environment: WESTON (weston binary), WESTON_CONFIG (a weston.ini), for an unpacked Weston.
# WESTON_BACKEND=x11 runs Weston in a window of the X server in WESTON_X11_DISPLAY (an Xvfb of its own)
# instead of headless: its seat has a keyboard and a pointer (the headless backend has no seat, and
# SDL 2.0.20 crashes on a compositor without one). The command still gets no DISPLAY in native mode.
set -eEuo pipefail
# Whatever stops the script says so (a bare exit code in a CI log tells nothing).
trap 'rc=$?; echo "with-compositor: stopped at line $LINENO with exit code $rc: $BASH_COMMAND" >&2' ERR

usage() { sed -n '2,13p' "$0" >&2; exit 2; }
[ $# -ge 3 ] || usage
compositor=$1 mode=$2
shift 2
scale=1 outputs=1 size=2560x1700
while [ $# -gt 0 ]; do
  case $1 in
    --scale) scale=$2; shift 2 ;;
    --outputs) outputs=$2; shift 2 ;;
    --size) size=$2; shift 2 ;;
    --) shift; break ;;
    *) usage ;;
  esac
done
[ $# -gt 0 ] || usage
case $mode in xwayland|native) ;; *) usage ;; esac
width=${size%x*} height=${size#*x}

work=$(mktemp -d "${TMPDIR:-/tmp}/gl-wl-XXXXXX")
export XDG_RUNTIME_DIR=$work/run
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
socket=gl-wayland-$$
log=$work/compositor.log
if [ "${WESTON_BACKEND:-}" = x11 ]; then WESTON_X11_DISPLAY=${WESTON_X11_DISPLAY:-${DISPLAY:-}}; fi  # (xvfb-run's display)
unset DISPLAY WAYLAND_DISPLAY
pid=
cleanup() {
  if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
  if [ -n "${COMPOSITOR_LOG_OUT:-}" ]; then cp "$log" "$COMPOSITOR_LOG_OUT" 2>/dev/null || true; fi
  rm -rf "$work"
}
trap cleanup EXIT

wait_for_socket() {
  for _ in $(seq 1 300); do
    [ -S "$XDG_RUNTIME_DIR/$socket" ] && return 0
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then break; fi
    sleep 0.1
  done
  echo "with-compositor: $compositor did not start" >&2
  tail -40 "$log" >&2 || true
  exit 1
}

wait_for_display() {  # the X display XWayland listens on, from the compositor's log
  for _ in $(seq 1 300); do
    d=$(grep -oE "(xserver listening on display|Xwayland.*display|DISPLAY=) ?:[0-9]+" "$log" 2>/dev/null | grep -oE ":[0-9]+" | tail -1 || true)
    if [ -n "$d" ]; then export DISPLAY=$d; return 0; fi
    sleep 0.1
  done
  echo "with-compositor: no XWayland display in the $compositor log" >&2
  tail -40 "$log" >&2 || true
  exit 1
}

# (no /tmp/.X11-unix at all without an X server, as in a mutter or KWin job: ls fails there, and
# under pipefail that used to end the script at once with ls's exit code 2 and no message)
x_before=$(ls /tmp/.X11-unix 2>/dev/null | tr '\n' ' ' || true)
wait_for_new_x_socket() {  # mutter and KWin: XWayland's display is the X socket that was not there before
  for _ in $(seq 1 300); do
    for s in $(ls /tmp/.X11-unix 2>/dev/null); do
      case " $x_before " in *" $s "*) ;; *) export DISPLAY=":${s#X}"; return 0 ;; esac
    done
    sleep 0.1
  done
  echo "with-compositor: no XWayland display from $compositor" >&2
  tail -40 "$log" >&2 || true
  exit 1
}

case $compositor in
  weston)
    if [ "${WESTON_BACKEND:-headless}" = x11 ]; then
      args=(--backend=x11-backend.so --use-pixman --width="$width" --height="$height" --scale="$scale" --socket="$socket" --idle-time=0)
    else
      args=(--backend=headless-backend.so --width="$width" --height="$height" --scale="$scale" --socket="$socket" --idle-time=0)
    fi
    [ "$outputs" -gt 1 ] && args+=(--output-count="$outputs")
    [ -n "${WESTON_CONFIG:-}" ] && args+=(--config="$WESTON_CONFIG")
    [ "$mode" = xwayland ] && args+=(--xwayland)
    DISPLAY=${WESTON_X11_DISPLAY:-} "${WESTON:-weston}" "${args[@]}" >"$log" 2>&1 &
    pid=$!
    wait_for_socket
    [ "$mode" = xwayland ] && wait_for_display
    ;;
  mutter)
    # Its own session bus. XWayland (on demand) gets the first free X display: a new X socket.
    args=(--wayland --headless --wayland-display="$socket" --virtual-monitor "${width}x${height}")
    [ "$mode" = native ] && args+=(--no-x11)
    dbus-run-session -- mutter "${args[@]}" >"$log" 2>&1 &
    pid=$!
    wait_for_socket
    [ "$mode" = xwayland ] && wait_for_new_x_socket
    ;;
  kwin)
    args=(--virtual --socket "$socket" --width "$width" --height "$height" --scale "$scale")
    [ "$outputs" -gt 1 ] && args+=(--output-count "$outputs")
    [ "$mode" = xwayland ] && args+=(--xwayland)
    dbus-run-session -- kwin_wayland "${args[@]}" >"$log" 2>&1 &
    pid=$!
    wait_for_socket
    [ "$mode" = xwayland ] && wait_for_new_x_socket
    ;;
  *) usage ;;
esac

export WAYLAND_DISPLAY=$socket
export XDG_SESSION_TYPE=wayland
if [ "$mode" = xwayland ]; then
  export SDL_VIDEODRIVER=wayland
else
  unset DISPLAY
fi
echo "with-compositor: $compositor ($mode) WAYLAND_DISPLAY=$WAYLAND_DISPLAY DISPLAY=${DISPLAY:-} scale $scale, $outputs output(s)" >&2
set +e
"$@"
rc=$?
set -e
exit $rc
