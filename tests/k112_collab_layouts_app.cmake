# Gridline - K-112: two people in one project, each in a different window layout, in two hidden
# Gridline windows (each with its own config) over real ICE:
#   1. each adds a channel; both songs end the same (edits in one layout reach the other);
#   2. Alex is on a channel, Sam on a mixer track (by kind and id, core/SemanticAnchor.h): each
#      receives the other's target and names it;
#   3. "Go to what they are on" in the other layout selects that object there and keeps the layout;
#      nothing in the song changes by it.
#   K-123: Alex's takes and the rating of one reach Sam.
#   Pairs: Studio with Tracker, Live with Studio, Logic with Tracker (development layouts).
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k112_collab_layouts_app.cmake
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

# The first collaborator's target in a "collab targets" file: kind:id, its id, its name here.
function(peerTarget file out)
  if(NOT EXISTS "${file}")
    set(${out} "" PARENT_SCOPE)
    return()
  endif()
  file(READ "${file}" j)
  string(JSON n LENGTH "${j}" peers)
  if(n EQUAL 0)
    set(${out} "" PARENT_SCOPE)
    return()
  endif()
  string(JSON t GET "${j}" peers 0 target)
  string(JSON id GET "${j}" peers 0 id)
  string(JSON shows GET "${j}" peers 0 shows)
  string(JSON lay GET "${j}" layout)
  set(${out} "${t}" PARENT_SCOPE)
  set(${out}_id "${id}" PARENT_SCOPE)
  set(${out}_shows "${shows}" PARENT_SCOPE)
  set(${out}_layout "${lay}" PARENT_SCOPE)
endfunction()

foreach(pair "studio;tracker" "live;studio" "logic;tracker")
  list(GET pair 0 LA)
  list(GET pair 1 LB)
  set(D "${OUT}/${LA}-${LB}")
  set(A "5 collab profile Alex
10 collab new \"${D}/Alex/Song\"
20 collab invite \"${D}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
40 rectake make 3
40 rectake rate 3 1 2
40 collab edit AlexCh
41 waitfor collab-channel:SamCh 90
42 waitfor collab-synced 60
43 collab target-dev channel ch0
44 waitfor never 4
45 collab targets \"${D}/a-targets.json\"
46 collab report \"${D}/a-before.json\"
47 collab goto
48 waitfor never 1
49 collab targets \"${D}/a-went.json\"
50 collab report \"${D}/a-after.json\"
51 waitfor never 4
90 quit
")
  set(B "5 collab profile Sam
10 collab join \"${D}/invite.txt\" \"${D}/Sam/Song\"
11 waitfor collab-joined 120
12 waitfor collab-synced 60
40 waitfor collab-channel:AlexCh 90
41 collab edit SamCh
42 collab target-dev mixer mx1
43 waitfor collab-synced 60
44 waitfor never 4
45 collab targets \"${D}/b-targets.json\"
46 collab report \"${D}/b-before.json\"
47 collab goto
48 waitfor never 1
49 collab targets \"${D}/b-went.json\"
50 collab report \"${D}/b-after.json\"
51 waitfor never 4
52 rectake list
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
    RESULTS_VARIABLE rcs OUTPUT_FILE "${D}/run.out" ERROR_FILE "${D}/run.err" TIMEOUT 420)
  if(NOT rcs STREQUAL "0;0")
    fail("${LA}/${LB}: exit codes ${rcs}")
  endif()
  # 1. the same song on both sides, with both channels
  foreach(f a-after b-after)
    if(NOT EXISTS "${D}/${f}.json")
      fail("${LA}/${LB}: no ${f} report")
    endif()
  endforeach()
  if(failures GREATER 0)
    continue()
  endif()
  file(READ "${D}/a-after.json" ja)
  file(READ "${D}/b-after.json" jb)
  string(JSON sa GET "${ja}" song)
  string(JSON sb GET "${jb}" song)
  string(JSON ca GET "${ja}" channels)
  if(NOT sa STREQUAL sb)
    fail("${LA}/${LB}: the songs differ")
  endif()
  foreach(ch AlexCh SamCh)
    string(FIND "${ca}" "\"${ch}\"" at)
    if(at LESS 0)
      fail("${LA}/${LB}: channel ${ch} missing: ${ca}")
    endif()
  endforeach()
  # Going to the other's target changes nothing in the song.
  file(READ "${D}/a-before.json" jab)
  file(READ "${D}/b-before.json" jbb)
  string(JSON sab GET "${jab}" song)
  string(JSON sbb GET "${jbb}" song)
  if(NOT sab STREQUAL sa OR NOT sbb STREQUAL sb)
    fail("${LA}/${LB}: going to a collaborator's target changed the song")
  endif()
  # K-123: a take rated in one layout is rated in the other (ratings are song data).
  file(READ "${D}/run.out" runout)
  if(NOT runout MATCHES "rectake frame 52: track 3 take 0 rating 0 comp 0 clips 1; take 1 rating 2 comp 0 clips 1;")
    fail("${LA}/${LB}: Sam (${LB}) did not get the take Alex rated")
  endif()
  # 2. each receives the other's target
  peerTarget("${D}/b-targets.json" TB)
  peerTarget("${D}/a-targets.json" TA)
  if(NOT TB MATCHES "^channel:[0-9]+$" OR TB_shows STREQUAL "")
    fail("${LA}/${LB}: Sam (${LB}) did not get Alex's channel: '${TB}' '${TB_shows}'")
  endif()
  if(NOT TA MATCHES "^mixer:[0-9]+$" OR TA_shows STREQUAL "")
    fail("${LA}/${LB}: Alex (${LA}) did not get Sam's mixer track: '${TA}' '${TA_shows}'")
  endif()
  # 3. "go to" selects it in the other layout, which stays as it is
  foreach(who a b)
    if(NOT EXISTS "${D}/${who}-went.json")
      fail("${LA}/${LB}: no ${who}-went report")
      set(went_${who} "{}")
    else()
      file(READ "${D}/${who}-went.json" went_${who})
    endif()
  endforeach()
  string(JSON bl ERROR_VARIABLE e1 GET "${went_b}" layout)
  string(JSON bch ERROR_VARIABLE e2 GET "${went_b}" channel)
  if(NOT bl STREQUAL LB OR NOT bch STREQUAL TB_id)
    fail("${LA}/${LB}: in ${LB}, going to Alex's channel ${TB_id} selected '${bch}' (layout '${bl}')")
  endif()
  string(JSON al ERROR_VARIABLE e3 GET "${went_a}" layout)
  string(JSON amx ERROR_VARIABLE e4 GET "${went_a}" mixer)
  if(NOT al STREQUAL LA OR NOT amx STREQUAL TA_id)
    fail("${LA}/${LB}: in ${LA}, going to Sam's mixer track ${TA_id} selected '${amx}' (layout '${al}')")
  endif()
  message(STATUS "${LA}/${LB}: Sam sees ${TB} (${TB_shows}), Alex sees ${TA} (${TA_shows})")
endforeach()
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - collaborators in different layouts edit one song and go to what the other is on")
