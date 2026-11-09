# Gridline - K-141: undo and redo in a collaborative project, through the real shortcuts, in two
# Gridline windows (hidden, each with its own config) over real ICE, once per window layout (each
# brings its own shortcut set):
#   1. Alex adds channel A1, Sam then adds S1;
#   2. Alex presses Undo: A1 goes, Sam's S1 stays (on both computers);
#   3. Alex presses Redo: A1 is back; Alex adds Mark;
#   4. Sam presses Undo: S1 goes, Alex's A1 and Mark stay; Sam adds MarkB;
#   5. (both end with the same song, checked after step 6);
#   6. Alex adds K1, Sam renames it, Alex presses Undo: the channel stays (Sam used it).
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k141_collab_undo_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

# Command on macOS (Ctrl elsewhere): Undo / Redo in every shortcut set.
if(APPLE)
  set(M super)
else()
  set(M ctrl)
endif()

function(channelsOf file out)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "FAIL: no report ${file}")
  endif()
  file(READ "${file}" j)
  string(JSON chans GET "${j}" channels)
  string(JSON song GET "${j}" song)
  set(${out} "${chans}" PARENT_SCOPE)
  set(${out}_song "${song}" PARENT_SCOPE)
endfunction()

function(expectChannels tag file)
  cmake_parse_arguments(E "" "" "HAS;NOT" ${ARGN})
  channelsOf("${file}" C)
  foreach(ch IN LISTS E_HAS)
    string(FIND "${C}" "\"${ch}\"" at)
    if(at LESS 0)
      message(FATAL_ERROR "FAIL (${tag}): channel ${ch} missing: ${C}")
    endif()
  endforeach()
  foreach(ch IN LISTS E_NOT)
    string(FIND "${C}" "\"${ch}\"" at)
    if(NOT at LESS 0)
      message(FATAL_ERROR "FAIL (${tag}): channel ${ch} should be gone: ${C}")
    endif()
  endforeach()
endfunction()

foreach(layout studio live logic)
  set(A "5 collab profile Alex
10 collab new \"${OUT}/${layout}/Alex/Song\"
20 collab invite \"${OUT}/${layout}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
40 collab edit A1
41 waitfor collab-channel:S1 60
42 waitfor collab-synced 60
43 waitfor never 2
50 sdlkey z down ${M}
51 sdlkey z up ${M}
52 waitfor never 3
53 collab report \"${OUT}/${layout}/a-undo.json\"
60 sdlkey z down ${M}+shift
61 sdlkey z up ${M}+shift
62 waitfor never 3
63 collab report \"${OUT}/${layout}/a-redo.json\"
64 collab edit Mark
70 waitfor collab-channel:MarkB 90
71 waitfor collab-synced 60
72 waitfor never 2
73 collab report \"${OUT}/${layout}/a-end.json\"
74 collab edit K1
75 waitfor collab-channel:K1s 90
76 waitfor collab-synced 60
77 waitfor never 2
78 sdlkey z down ${M}
79 sdlkey z up ${M}
80 waitfor never 3
81 collab report \"${OUT}/${layout}/a-kept.json\"
90 quit
")
  set(B "5 collab profile Sam
10 collab join \"${OUT}/${layout}/invite.txt\" \"${OUT}/${layout}/Sam/Song\"
11 waitfor collab-joined 120
12 waitfor collab-synced 60
40 waitfor collab-channel:A1 60
41 collab edit S1
42 waitfor collab-channel:Mark 120
43 waitfor collab-synced 60
44 waitfor never 2
45 collab report \"${OUT}/${layout}/b-mark.json\"
50 sdlkey z down ${M}
51 sdlkey z up ${M}
52 waitfor never 3
53 collab edit MarkB
54 waitfor collab-synced 60
55 waitfor never 4
56 collab report \"${OUT}/${layout}/b-end.json\"
57 waitfor collab-channel:K1 90
58 waitfor never 1
59 collab rename-channel K1 K1s
60 waitfor never 10
61 collab report \"${OUT}/${layout}/b-kept.json\"
90 quit
")
  file(MAKE_DIRECTORY "${OUT}/${layout}")
  file(WRITE "${OUT}/${layout}/a.txt" "${A}")
  file(WRITE "${OUT}/${layout}/b.txt" "${B}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/${layout}/cfgA HOME=${OUT}/${layout}/homeA
            "${APP}" --hidden --no-ui-sounds --layout ${layout} --dev-input "${OUT}/${layout}/a.txt"
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/${layout}/cfgB HOME=${OUT}/${layout}/homeB
            "${APP}" --hidden --no-ui-sounds --layout ${layout} --dev-input "${OUT}/${layout}/b.txt"
    RESULTS_VARIABLE rcs OUTPUT_FILE "${OUT}/${layout}/run.out" ERROR_FILE "${OUT}/${layout}/run.err" TIMEOUT 420)
  message(STATUS "${layout}: exit codes ${rcs}")
  expectChannels("${layout}: Alex's undo" "${OUT}/${layout}/a-undo.json" HAS S1 NOT A1)
  expectChannels("${layout}: Alex's redo" "${OUT}/${layout}/a-redo.json" HAS S1 A1)
  expectChannels("${layout}: Sam sees Alex's redo" "${OUT}/${layout}/b-mark.json" HAS A1 S1 Mark)
  expectChannels("${layout}: Sam's undo, Alex's side" "${OUT}/${layout}/a-end.json" HAS A1 Mark MarkB NOT S1)
  expectChannels("${layout}: Sam's undo, Sam's side" "${OUT}/${layout}/b-end.json" HAS A1 Mark MarkB NOT S1)
  # 6. Alex undoes adding a channel Sam has renamed since: it stays (with a message), on both sides
  expectChannels("${layout}: kept, Alex's side" "${OUT}/${layout}/a-kept.json" HAS K1s A1 MarkB NOT S1)
  expectChannels("${layout}: kept, Sam's side" "${OUT}/${layout}/b-kept.json" HAS K1s)
  channelsOf("${OUT}/${layout}/a-kept.json" EA)
  channelsOf("${OUT}/${layout}/b-kept.json" EB)
  if(NOT EA_song STREQUAL EB_song)
    message(FATAL_ERROR "FAIL (${layout}): the songs differ:\n${EA}\n${EB}")
  endif()
  if(NOT rcs STREQUAL "0;0")
    message(FATAL_ERROR "FAIL (${layout}): exit codes ${rcs}")
  endif()
endforeach()
message(STATUS "ok - undo and redo take back only your own steps, with the real shortcuts, in every layout")
