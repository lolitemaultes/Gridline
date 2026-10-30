# Gridline: J-188, smooth motion in the playlist (hidden, isolated configs, 1440x900, the playlist
# shown, a fixed 16 ms frame time, the drawn geometry of every frame from "plgeom").
#   1. smooth snapping: a snapped clip drag glides over several frames between grid cells (on by
#      default); off (in the settings file at startup, or switched off while running) the clip jumps
#      with no frames in between; switched on in the middle of a drag, the next step glides at once;
#   2. smooth zooming: one Ctrl+wheel notch over the playlist glides over several frames, the point
#      under the pointer stays put (within 1 px) at every frame, and it ends at exactly the zoom and
#      scroll the instant zoom (smooth zooming off) gives; a trackpad's fractional Ctrl+scroll
#      applies at once.
#   cmake -DAPP=<gridline-app> -DDEMO=<song.gridline.json> -DOUT=<dir> -P j188_smooth_motion.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_REDUCED_MOTION} "0")  # the glides do not depend on the machine's accessibility setting
file(READ "${DEMO}" demo)  # a copy: the fixture is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
# A modifier that is Ctrl for ImGui on every platform (macOS swaps Cmd and Ctrl).
if(APPLE)
  set(ctrl super)
else()
  set(ctrl ctrl)
endif()

