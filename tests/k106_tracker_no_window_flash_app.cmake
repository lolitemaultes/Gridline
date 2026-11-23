# Gridline - K-106 cp22: in the Tracker, clicking an instrument never shows the shared plugin or
# channel settings window, not even for one frame. Before, a click in the instrument list, on
# Instrument or Sample, a double-click, Ctrl+Alt+I / Ctrl+Alt+E or the Steps page's channel button
# drew the other layouts' channel settings window over the Tracker for a frame or more before the
# lower band's editor (or Channel frame) claimed them.
#   Every path is clicked with the real mouse or keys; the app counts, each frame, whether the
#   channel settings or effect editor window (or an installed plugin's own window) was up without
#   "Open plugin window" asking for it ("tracker editors windows"). It must stay 0, and each path
#   must still open the band. "Open plugin window" then floats the channel settings as before.
#   A double-click on an instrument without a Tracker screen of its own (a synth) floats its window,
#   as it does in Studio (asked for, so not counted): nothing of it in the band.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DWAV=<audio file> -DOUT=<dir> -P k106_tracker_no_window_flash_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"tracker\", \"keys.profile\": \"tracker\", \"ui.menuStyle\": \"tracker\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
configure_file("${WAV}" "${OUT}/smp.wav" COPYONLY)

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n(the run's log: ${OUT}/run.log)")
endfunction()

# The header at 1440x900: the instrument list's rows, the Instrument and Sample buttons.
set(LISTX 930)
set(ROW0 41)
set(ROW1 57)
set(ROW2 73)
set(INSX 809)
set(INSY 164)
set(SMPX 809)
set(SMPY 185)

set(E "tracker editors")
# The first instrument becomes a Tracker Sampler with a sample (the Sample path needs one).
file(WRITE "${OUT}/input.txt" "5 toasts clear
6 ${E} open frame 0
8 replace gl.tracker
10 ${E} load \"${OUT}/smp.wav\"
14 ${E} close
18 ${E} windows reset
20 ${E} windows
22 move ${LISTX} ${ROW1}
24 down 0
25 up 0
30 ${E} windows
32 move ${LISTX} ${ROW1}
34 down 0
35 up 0
36 down 0
37 up 0
44 ${E} windows
46 ${E} close
50 move ${LISTX} ${ROW0}
52 down 0
53 up 0
56 move ${INSX} ${INSY}
58 down 0
59 up 0
66 ${E} windows
68 move ${INSX} ${INSY}
70 down 0
71 up 0
76 ${E} windows
78 move ${SMPX} ${SMPY}
80 down 0
81 up 0
88 ${E} windows
90 ${E} close
94 sdlkey i down ctrl+alt
95 sdlkey i up ctrl+alt
96 sdlkey #224 up
97 sdlkey #226 up
102 ${E} windows
104 ${E} close
108 sdlkey e down ctrl+alt
109 sdlkey e up ctrl+alt
110 sdlkey #224 up
111 sdlkey #226 up
116 ${E} windows
118 ${E} close
122 move ${LISTX} ${ROW2}
124 down 0
125 up 0
126 down 0
127 up 0
134 ${E} windows
136 ${E} close
140 tracker page Steps
146 ${E} windows
148 move 60 328
150 down 0
151 up 0
152 down 0
153 up 0
158 ${E} windows
160 ${E} close
164 tracker page Pattern
168 ${E} windows
170 ${E} plugin 1
178 ${E} windows
180 quit discard
")

execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --video-clock 60 --size 1440x900 --layout tracker
                        --dev-input "${OUT}/input.txt" --fixed-dt 16 --frames 200 --screenshot "${OUT}/end.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed (${rc})")
endif()

# frame, what the band must show, and the path's name
macro(expect frame band what)
  if(NOT out MATCHES "tracker windows frame ${frame}: watched ([0-9]+) shown ([0-9]+) hosted ([0-9]+) blocked ([0-9]+) first (-?[0-9]+) ([^\n]*) plugin ([a-z]+) band ([a-z]+)")
    fail("no window report on frame ${frame} (${what})")
  endif()
  set(_watched ${CMAKE_MATCH_1})
  set(_shown ${CMAKE_MATCH_2})
  set(_hosted ${CMAKE_MATCH_3})
  set(_first ${CMAKE_MATCH_5})
  set(_band ${CMAKE_MATCH_8})
  if(_watched LESS 1)
    fail("${what}: the window watch did not run")
  endif()
  if(NOT _shown EQUAL 0 OR NOT _hosted EQUAL 0)
    fail("${what}: a plugin or channel window showed over the Tracker (${_shown} frames, plugin windows ${_hosted}, first on frame ${_first})")
  endif()
  if("${band}" STREQUAL "float")  # (its window floats, asked for: the band stays closed)
    if(NOT _band STREQUAL "closed" OR NOT CMAKE_MATCH_7 STREQUAL "open")
      fail("${what}: expected its window floating and the band closed (plugin ${CMAKE_MATCH_7}, band ${_band})")
    endif()
  elseif(NOT "${band}" STREQUAL "any" AND NOT _band STREQUAL "${band}")
    fail("${what}: the lower band shows '${_band}', expected '${band}'")
  endif()
  message(STATUS "${what}: no window, band ${_band}")
endmacro()

expect(20 closed "start")
expect(30 closed "a click in the instrument list (selects)")
expect(44 float "a double-click in the instrument list (a synth: its window floats)")
expect(66 instrument "Instrument")
expect(76 closed "Instrument again (closes)")
expect(88 sample "Sample")
expect(102 instrument "Ctrl+Alt+I")
expect(116 sample "Ctrl+Alt+E")
expect(134 float "a double-click on another instrument")
expect(146 closed "the Steps page")
expect(158 float "a double-click on a channel on the Steps page")

# "Open plugin window" still floats the channel settings (and the band leaves them alone).
if(NOT out MATCHES "tracker windows frame 178: [^\n]* plugin open band ([a-z]+)")
  fail("Open plugin window did not open the plugin window")
endif()
if(NOT out MATCHES "tracker windows frame 178: watched [0-9]+ shown 0 hosted 0")
  fail("the window watch counted the plugin window Open plugin window asked for")
endif()

# A project saved with a built-in's window open (K-297's restore, Song::pluginWindows) opened in the
# Tracker: no window on any frame (the band may show the instrument).
file(READ "${DEMO}" song)
if(NOT song MATCHES "\"id\": ([0-9]+)")
  fail("the song has no channel id")
endif()
string(REGEX REPLACE "^{" "{\n  \"pluginWindows\": [[${CMAKE_MATCH_1}, -1]]," song "${song}")
file(WRITE "${OUT}/restore.gridline.json" "${song}")
file(WRITE "${OUT}/input2.txt" "30 ${E} windows
32 quit discard
")
execute_process(COMMAND "${APP}" "${OUT}/restore.gridline.json" --hidden --no-ui-sounds --video-clock 60 --size 1440x900 --layout tracker
                        --dev-input "${OUT}/input2.txt" --fixed-dt 16 --frames 40 --screenshot "${OUT}/end2.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
file(WRITE "${OUT}/run2.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed on the saved plugin window (${rc})")
endif()
expect(30 any "a project saved with a plugin window open")
message(STATUS "k106_tracker_no_window_flash_app: ok")
