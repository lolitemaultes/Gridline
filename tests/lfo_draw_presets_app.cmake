# Gridline - 0.7.9: drawing LFO shapes and LFO presets in the real app (hidden, isolated config and
# HOME, SDL's dummy audio).
#   1. "Edit this shape" turns a sine into points; a pencil stroke on a 16 grid in the inspector's
#      editor is one undo step and makes step points; undo takes the stroke back;
#   2. a point stroke: clicking the editor adds a point (one step);
#   3. a preset saved from a source lands in the user preset folder (lfos/) and lists after the
#      factory presets; loading it into another source and making a new source from it work; the
#      targets stay;
#   4. in each of the four layouts the board, the inspector's preset menu, the New menu and the
#      Browser's LFO presets draw (screenshots).
# Screenshots: <OUT>/<tag>.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<song.gridline.json> -DOUT=<dir> -P lfo_draw_presets_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home" "${OUT}/cfg/gridline")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_CACHE_DIR} "${OUT}/cache")
file(WRITE "${OUT}/cfg/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true}}\n")
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 ${ARGN}
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
# undoAt(<var> <n>): the undo position of the n-th "[lfo] frame" report.
function(undoAt var n)
  string(REGEX MATCHALL "\\[lfo\\] frame [^\n]* undo [0-9]+" reps "${out}")
  list(GET reps ${n} r)
  string(REGEX MATCH "undo ([0-9]+)" m "${r}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
# points(<p> <n>): the n-th "[lfo] points" block: <p>_count, <p>_shape, <p>_steps (step points).
function(points p n)
  string(REGEX MATCHALL "\\[lfo\\] points [0-9]+ shape [A-Za-z ]+(\n\\[lfo\\]   point [^\n]*)*" blocks "${out}")
  list(GET blocks ${n} b)
  string(REGEX MATCH "points ([0-9]+) shape ([A-Za-z]+)" m "${b}")
  set(${p}_count "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${p}_shape "${CMAKE_MATCH_2}" PARENT_SCOPE)
  string(REGEX MATCHALL "kind 2" st "${b}")
  list(LENGTH st ns)
  set(${p}_steps "${ns}" PARENT_SCOPE)
endfunction()

# Two LFOs; LFO 1 moves the Kick's pan (owner 20, index 1). The board popped out (cards and the inspector).
set(setup "2 lfo new\n2 lfo new\n3 lfo link 1 20 1 0.4\n4 lfo open\n8 lfo popout\n10 lfo select 1\n")

# ---- 1. Edit this shape, then a pencil stroke on a 16 grid
run(editor 40 "${setup}16 lfo editshape 1\n24 lfo editor\n26 lfo points 1\n38 quit discard\n")
string(REGEX MATCH "\\[lfo\\] editor ([-0-9]+) ([-0-9]+) ([-0-9]+) ([-0-9]+)" m "${out}")
set(ex0 "${CMAKE_MATCH_1}")
set(ey0 "${CMAKE_MATCH_2}")
set(ex1 "${CMAKE_MATCH_3}")
set(ey1 "${CMAKE_MATCH_4}")
math(EXPR ew "${ex1} - ${ex0}")
check("the inspector shows the shape editor (${ew} wide)" ew GREATER 200)
points(e 0)
check("Edit this shape: the sine as two smooth points (${e_count} ${e_shape})" e_count EQUAL 2 AND e_shape STREQUAL "Drawn")
math(EXPR sy "${ey0} + 12")
math(EXPR sxa "${ex0} + 6")
math(EXPR sxb "${ex0} + (${ex1} - ${ex0}) * 30 / 100")
math(EXPR sxm "(${sxa} + ${sxb}) / 2")
run(stroke 90 "${setup}16 lfo editshape 1\n24 lfo tool 1 3\n26 lfo\n28 move ${sxa} ${sy}\n30 down 0\n32 move ${sxm} ${sy}\n34 move ${sxb} ${sy}\n36 move ${sxb} ${sy}\n38 up 0\n42 lfo\n44 lfo points 1\n50 undo\n54 lfo\n56 lfo points 1\n88 quit discard\n")
undoAt(u0 0)
undoAt(u1 1)
undoAt(u2 2)
math(EXPR d1 "${u1} - ${u0}")
check("a pencil stroke of many moves is one undo step (${d1})" d1 EQUAL 1)
points(s 0)
check("the stroke on a 16 grid made step points (${s_steps} of ${s_count})" s_steps GREATER 3)
points(t 1)
check("undo takes the stroke back (${t_count} points)" t_count EQUAL 2 AND u2 EQUAL u0)

# ---- 2. Points tool: a click adds a point
math(EXPR px "${ex0} + (${ex1} - ${ex0}) * 60 / 100")
math(EXPR py "${ey0} + 14")
run(addpoint 60 "${setup}16 lfo editshape 1\n24 lfo tool 0 0\n26 lfo\n28 move ${px} ${py}\n30 down 0\n32 up 0\n36 lfo\n38 lfo points 1\n58 quit discard\n")
undoAt(a0 0)
undoAt(a1 1)
points(ap 0)
math(EXPR d2 "${a1} - ${a0}")
check("a click adds a point (${ap_count} points, ${d2} step)" ap_count EQUAL 3 AND d2 EQUAL 1)

# ---- 3. presets: save LFO 1, list, load into LFO 2, a new source from it
run(presets 70 "${setup}14 lfo preset 1 8\n18 lfo presetsave 1 Tests/My stutter\n22 lfo presets\n26 lfo preset 2 99\n28 lfo preset new 0\n40 lfo\n42 lfo points 2\n68 quit discard\n")
check("the preset is saved" out MATCHES "\\[lfo\\] presetsave ok")
check("it lands in the user preset folder" EXISTS "${OUT}/cfg/gridline/presets/lfos/Tests/My stutter.json")
string(REGEX MATCHALL "\\[lfo\\] preset [0-9]+ factory" fac "${out}")
list(LENGTH fac nf)
check("the factory presets list (${nf})" nf GREATER 7 AND nf LESS 13)
string(REGEX MATCH "\\[lfo\\] preset ([0-9]+) user Tests/My stutter" m "${out}")
set(ui "${CMAKE_MATCH_1}")
check("the user's preset lists after them (${ui})" ui EQUAL nf)
check("a factory preset loads into LFO 1" out MATCHES "\\[lfo\\] preset ok Stutter gate")
run(presets2 70 "${setup}14 lfo preset 1 8\n18 lfo presetsave 1 Tests/My stutter\n22 lfo preset 2 ${ui}\n26 lfo preset new ${ui}\n40 lfo\n42 lfo points 2\n44 lfo points 3\n68 quit discard\n")
check("the user preset loads into LFO 2" out MATCHES "\\[lfo\\] preset ok My stutter")
points(l2 0)
points(l3 1)
check("LFO 2 has the saved drawn shape (${l2_count} points, ${l2_steps} steps)" l2_count EQUAL 16 AND l2_steps EQUAL 16)
check("a new source from the preset (${l3_count} points)" l3_count EQUAL 16)
string(REGEX MATCH "lfos ([0-9]+) targets ([0-9]+)" m "${out}")
check("three sources, LFO 1 keeps its target (${CMAKE_MATCH_1} ${CMAKE_MATCH_2})" CMAKE_MATCH_1 EQUAL 3 AND CMAKE_MATCH_2 EQUAL 1)

# ---- 4. every layout: the board, the inspector's presets, the New menu (screenshots)
foreach(lay studio live logic tracker)
  run(board_${lay} 50 "2 lfo new\n2 lfo new\n3 lfo link 1 20 1 0.4\n4 lfo open\n10 lfo select 1\n12 lfo preset 2 8\n30 lfo\n48 quit discard\n" --layout ${lay})
  check("${lay}: the board draws" out MATCHES "\\[lfo\\] frame [^\n]* open 1")
  run(presetsmenu_${lay} 50 "2 lfo new\n3 lfo link 1 20 1 0.4\n4 lfo open\n8 lfo popout\n10 lfo select 1\n20 lfo menu presets 1\n30 lfo\n48 quit discard\n" --layout ${lay})
  run(newmenu_${lay} 50 "2 lfo new\n4 lfo open\n8 lfo popout\n20 lfo menu new\n48 quit discard\n" --layout ${lay})
endforeach()
message(STATUS "screenshots in ${OUT}")
