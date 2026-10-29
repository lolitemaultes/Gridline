# Gridline — J-165: Tideline's trigger Source list in the app (hidden, off-screen,
# isolated config). Opens Tideline on the demo song, clicks the Source dropdown with
# real SDL mouse events and picks an entry:
#  - MIDI mode: the first channel of the list -> Note source Channel with that channel's id;
#    undo -> back to MIDI Out ports.
#  - Audio mode: the first mixer track of the list -> a sidechain route from it (the
#    dropdown then reads "sidechain" with that track's id); undo -> own input again.
# The dropdown reports itself through the dev points "tideline.source" (point 0: what is
# picked, k = id) and "tideline.sources" (one point per entry, k = its id).
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j165_tideline_source.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

# entry: index in the open list to pick (MIDI: 3 = first channel; Audio: 1 = first mixer track)
function(run_case name trig entry)
  set(script "5 toasts clear
10 pts tideline.source
12 sdlpt tideline.source 0.5 0.5
14 sdldown 0
16 sdlup 0
24 pts tideline.sources
26 sdlpt tideline.sources pt ${entry}
28 sdldown 0
30 sdlup 0
40 pts tideline.source
42 undo
50 pts tideline.source
")
  file(WRITE "${OUT}/${name}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --plugin gl.tideline --param 3 ${trig} --size 1600x1000 --frames 56
                          --screenshot "${OUT}/${name}.ppm" --dev-input "${OUT}/${name}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: gridline-app failed (${rc}): ${err}")
  endif()
  if(out MATCHES "not shown")
    message(FATAL_ERROR "FAIL (${name}): the Source dropdown or its list is not on screen. Output:\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

# The value of point 0 (x = kind, k = id) of "tideline.source" printed at frame `fr`.
function(source_at fr kindVar idVar)
  string(REGEX MATCH "pts frame ${fr}: tideline\\.source [^\n]*\n  pt 0 x ([-0-9.]+) v [-0-9.]+ k ([-0-9.]+)" m "${out}")
  if(NOT m)
    message(FATAL_ERROR "FAIL: no source report at frame ${fr}. Output:\n${out}")
  endif()
  set(${kindVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${idVar} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# The id of entry `i` of the open list (frame 24).
function(entry_id i idVar)
  string(REGEX MATCH "pts frame 24: tideline\\.sources [^\n]*\n(  pt [^\n]*\n)*  pt ${i} x [-0-9.]+ v [-0-9.]+ k ([-0-9.]+)" m "${out}")
  if(NOT m)
    message(FATAL_ERROR "FAIL: list entry ${i} not reported. Output:\n${out}")
  endif()
  set(${idVar} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# ---- MIDI: pick a channel
run_case(midi 2 3)
entry_id(3 want)
source_at(10 k0 i0)
source_at(40 k1 i1)
source_at(50 k2 i2)
if(NOT k0 STREQUAL "10.0000" OR NOT k1 STREQUAL "11.0000" OR NOT i1 STREQUAL "${want}" OR want STREQUAL "-1.0000" OR NOT k2 STREQUAL "10.0000")
  message(FATAL_ERROR "FAIL (midi): before ${k0}, after pick ${k1} id ${i1} (wanted channel ${want}), after undo ${k2}. Output:\n${out}")
endif()
message(STATUS "ok - MIDI: Source list picks channel ${want}; undo restores MIDI Out ports")

# ---- Audio: pick a mixer track
run_case(audio 1 1)
entry_id(1 want)
source_at(10 k0 i0)
source_at(40 k1 i1)
source_at(50 k2 i2)
if(NOT k0 STREQUAL "0.0000" OR NOT k1 STREQUAL "1.0000" OR NOT i1 STREQUAL "${want}" OR NOT k2 STREQUAL "0.0000")
  message(FATAL_ERROR "FAIL (audio): before ${k0}, after pick ${k1} id ${i1} (wanted track ${want}), after undo ${k2}. Output:\n${out}")
endif()
message(STATUS "ok - Audio: Source list routes mixer track ${want} as the sidechain; undo removes it")
