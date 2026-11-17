# Gridline - K-112D (D5, R11-06): Smooth's Smart Controls learn in place, as the reference's two Learn
# buttons do (no dialog). External Assignment > Learn: the next hardware control moved links to the
# control in focus, and moving it again drives that control (no second link). Parameter Mapping > Learn:
# the next parameter moved on the track's plugins takes the focused control's place; the mapping is
# saved with the project (Song::smooth.controls) and read back.
# D6 (R12-02): a mapping is an undo step that marks the project modified: Cmd+Z takes back the mapping
# itself (the moved parameter's step stays), Cmd+Shift+Z puts it back; a reset (Option-click Learn) too.
# D6 (R12-04): the mapped control keeps its place: only that position changes, the rest stay.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112d_smart_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"logic\", \"keys.profile\": \"logic\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
file(WRITE "${OUT}/input.txt" "5 toasts clear
10 key y
20 parts smart state
22 parts smart focus 1
24 parts smart learn ext
30 parts smart cc 21 64
36 parts smart state
38 parts smart cc 21 100
44 parts smart state
46 parts smart learn param
50 parts smart touch 1 6
56 parts smart state
57 parts smart learn off
58 save ${OUT}/before-undo.gridline.json
59 mod super 1
60 key z
61 mod super 0
64 parts smart state
65 docstate
66 mod super 1
67 mod shift 1
68 key z
69 mod shift 0
70 mod super 0
73 parts smart state
74 docstate
76 save ${OUT}/smart.gridline.json
80 open ${OUT}/smart.gridline.json
92 parts smart state
94 quit discard
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --layout-dev --layout logic --size 1920x1080
                        --dev-input "${OUT}/input.txt" --fixed-dt 16 --frames 100
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app failed (${rc})")
endif()
string(REGEX MATCHALL "smart state: [^\n]*" states "${out}")
list(LENGTH states n)
if(NOT n EQUAL 7)
  message(FATAL_ERROR "FAIL: expected 7 state lines, got ${n}: ${states}")
endif()
string(REGEX MATCHALL "docstate modified [01]" docs "${out}")
list(LENGTH docs nd)
if(NOT nd EQUAL 2)
  message(FATAL_ERROR "FAIL: expected 2 docstate lines, got ${nd}")
endif()
foreach(i 0 1 2 3 4 5 6)
  list(GET states ${i} s${i})
  string(REGEX REPLACE ".* knobs ([^ ]*) .*" "\\1" k${i} "${s${i}}")
  string(REPLACE "," ";" k${i} "${k${i}}")
  string(REGEX REPLACE ".* links ([^ ]*)$" "\\1" l${i} "${s${i}}")
endforeach()
list(LENGTH k0 nk)
if(nk LESS 6)
  message(FATAL_ERROR "FAIL: the Kick track shows too few smart controls: ${s0}")
endif()
list(GET k0 1 focused)
list(GET k0 4 eqKnob)
# External Assignment > Learn: CC 21 links to the control in focus
if(NOT s1 MATCHES "learn 2" OR NOT "${l1}" MATCHES "(^|,)21>${focused}(,|$)")
  message(FATAL_ERROR "FAIL: CC 21 did not link to the control in focus ${focused} in place (${s1})")
endif()
string(REGEX MATCHALL "21>" c1 "${l1}")
string(REGEX MATCHALL "21>" c2 "${l2}")
list(LENGTH c1 n1)
list(LENGTH c2 n2)
if(NOT n1 EQUAL 1 OR NOT n2 EQUAL 1)
  message(FATAL_ERROR "FAIL: moving the learned control again made another link (${l1} / ${l2})")
endif()
# Parameter Mapping > Learn: a parameter of the second effect not on the band (its 7th) takes the focused control's place
string(REGEX REPLACE ":.*" "" eqOwner "${eqKnob}")
list(GET k3 1 mapped)
if(NOT s3 MATCHES "mapped 1" OR NOT mapped STREQUAL "${eqOwner}:2006")
  message(FATAL_ERROR "FAIL: Parameter Mapping > Learn did not put the moved parameter on the control (${s3})")
endif()
file(READ "${OUT}/smart.gridline.json" saved)
if(NOT saved MATCHES "\"controls\"")
  message(FATAL_ERROR "FAIL: the mapping is not saved with the project")
endif()
# R12-04: only the mapped position changed (every other control stays in its place)
list(LENGTH k2 nk2)
list(LENGTH k3 nk3)
if(NOT nk2 EQUAL nk3)
  message(FATAL_ERROR "FAIL: the mapping changed the number of controls (${s2} / ${s3})")
endif()
math(EXPR last "${nk2} - 1")
foreach(i RANGE ${last})
  list(GET k2 ${i} a)
  list(GET k3 ${i} b)
  if(NOT i EQUAL 1 AND NOT a STREQUAL b)
    message(FATAL_ERROR "FAIL: control ${i} moved when control 1 was mapped (${s2} / ${s3})")
  endif()
endforeach()
# R12-02: Cmd+Z takes back the mapping itself (the move stays: still modified), Cmd+Shift+Z redoes it
if(NOT s4 MATCHES "mapped 0" OR NOT k4 STREQUAL k2)
  message(FATAL_ERROR "FAIL: Cmd+Z did not take back the mapping (${s4})")
endif()
list(GET docs 0 d0)
list(GET docs 1 d1)
# (saved with the mapping just before: the undo makes it modified, the redo brings back the saved state)
if(NOT d0 STREQUAL "docstate modified 1" OR NOT d1 STREQUAL "docstate modified 0")
  message(FATAL_ERROR "FAIL: the undo should mark the project modified, the redo back to the saved state (${d0} / ${d1})")
endif()
if(NOT s5 MATCHES "mapped 1" OR NOT k5 STREQUAL k3)
  message(FATAL_ERROR "FAIL: Cmd+Shift+Z did not put the mapping back (${s5})")
endif()
file(READ "${OUT}/before-undo.gridline.json" saved0)
if(NOT saved0 STREQUAL saved)
  message(FATAL_ERROR "FAIL: the save after undo and redo differs from the one before")
endif()
if(NOT s6 MATCHES "mapped 1" OR NOT k6 STREQUAL k3)
  message(FATAL_ERROR "FAIL: the saved mapping did not come back on open (${s3} / ${s6})")
endif()
message(STATUS "ok - Smart Controls learn in place: a hardware control links to the control in focus once; a moved parameter maps onto it in place, as an undo step, saved with the project")
