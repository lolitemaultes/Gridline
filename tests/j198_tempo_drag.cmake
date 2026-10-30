# Gridline: J-198, the tempo display's two drag zones (hidden, isolated config and HOME, a fixed 16 ms
# frame time). The song is set to 96.250 BPM; then
#   1. dragging the whole number up 20 px steps whole BPM (4 px each) and keeps the decimals:
#      98.250 half way, 101.250 at the end, one undo step;
#   2. dragging the decimals up 8 px moves smoothly as before (0.25 BPM per pixel): 103.250;
#   3. one scroll notch over the decimals: +0.1 (103.350); over the whole number: +1 (104.350).
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j198_tempo_drag.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)  # a copy: never the fixture itself

function(run tag script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 240)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${o}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()
function(at name)
  if(NOT out MATCHES "sdlat frame [0-9]+: ${name} at ([0-9]+),([0-9]+)")
    message(FATAL_ERROR "FAIL: no ${name} target\n${out}")
  endif()
  set(${name}_x ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${name}_y ${CMAKE_MATCH_2} PARENT_SCOPE)
endfunction()
function(expect frame bpm what)
  if(NOT out MATCHES "tempo frame ${frame}: ([0-9.]+) undo ([0-9]+)")
    message(FATAL_ERROR "FAIL: no tempo at frame ${frame}\n${out}")
  endif()
  if(NOT CMAKE_MATCH_1 STREQUAL "${bpm}")
    message(FATAL_ERROR "FAIL: ${what}: ${CMAKE_MATCH_1}, want ${bpm}\n${out}")
  endif()
  set(undo_${frame} ${CMAKE_MATCH_2} PARENT_SCOPE)
  message(STATUS "ok: ${what} (${bpm})")
endfunction()

run(geo "4 tempo 96.25\n8 sdlat tempoint\n9 sdlat tempofrac\n12 quit discard\n")
at(tempoint)
at(tempofrac)
math(EXPR iy1 "${tempoint_y} - 10")
math(EXPR iy2 "${tempoint_y} - 20")
math(EXPR fy1 "${tempofrac_y} - 8")
run(drag "4 tempo 96.25
8 tempo
10 sdlmove ${tempoint_x} ${tempoint_y}
12 sdldown 0
14 sdlmove ${tempoint_x} ${iy1}
16 tempo
18 sdlmove ${tempoint_x} ${iy2}
20 sdlup 0
22 tempo
24 sdlmove ${tempofrac_x} ${tempofrac_y}
26 sdldown 0
28 sdlmove ${tempofrac_x} ${fy1}
30 sdlup 0
32 tempo
34 sdlmove ${tempofrac_x} ${tempofrac_y}
36 wheel 1
38 tempo
40 sdlmove ${tempoint_x} ${tempoint_y}
42 wheel 1
44 tempo
46 quit discard
")
expect(8 96.250 "start")
expect(16 98.250 "whole-number drag half way: whole steps, decimals kept")
expect(22 101.250 "whole-number drag: +5 BPM for 20 px, decimals kept")
math(EXPR steps "${undo_22} - ${undo_8}")
if(NOT steps EQUAL 1)
  message(FATAL_ERROR "FAIL: the whole-number drag took ${steps} undo steps, want 1\n${out}")
endif()
expect(32 103.250 "decimal drag: smooth, 0.25 BPM per pixel")
expect(38 103.350 "scroll over the decimals: 0.1")
expect(44 104.350 "scroll over the whole number: 1, decimals kept")
