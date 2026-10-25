# Gridline — J-117: the elastic boundary of windows floating inside the main window, in the real app
# (hidden, isolated configs, off-screen at 1440x900, a fixed 16 ms frame time, SDL's dummy audio
# driver). The scripted pointer drives ImGui like the mouse.
#   1. the channel rack dragged far past each edge of the work area (left, right, above the top
#      bar, below the status bar) follows with damped overshoot: past the edge, never more than
#      the elastic bound (48 px); its pop-out button stays in its title bar right beside the close
#      button (it used to be drawn where an after-the-fact clamp put the window, ~50 px away);
#   2. let go, it springs back: still on its way a few frames later (no jump), fully inside within
#      40 frames, resting exactly on the edge;
#   3. the main window made smaller: a window resting against the far edge comes back inside;
#   4. docking by dragging still works: the rack dropped on the dock space's outer right target
#      (by the main window's edge) and on the browser's centre target docks;
#   5. a docked panel (the mixer) dragged out by its tab and past the edge stays bounded and comes
#      back inside: never detached by the drag (J-116).
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j117_rubber.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x (CMP0054: a quoted "keep" is that string, never a variable)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# run(<tag> <frames> <script> [extra args...]): a fresh config ${OUT}/cfg_<tag>.
function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
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
function(ok msg)
  message(STATUS "ok - ${msg}")
endfunction()

