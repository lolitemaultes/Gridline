# Gridline - J-111e: removing someone, leaving, and what happens to the files, with real Gridline
# windows (hidden, each with its own config) over real ICE (libjuice), driven by dev-input "collab"
# commands (src/app/CollabUi.cpp devCommand).
#   1. Alex makes a project collaborative, Sam joins; both edit and converge;
#      Alex removes Sam (with a note) and keeps working: Alex's project has one person, one removal;
#      Sam's open session stops by itself: removed, by Alex, with the note (the dialog's state);
#      Sam keeps a private copy: a normal project (no .gridline-collab) with the music, opened as such;
#      Sam's shared folder says "removed".
#   2. Sam opens his old folder again, alone: it opens removed at once (nothing syncs).
#   3. Kim joins, then leaves and keeps her copy: her folder is a normal project, opened;
#      Alex sees one person left.
#   4. Lee joins, then leaves and deletes his copy: the folder is in the (test) trash.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j111e_collab_remove_app.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x (CMP0054: a quoted "keep" is that string, never a variable)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_COLLAB_DEBUG} "1")

# Two people at once (a pipeline: the second must quit last, or the first meets a closed pipe);
# `cfgA` / `cfgB` name their configs (the same person keeps theirs).
function(runPair tag cfgA cfgB scriptA scriptB timeout)
  file(WRITE "${OUT}/${tag}-a.txt" "${scriptA}")
  file(WRITE "${OUT}/${tag}-b.txt" "${scriptB}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/${cfgA} "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/${tag}-a.txt"
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/${cfgB} ${ARGN} "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/${tag}-b.txt"
    RESULTS_VARIABLE rcs OUTPUT_FILE "${OUT}/${tag}.out" ERROR_FILE "${OUT}/${tag}.err" TIMEOUT ${timeout})
  message(STATUS "${tag}: exit codes ${rcs}")
  if(NOT rcs STREQUAL "0;0")
    message(FATAL_ERROR "FAIL (${tag}): exit codes ${rcs}")
  endif()
endfunction()

function(field file key var)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "FAIL: no report ${file}")
  endif()
  file(READ "${file}" j)
  string(JSON v ERROR_VARIABLE e GET "${j}" ${key})
  set(${var} "${v}" PARENT_SCOPE)
endfunction()

function(expectField file key want)
  field("${file}" ${key} v)
  if(NOT v STREQUAL "${want}")
    file(READ "${file}" j)
    message(FATAL_ERROR "FAIL: ${file}: ${key} is '${v}', expected '${want}'\n${j}")
  endif()
endfunction()

