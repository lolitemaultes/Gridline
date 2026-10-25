# Gridline - J-111: two Gridline windows (hidden, each with its own config) collaborate over real ICE
# (libjuice) with the app's default network settings, driven by dev-input "collab" commands
# (src/app/main.cpp), then quit and reopen the project and find each other again.
#   1. Alex makes a new project collaborative and writes an invite code to a file;
#      Sam joins with it 8 s later (the delay that used to matter);
#   2. both add a channel; each sees the other's; both report the same song, two members, nothing unseen;
#   3. both quit; Alex reopens the project file (as Recent files does), Sam 15 s later the project
#      folder (as File > Open collaborative project does): they reconnect by themselves (stored
#      addresses and ports, no code); Sam is the same member (still two members);
#   4. both add a channel again; both see all four channels, the same song, nothing unseen;
#   5. Alex invites Sam again and Sam "joins" into another folder: his copy reconnects instead
#      (same member, same folder, nothing copied again).
# Also: Sam's join is held back (GRIDLINE_COLLAB_TEST_ICE_DELAY, 32 s: ICE that connects only after the
# old 30 s handshake budget ran out) in a second run of step 1 and must still connect.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j111_collab_app.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_COLLAB_DEBUG} "1")

# Runs Alex's and Sam's scripts at the same time (a pipeline); reports land in ${OUT}/<tag>-<who>.json.
function(runPair tag scriptA scriptB timeout)
  file(WRITE "${OUT}/${tag}-a.txt" "${scriptA}")
  file(WRITE "${OUT}/${tag}-b.txt" "${scriptB}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgA${tag}
            "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/${tag}-a.txt"
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgB${tag} ${ARGN}
            "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/${tag}-b.txt"
    RESULTS_VARIABLE rcs OUTPUT_FILE "${OUT}/${tag}.out" ERROR_FILE "${OUT}/${tag}.err" TIMEOUT ${timeout})
  set(rcs_${tag} "${rcs}" PARENT_SCOPE)
endfunction()

function(readReport file prefix)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "FAIL: no report ${file}")
  endif()
  file(READ "${file}" j)
  foreach(k song members unseen connected ready)
    string(JSON v GET "${j}" ${k})
    set(${prefix}_${k} "${v}" PARENT_SCOPE)
  endforeach()
  string(JSON chans GET "${j}" channels)
  set(${prefix}_channels "${chans}" PARENT_SCOPE)
endfunction()

function(expectConverged tag a b)
  readReport("${OUT}/${a}.json" A)
  readReport("${OUT}/${b}.json" B)
  message(STATUS "${tag}: alex ${A_song} members ${A_members} unseen ${A_unseen}; sam ${B_song} members ${B_members} unseen ${B_unseen}")
  if(NOT A_connected EQUAL 1 OR NOT B_connected EQUAL 1 OR NOT A_ready EQUAL 1 OR NOT B_ready EQUAL 1)
    message(FATAL_ERROR "FAIL (${tag}): not connected / ready (alex ${A_connected}/${A_ready}, sam ${B_connected}/${B_ready})")
  endif()
  if(NOT A_members EQUAL 2 OR NOT B_members EQUAL 2)
    message(FATAL_ERROR "FAIL (${tag}): members ${A_members} / ${B_members} (expected 2)")
  endif()
  if(NOT A_song STREQUAL B_song)
    message(FATAL_ERROR "FAIL (${tag}): the songs differ:\n${A_channels}\n${B_channels}")
  endif()
  if(NOT A_unseen EQUAL 0 OR NOT B_unseen EQUAL 0)
    message(FATAL_ERROR "FAIL (${tag}): changes not seen yet: alex ${A_unseen}, sam ${B_unseen}")
  endif()
  foreach(ch IN LISTS ARGN)
    string(FIND "${B_channels}" "\"${ch}\"" at)
    if(at LESS 0)
      message(FATAL_ERROR "FAIL (${tag}): channel ${ch} missing: ${B_channels}")
    endif()
  endforeach()
endfunction()

