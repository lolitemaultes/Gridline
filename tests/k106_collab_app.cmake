# Gridline - K-106: the tracker in a collaborative project, with a collaborator in another window
# layout, in two hidden Gridline windows (each with its own config) over real ICE:
#   1. both edit the same pattern of the same channel at the same moment: the tracker types a note
#      on row 0, the other adds a note off the row grid two rows on (under the typed note: it overlaps);
#      both songs end the same, with both notes, the off-grid note exactly as it was made;
#   2. again: the tracker types on row 4, the other adds a note later on; all four notes on both
#      sides, nothing moved or quantised;
#   3. presence: each side gets the other's edit cursor (the tracker's cursor; the other side's
#      as set), by channel and pattern tick;
#   4. (cp16) the channel the other side made is a track of the Tracker joining it.
#   5. (cp20, K-316 7.3 A/B) the Tracker makes that channel a Tracker Sampler, loads a sample, then edits the
#      sample (reversed in the Sample editor) and the volume envelope (a point added in the Instrument editor)
#      while the other side has the instrument's plugin window open: the other side reads the same
#      envelope, the same sample length and the edited file, and the songs are equal.
#   Pairs: Tracker with Studio, Clips with Tracker, Tracker with Smooth (development layouts).
#   Each side reads its report of a round before the other starts the next round's edits (the two
#   windows count their own frames: on a slow runner one side's report came after the other's next
#   edit), and neither starts round 3 (or quits) before the other has read the cursors.
#   cmake -DAPP=<gridline-app> -DWAV=<audio file> -DOUT=<dir> -P k106_collab_app.cmake
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
# cp20: the Tracker edits a sample and an envelope while the other side watches its plugin window.
set(E "tracker editors")
set(TRK3 "86 ${E} open sample Lead
87 replace gl.tracker
88 ${E} open sample Lead
90 ${E} load \"${WAV}\"
91 waitfor collab-synced 60
92 waitfor never 4
93 ${E} op reverse
95 ${E} open instrument Lead
96 ${E} ins env vol add 30 50
97 ${E} ins env vol end
98 waitfor collab-synced 60
99 waitfor never 6
100 ${E} ins dump
101 ${E} dump
102 collab report \"@D@/t-3.json\"
103 waitfor never 30
")
set(OTH3 "86 waitfor never 3
87 ${E} plugin Lead
88 waitfor never 8
89 waitfor collab-synced 60
90 ${E} plugin Lead
91 waitfor never 8
92 waitfor collab-synced 60
93 waitfor never 8
94 waitfor collab-files-done 120
95 waitfor never 2
100 ${E} ins dump
101 ${E} dump
102 collab report \"@D@/o-3.json\"
103 waitfor never 2
")

foreach(pair "tracker;studio" "live;tracker" "tracker;logic")
  list(GET pair 0 LA)
  list(GET pair 1 LB)
  set(D "${OUT}/${LA}-${LB}")
  if(LA STREQUAL "tracker")
    set(A1 "${TRK1}")
    set(A2 "${TRK2}")
    set(A3 "${TRK3}")
    set(B1 "${OTH1}")
    set(B2 "${OTH2}")
    set(B3 "${OTH3}")
  else()
    set(A1 "${OTH1}")
    set(A2 "${OTH2}")
    set(A3 "${OTH3}")
    set(B1 "${TRK1}")
    set(B2 "${TRK2}")
    set(B3 "${TRK3}")
  endif()
  string(REPLACE "@D@" "${D}" A3 "${A3}")
  string(REPLACE "@D@" "${D}" B3 "${B3}")
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
85 waitfor \"file:${D}/b-edits.json\" 120
${A3}110 quit
")
  set(B "5 collab profile Sam
10 collab join \"${D}/invite.txt\" \"${D}/Sam/Song\"
11 waitfor collab-joined 120
12 waitfor collab-synced 60
13 waitfor collab-channel:Lead 90
14 waitfor collab-synced 60
15 waitfor never 3
16 tracker tracks
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
85 waitfor \"file:${D}/a-edits.json\" 120
${B3}110 quit
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
    RESULTS_VARIABLE rcs OUTPUT_FILE "${D}/run.out" ERROR_FILE "${D}/run.err" TIMEOUT 600)
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
  # cp16: the channel the other side made (Lead) is a track of its own in the Tracker joining
  # (every rack channel has its track, a peer's new one too).
  if(LB STREQUAL "tracker")
    file(READ "${D}/run.out" ro)
    string(REGEX MATCH "tracker tracks frame [0-9]+: n ([0-9]+) rack ([0-9]+)[^\n]*" tl "${ro}")
    if(NOT tl OR NOT CMAKE_MATCH_1 STREQUAL CMAKE_MATCH_2 OR NOT tl MATCHES " Lead\\([0-9]+\\)")
      fail("${LA}/${LB}: the peer's channel Lead is no track of the Tracker: '${tl}'")
    endif()
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
  # 5. (cp20) the sample and the envelope the Tracker edited. The second app's dumps reach run.out (the
  #    first app's output is piped into it); the shared song's digest in both reports covers the first.
  file(READ "${D}/run.out" ro)
  set(e0 "")
  set(l0 "")
  if(NOT ro MATCHES "tracker editors frame 100: ins shown [01] [^\n]*(vol \\[[^]]*\\])")
    fail("${LA}/${LB}: round 3: no instrument dump from ${LB}")
  else()
    set(e0 "${CMAKE_MATCH_1}")
    if(NOT e0 MATCHES "30:50")
      fail("${LA}/${LB}: round 3: ${LB} reads the volume envelope without the added point 30:50: '${e0}'")
    endif()
  endif()
  if(NOT ro MATCHES "tracker editors frame 101: shown [^\n]* tracker 1 slot 0 frames ([0-9]+) ")
    fail("${LA}/${LB}: round 3: ${LB} has no Tracker Sampler sample on Lead")
  else()
    set(l0 "${CMAKE_MATCH_1}")
    if(l0 EQUAL 0)
      fail("${LA}/${LB}: round 3: the sample did not arrive at ${LB}")
    endif()
  endif()
  if(EXISTS "${D}/t-3.json" AND EXISTS "${D}/o-3.json")
    file(READ "${D}/t-3.json" j3t)
    file(READ "${D}/o-3.json" j3o)
    string(JSON s3t GET "${j3t}" song)
    string(JSON s3o GET "${j3o}" song)
    if(NOT s3t STREQUAL s3o)
      fail("${LA}/${LB}: round 3: the songs differ after the sample and envelope edits")
    endif()
  else()
    fail("${LA}/${LB}: round 3: no reports")
  endif()
  message(STATUS "${LA}/${LB}: both sides '${round_2}'; round 3 envelope '${e0}', sample ${l0} frames")
endforeach()
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - the tracker and another layout edit one pattern at once, nothing lost, and see each other's cursors")