# ---- 1. join, converge, remove Sam while his project is open; he keeps a private copy ----
set(A1 "5 collab profile Alex
10 collab new \"${OUT}/Alex/Song\"
20 collab invite \"${OUT}/invite1.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
40 collab edit FromAlex1
41 waitfor collab-channel:FromSam1 60
42 waitfor collab-synced 60
43 waitfor never 2
50 collab remove Sam Testing removal
51 waitfor never 6
52 collab edit AfterRemoval
53 waitfor never 1
55 collab report \"${OUT}/a1.json\"
60 quit
")
set(B1 "5 collab profile Sam
10 collab join \"${OUT}/invite1.txt\" \"${OUT}/Sam/Song\"
11 waitfor collab-joined 120
40 collab edit FromSam1
41 waitfor collab-channel:FromAlex1 60
42 waitfor collab-synced 60
50 waitfor collab-removed 60
51 collab report \"${OUT}/b1.json\"
52 collab keep-copy \"${OUT}/SamCopy\"
53 waitfor collab-copy-done 10
54 collab report \"${OUT}/b1c.json\"
55 waitfor never 12
60 quit
")
runPair(1 cfgA cfgB "${A1}" "${B1}" 300)
expectField("${OUT}/a1.json" active 1)
expectField("${OUT}/a1.json" activeMembers 1)
expectField("${OUT}/a1.json" removed 1)
expectField("${OUT}/b1.json" active 0)
expectField("${OUT}/b1.json" exited removed)
expectField("${OUT}/b1.json" exitedBy Alex)
expectField("${OUT}/b1.json" exitedReason "Testing removal")
expectField("${OUT}/b1c.json" exited "")
expectField("${OUT}/b1c.json" active 0)
expectField("${OUT}/b1c.json" appProject "${OUT}/SamCopy/Song.gridline.json")
if(EXISTS "${OUT}/SamCopy/.gridline-collab" OR NOT EXISTS "${OUT}/SamCopy/Song.gridline.json")
  message(FATAL_ERROR "FAIL: the private copy is not a normal project")
endif()
file(READ "${OUT}/SamCopy/Song.gridline.json" copy)
foreach(ch FromAlex1 FromSam1)
  string(FIND "${copy}" "\"${ch}\"" at)
  if(at LESS 0)
    message(FATAL_ERROR "FAIL: the private copy lacks channel ${ch}")
  endif()
endforeach()
string(FIND "${copy}" "AfterRemoval" at)
if(NOT at LESS 0)
  message(FATAL_ERROR "FAIL: an edit made after the removal reached the removed member")
endif()
file(READ "${OUT}/Sam/Song/.gridline-collab/collab.json" flag)
string(JSON by GET "${flag}" removed byName)
if(NOT by STREQUAL "Alex")
  message(FATAL_ERROR "FAIL: Sam's shared folder is not marked removed: ${flag}")
endif()

# ---- 2. Sam opens his old folder again (alone): removed at once, nothing syncs ----
file(WRITE "${OUT}/2.txt" "10 collab open \"${OUT}/Sam/Song\"
11 waitfor collab-removed 20
12 collab report \"${OUT}/b2.json\"
20 quit
")
execute_process(COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgB "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/2.txt"
                RESULT_VARIABLE rc2 OUTPUT_FILE "${OUT}/2.out" ERROR_FILE "${OUT}/2.err" TIMEOUT 60)
if(NOT rc2 EQUAL 0)
  message(FATAL_ERROR "FAIL (reopen removed): exit code ${rc2}")
endif()
expectField("${OUT}/b2.json" active 0)
expectField("${OUT}/b2.json" exited removed)
expectField("${OUT}/b2.json" appProject "")

# ---- 3. Kim joins, then leaves and keeps her copy as a normal project ----
set(A3 "10 collab open \"${OUT}/Alex/Song\"
11 waitfor collab-ready 30
20 collab invite \"${OUT}/invite3.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
40 collab edit FromAlex3
41 waitfor collab-channel:FromKim3 60
42 waitfor never 15
45 collab report \"${OUT}/a3.json\"
50 quit
")
set(K3 "5 collab profile Kim
10 collab join \"${OUT}/invite3.txt\" \"${OUT}/Kim/Song\"
11 waitfor collab-joined 120
40 collab edit FromKim3
41 waitfor collab-channel:FromAlex3 60
42 waitfor collab-synced 60
43 collab leave keep
44 waitfor collab-left 30
45 collab report \"${OUT}/k3.json\"
46 waitfor never 25
50 quit
")
runPair(3 cfgA cfgC "${A3}" "${K3}" 300)
expectField("${OUT}/a3.json" left 1)
expectField("${OUT}/a3.json" activeMembers 1)
expectField("${OUT}/k3.json" active 0)
expectField("${OUT}/k3.json" appProject "${OUT}/Kim/Song/Song.gridline.json")
if(EXISTS "${OUT}/Kim/Song/.gridline-collab" OR NOT EXISTS "${OUT}/Kim/Song/Song.gridline.json")
  message(FATAL_ERROR "FAIL: Kim's copy is not a normal project after Leave > keep")
endif()
file(READ "${OUT}/cfgC/gridline/collab/known-projects.json" known)
string(FIND "${known}" "Kim/Song" at)
if(NOT at LESS 0)
  message(FATAL_ERROR "FAIL: the project Kim left is still in her collaborative projects")
endif()

# ---- 4. Lee joins, then leaves and deletes his copy: it goes to the trash ----
set(A4 "10 collab open \"${OUT}/Alex/Song\"
11 waitfor collab-ready 30
20 collab invite \"${OUT}/invite4.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
40 collab edit FromAlex4
41 waitfor collab-channel:FromLee4 60
42 waitfor never 12
45 collab report \"${OUT}/a4.json\"
50 quit
")
set(L4 "5 collab profile Lee
10 collab join \"${OUT}/invite4.txt\" \"${OUT}/Lee/Song\"
11 waitfor collab-joined 120
40 collab edit FromLee4
41 waitfor collab-channel:FromAlex4 60
42 waitfor collab-synced 60
43 collab leave delete
44 waitfor collab-left 30
45 collab report \"${OUT}/l4.json\"
46 waitfor never 22
50 quit
")
runPair(4 cfgA cfgD "${A4}" "${L4}" 300 GRIDLINE_TEST_TRASH_DIR=${OUT}/trash GRIDLINE_TEST_TRASH_ROOT=${OUT}/Lee)
expectField("${OUT}/a4.json" left 2)
expectField("${OUT}/l4.json" active 0)
expectField("${OUT}/l4.json" appProject "")
if(EXISTS "${OUT}/Lee/Song" OR NOT EXISTS "${OUT}/trash/Song/Song.gridline.json")
  message(FATAL_ERROR "FAIL: Leave > delete did not move the folder to the trash")
endif()
message(STATUS "ok - remove with the project open, private copy, reopen removed, leave keep, leave delete")