# The "viewports" dump of <frame> for window <name>: <prefix>_x _y _w _h (integers), _vp, _docked, and _detached.
function(win prefix frame name)
  string(REGEX MATCH "viewports frame ${frame}:[^\n]*\n(  [^\n]*\n)*" d "${out}")
  if(d STREQUAL "")
    fail("no viewports dump at frame ${frame}")
  endif()
  string(REGEX MATCH "detached ([0-9]+)" _ "${d}")
  set(${prefix}_detached ${CMAKE_MATCH_1} PARENT_SCOPE)
  string(REGEX MATCH "  window \"${name}\" pos ([-0-9]+)[.0-9]* ([-0-9]+)[.0-9]* size ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* viewport ([-0-9]+) docked ([01])" line "${d}")
  if(line STREQUAL "")
    fail("window ${name} missing at frame ${frame}:\n${d}")
  endif()
  set(${prefix}_x ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${prefix}_y ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${prefix}_w ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(${prefix}_h ${CMAKE_MATCH_4} PARENT_SCOPE)
  set(${prefix}_vp ${CMAKE_MATCH_5} PARENT_SCOPE)
  set(${prefix}_docked ${CMAKE_MATCH_6} PARENT_SCOPE)
  string(REGEX MATCH "  window \"${name}\" [^\n]* laid ([-0-9]+) ([-0-9]+)" line "${d}")
  set(${prefix}_lx "${CMAKE_MATCH_1}" PARENT_SCOPE)  # where it was laid out and drawn this frame
  set(${prefix}_ly "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()
# The pop-out button ("titlebtn") and the close button ("closebtn") of <name> at <frame>:
# <prefix>_bx _by (pop-out), _cx _cy (close), _lx _ly (where the window was laid out and drawn),
# _nx _ny (where it is at the end of the frame).
function(btns prefix frame name)
  if(NOT out MATCHES "titlebtn frame ${frame}: ${name} x ([-0-9]+) y ([-0-9]+) size ([0-9]+) state ([a-z]+)")
    fail("no pop-out button for ${name} at frame ${frame}")
  endif()
  set(${prefix}_bx ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${prefix}_by ${CMAKE_MATCH_2} PARENT_SCOPE)
  if(NOT out MATCHES "closebtn frame ${frame}: ${name} x ([-0-9]+) y ([-0-9]+) size ([0-9]+) window ([-0-9]+) ([-0-9]+) now ([-0-9]+) ([-0-9]+)")
    fail("no close button for ${name} at frame ${frame}")
  endif()
  set(${prefix}_cx ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${prefix}_cy ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${prefix}_lx ${CMAKE_MATCH_4} PARENT_SCOPE)
  set(${prefix}_ly ${CMAKE_MATCH_5} PARENT_SCOPE)
  set(${prefix}_nx ${CMAKE_MATCH_6} PARENT_SCOPE)
  set(${prefix}_ny ${CMAKE_MATCH_7} PARENT_SCOPE)
endfunction()

# The work area at 1440x900: between the top bar (48) and the status bar (26).
set(A0X 0)
set(A1X 1440)
set(A0Y 48)
set(A1Y 874)
set(MAXOVER 48)

# ---- 0. geometry ----
run(geo 12 "8 viewports\n9 titlebtn \"Channel rack\"\n9 closebtn \"Channel rack\"\n")
win(g 8 "Channel rack")
btns(gb 9 "Channel rack")
if(NOT g_docked EQUAL 0 OR NOT g_vp EQUAL 0)
  fail("the channel rack should start floating in the main window")
endif()
math(EXPR gap "${gb_cx} - ${gb_bx}")  # the pop-out button's distance from the close button at rest
if(gap LESS 10 OR gap GREATER 40 OR NOT gb_by EQUAL gb_cy)
  fail("at rest the pop-out button should sit just left of the close button (pop-out ${gb_bx},${gb_by} close ${gb_cx},${gb_cy})")
endif()
math(EXPR grabX "${g_x} + 94")  # a point of the rack's title bar
math(EXPR grabY "${g_y} + 10")
math(EXPR restR "${A1X} - ${g_w}")  # the rack's x resting against the right edge
math(EXPR restB "${A1Y} - ${g_h}")  # its y resting on the status bar
ok("the rack floats at ${g_x},${g_y} (${g_w}x${g_h}); its pop-out button sits ${gap} px left of the close button")

# ---- 1. + 2. dragged far past each edge, then let go ----
# edge(<tag> <axis x|y> <edge value> <sign -1|1> <moves...>): drags the rack by its title bar
# through the pointer positions given (frames 14, 16, ...), checks the overshoot at frame 30 and
# the spring back after the release at frame 32.
function(edge tag axis edgeV sign)
  set(script "10 move ${grabX} ${grabY}\n12 down 0\n")
  set(f 14)
  foreach(p IN LISTS ARGN)
    string(REPLACE "," " " p "${p}")
    string(APPEND script "${f} move ${p}\n")
    math(EXPR f "${f} + 2")
  endforeach()
  string(APPEND script "30 viewports\n30 titlebtn \"Channel rack\"\n30 closebtn \"Channel rack\"\n32 up 0\n35 viewports\n72 viewports\n")
  run(${tag} 76 "${script}")
  win(d 30 "Channel rack")
  set(v ${d_${axis}})
  math(EXPR over "(${v} - ${edgeV}) * ${sign}")
  if(over LESS_EQUAL 0 OR over GREATER_EQUAL ${MAXOVER})
    fail("${tag}: dragged far past the edge the rack should be out by less than ${MAXOVER} px, damped (out ${over} px, ${axis} ${v}, edge ${edgeV})")
  endif()
  if(NOT d_vp EQUAL 0 OR NOT d_detached EQUAL 0 OR NOT d_docked EQUAL 0)
    fail("${tag}: the drag should leave the rack floating in the main window")
  endif()
  btns(b 30 "Channel rack")
  math(EXPR bgap "${b_cx} - ${b_bx}")
  if(NOT bgap EQUAL gap OR NOT b_by EQUAL b_cy)
    fail("${tag}: the pop-out button should stay beside the close button in the title bar (pop-out ${b_bx},${b_by} close ${b_cx},${b_cy}, gap ${bgap} not ${gap})")
  endif()
  if(NOT b_lx EQUAL b_nx OR NOT b_ly EQUAL b_ny)
    fail("${tag}: nothing should move the rack after it was drawn (drawn at ${b_lx},${b_ly}, at ${b_nx},${b_ny} at the end of the frame)")
  endif()
  win(s 35 "Channel rack")
  set(sv ${s_${axis}})
  math(EXPR sover "(${sv} - ${edgeV}) * ${sign}")
  if(sover LESS_EQUAL 0 OR sover GREATER_EQUAL over)
    fail("${tag}: three frames after the release the rack should be springing back, not jump (out ${sover} px, was ${over})")
  endif()
  win(r 72 "Channel rack")
  if(r_x LESS A0X OR r_y LESS A0Y)
    fail("${tag}: let go, the rack should be back fully inside (${r_x},${r_y})")
  endif()
  math(EXPR rr "${r_x} + ${r_w}")
  math(EXPR rb "${r_y} + ${r_h}")
  if(rr GREATER A1X OR rb GREATER A1Y)
    fail("${tag}: let go, the rack should be back fully inside (${r_x},${r_y} ${r_w}x${r_h})")
  endif()
  if(NOT r_${axis} EQUAL edgeV)
    fail("${tag}: the rack should rest against the edge it was dragged past (${axis} ${r_${axis}}, edge ${edgeV})")
  endif()
  ok("${tag}: dragged far past the edge it is out ${over} px (damped, < ${MAXOVER}), the pop-out button stays beside the close button; let go it springs back (${sover} px out 3 frames later) and rests on the edge")
endfunction()

math(EXPR far "${grabY} + 8")
edge(left x ${A0X} -1 "300,${far}" "0,${far}" "-300,${far}" "-500,${far}" "-700,${far}")
edge(right x ${restR} 1 "1300,${far}" "1500,${far}" "1700,${far}" "1900,${far}" "2100,${far}")
edge(top y ${A0Y} -1 "${grabX},100" "${grabX},0" "${grabX},-200" "${grabX},-400")
edge(bottom y ${restB} 1 "${grabX},600" "${grabX},900" "${grabX},1200" "${grabX},1500")

# ---- 3. the main window made smaller ----
# The rack resting against the right edge; the main window then shrinks to 1100x700 (halfway
# through the run): it comes back inside the smaller work area.
run(shrink 160 "10 move ${grabX} ${grabY}
12 down 0
14 move 1300 ${far}
16 move 1900 ${far}
18 up 0
60 viewports
150 viewports
" --resize 1100x700)
win(s0 60 "Channel rack")
if(NOT s0_x EQUAL restR)
  fail("shrink: the rack should first rest against the right edge (${s0_x}, not ${restR})")
endif()
win(s1 150 "Channel rack")
math(EXPR s1r "${s1_x} + ${s1_w}")
math(EXPR s1b "${s1_y} + ${s1_h}")
math(EXPR nb "700 - 26")
if(s1r GREATER 1100 OR s1b GREATER nb OR s1_x LESS 0 OR s1_y LESS 48)
  fail("shrink: the rack should be back inside the smaller work area (${s1_x},${s1_y} ${s1_w}x${s1_h})")
endif()
ok("the main window shrinks: the rack comes back inside (${s1_x},${s1_y})")

# ---- 4. docking by dragging still works ----
# The dock space's outer right target sits by the main window's right edge; the rack then hangs
# back (elastic) while the pointer is on the target: the drop docks it all the same.
run(dockedge 60 "10 move ${grabX} ${grabY}
12 down 0
14 move 900 400
16 move 1300 455
18 move 1410 461
20 move 1418 461
22 move 1419 461
24 viewports
26 up 0
40 viewports
")
win(de 24 "Channel rack")
if(NOT de_docked EQUAL 0)
  fail("dock (edge): still dragged, the rack should not be docked yet")
endif()
win(dd 40 "Channel rack")
if(NOT dd_docked EQUAL 1 OR NOT dd_vp EQUAL 0)
  fail("dock (edge): dropped on the dock space's right target the rack should dock (docked ${dd_docked}, viewport ${dd_vp})")
endif()
ok("docking by drag: dropped on the target by the main window's right edge it docks (${dd_x},${dd_y} ${dd_w}x${dd_h})")
run(dockbrowser 60 "10 move ${grabX} ${grabY}
12 down 0
14 move 300 300
16 move 120 440
18 move 110 461
20 move 110 461
26 up 0
40 viewports
")
win(db 40 "Channel rack")
win(br 40 "Browser")
if(NOT db_docked EQUAL 1 OR NOT db_x EQUAL br_x)
  fail("dock (centre): dropped on the browser's centre target the rack should become a tab beside it (docked ${db_docked}, x ${db_x} / ${br_x})")
endif()
ok("docking by drag: dropped on the browser's centre target it becomes a tab of its node")

# ---- 5. a docked panel dragged out by its tab and far past the edge ----
run(mixer 60 "8 viewports\n")
win(m0 8 "Mixer")
math(EXPR tabX "${m0_x} + 28")
math(EXPR tabY "${m0_y} + 12")
run(mixer 80 "10 move ${tabX} ${tabY}
12 down 0
14 move ${tabX} 100
16 move 1300 140
18 move 1450 180
20 move 1600 200
22 move 1800 220
24 move 2000 220
26 viewports
28 up 0
72 viewports
")
win(m1 26 "Mixer")
math(EXPR m1r "${m1_x} + ${m1_w}")
math(EXPR m1lim "${A1X} + ${MAXOVER}")
if(NOT m1_docked EQUAL 0 OR NOT m1_vp EQUAL 0 OR NOT m1_detached EQUAL 0)
  fail("mixer: dragged out by its tab it should float in the main window (docked ${m1_docked}, viewport ${m1_vp}, detached ${m1_detached})")
endif()
if(m1r GREATER_EQUAL m1lim OR m1r LESS_EQUAL A1X)
  fail("mixer: dragged far past the right edge it should be out by less than ${MAXOVER} px (right edge ${m1r})")
endif()
win(m2 72 "Mixer")
math(EXPR m2r "${m2_x} + ${m2_w}")
if(NOT m2_vp EQUAL 0 OR NOT m2_detached EQUAL 0 OR m2r GREATER A1X)
  fail("mixer: let go, it should be back inside the main window (right edge ${m2r}, viewport ${m2_vp}, detached ${m2_detached})")
endif()
ok("the mixer dragged out by its tab and past the edge: bounded (right edge ${m1r}), back inside once let go, never detached")

# ---- 6. a floating dock node holding two tabs (the mixer docked into the rack) ----
# Dragged by its tab bar far past the right edge: bounded like a window, its tab bar drawn where
# the node is (ImGui lays floating nodes out inside NewFrame: they are kept in bounds before
# that) and the pop-out button at the same place of its tab bar as at rest.
math(EXPR barX "${g_x} + ${g_w} - 120")  # an empty part of the node's tab bar
run(node 120 "10 move ${tabX} ${tabY}
12 down 0
14 move ${tabX} 120
16 move 900 300
18 move 660 390
20 move 656 386
22 move 656 386
24 up 0
40 viewports
40 titlebtn Mixer
50 move ${barX} ${grabY}
52 down 0
54 move 1200 ${grabY}
56 move 1600 ${far}
58 move 2000 ${far}
60 move 2200 ${far}
62 viewports
62 titlebtn Mixer
64 up 0
110 viewports
")
set(NODE "##DockNode_[0-9A-F]+")
win(n0 40 "${NODE}")
win(nm 40 "Mixer")
if(NOT nm_docked EQUAL 1 OR NOT nm_x EQUAL n0_x)
  fail("node: the mixer dropped on the rack's centre should become its second tab (docked ${nm_docked}, x ${nm_x} / ${n0_x})")
endif()
if(NOT out MATCHES "titlebtn frame 40: Mixer x ([-0-9]+) y ([-0-9]+)")
  fail("node: no pop-out button on the floating node's tab bar")
endif()
math(EXPR nbOff "${CMAKE_MATCH_1} - ${n0_x}")
win(n1 62 "${NODE}")
math(EXPR n1r "${n1_x} + ${n1_w}")
math(EXPR n1over "${n1r} - ${A1X}")
if(n1over LESS_EQUAL 0 OR n1over GREATER_EQUAL MAXOVER)
  fail("node: dragged far past the right edge the node should be out by less than ${MAXOVER} px (${n1over})")
endif()
if(NOT n1_lx EQUAL n1_x OR NOT n1_ly EQUAL n1_y)
  fail("node: its tab bar should be drawn where the node is (laid out at ${n1_lx},${n1_ly}, node at ${n1_x},${n1_y})")
endif()
if(NOT out MATCHES "titlebtn frame 62: Mixer x ([-0-9]+) y ([-0-9]+)")
  fail("node: no pop-out button while dragged")
endif()
math(EXPR nbOff1 "${CMAKE_MATCH_1} - ${n1_x}")
if(NOT nbOff1 EQUAL nbOff)
  fail("node: the pop-out button should stay at its place in the tab bar (${nbOff1} px from the node's left, ${nbOff} at rest)")
endif()
win(n2 110 "${NODE}")
math(EXPR n2r "${n2_x} + ${n2_w}")
if(n2r GREATER A1X)
  fail("node: let go, it should spring back inside (right edge ${n2r})")
endif()
ok("a floating dock node of two tabs: bounded past the edge (${n1over} px out), its tab bar and pop-out button drawn with it, back inside once let go")
