# Gridline — J-212: the transport's Stop button, clicked with the transport already stopped (a
# second click, or the second half of a double-click), puts the song and pattern position back at
# the beginning and clears the set position; the first click still returns to the set position.
# Real SDL clicks on the top bar's Stop button (hidden, isolated config).
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j212_stop_to_start.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x (CMP0054: a quoted "keep" is that string, never a variable)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")

set(script "5 toasts clear
6 seekbeat 2 seek
8 transport
10 sdlat play
12 sdldown 0
13 sdlup 0
40 transport
42 sdlat stop
44 sdldown 0
45 sdlup 0
47 transport
50 sdldown 0
51 sdlup 0
53 transport
55 seekbeat 2 seek
57 sdlat play
59 sdldown 0
60 sdlup 0
85 transport
87 sdlat stop
89 sdldown 0
90 sdlup 0
91 sdldown 0
92 sdlup 0
94 transport
96 seekbeat 3 seek
98 transport
100 sdldown 0
101 sdlup 0
103 transport
")
file(WRITE "${OUT}/click.txt" "${script}")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --size 1600x900 --frames 108
                        --screenshot "${OUT}/s.ppm" --dev-input "${OUT}/click.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 200)
file(WRITE "${OUT}/run.log" "${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}")
endif()
foreach(button play stop)
  if(out MATCHES "sdlat frame [0-9]+: ${button} not shown")
    message(FATAL_ERROR "FAIL: the ${button} button is not in the top bar. Output:\n${out}")
  endif()
endforeach()

# "transport frame <f>: pos <p> playing <0|1> seek <s> mode <m>"
function(state f)
  if(NOT out MATCHES "transport frame ${f}: pos ([0-9]+) playing ([01]) seek ([0-9]+)")
    message(FATAL_ERROR "FAIL: no transport report at frame ${f}. Output:\n${out}")
  endif()
  set(pos "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(playing "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(seek "${CMAKE_MATCH_3}" PARENT_SCOPE)
endfunction()
function(want f what pos_ok playing_ok seek_ok)
  state(${f})
  if(NOT (pos STREQUAL "${pos_ok}" AND playing STREQUAL "${playing_ok}" AND seek STREQUAL "${seek_ok}"))
    message(FATAL_ERROR "FAIL (${what}): frame ${f} pos ${pos} playing ${playing} seek ${seek}, "
                        "wanted pos ${pos_ok} playing ${playing_ok} seek ${seek_ok}. Output:\n${out}")
  endif()
endfunction()

state(8)
set(marker "${seek}")
if(marker EQUAL 0 OR NOT pos STREQUAL marker)
  message(FATAL_ERROR "FAIL: the set position did not take (pos ${pos} seek ${seek}). Output:\n${out}")
endif()
state(40)
if(NOT playing EQUAL 1 OR NOT pos GREATER marker)
  message(FATAL_ERROR "FAIL: Play did not play past the set position (pos ${pos} playing ${playing}). Output:\n${out}")
endif()
want(47 "one click on Stop while playing: back to the set position" "${marker}" 0 "${marker}")
want(53 "a second click on Stop: back to the beginning" 0 0 0)
state(85)
if(NOT playing EQUAL 1)
  message(FATAL_ERROR "FAIL: Play did not start again. Output:\n${out}")
endif()
want(94 "a double-click on Stop while playing: back to the beginning" 0 0 0)
state(98)
if(seek EQUAL 0 OR NOT pos STREQUAL seek)
  message(FATAL_ERROR "FAIL: the set position did not take while stopped (pos ${pos} seek ${seek}). Output:\n${out}")
endif()
want(103 "one click on Stop with the transport stopped: back to the beginning" 0 0 0)
message(STATUS "j212: Stop returns to the set position, then (second click, double-click, stopped) to the beginning")
