# Gridline: K-20, Shift+click in the piano roll draws a note and the drag sets its length (FL-familiar).
# Runs the app hidden (isolated config and HOME, fixed frame time) on a copy of the test song, editing
# pattern 0 ("Beat") on channel 6 ("Chords", no notes in that pattern), and scripts the mouse:
#   1. Shift+click and drag right: one new note, starting at the click, its length set by the drag;
#   2. Shift+click without a drag: one new note of the default length (as a plain click);
#   3. a plain click and drag: the new note moves with the pointer and keeps its length;
#   each gesture is one undo step.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k20_shift_draw.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)  # a copy: never the fixture itself

function(run tag script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --edit 0 6 --hide rack
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# The piano roll's geometry: x of tick t = x0 + t * pxPerTick; pitch p's row starts at pitchY0 + (127 - p) * rowH.
run(geo "6 lane\n8 notes\n10 quit discard\n")
if(NOT geo_out MATCHES "lane frame [0-9]+: x0 ([-0-9.]+) pxPerTick ([0-9.]+) yTop ([-0-9.]+) [^\n]* pitchY0 ([-0-9.]+) rowH ([0-9.]+)")
  message(FATAL_ERROR "FAIL: no piano roll geometry\n${geo_out}")
endif()
set(x0 ${CMAKE_MATCH_1})
set(ppt ${CMAKE_MATCH_2})
set(laneTop ${CMAKE_MATCH_3})
set(py0 ${CMAKE_MATCH_4})
set(rowH ${CMAKE_MATCH_5})
if(NOT geo_out MATCHES "notes frame [0-9]+: 0 notes")
  message(FATAL_ERROR "FAIL: the edited channel should start with no notes in the pattern\n${geo_out}")
endif()
# Whole pixels (CMake's math has no fractions): pxPerTick is printed with 6 decimals.
foreach(v x0 laneTop py0 rowH)
  string(REGEX REPLACE "\\..*" "" ${v} "${${v}}")
endforeach()
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)$" _p "${ppt}")
set(pptInt ${CMAKE_MATCH_1})
string(REGEX REPLACE "^0+" "" pptFrac "${CMAKE_MATCH_2}")
if(pptFrac STREQUAL "")
  set(pptFrac 0)
endif()
# A pitch row well inside the visible note grid (between the toolbars and the event lane).
math(EXPR ymid "(${laneTop} + 200) / 2")
math(EXPR pitch "127 - (${ymid} - ${py0}) / ${rowH}")
math(EXPR y "${py0} + (127 - ${pitch}) * ${rowH} + ${rowH} / 2")
# x of a tick, a few pixels after it so snapping goes down to it.
function(xat tick out)
  math(EXPR v "${x0} + ${tick} * ${pptInt} + ${tick} * ${pptFrac} / 1000000 + 3")
  set(${out} ${v} PARENT_SCOPE)
endfunction()
xat(96 xs)
xat(384 xe)
xat(192 xm)

# 1. Shift+click and drag right.
run(drag "6 move ${xs} ${y}\n7 mod shift 1\n8 down 0\n10 move ${xm} ${y}\n12 move ${xe} ${y}\n14 up 0\n15 mod shift 0\n17 notes\n18 dump\n19 quit discard\n")
if(NOT drag_out MATCHES "notes frame [0-9]+: 1 notes[^\n]*\n  note 0 start ([0-9]+) len ([0-9]+) pitch ([0-9]+)")
  message(FATAL_ERROR "FAIL: Shift+click and drag should make one note\n${drag_out}")
endif()
set(s1 ${CMAKE_MATCH_1})
set(l1 ${CMAKE_MATCH_2})
if(NOT s1 EQUAL 96)
  message(FATAL_ERROR "FAIL: the note starts at the click (want 96, got ${s1})\n${drag_out}")
endif()
math(EXPR end1 "${s1} + ${l1}")
if(end1 LESS 360 OR end1 GREATER 408)
  message(FATAL_ERROR "FAIL: the drag sets the note's end near tick 384 (got ${end1}, length ${l1})\n${drag_out}")
endif()
if(NOT drag_out MATCHES "dump frame [0-9]+: [0-9]+ clips, [0-9]+ selected, undo 1")
  message(FATAL_ERROR "FAIL: drawing and sizing the note should be one undo step\n${drag_out}")
endif()
message(STATUS "ok - Shift+click and drag draws a note from 96 to ${end1}, one undo step")

# 2. Shift+click without a drag; 3. a plain click and drag (the default length, moved).
run(click "6 move ${xs} ${y}\n7 mod shift 1\n8 down 0\n10 up 0\n11 mod shift 0\n13 notes\n15 quit discard\n")
if(NOT click_out MATCHES "notes frame [0-9]+: 1 notes[^\n]*\n  note 0 start 96 len ([0-9]+) ")
  message(FATAL_ERROR "FAIL: Shift+click without a drag should make one note at 96\n${click_out}")
endif()
set(defLen ${CMAKE_MATCH_1})
run(plain "6 move ${xs} ${y}\n8 down 0\n10 move ${xm} ${y}\n12 up 0\n14 notes\n16 quit discard\n")
if(NOT plain_out MATCHES "notes frame [0-9]+: 1 notes[^\n]*\n  note 0 start ([0-9]+) len ([0-9]+) ")
  message(FATAL_ERROR "FAIL: a plain click and drag should make one note\n${plain_out}")
endif()
if(NOT CMAKE_MATCH_2 EQUAL defLen OR NOT CMAKE_MATCH_1 GREATER 96)
  message(FATAL_ERROR "FAIL: a plain drag moves the note and keeps the default length ${defLen} (start ${CMAKE_MATCH_1}, length ${CMAKE_MATCH_2})\n${plain_out}")
endif()
message(STATUS "ok - Shift+click alone keeps the default length (${defLen}); a plain drag still moves the note")
