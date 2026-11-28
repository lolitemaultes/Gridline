# Gridline: Import from URL into a plugin in a collaborative project, in two hidden app windows
# (each with its own config) over the app's real network path, with a stand-in yt-dlp / ffmpeg
# (no network):
#   1. Alex makes a project collaborative, invites Sam, adds a Sampler channel;
#   2. Alex imports a link's audio into it (the plugin's own load): Sam's project has the channel
#      playing the file, and the file itself arrives in Sam's project audio, as a dropped file does;
#   3. Alex undoes the import: it is undone on both computers.
#   cmake -DAPP=<gridline-app> -DTOOL=<fake_media_tool> -DOUT=<dir> -P k443_url_collab_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_NO_UPDATE_CHECK} "1")
set(ENV{GRIDLINE_YTDLP} "${TOOL}")
set(ENV{GRIDLINE_FFMPEG} "${TOOL}")
set(ENV{GL_FAKE_SCENARIO} "ok")
unset(ENV{GRIDLINE_URLIMPORT_FAKE_NET})
if(APPLE)
  set(M super)
else()
  set(M ctrl)
endif()
set(NAME "Fake Song Take One")

set(A "5 collab profile Alex
10 collab new \"${OUT}/Alex/Song\"
20 collab invite \"${OUT}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
35 addvia rack gl.sampler
36 waitfor collab-synced 60
37 waitfor never 2
38 collab report \"${OUT}/a0.json\"
40 urlimport forplugin sel 0
41 urlimport url https://example.com/watch?v=k443collab
42 urlimport fetch
43 waitfor urlimport-preview 60
45 urlimport import
46 waitfor urlimport-done 60
47 waitfor collab-synced 60
48 waitfor never 3
49 collab report \"${OUT}/a1.json\"
50 waitfor \"file:${OUT}/b1.json\" 180
55 sdlkey z down ${M}
56 sdlkey z up ${M}
57 waitfor collab-synced 60
58 waitfor never 3
59 collab report \"${OUT}/a2.json\"
60 waitfor \"file:${OUT}/b2.json\" 180
70 quit discard
")
set(B "5 collab profile Sam
10 collab join \"${OUT}/invite.txt\" \"${OUT}/Sam/Song\"
11 waitfor collab-joined 120
12 waitfor collab-synced 60
40 waitfor \"collab-channel:${NAME}\" 180
41 waitfor never 6
42 collab report \"${OUT}/b1.json\"
43 waitfor \"file:${OUT}/a2.json\" 180
44 waitfor never 6
45 collab report \"${OUT}/b2.json\"
50 quit discard
")
file(WRITE "${OUT}/a.txt" "${A}")
file(WRITE "${OUT}/b.txt" "${B}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgA HOME=${OUT}/homeA "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/a.txt"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgB HOME=${OUT}/homeB "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/b.txt"
  RESULTS_VARIABLE rcs OUTPUT_FILE "${OUT}/run.out" ERROR_FILE "${OUT}/run.err" TIMEOUT 600)
message(STATUS "exit codes: ${rcs}")

function(load name prefix)
  if(NOT EXISTS "${OUT}/${name}.json")
    message(FATAL_ERROR "FAIL: no report ${name}.json (exit codes ${rcs})")
  endif()
  file(READ "${OUT}/${name}.json" j)
  set(${prefix} "${j}" PARENT_SCOPE)
endfunction()
function(must what)
  if(${ARGN})
    message(STATUS "ok: ${what}")
  else()
    message(FATAL_ERROR "FAIL: ${what} (${ARGN})")
  endif()
endfunction()

foreach(r a0 a1 b1 a2 b2)
  load(${r} J)
  string(JSON ch_${r} GET "${J}" channels)
  string(JSON song_${r} GET "${J}" song)
endforeach()
string(FIND "${ch_a0}" "\"${NAME}\"" at)
must("before the import no channel plays the file" at EQUAL -1)
string(FIND "${ch_a1}" "\"${NAME}\"" at)
must("Alex's Sampler plays the imported file (named after it)" at GREATER -1)
string(FIND "${ch_b1}" "\"${NAME}\"" at)
must("Sam's project has the same channel" at GREATER -1)
must("both songs match after the import" song_a1 STREQUAL song_b1)
file(GLOB samWavs "${OUT}/Sam/Song/Audio/*.wav")
list(LENGTH samWavs nw)
must("the imported file arrived in Sam's project audio (${nw})" nw GREATER 0)
string(FIND "${ch_a2}" "\"${NAME}\"" at)
must("Alex's undo takes the import back" at EQUAL -1)
string(FIND "${ch_b2}" "\"${NAME}\"" at)
must("and on Sam's computer too" at EQUAL -1)
must("both songs match after the undo" song_a2 STREQUAL song_b2)
must("both songs are back where they were" song_a2 STREQUAL song_a0)
if(NOT rcs STREQUAL "0;0")
  message(FATAL_ERROR "FAIL: exit codes ${rcs}")
endif()
