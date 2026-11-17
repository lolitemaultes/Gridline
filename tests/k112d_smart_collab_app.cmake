# Gridline - K-112D (D6, R12-03): Smooth's Smart Control mappings are song data in a shared project.
# Two hidden Gridline windows (own configs) over real ICE: Alex in Smooth, Sam in Studio (then Sam in
# Smooth too). Alex maps a Smart Control (Parameter Mapping > Learn); each adds a channel. Sam's song
# gets the mapping, and both saves are equal (the Smooth Library pane, each computer's own, aside).
#   cmake -DAPP=<gridline-app> -DSONG=<song> -DOUT=<dir> -P k112d_smart_collab_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(failures 0)
macro(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR failures "${failures} + 1")
endmacro()

# A save's JSON with what stays on one computer taken out (the Smooth Library pane).
function(sharedJson file out)
  file(READ "${file}" j)
  string(JSON has ERROR_VARIABLE e GET "${j}" smooth library)
  if(NOT e)
    string(JSON j REMOVE "${j}" smooth library)
  endif()
  set(${out} "${j}" PARENT_SCOPE)
endfunction()

foreach(pair "logic;studio" "logic;logic")
  list(GET pair 0 LA)
  list(GET pair 1 LB)
  set(D "${OUT}/${LA}-${LB}")
  set(A "5 collab profile Alex
10 collab new \"${D}/Alex/Song\"
20 collab invite \"${D}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
33 key y
35 parts smart focus 1
36 parts smart learn param
37 parts smart touch 1 6
39 parts smart learn off
40 parts smart state
41 collab edit AlexCh
42 waitfor collab-channel:SamCh 90
43 waitfor collab-synced 60
44 waitfor never 4
52 save \"${D}/alex.gridline.json\"
53 waitfor never 2
90 quit discard
")
  set(B "5 collab profile Sam
10 collab join \"${D}/invite.txt\" \"${D}/Sam/Song\"
11 waitfor collab-joined 120
12 waitfor collab-synced 60
40 waitfor collab-channel:AlexCh 90
41 collab edit SamCh
43 waitfor collab-synced 60
44 waitfor never 6
52 save \"${D}/sam.gridline.json\"
53 waitfor never 2
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
            "${APP}" "${SONG}" --hidden --no-ui-sounds --layout ${LA} --layout-dev --dev-input "${D}/a.txt"
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfgB HOME=${D}/homeB
            "${APP}" --hidden --no-ui-sounds --layout ${LB} --layout-dev --dev-input "${D}/b.txt"
    RESULTS_VARIABLE rcs OUTPUT_VARIABLE out ERROR_FILE "${D}/run.err" TIMEOUT 420)
  file(WRITE "${D}/run.out" "${out}")
  if(NOT rcs STREQUAL "0;0")
    fail("${LA}/${LB}: exit codes ${rcs}")
    continue()
  endif()
  if(NOT EXISTS "${D}/alex.gridline.json" OR NOT EXISTS "${D}/sam.gridline.json")
    fail("${LA}/${LB}: a save is missing")
    continue()
  endif()
  sharedJson("${D}/alex.gridline.json" ja)
  sharedJson("${D}/sam.gridline.json" jb)
  string(JSON ca ERROR_VARIABLE ea GET "${ja}" smooth controls)
  string(JSON cb ERROR_VARIABLE eb GET "${jb}" smooth controls)
  if(ea OR eb)
    fail("${LA}/${LB}: a save has no smooth.controls (Alex: ${ea}, Sam: ${eb})")
    continue()
  endif()
  string(JSON nca LENGTH "${ca}")
  if(NOT nca EQUAL 1 OR NOT ca STREQUAL cb)
    fail("${LA}/${LB}: the mappings differ (Alex ${ca}, Sam ${cb})")
  endif()
  if(NOT ja MATCHES "AlexCh" OR NOT ja MATCHES "SamCh" OR NOT jb MATCHES "AlexCh" OR NOT jb MATCHES "SamCh")
    fail("${LA}/${LB}: the channel edits did not reach both")
  endif()
  string(JSON eq EQUAL "${ja}" "${jb}")
  if(NOT eq)
    file(WRITE "${D}/a.shared.json" "${ja}")
    file(WRITE "${D}/b.shared.json" "${jb}")
    fail("${LA}/${LB}: the two saves differ (a.shared.json / b.shared.json)")
  endif()
endforeach()
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - a Smart Control mapped in Smooth reaches a Studio (and a Smooth) collaborator; both saves equal")
