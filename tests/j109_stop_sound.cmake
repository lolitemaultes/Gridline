# Gridline — J-109: Stop silences sound started outside the song, in the real app (hidden,
# isolated config, SDL's dummy audio driver: the engine runs in real time, nothing is heard).
# The demo copy is opened with its bass channel selected; the script then
#   1. holds a preview key (a live note, as the piano roll keyboard / pads / auditions send) and
#      clicks Stop: the note is released and dies out (it used to ring forever);
#   2. holds a typed key (typing keyboard, a real SDL key-down, never released) and takes the
#      focus away: the note is released;
#   3. starts the browser's sample preview and clicks Stop: the preview stops;
#   4. plays the song, stops with Space (a release, not a panic), clicks Stop again: the panic
#      (all sound off, tails cleared): silence.
#   cmake -DAPP=<gridline-app> -DCLI=<gridline> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j109_stop_sound.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
configure_file("${DEMO}" "${OUT}/demo copy.gridline.json" COPYONLY)
# A long sample for the browser preview: the demo song rendered.
execute_process(COMMAND "${CLI}" render "${OUT}/demo copy.gridline.json" -o "${OUT}/preview.wav"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "rendering the preview sample failed (${rc}): ${err}")
endif()

set(script "5 toasts clear
8 sound
10 noteon 5 48
20 waitfor j109-none 0.4
21 sound
22 sdlat stop
24 sdldown 0
25 sdlup 0
27 sound
28 waitfor j109-none 0.15
29 sound
30 waitfor j109-none 5
31 sound
40 typing on
42 sdlkey z down
50 waitfor j109-none 0.4
51 sound
52 focuslost
54 waitfor j109-none 0.2
55 sound
60 waitfor j109-none 5
61 sound
62 typing off
70 previewfile \"${OUT}/preview.wav\"
72 waitfor j109-none 0.4
73 sound
74 sdlat stop
76 sdldown 0
77 sdlup 0
79 sound
80 waitfor j109-none 0.3
81 sound
90 sdlat play
92 sdldown 0
93 sdlup 0
95 waitfor j109-none 1.5
96 sound
98 key space
100 waitfor j109-none 0.1
101 sound
102 sdlat stop
104 sdldown 0
105 sdlup 0
107 waitfor j109-none 0.3
108 sound
110 waitfor j109-none 0.5
111 sound
120 quit discard
")
file(WRITE "${OUT}/stop.txt" "${script}")
execute_process(COMMAND "${APP}" "${OUT}/demo copy.gridline.json" --hidden --no-ui-sounds --size 1440x900 --chan 5
                        --dev-input "${OUT}/stop.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${out}")
endif()
if(out MATCHES "sdlat frame [0-9]+: [a-z]+ not shown")
  message(FATAL_ERROR "FAIL: a transport button is not in the top bar. Output:\n${out}")
endif()

# "sound frame F: peak P live L previewing V panics N playing X"
function(sound frame var)
  if(NOT out MATCHES "sound frame ${frame}: peak ([0-9.]+) live ([0-9]+) previewing ([01]) panics ([0-9]+) playing ([01])")
    message(FATAL_ERROR "FAIL: no sound report at frame ${frame}. Output:\n${out}")
  endif()
  set(${var}_peak ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${var}_live ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${var}_pv ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(${var}_panics ${CMAKE_MATCH_4} PARENT_SCOPE)
  set(${var}_playing ${CMAKE_MATCH_5} PARENT_SCOPE)
endfunction()
macro(gt a b what)
  if(NOT ${a} GREATER ${b})
    message(FATAL_ERROR "FAIL: ${what} (${a} <= ${b}). Output:\n${out}")
  endif()
  message(STATUS "ok - ${what}")
endmacro()
macro(lt a b what)
  if(NOT ${a} LESS ${b})
    message(FATAL_ERROR "FAIL: ${what} (${a} >= ${b}). Output:\n${out}")
  endif()
  message(STATUS "ok - ${what}")
endmacro()
# (peaks as integers in millionths: CMake compares integers)
macro(micro var)
  string(REGEX REPLACE "^([0-9]+)\\.([0-9][0-9][0-9][0-9][0-9][0-9]).*" "\\1\\2" ${var} "${${var}}")
  math(EXPR ${var} "${${var}} + 0")
endmacro()

# 1. A preview key held, then Stop: released, and silent once its tail has died out.
sound(21 a)
micro(a_peak)
gt(${a_peak} 10000 "preview key: the held note sounds while stopped")
gt(${a_live} 0 "preview key: held in the engine")
sound(27 b)
lt(${b_live} 1 "first Stop: the preview key is released")
if(NOT b_panics EQUAL a_panics)
  message(FATAL_ERROR "FAIL: the first Stop should release, not panic. Output:\n${out}")
endif()
message(STATUS "ok - first Stop: a release, not a panic")
sound(31 c)
micro(c_peak)
lt(${c_peak} 1000 "first Stop: silent after the release tail (it used to ring forever)")

# 2. A typed key held when the window loses focus: released (the key-up never comes).
sound(51 d)
micro(d_peak)
gt(${d_peak} 100000 "typed key: sounds")
gt(${d_live} 0 "typed key: held")
sound(55 e)
lt(${e_live} 1 "focus lost: the typed key is released")
sound(61 f)
micro(f_peak)
lt(${f_peak} 100000 "focus lost: the typed note dies out (a stuck note would hold its level)")

# 3. The browser's sample preview, then Stop.
sound(73 g)
gt(${g_pv} 0 "browser preview: playing")
sound(79 h)
lt(${h_pv} 1 "Stop: the browser preview stops")
sound(81 i)
micro(i_peak)
lt(${i_peak} 1 "Stop: silent after the browser preview")

# 4. Playing; Space stops (release), Stop again: the panic, silence at once.
sound(96 j)
gt(${j_playing} 0 "the song plays")
sound(101 k)
lt(${k_playing} 1 "Space stops the song")
if(NOT k_panics EQUAL j_panics)
  message(FATAL_ERROR "FAIL: Space while playing should release, not panic. Output:\n${out}")
endif()
sound(108 l)
math(EXPR want "${j_panics} + 1")
if(NOT l_panics EQUAL want)
  message(FATAL_ERROR "FAIL: the second Stop should run the panic (${l_panics} panics, want ${want}). Output:\n${out}")
endif()
message(STATUS "ok - Stop pressed again while stopped: the panic")
sound(111 m)
micro(m_peak)
lt(${m_peak} 1 "after the panic: silence, the mixer's delay / reverb tails included")
