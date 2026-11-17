# Gridline - K-112B (checkpoint 53, R13-02): the cycle area is shared between two Smooth collaborators,
# with Smooth's view running on both (the in-memory session test cannot see the view write back).
# Two hidden Gridline windows (own configs) over real ICE, both in Smooth. Alex opens a song whose
# cycle is on (bars 9 to 17) and hosts; Sam joins.
#   1. Sam, after joining, has the host's cycle, on (his empty view used to write it off).
#   2. Alex turns Cycle off (C): Sam's song has it off, its range kept.
#   3. Sam drags a new cycle across the bar numbers: Alex's song gets it, on (the receiver's view
#      used to write its own back, and Sam's edit was lost).
#   4. Alex drags another: Sam's song gets it.
#   5. The final saves are equal (each computer's Smooth Library pane aside).
# Each step ends with a channel named for it, which the other side waits for.
#   cmake -DAPP=<gridline-app> -DSONG=<song> -DOUT=<dir> -P k112b_cycle_collab_app.cmake
cmake_minimum_required(VERSION 3.19)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(failures 0)
macro(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR failures "${failures} + 1")
endmacro()

# The song: the demo with its cycle on, 1536 to 3072 (bars 5 to 9).
file(READ "${SONG}" demo)
string(JSON demo SET "${demo}" cycle "{\"start\": 1536, \"end\": 3072, \"on\": true}")
set(S "${OUT}/song.gridline.json")
file(WRITE "${S}" "${demo}")

# "start end on" of a save's cycle ("none" without one).
function(cycleOf file out)
  file(READ "${file}" j)
  string(JSON c ERROR_VARIABLE e GET "${j}" cycle)
  if(e)
    set(${out} "none" PARENT_SCOPE)
    return()
  endif()
  string(JSON a GET "${c}" start)
  string(JSON b GET "${c}" end)
  # (a variable named "on" would read as the constant ON in if())
  string(JSON lit ERROR_VARIABLE e2 GET "${c}" on)
  if(NOT e2 AND lit)
    set(state "on")
  else()
    set(state "off")
  endif()
  set(${out} "${a} ${b} ${state}" PARENT_SCOPE)
endfunction()

function(sharedJson file out)
  file(READ "${file}" j)
  string(JSON has ERROR_VARIABLE e GET "${j}" smooth library)
  if(NOT e)
    string(JSON j REMOVE "${j}" smooth library)
  endif()
  set(${out} "${j}" PARENT_SCOPE)
endfunction()