# run(<tag> <frames> <script> [settings values json]): output in ${out}.
function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}/gridline")
  if(ARGC GREATER 3)
    file(WRITE "${OUT}/cfg_${tag}/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {${ARGV3}}}\n")
  endif()
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --show playlist
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    fail("${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
function(int var v)  # "123.4" -> 123
  string(REGEX REPLACE "\\..*" "" i "${v}")
  set(${var} ${i} PARENT_SCOPE)
endfunction()
function(fixed var v)  # "337.000" / "0.170000" -> 337000 / 170000 (digits after the point kept)
  string(REPLACE "." "" v "${v}")
  string(REGEX MATCH "^(-?)0*([0-9]+)$" _ "${v}")
  set(${var} "${CMAKE_MATCH_1}${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# The view at <frame>: x0 (thousandths of a px) and pxPerTick (millionths) -> v_x0, v_ppt.
function(view frame)
  string(REGEX MATCH "plgeom frame ${frame}: valid 1 x0 ([-0-9.]+) pxPerTick ([0-9.]+)" _ "${out}")
  if(CMAKE_MATCH_1 STREQUAL "")
    fail("no view at frame ${frame}")
  endif()
  set(a "${CMAKE_MATCH_1}")
  set(b "${CMAKE_MATCH_2}")
  fixed(x "${a}")
  fixed(p "${b}")
  set(v_x0 ${x} PARENT_SCOPE)
  set(v_ppt ${p} PARENT_SCOPE)
endfunction()
# Left edge of clip <id> at <frame> -> cx (whole px).
function(clipx frame id)
  string(REGEX MATCH "plgeom frame ${frame}:[^\n]*\n(  [^\n]*\n)*" d "${out}")
  string(REGEX MATCH "  clip ${id} x ([-0-9.]+) " _ "${d}")
  if(CMAKE_MATCH_1 STREQUAL "")
    fail("no geometry for clip ${id} at frame ${frame}")
  endif()
  int(v "${CMAKE_MATCH_1}")
  set(cx ${v} PARENT_SCOPE)
endfunction()

# ---- geometry: clip 31 (the first pattern clip on track 1) ----
run(geo 12 "8 plgeom\n")
string(REGEX MATCH "  clip 31 x ([-0-9.]+) ([-0-9.]+) y ([-0-9.]+) ([-0-9.]+) head ([0-9.]+)" _ "${out}")
if(CMAKE_MATCH_1 STREQUAL "")
  fail("the fixture's clip 31 is not drawn")
endif()
int(c0 "${CMAKE_MATCH_1}")
int(c1 "${CMAKE_MATCH_2}")
int(cy0 "${CMAKE_MATCH_3}")
int(cy1 "${CMAKE_MATCH_4}")
int(chead "${CMAKE_MATCH_5}")
math(EXPR cw "${c1} - ${c0}")
math(EXPR gx "${c0} + ${cw} / 3")        # grab point: inside the body, away from the edges
math(EXPR gy "(${cy0} + ${chead} + ${cy1}) / 2")
math(EXPR gx1 "${gx} + 5")
math(EXPR gx2 "${gx} + 10")              # past the drag threshold
math(EXPR gx3 "${gx} + ${cw}")           # one clip length further: several grid cells
math(EXPR gx4 "${gx} + 2 * ${cw}")

# One drag of clip 31: down, past the threshold, then a big step at frame 16 (and another at 26).
set(drag "2 typing off
8 move ${gx} ${gy}
10 down 0
12 move ${gx1} ${gy}
14 move ${gx2} ${gy}
16 move ${gx3} ${gy}
26 move ${gx4} ${gy}
38 up 0
")
foreach(f RANGE 15 37)
  string(APPEND drag "${f} plgeom\n")
endforeach()

# In-between frames of clip 31 after the step at frame <from>: frames <from>..<to> whose drawn left
# edge is neither where it settles (frame <to>) nor where it was before the step (frame <from> - 1).
function(between var from to)
  math(EXPR before "${from} - 1")
  clipx(${before} 31)
  set(x0 ${cx})
  clipx(${to} 31)
  set(x1 ${cx})
  set(n 0)
  foreach(f RANGE ${from} ${to})
    clipx(${f} 31)
    if(NOT cx EQUAL x1 AND NOT cx EQUAL x0)
      math(EXPR n "${n} + 1")
    endif()
  endforeach()
  set(${var} ${n} PARENT_SCOPE)
  set(${var}_from ${x0} PARENT_SCOPE)
  set(${var}_to ${x1} PARENT_SCOPE)
endfunction()

# ---- 1. smooth snapping ----
run(snap_on 42 "${drag}")
between(n 17 25)
check("smooth snapping on (default): the clip moved (${n_from} -> ${n_to})" n_to GREATER n_from)
# (J-37's 12 ms ease covered 75 % of a step in the first frame and was done in three: on, but not visible.)
check("smooth snapping on: the step glides over several frames (${n} in between)" n GREATER_EQUAL 5)

run(snap_off_file 42 "${drag}" "\"ui.smoothSnap\": false")
between(n 17 25)
check("smooth snapping off at startup: the clip moved (${n_from} -> ${n_to})" n_to GREATER n_from)
check("smooth snapping off at startup: it jumps, no frames in between (${n})" n EQUAL 0)

string(REPLACE "2 typing off\n" "2 typing off\n3 setting ui.smoothSnap off\n24 setting ui.smoothSnap on\n" toggled "${drag}")
run(snap_live 42 "${toggled}")
between(n 17 25)
check("switched off while running: it jumps (${n})" n EQUAL 0)
between(m 27 37)
check("switched on in the middle of the drag: the next step glides (${m} in between, ${m_from} -> ${m_to})" m GREATER_EQUAL 5 AND m_to GREATER m_from)

# ---- 2. smooth zooming ----
# The pointer over the grid, one Ctrl+wheel notch at frame 12 (seen from frame 13).
set(mx 900)
set(zoom "2 typing off
8 move ${mx} 400
10 mod ${ctrl} 1
12 wheel 1
32 mod ${ctrl} 0
")
foreach(f RANGE 11 34)
  string(APPEND zoom "${f} plgeom\n")
endforeach()

run(zoom_off 38 "${zoom}" "\"ui.smoothZoom\": false")
view(11)
set(off_x0 ${v_x0})
set(off_p0 ${v_ppt})
view(34)
set(off_x1 ${v_x0})
set(off_p1 ${v_ppt})
check("smooth zooming off: the notch zooms in (${off_p0} -> ${off_p1})" off_p1 GREATER off_p0)
set(n 0)
foreach(f RANGE 11 34)
  view(${f})
  if(NOT v_ppt EQUAL off_p0 AND NOT v_ppt EQUAL off_p1)
    math(EXPR n "${n} + 1")
  endif()
endforeach()
check("smooth zooming off: at once, no frames in between (${n})" n EQUAL 0)

run(zoom_on 38 "${zoom}")
view(11)
set(x0 ${v_x0})
set(p0 ${v_ppt})
check("the same view before the notch" x0 EQUAL off_x0 AND p0 EQUAL off_p0)
math(EXPR off0 "${mx} * 1000 - ${x0}")  # pointer offset from tick 0, thousandths of a px
set(n 0)
set(worst 0)
set(prev ${p0})
set(monotone 1)
foreach(f RANGE 11 34)
  view(${f})
  if(NOT v_ppt EQUAL p0 AND NOT v_ppt EQUAL off_p1)
    math(EXPR n "${n} + 1")
  endif()
  if(v_ppt LESS prev)
    set(monotone 0)
  endif()
  set(prev ${v_ppt})
  # The tick under the pointer before the notch must still be under it: its offset scales with the zoom.
  math(EXPR want "${off0} * ${v_ppt} / ${p0}")
  math(EXPR got "${mx} * 1000 - ${v_x0}")
  math(EXPR d "${got} - ${want}")
  if(d LESS 0)
    math(EXPR d "0 - ${d}")
  endif()
  if(d GREATER worst)
    set(worst ${d})
  endif()
endforeach()
view(34)
check("smooth zooming on: the notch glides over several frames (${n} in between)" n GREATER_EQUAL 4)
check("smooth zooming on: the zoom only grows on the way (no overshoot)" monotone EQUAL 1)
check("smooth zooming on: the point under the pointer stays put at every frame (worst ${worst} / 1000 px)" worst LESS_EQUAL 1000)
check("smooth zooming on: ends at the instant zoom (${v_ppt} vs ${off_p1})" v_ppt EQUAL off_p1)
math(EXPR d "${v_x0} - ${off_x1}")
check("smooth zooming on: ends at the instant scroll (${v_x0} vs ${off_x1})" d LESS_EQUAL 10 AND d GREATER_EQUAL -10)

# A trackpad's fractional Ctrl+scroll follows the fingers: applied the frame it arrives.
string(REPLACE "12 wheel 1\n" "12 wheel 0.25\n" pad "${zoom}")
run(zoom_pad 38 "${pad}")
view(11)
set(p0 ${v_ppt})
view(34)
set(p1 ${v_ppt})
set(n 0)
foreach(f RANGE 11 34)
  view(${f})
  if(NOT v_ppt EQUAL p0 AND NOT v_ppt EQUAL p1)
    math(EXPR n "${n} + 1")
  endif()
endforeach()
check("trackpad Ctrl+scroll zooms (${p0} -> ${p1})" p1 GREATER p0)
check("trackpad Ctrl+scroll: 1:1, no glide (${n} in between)" n EQUAL 0)
