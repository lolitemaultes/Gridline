# Gridline - K-106: the tracker in a collaborative project, with a collaborator in another window
# layout, in two hidden Gridline windows (each with its own config) over real ICE:
#   1. both edit the same pattern of the same channel at the same moment: the tracker types a note
#      on row 0, the other adds a note off the row grid two rows on (under the typed note: it overlaps);
#      both songs end the same, with both notes, the off-grid note exactly as it was made;
#   2. again: the tracker types on row 4, the other adds a note later on; all four notes on both
#      sides, nothing moved or quantised;
#   3. presence: each side gets the other's edit cursor (the tracker's cursor; the other side's
#      as set), by channel and pattern tick.
#   Pairs: Tracker with Studio, Clips with Tracker, Tracker with Smooth (development layouts).
#   Each side reads its report of a round before the other starts the next round's edits (the two
#   windows count their own frames: on a slow runner one side's report came after the other's next
#   edit), and neither quits before the other has read the cursors.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k106_collab_app.cmake
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

# The tracker side's edits at frames 40 and 70; the other side's.
set(TRK1 "40 tracker focus
42 tracker cursor Lead 0 0 0
43 tracker edit on
44 sdlkey z down
45 sdlkey z up
")
set(OTH1 "44 collab note Lead 55 30 67
")
set(TRK2 "70 tracker cursor Lead 4 0 0
72 sdlkey x down
73 sdlkey x up
")
set(OTH2 "72 collab note Lead 200 48 72
73 collab edit-cursor-dev Lead 96
")

foreach(pair "tracker;studio" "live;tracker" "tracker;logic")
  list(GET pair 0 LA)
  list(GET pair 1 LB)
  set(D "${OUT}/${LA}-${LB}")
  if(LA STREQUAL "tracker")
    set(A1 "${TRK1}")
    set(A2 "${TRK2}")
    set(B1 "${OTH1}")
    set(B2 "${OTH2}")
  else()
    set(A1 "${OTH1}")
    set(A2 "${OTH2}")
    set(B1 "${TRK1}")
    set(B2 "${TRK2}")
  endif()
  set(A "5 collab profile Alex
10 collab new \"${D}/Alex/Song\"
20 collab invite \"${D}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
32 collab edit Lead
33 waitfor collab-synced 60
34 waitfor collab-channel:Lead 30
35 waitfor never 3
${A1}50 waitfor collab-notes:Lead:2 90
51 waitfor collab-synced 60
52 waitfor never 2
53 collab report \"${D}/a-1.json\"
54 waitfor \"file:${D}/b-1.json\" 120
${A2}80 waitfor collab-notes:Lead:4 90
81 waitfor collab-synced 60
82 waitfor never 3
83 collab report \"${D}/a-2.json\"
84 collab edits \"${D}/a-edits.json\"
85 waitfor never 3
86 waitfor \"file:${D}/b-edits.json\" 120
90 quit
")
  set(B "5 collab profile Sam
10 collab join \"${D}/invite.txt\" \"${D}/Sam/Song\"
11 waitfor collab-joined 120
12 waitfor collab-synced 60
13 waitfor collab-channel:Lead 90
14 waitfor collab-synced 60
15 waitfor never 3
${B1}50 waitfor collab-notes:Lead:2 90
51 waitfor collab-synced 60
52 waitfor never 2
53 collab report \"${D}/b-1.json\"
54 waitfor \"file:${D}/a-1.json\" 120
${B2}80 waitfor collab-notes:Lead:4 90
81 waitfor collab-synced 60
82 waitfor never 3
83 collab report \"${D}/b-2.json\"
84 collab edits \"${D}/b-edits.json\"
85 waitfor never 3
86 waitfor \"file:${D}/a-edits.json\" 120
90 quit
")
  file(MAKE_DIRECTORY "${D}")
  foreach(who A B)
    file(MAKE_DIRECTORY "${D}/cfg${who}/gridline" "${D}/home${who}")
    file(WRITE "${D}/cfg${who}/gridline/settings.json"
         "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true}}\n")
  endforeach()
  file(WRITE "${D}/a.txt" "${A}")
  file(WRITE "${D}/b.txt" "${B}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfgA HOME=${D}/homeA
            "${APP}" --hidden --no-ui-sounds --layout ${LA} --dev-input "${D}/a.txt"
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfgB HOME=${D}/homeB
            "${APP}" --hidden --no-ui-sounds --layout ${LB} --dev-input "${D}/b.txt"
    RESULTS_VARIABLE rcs OUTPUT_FILE "${D}/run.out" ERROR_FILE "${D}/run.err" TIMEOUT 480)
  if(NOT rcs STREQUAL "0;0")
    fail("${LA}/${LB}: exit codes ${rcs}")
  endif()
  set(missing 0)
  foreach(f a-1 b-1 a-2 b-2 a-edits b-edits)
    if(NOT EXISTS "${D}/${f}.json")
      fail("${LA}/${LB}: no ${f} report")
      set(missing 1)
    endif()
  endforeach()
  if(missing)
    continue()
  endif()
  foreach(round 1 2)
    file(READ "${D}/a-${round}.json" ja)
    file(READ "${D}/b-${round}.json" jb)
    string(JSON sa GET "${ja}" song)
    string(JSON sb GET "${jb}" song)
    string(JSON na GET "${ja}" notes Lead)
    string(JSON nb GET "${jb}" notes Lead)
    if(NOT sa STREQUAL sb OR NOT na STREQUAL nb)
      fail("${LA}/${LB} round ${round}: the songs differ: '${na}' / '${nb}'")
    endif()
    if(NOT na MATCHES "(^| )0:[0-9]+:60( |$)" OR NOT na MATCHES "(^| )55:30:67( |$)")
      fail("${LA}/${LB} round ${round}: the typed C-4 at tick 0 and the off-grid note 55:30:67 are not both there: '${na}'")
    endif()
    set(round_${round} "${na}")
  endforeach()
  if(NOT round_2 MATCHES "(^| )[1-9][0-9]*:[0-9]+:62( |$)" OR NOT round_2 MATCHES "(^| )200:48:72( |$)")
    fail("${LA}/${LB}: the second round's notes (D-4 on row 4, 200:48:72) are not both there: '${round_2}'")
  endif()
  # 3. presence: the tracker side's cursor reaches the other side, the other side's set cursor the tracker.
  if(LA STREQUAL "tracker")
    set(trkEdits "${D}/a-edits.json")
    set(othEdits "${D}/b-edits.json")
  else()
    set(trkEdits "${D}/b-edits.json")
    set(othEdits "${D}/a-edits.json")
  endif()
  file(READ "${othEdits}" jo)
  file(READ "${trkEdits}" jt)
  string(JSON no LENGTH "${jo}" peers)
  string(JSON nt LENGTH "${jt}" peers)
  if(no LESS 1)
    fail("${LA}/${LB}: the tracker's edit cursor did not reach the other side: ${jo}")
  else()
    string(JSON och GET "${jo}" peers 0 channel)
    string(JSON optr GET "${jo}" peers 0 pointer)
    if(NOT och STREQUAL "Lead" OR NOT optr EQUAL 0)
      fail("${LA}/${LB}: the tracker's edit cursor arrived as '${och}' (pointer ${optr})")
    endif()
  endif()
  if(nt LESS 1)
    fail("${LA}/${LB}: the other side's edit cursor did not reach the tracker: ${jt}")
  else()
    string(JSON tch GET "${jt}" peers 0 channel)
    string(JSON ttk GET "${jt}" peers 0 tick)
    if(NOT tch STREQUAL "Lead" OR NOT ttk EQUAL 96)
      fail("${LA}/${LB}: the other side's edit cursor arrived as '${tch}' tick ${ttk}")
    endif()
  endif()
  message(STATUS "${LA}/${LB}: both sides '${round_2}'")
endforeach()
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - the tracker and another layout edit one pattern at once, nothing lost, and see each other's cursors")
