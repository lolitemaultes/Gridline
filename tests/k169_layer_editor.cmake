# Gridline: the layer editor in the real app: hidden, isolated config and HOME, SDL's dummy audio
# driver, real SDL mouse events.
#   1. every built-in layer opens in its window with one lane per child and no overlapping text at
#      100% (1440x900 and 1920x1080, dark and light);
#   2. dragging the high end of a zone bar narrows that child's key zone, as one undo step;
#   3. a layer saved as a preset file loads back as a new layer with its zones.
# Screenshots: <OUT>/dark1440.ppm, <OUT>/light1440.ppm, <OUT>/dark1080.ppm, <OUT>/light1080.ppm (100%),
# <OUT>/default1080.ppm (the default plugin window size).
#   cmake -DAPP=<gridline-app> -DDEMO=<song.gridline.json> -DOUT=<dir> -P k169_layer_editor.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home" "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_CACHE_DIR} "${OUT}/cache")
set(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE} "1")  # the editor at its design size (100%), as the suite runs it
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag size frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size ${size} ${ARGN}
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
# ed(<p> <n>): the n-th "layered frame" report into <p>_drawn _texts _overlaps _lanes, the first
# lane into <p>_x0 _y0 _x1 _y1 _zx0 _zx1, the first zone into <p>_lo _hi.
function(ed p n)
  string(REGEX MATCHALL "layered frame [0-9]+: drawn [^\n]*(\n  [^\n]*)*" reps "${out}")
  list(LENGTH reps count)
  if(NOT n LESS count)
    message(FATAL_ERROR "FAIL: ${p}: only ${count} layered reports\n${out}")
  endif()
  list(GET reps ${n} r)
  string(REGEX MATCH "drawn ([-0-9]+) texts ([0-9]+) overlaps ([0-9]+) box [-0-9. ]+ lanes ([0-9]+)" m "${r}")
  set(${p}_drawn "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${p}_texts "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${p}_overlaps "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(${p}_lanes "${CMAKE_MATCH_4}" PARENT_SCOPE)
  string(REGEX MATCH "lane 0 ([-0-9]+) ([-0-9]+) ([-0-9]+) ([-0-9]+) zone ([-0-9]+) ([-0-9]+)" m "${r}")
  set(${p}_x0 "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${p}_y0 "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${p}_x1 "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(${p}_y1 "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(${p}_zx0 "${CMAKE_MATCH_5}" PARENT_SCOPE)
  set(${p}_zx1 "${CMAKE_MATCH_6}" PARENT_SCOPE)
  string(REGEX MATCH "zone [0-9]+ keys ([0-9]+) ([0-9]+)" m "${r}")
  set(${p}_lo "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${p}_hi "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# ---- 1. every built-in layer, no overlapping text, both themes, two window sizes
foreach(k 0 1 2 3)
  run(preset${k} 1440x900 40 "5 layered load ${k}\n36 layered\n")
  string(FIND "${out}" "loaded 1 index" pos)
  check("built-in layer ${k} loads" pos GREATER -1)
  ed(e 0)
  check("built-in layer ${k}: the editor is drawn (frame ${e_drawn}), ${e_lanes} lanes, ${e_texts} texts, ${e_overlaps} overlaps"
        e_drawn GREATER 0 AND e_lanes GREATER 1 AND e_texts GREATER 10 AND e_overlaps EQUAL 0)
endforeach()
foreach(v dark1440:1440x900 light1440:1440x900:--light dark1080:1920x1080 light1080:1920x1080:--light)
  string(REPLACE ":" ";" v "${v}")
  list(GET v 0 tag)
  list(GET v 1 size)
  list(LENGTH v vn)
  set(extra "")
  if(vn GREATER 2)
    list(GET v 2 extra)
  endif()
  run(${tag} ${size} 40 "5 layered load 0\n36 layered\n" ${extra})
  ed(e 0)
  check("${tag}: ${e_lanes} lanes, ${e_overlaps} overlaps" e_drawn GREATER 0 AND e_lanes EQUAL 2 AND e_overlaps EQUAL 0)
  check("screenshot ${tag}.ppm" EXISTS "${OUT}/${tag}.ppm")
endforeach()

# The default window size (the editor reduced, as it first opens): a screenshot to look at.
set(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE} "")
run(default1080 1920x1080 40 "5 layered load 0\n36 layered\n")
set(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE} "1")
check("screenshot default1080.ppm" EXISTS "${OUT}/default1080.ppm")

# ---- 2. drag the high end of the first child's zone 40 px to the left, then undo
run(geo 1440x900 40 "5 layered load 0\n36 layered\n")
ed(g 0)
check("the first child plays a key zone (${g_lo} .. ${g_hi}, bar ${g_zx0} .. ${g_zx1})" g_hi LESS 127 AND g_zx1 GREATER g_zx0)
math(EXPR ex "${g_zx1} - 1")
math(EXPR ey "(${g_y0} + ${g_y1}) / 2")
math(EXPR ex1 "${ex} - 20")
math(EXPR ex2 "${ex} - 40")
run(drag 1440x900 80 "5 layered load 0
30 sdlmove ${ex} ${ey}
32 sdlmove ${ex} ${ey}
34 sdldown 0
36 sdlmove ${ex1} ${ey}
38 sdlmove ${ex2} ${ey}
40 sdlmove ${ex2} ${ey}
42 sdlup 0
46 layered
50 undo
56 layered
")
ed(d0 0)
math(EXPR want "${g_hi} - 40 * 128 / (${g_x1} - ${g_x0})")
math(EXPR lo "${want} - 2")
math(EXPR hi "${want} + 2")
check("the drag narrows the zone (${g_hi} -> ${d0_hi}, about ${want})" d0_lo EQUAL g_lo AND d0_hi GREATER lo AND d0_hi LESS hi)
ed(d1 1)
check("one undo restores it (${d1_lo} .. ${d1_hi})" d1_lo EQUAL g_lo AND d1_hi EQUAL g_hi)

# ---- 3. preset file round trip
run(save 1440x900 30 "5 layered load 1\n20 layered save \"${OUT}/round.gllayer\"\n")
string(FIND "${out}" "saved 1" pos)
check("the layer saves as a preset file" pos GREATER -1 AND EXISTS "${OUT}/round.gllayer")
run(file 1440x900 40 "5 layered file \"${OUT}/round.gllayer\"\n36 layered\n")
string(FIND "${out}" "loaded 1 index" pos)
check("the preset file loads back as a layer" pos GREATER -1)
ed(f 0)
string(REGEX MATCH "mode [0-9]+ children 2 zones 2" m "${out}")
set(found 0)
if(m)
  set(found 1)
endif()
check("with its two children and their zones (${f_lanes} lanes, ${f_overlaps} overlaps)" found EQUAL 1 AND f_lanes EQUAL 2 AND f_overlaps EQUAL 0)
