# Gridline - K-202: the curve the drawing tools write is chosen in the automation clip editor's
# toolbar too, shared with the piano roll's automation lane and remembered; its menu also sets the
# curve of the selected points. Runs the app hidden (isolated config, fixed frame time) with the
# demo automation clip (`--show autoedit`: 8 points of mixed curves, points 1 and 2 selected):
#   A. the editor's chooser shows the lane's curve (linear by default), and follows the lane's;
#   B. a click with the Draw tool adds a point with the chosen curve (smooth here), one undo step;
#   C. the chooser's menu: "Draw with Smooth" changes the shared curve; "Set the 2 selected points
#      to Smooth" changes those two points only, in one undo step;
#   D. the choice is remembered at the next start (same config folder).
# Screenshots: <OUT>/menu.ppm (the chooser's menu open), <OUT>/menu_light.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k202_auto_draw_curve_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# run(tag cfg frames script [extra args]): cfg names the config folder (kept between runs that share it).
function(run tag cfg frames script)
  file(MAKE_DIRECTORY "${OUT}/cfg_${cfg}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${cfg}")
  set(ENV{HOME} "${OUT}/cfg_${cfg}")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames ${frames}
                          --show autoedit ${ARGN} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${last_out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# The "aepoints" line printed at <frame>: <prefix>_chip (x0 y0 x1 y1), _gridy, _gridx, _shape, _undo,
# _items (6 x/y pairs), _points, _sel, _pts (pos:value:shape[*]).
function(aep out frame prefix)
  string(REGEX MATCH "aepoints frame ${frame}: [^\n]*" line "${out}")
  if(NOT line)
    message(FATAL_ERROR "FAIL: no aepoints line at frame ${frame}\n${out}")
  endif()
  # (CMake keeps nine regex groups at most: the line is read in two parts.)
  string(REGEX MATCH "chip ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) gridy ([-0-9.]+) ([-0-9.]+) gridx ([-0-9.]+) ([-0-9.]+) shape ([0-9]+)" _ "${line}")
  if("${CMAKE_MATCH_9}" STREQUAL "")
    message(FATAL_ERROR "FAIL: unreadable aepoints line: ${line}")
  endif()
  set(${prefix}_shape ${CMAKE_MATCH_9} PARENT_SCOPE)
  foreach(k 1 2 3 4 5 6 7 8)  # (copied first: a regex call below resets the CMAKE_MATCH_ values)
    set(m${k} "${CMAKE_MATCH_${k}}")
  endforeach()
  foreach(k 1 2 3 4 5 6 7 8)
    string(REGEX REPLACE "\\..*" "" v${k} "${m${k}}")
  endforeach()
  set(${prefix}_chip ${v1} ${v2} ${v3} ${v4} PARENT_SCOPE)
  set(${prefix}_gridy ${v5} ${v6} PARENT_SCOPE)
  set(${prefix}_gridx ${v7} ${v8} PARENT_SCOPE)
  string(REGEX MATCH "undo ([0-9]+) items([-0-9 ]+) points ([0-9]+) sel ([0-9]+) pts(.*)$" _ "${line}")
  if("${CMAKE_MATCH_3}" STREQUAL "")
    message(FATAL_ERROR "FAIL: unreadable aepoints line: ${line}")
  endif()
  set(${prefix}_undo ${CMAKE_MATCH_1} PARENT_SCOPE)
  string(STRIP "${CMAKE_MATCH_2}" items)
  string(REPLACE " " ";" items "${items}")
  set(${prefix}_items "${items}" PARENT_SCOPE)
  set(${prefix}_points ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(${prefix}_sel ${CMAKE_MATCH_4} PARENT_SCOPE)
  string(STRIP "${CMAKE_MATCH_5}" pts)
  string(REPLACE " " ";" pts "${pts}")
  set(${prefix}_pts "${pts}" PARENT_SCOPE)
endfunction()

# The curve (AutoShape number) of point i in a pts list.
function(ptshape pts i outvar)
  list(GET pts ${i} e)
  string(REPLACE "*" "" e "${e}")
  string(REPLACE ":" ";" e "${e}")
  list(GET e 2 v)
  set(${outvar} ${v} PARENT_SCOPE)
endfunction()

# AutoShape numbers: Curve (linear) 0, Hold (stepped) 2, Smooth 4.

# ---- A. the chooser follows the shared curve
run(probe probe 30 "20 aepoints\n22 alaneshape 2\n26 aepoints\n")
set(last_out "${probe_out}")
aep("${probe_out}" 20 A0)
aep("${probe_out}" 26 A1)
check("the editor shows the demo clip with two points selected (${A0_points}, ${A0_sel})" A0_points EQUAL 8 AND A0_sel EQUAL 2)
list(GET A0_chip 0 cx0)
list(GET A0_chip 1 cy0)
list(GET A0_chip 2 cx1)
list(GET A0_chip 3 cy1)
check("the draw-curve chooser is in the toolbar (${cx0} ${cy0} ${cx1} ${cy1})" cx1 GREATER cx0 AND cy1 GREATER cy0)
check("it starts on the default curve, linear (${A0_shape})" A0_shape EQUAL 0)
check("choosing smooth for the lane changes the editor's curve too (${A1_shape})" A1_shape EQUAL 4)

# ---- B. the Draw tool writes the chosen curve
list(GET A0_gridx 1 gx1)
list(GET A0_gridy 0 gy0)
list(GET A0_gridy 1 gy1)
math(EXPR dx "${gx1} - 30")
math(EXPR dy "(${gy0} + ${gy1}) / 2")
run(draw draw 40 "20 alaneshape 2\n22 aepoints\n24 move ${dx} ${dy}\n26 down 0\n28 up 0\n34 aepoints\n")
set(last_out "${draw_out}")
aep("${draw_out}" 22 B0)
aep("${draw_out}" 34 B1)
math(EXPR undoB "${B1_undo} - ${B0_undo}")
check("a click with the Draw tool adds one point (${B0_points} -> ${B1_points}) in one undo step (${undoB})" B1_points EQUAL 9 AND undoB EQUAL 1)
math(EXPR last "${B1_points} - 1")
ptshape("${B1_pts}" ${last} newShape)
check("... with the chosen curve, smooth (${newShape})" newShape EQUAL 4)

# ---- C. the chooser's menu
math(EXPR mx "(${cx0} + ${cx1}) / 2")
math(EXPR my "(${cy0} + ${cy1}) / 2")
run(menuprobe menuprobe 34 "20 move ${mx} ${my}\n22 down 0\n23 up 0\n30 aepoints\n")
set(last_out "${menuprobe_out}")
aep("${menuprobe_out}" 30 C0)
list(GET C0_items 4 sx)   # item 2: draw with smooth
list(GET C0_items 5 sy)
list(GET C0_items 10 tx)  # item 5: set the selection to smooth
list(GET C0_items 11 ty)
check("a click on the chooser opens its menu, with the selection's items (${C0_items})" sx GREATER 0 AND tx GREATER 0 AND ty GREATER sy)
run(menudraw menudraw 44 "20 move ${mx} ${my}\n22 down 0\n23 up 0\n28 move ${sx} ${sy}\n30 move ${sx} ${sy}\n32 down 0\n33 up 0\n38 aepoints\n")
set(last_out "${menudraw_out}")
aep("${menudraw_out}" 38 C1)
check("Draw with: Smooth changes the shared curve (${C1_shape})" C1_shape EQUAL 4)
check("... and leaves the points alone (${C1_points})" C1_points EQUAL 8)
run(menusel menusel 44 "18 aepoints\n20 move ${mx} ${my}\n22 down 0\n23 up 0\n28 move ${tx} ${ty}\n30 move ${tx} ${ty}\n32 down 0\n33 up 0\n38 aepoints\n")
set(last_out "${menusel_out}")
aep("${menusel_out}" 18 C2)
aep("${menusel_out}" 38 C3)
math(EXPR undoC "${C3_undo} - ${C2_undo}")
ptshape("${C3_pts}" 1 s1)
ptshape("${C3_pts}" 2 s2)
ptshape("${C3_pts}" 4 s4)
ptshape("${C2_pts}" 4 s4before)
check("Set the selected points to: Smooth changes points 1 and 2 (${s1} ${s2}) in one undo step (${undoC})" s1 EQUAL 4 AND s2 EQUAL 4 AND undoC EQUAL 1)
check("... and no other point (${s4before} -> ${s4})" s4 EQUAL s4before)
check("... and not the drawing curve (${C3_shape})" C3_shape EQUAL 0)

# ---- D. remembered at the next start
run(keep1 keep 24 "20 alaneshape 2\n")
run(keep2 keep 26 "22 aepoints\n")
set(last_out "${keep2_out}")
aep("${keep2_out}" 22 D1)
check("the chosen curve (smooth) is remembered at the next start (${D1_shape})" D1_shape EQUAL 4)

# ---- screenshots: the menu open
run(menu menushot 30 "20 move ${mx} ${my}\n22 down 0\n23 up 0\n")
run(menu_light menushotl 30 "20 move ${mx} ${my}\n22 down 0\n23 up 0\n" --light)
message(STATUS "screenshots: ${OUT}/menu.ppm ${OUT}/menu_light.ppm")
