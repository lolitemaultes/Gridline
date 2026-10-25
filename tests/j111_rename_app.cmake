# Gridline - J-111f: renaming a collaborative project, in two Gridline windows (hidden, each with its
# own config) over real ICE (libjuice), driven by dev-input "collab" commands (src/app/main.cpp).
#   1. Alex makes a project collaborative, Sam joins; Alex renames it to "Night Drive: take 2" while both
#      are online: Sam's name, window title and project file follow at once (the file name without the
#      colon), and Sam's Recent collaborative projects shows the new name;
#   2. Alex opens the project alone and renames it to "Offline rename" (Sam's Gridline is closed);
#   3. both open it again (Sam from the file his Recent files remembers): Sam catches up and gets the
#      new name, title and file.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j111_rename_app.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_COLLAB_DEBUG} "1")

# (a pipeline: Alex's output goes to Sam's input, so Sam's window stays open until Alex quit)
function(runPair tag scriptA scriptB timeout)
  file(WRITE "${OUT}/${tag}-a.txt" "${scriptA}")
  file(WRITE "${OUT}/${tag}-b.txt" "${scriptB}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgA "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/${tag}-a.txt"
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgB "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/${tag}-b.txt"
    RESULTS_VARIABLE rcs OUTPUT_FILE "${OUT}/${tag}.out" ERROR_FILE "${OUT}/${tag}.err" TIMEOUT ${timeout})
  if(NOT rcs STREQUAL "0;0")
    message(FATAL_ERROR "FAIL (${tag}): exit codes ${rcs}")
  endif()
endfunction()

# The report of one window: its project's name, the window title, the project file it uses.
function(expectName report name file)
  if(NOT EXISTS "${OUT}/${report}.json")
    message(FATAL_ERROR "FAIL: no report ${report}")
  endif()
  file(READ "${OUT}/${report}.json" j)
  string(JSON n GET "${j}" name)
  string(JSON t GET "${j}" title)
  string(JSON pf GET "${j}" projectFile)
  string(JSON ap GET "${j}" appProject)
  string(JSON ready GET "${j}" ready)
  message(STATUS "${report}: name '${n}', title '${t}', file ${pf}")
  if(NOT ready EQUAL 1 OR NOT n STREQUAL "${name}")
    message(FATAL_ERROR "FAIL (${report}): name '${n}' (expected '${name}')")
  endif()
  string(FIND "${t}" "${name}" at)
  if(at LESS 0)
    message(FATAL_ERROR "FAIL (${report}): the window title '${t}' does not show '${name}'")
  endif()
  if(NOT pf STREQUAL "${file}" OR NOT ap STREQUAL "${file}" OR NOT EXISTS "${file}")
    message(FATAL_ERROR "FAIL (${report}): project file ${pf} / app ${ap} (expected ${file})")
  endif()
endfunction()

# ---- 1: a live rename ----
runPair(1 "5 collab profile Alex
10 collab new \"${OUT}/Alex/Song\"
20 collab invite \"${OUT}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
40 collab rename \"Night Drive: take 2\"
41 waitfor never 4
42 collab report \"${OUT}/a1.json\"
50 quit
" "5 collab profile Sam
10 collab join \"${OUT}/invite.txt\" \"${OUT}/Sam/Song\"
11 waitfor collab-joined 120
40 waitfor \"collab-name:Night Drive: take 2\" 90
41 waitfor never 3
42 collab report \"${OUT}/b1.json\"
43 waitfor never 8
50 quit
" 300)
expectName(a1 "Night Drive: take 2" "${OUT}/Alex/Song/Night Drive- take 2.gridline.json")
expectName(b1 "Night Drive: take 2" "${OUT}/Sam/Song/Night Drive- take 2.gridline.json")
if(EXISTS "${OUT}/Sam/Song/Song.gridline.json")
  message(FATAL_ERROR "FAIL (live): Sam's old project file is still there")
endif()
file(READ "${OUT}/cfgB/gridline/collab/known-projects.json" known)
string(FIND "${known}" "Night Drive: take 2" at)
if(at LESS 0)
  message(FATAL_ERROR "FAIL (live): Sam's Recent collaborative projects keeps the old name: ${known}")
endif()

# ---- 2: Alex renames while Sam's Gridline is closed ----
file(WRITE "${OUT}/2-a.txt" "10 collab open \"${OUT}/Alex/Song\"
11 waitfor collab-ready 30
20 collab rename \"Offline rename\"
21 waitfor never 3
22 collab report \"${OUT}/a2.json\"
30 quit
")
execute_process(COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgA "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/2-a.txt"
                RESULT_VARIABLE rc2 OUTPUT_FILE "${OUT}/2.out" ERROR_FILE "${OUT}/2.err" TIMEOUT 120)
if(NOT rc2 EQUAL 0)
  message(FATAL_ERROR "FAIL (offline rename): exit code ${rc2}")
endif()
expectName(a2 "Offline rename" "${OUT}/Alex/Song/Offline rename.gridline.json")

# ---- 3: both open it again; Sam (from his old Recent files entry) catches up ----
runPair(3 "10 collab open \"${OUT}/Alex/Song/Offline rename.gridline.json\"
11 waitfor collab-ready 30
12 waitfor collab-connected 200
13 waitfor collab-synced 60
14 waitfor never 4
15 collab report \"${OUT}/a3.json\"
30 quit
" "10 collab open \"${OUT}/Sam/Song/Night Drive- take 2.gridline.json\"
11 waitfor collab-ready 30
12 waitfor collab-connected 200
13 waitfor \"collab-name:Offline rename\" 60
14 waitfor never 3
15 collab report \"${OUT}/b3.json\"
16 waitfor never 8
30 quit
" 400)
expectName(b3 "Offline rename" "${OUT}/Sam/Song/Offline rename.gridline.json")
file(READ "${OUT}/cfgB/gridline/collab/known-projects.json" known)
string(FIND "${known}" "Offline rename" at)
if(at LESS 0)
  message(FATAL_ERROR "FAIL (catch-up): Sam's Recent collaborative projects keeps the old name: ${known}")
endif()
message(STATUS "ok - live rename, offline rename caught up after reconnecting")