# Drags across the bar numbers (1920x1080, the demo's default zoom: bar 1 at x 880, 65.28 px a bar, the
# ruler at y 128 to 154: cp54 made it one 26 px tier, the numbers on top); the ruler snaps. Sam's drag is a new range, Alex's makes bars 6 to 8.
set(RULER_Y 145)
set(SAM_X0 1012)
set(SAM_XM 1080)
set(SAM_X1 1143)
set(ALEX_X0 1208)
set(ALEX_XM 1270)
set(ALEX_X1 1339)
set(ALEX_CYCLE "1920 2688")
set(D "${OUT}/logic-logic")
set(A "5 collab profile Alex
6 toasts clear
10 collab new \"${D}/Alex/Song\"
20 collab invite \"${D}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
32 waitfor collab-channel:SamJoined 90
33 waitfor never 2
34 save \"${D}/alex0.gridline.json\"
35 key c
38 save \"${D}/alex1.gridline.json\"
39 collab edit AlexOff
40 waitfor collab-channel:SamDrag 90
41 waitfor collab-synced 60
42 waitfor never 3
43 save \"${D}/alex2.gridline.json\"
44 move ${ALEX_X0} ${RULER_Y}
47 down 0
50 move ${ALEX_XM} ${RULER_Y}
53 move ${ALEX_X1} ${RULER_Y}
56 up 0
60 collab edit AlexDrag
61 waitfor collab-channel:SamEnd 90
62 waitfor collab-synced 60
63 waitfor never 3
64 save \"${D}/alex3.gridline.json\"
65 shot \"${D}/alex3.ppm\"
66 waitfor never 2
90 quit discard
")
set(B "5 collab profile Sam
6 toasts clear
10 collab join \"${D}/invite.txt\" \"${D}/Sam/Song\"
11 waitfor collab-joined 120
12 waitfor collab-synced 60
13 waitfor never 4
14 save \"${D}/sam0.gridline.json\"
15 shot \"${D}/sam0.ppm\"
16 collab edit SamJoined
20 waitfor collab-channel:AlexOff 90
21 waitfor collab-synced 60
22 waitfor never 3
23 save \"${D}/sam1.gridline.json\"
24 move ${SAM_X0} ${RULER_Y}
27 down 0
30 move ${SAM_XM} ${RULER_Y}
33 move ${SAM_X1} ${RULER_Y}
36 up 0
39 save \"${D}/sam2.gridline.json\"
40 collab edit SamDrag
41 waitfor collab-channel:AlexDrag 90
42 waitfor collab-synced 60
43 waitfor never 3
44 save \"${D}/sam3.gridline.json\"
45 shot \"${D}/sam3.ppm\"
46 collab edit SamEnd
47 waitfor collab-synced 60
48 waitfor never 4
49 save \"${D}/sam4.gridline.json\"
90 quit discard
")
file(MAKE_DIRECTORY "${D}")
foreach(who A B)
  file(MAKE_DIRECTORY "${D}/cfg${who}/gridline" "${D}/home${who}")
  file(WRITE "${D}/cfg${who}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"keys.profile\": \"logic\"}}\n")
endforeach()
file(WRITE "${D}/a.txt" "${A}")
file(WRITE "${D}/b.txt" "${B}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfgA HOME=${D}/homeA
          "${APP}" "${S}" --hidden --no-ui-sounds --layout logic --layout-dev --size 1920x1080 --dev-input "${D}/a.txt"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfgB HOME=${D}/homeB
          "${APP}" --hidden --no-ui-sounds --layout logic --layout-dev --size 1920x1080 --dev-input "${D}/b.txt"
  RESULTS_VARIABLE rcs OUTPUT_FILE "${D}/run.out" ERROR_FILE "${D}/run.err" TIMEOUT 480)
if(NOT rcs STREQUAL "0;0")
  message(FATAL_ERROR "exit codes ${rcs} (see ${D}/run.err)")
endif()
foreach(f alex0 sam0 alex1 sam1 sam2 alex2 sam3 alex3 sam4)
  if(NOT EXISTS "${D}/${f}.gridline.json")
    message(FATAL_ERROR "${f}: the save is missing")
  endif()
  cycleOf("${D}/${f}.gridline.json" c_${f})
  message(STATUS "${f}: cycle ${c_${f}}")
endforeach()
if(NOT c_alex0 STREQUAL "1536 3072 on")
  fail("Alex's own cycle at the start: ${c_alex0}")
endif()
if(NOT c_sam0 STREQUAL "1536 3072 on")
  fail("Sam after joining: ${c_sam0} (the host's 1536 3072 on expected)")
endif()
if(NOT c_alex1 STREQUAL "1536 3072 off")
  fail("Alex after turning Cycle off: ${c_alex1}")
endif()
if(NOT c_sam1 STREQUAL "1536 3072 off")
  fail("Sam after Alex turned Cycle off: ${c_sam1}")
endif()
# (Sam's window lays out a little differently from the host's, so his drag is checked against his own save.)
if(NOT c_sam2 MATCHES " on$" OR c_sam2 STREQUAL "1536 3072 on")
  fail("Sam's drag made no new cycle: ${c_sam2}")
endif()
if(NOT c_alex2 STREQUAL c_sam2)
  fail("Alex after Sam's drag: ${c_alex2} (Sam's ${c_sam2} expected)")
endif()
if(NOT c_sam3 STREQUAL "${ALEX_CYCLE} on")
  fail("Sam after Alex's drag: ${c_sam3} (${ALEX_CYCLE} on expected)")
endif()
if(NOT c_alex3 STREQUAL "${ALEX_CYCLE} on" OR NOT c_sam4 STREQUAL "${ALEX_CYCLE} on")
  fail("the last cycles: Alex ${c_alex3}, Sam ${c_sam4} (${ALEX_CYCLE} on expected)")
endif()
sharedJson("${D}/alex3.gridline.json" ja)
sharedJson("${D}/sam4.gridline.json" jb)
string(JSON eq EQUAL "${ja}" "${jb}")
if(NOT eq)
  file(WRITE "${D}/a.shared.json" "${ja}")
  file(WRITE "${D}/b.shared.json" "${jb}")
  fail("the two last saves differ (a.shared.json / b.shared.json)")
endif()
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - the cycle is shared between two Smooth collaborators, each side's edits kept")