# ---- 1 + 2: create, invite, join 8 s later, edits converge, quit ----
set(A1 "5 collab profile Alex
10 collab new \"${OUT}/Alex/Song\"
20 collab invite \"${OUT}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
40 collab edit FromAlex1
41 waitfor collab-channel:FromSam1 60
42 waitfor collab-synced 60
43 waitfor never 3
44 collab links
45 collab report \"${OUT}/a1.json\"
50 quit
")
set(B1 "5 collab profile Sam
6 waitfor never 8
10 collab join \"${OUT}/invite.txt\" \"${OUT}/Sam/Song\"
11 waitfor collab-joined 120
40 collab edit FromSam1
41 waitfor collab-channel:FromAlex1 60
42 waitfor collab-synced 60
43 waitfor never 3
44 collab links
45 collab report \"${OUT}/b1.json\"
50 quit
")
runPair(1 "${A1}" "${B1}" 300)
message(STATUS "phase 1 exit codes: ${rcs_1}")
expectConverged("join" a1 b1 FromAlex1 FromSam1)
if(NOT rcs_1 STREQUAL "0;0")
  message(FATAL_ERROR "FAIL (join): exit codes ${rcs_1}")
endif()

# ---- 3 + 4: both reopen (Alex the project file, Sam the folder 15 s later), reconnect, converge ----
set(A2 "10 collab open \"${OUT}/Alex/Song/Song.gridline.json\"
11 waitfor collab-ready 30
12 waitfor collab-connected 200
13 collab links
40 collab edit FromAlex2
41 waitfor collab-channel:FromSam2 60
42 waitfor collab-synced 60
43 waitfor never 3
45 collab report \"${OUT}/a2.json\"
50 quit
")
set(B2 "5 waitfor never 15
10 collab open \"${OUT}/Sam/Song\"
11 waitfor collab-ready 30
12 waitfor collab-connected 200
13 collab links
40 collab edit FromSam2
41 waitfor collab-channel:FromAlex2 60
42 waitfor collab-synced 60
43 waitfor never 3
45 collab report \"${OUT}/b2.json\"
50 quit
")
# (the configs of phase 1: the same people on the same computers)
file(WRITE "${OUT}/2-a.txt" "${A2}")
file(WRITE "${OUT}/2-b.txt" "${B2}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgA1 "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/2-a.txt"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgB1 "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/2-b.txt"
  RESULTS_VARIABLE rcs2 OUTPUT_FILE "${OUT}/2.out" ERROR_FILE "${OUT}/2.err" TIMEOUT 400)
message(STATUS "phase 2 exit codes: ${rcs2}")
expectConverged("reopen" a2 b2 FromAlex1 FromSam1 FromAlex2 FromSam2)
if(NOT rcs2 STREQUAL "0;0")
  message(FATAL_ERROR "FAIL (reopen): exit codes ${rcs2}")
endif()
file(READ "${OUT}/cfgB1/gridline/collab/known-projects.json" known)
string(FIND "${known}" "Sam/Song" at)
if(at LESS 0)
  message(FATAL_ERROR "FAIL: the project is not in Sam's collaborative projects: ${known}")
endif()

# ---- 5: re-inviting an existing member reconnects him ----
file(WRITE "${OUT}/4-a.txt" "10 collab open \"${OUT}/Alex/Song\"
11 waitfor collab-ready 30
20 collab invite \"${OUT}/invite4.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
40 collab edit FromAlex4
41 waitfor collab-channel:FromSam4 60
42 waitfor collab-synced 60
43 waitfor never 3
45 collab report \"${OUT}/a4.json\"
50 quit
")
file(WRITE "${OUT}/4-b.txt" "10 collab join \"${OUT}/invite4.txt\" \"${OUT}/Sam/Other folder\"
11 waitfor collab-joined 120
40 collab edit FromSam4
41 waitfor collab-channel:FromAlex4 60
42 waitfor collab-synced 60
43 waitfor never 3
45 collab report \"${OUT}/b4.json\"
50 quit
")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgA1 "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/4-a.txt"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgB1 "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/4-b.txt"
  RESULTS_VARIABLE rcs4 OUTPUT_FILE "${OUT}/4.out" ERROR_FILE "${OUT}/4.err" TIMEOUT 300)
expectConverged("re-invite" a4 b4 FromAlex1 FromSam2 FromAlex4 FromSam4)
file(READ "${OUT}/b4.json" j)
string(JSON pf GET "${j}" projectFile)
if(NOT pf MATCHES "/Sam/Song/Song.gridline.json$" OR EXISTS "${OUT}/Sam/Other folder")
  message(FATAL_ERROR "FAIL (re-invite): Sam's project is ${pf} (expected his existing copy in Sam/Song)")
endif()

# ---- ICE that connects late (32 s) still joins ----
set(A3 "5 collab profile Alex
10 collab new \"${OUT}/Alex3/Song\"
20 collab invite \"${OUT}/invite3.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
40 collab edit FromAlex3
41 waitfor collab-channel:FromSam3 60
42 waitfor collab-synced 60
43 waitfor never 3
45 collab report \"${OUT}/a3.json\"
50 quit
")
set(B3 "5 collab profile Sam
10 collab join \"${OUT}/invite3.txt\" \"${OUT}/Sam3/Song\"
11 waitfor collab-joined 120
40 collab edit FromSam3
41 waitfor collab-channel:FromAlex3 60
42 waitfor collab-synced 60
43 waitfor never 3
45 collab report \"${OUT}/b3.json\"
50 quit
")
runPair(3 "${A3}" "${B3}" 300 GRIDLINE_COLLAB_TEST_ICE_DELAY=32)
message(STATUS "late ICE exit codes: ${rcs_3}")
expectConverged("late ICE" a3 b3 FromAlex3 FromSam3)
message(STATUS "ok - join, reopen and reconnect, re-invite, late ICE")
