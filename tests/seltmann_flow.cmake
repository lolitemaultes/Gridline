# Gridline — J-107: Seltmann's one-step flow, driven like a user through the J-75 drag hooks.
# Runs the app hidden (off-screen, isolated config) with a Seltmann channel open:
#   1. the empty editor shows the drop zone;
#   2. a synthetic 2-bar break dropped on it (the same ImGui drag a file from Finder, Tracklib or
#      the browser makes) loads into Seltmann and is analysed on its own: a break at its tempo,
#      chopped automatically;
#   3. a click on the first layout card applies the layout and the pattern starts playing;
#   4. J-149: the sequence starts in order (A B C ...); Alt+Right moves the selected step, Delete over
#      the lane deletes the region under the pointer (it joins its neighbour).
# The trace lines come from the editor's GRIDLINE_SELTMANN_TRACE hook. Screenshots: <OUT>/empty.ppm,
# <OUT>/ready.ppm, <OUT>/playing.ppm.
# J-108: no frame counts to guess. The script waits ("waitfor", the frame count held) until the
# editor reports the analysis ready and the engine's first pattern step, with generous upper
# bounds; the engine runs on SDL's dummy audio driver, so a machine without a sound device (CI
# containers: no audio server, no /dev/snd, so no audio callback and no step ever played) plays
# the pattern too, in real time, and nothing sounds on a developer's speakers.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P seltmann_flow.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x (CMP0054: a quoted "keep" is that string, never a variable)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_SELTMANN_TRACE} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --plugin gl.seltmann --size 1440x960 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${last_out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# ---- 1. the empty editor: the drop zone
run(empty 20 "# nothing\n")
set(last_out "${empty_out}")
string(REGEX MATCH "seltmann: zone ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+)" _ "${empty_out}")
set(zone "${CMAKE_MATCH_1}")
check("the empty editor shows the drop zone" zone MATCHES "[0-9]")
math(EXPR zx "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
math(EXPR zy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
check("... and nothing loaded yet" empty_out MATCHES "seltmann: state empty")

# ---- 2. drop the break on the zone: it is analysed and chopped on its own
set(drop "10 dragenter ${zx} ${zy} \"${OUT}/Break 96.wav\"
12 dragmove ${zx} ${zy}
14 dragmove ${zx} ${zy}
16 drop
17 waitfor seltmann-ready 120
")
run(ready 30 "${drop}")
set(last_out "${ready_out}")
check("the drop loads the file and the editor analyses it" ready_out MATCHES "seltmann: state analysing|seltmann: state ready")
check("... the analysis finishes in time" ready_out MATCHES "waitfor seltmann-ready: reached")
string(REGEX MATCH "seltmann: state ready kind ([a-z]+) bpm ([0-9.]+) slices ([0-9]+)" _ "${ready_out}")
set(kind "${CMAKE_MATCH_1}")
set(bpm "${CMAKE_MATCH_2}")
set(slices "${CMAKE_MATCH_3}")
check("... analysed: a break (${kind})" kind STREQUAL "break")
check("... at its tempo (${bpm} BPM)" bpm MATCHES "^9[56]\\.[0-9]+" AND bpm GREATER 95.5 AND bpm LESS 96.5)
check("... chopped automatically: 2 bars in eighths (${slices} slices)" slices EQUAL 16)
string(REGEX MATCH "seltmann: card \"([^\"]+)\" ([0-9]+) ([0-9]+)" _ "${ready_out}")
set(card "${CMAKE_MATCH_1}")
set(cx "${CMAKE_MATCH_2}")
set(cy "${CMAKE_MATCH_3}")
check("... the layout cards are shown (first: ${card})" cx MATCHES "[0-9]")

# ---- 3. pick a layout: it applies and plays
run(playing 40 "${drop}20 move ${cx} ${cy}
24 down 0
27 up 0
28 waitfor seltmann-playing 60
")
set(last_out "${playing_out}")
check("clicking \"${card}\" applies the layout" playing_out MATCHES "preset \"${card}\"")
check("... and the pattern plays" playing_out MATCHES "preset \"${card}\" pattern 1")
check("... the engine reports the pattern's steps" playing_out MATCHES "preset \"${card}\" pattern 1 playing 1")
# ---- 4. J-149: the sequence: in order after the drop; reorder with Alt+Right; Delete joins a region
check("the sequence starts in order" ready_out MATCHES "slices 16 [^\n]*order \"A B C D E F G H I J K L M N O P\"")
string(REGEX MATCH "seltmann: seq 0 ([0-9]+) ([0-9]+)" _ "${ready_out}")
set(s0x "${CMAKE_MATCH_1}")
set(s0y "${CMAKE_MATCH_2}")
string(REGEX MATCH "seltmann: seq 3 ([0-9]+) ([0-9]+)" _ "${ready_out}")
set(s3x "${CMAKE_MATCH_1}")
check("... the sequence blocks are shown" s0x MATCHES "[0-9]" AND s3x MATCHES "[0-9]")
run(sequence 44 "${drop}20 move ${s0x} ${s0y}
22 down 0
23 up 0
25 mod alt 1
26 key right
27 mod alt 0
30 move ${s3x} ${s0y}
32 key delete
36 move 10 10
")
set(last_out "${sequence_out}")
check("Alt+Right moves A after B" sequence_out MATCHES "slices 16 [^\n]*order \"B A C D E F G H I J K L M N O P\"")
check("... Delete joins D into C (15 regions, letters re-packed)" sequence_out MATCHES "slices 15 [^\n]*order \"B A C D E F G H I J K L M N O\"")
foreach(f empty ready playing sequence)
  check("screenshot ${f}.ppm written" EXISTS "${OUT}/${f}.ppm")
endforeach()
