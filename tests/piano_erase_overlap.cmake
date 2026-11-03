# Gridline: a right-click on overlapping notes of one pitch deletes only the top note, driven by scripted
# input in the real app (hidden, isolated config and HOME, fixed frame time) on a copy of the test song,
# editing pattern 0 on channel 6. Three notes of pitch 60 start at ticks 0, 96 and 192, each 384 long, so
# all three lie under tick 240:
#   1. a right press held for many frames takes the top note and leaves the two under it;
#   2. the same with the pointer slipping a few pixels while held;
#   3. the same with the delete tool and the left button;
#   4. a second right-click takes the next note, and undo brings a note back;
#   5. dragging off the deleted note onto a part of the note beneath that nothing covered deletes it too.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P piano_erase_overlap.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)

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

set(make "2 mknotes 3 96 384\n3 typing off\n")
run(geo "${make}14 lane\n15 notes\n17 quit discard\n")
if(NOT geo_out MATCHES "lane frame [0-9]+: x0 ([-0-9]+)[.0-9]* pxPerTick ([0-9.]+) yTop [-0-9.]+ [^\n]* pitchY0 ([-0-9]+)[.0-9]* rowH ([0-9]+)")
  message(FATAL_ERROR "FAIL: no piano roll geometry\n${geo_out}")
endif()
set(x0 ${CMAKE_MATCH_1})
set(ppt ${CMAKE_MATCH_2})
set(py0 ${CMAKE_MATCH_3})
set(rowH ${CMAKE_MATCH_4})
string(REGEX REPLACE "^([0-9]+)\\.?([0-9]?[0-9]?[0-9]?).*" "\\1;\\2000" parts "${ppt}")
list(GET parts 0 pi)
list(GET parts 1 pf)
string(SUBSTRING "${pf}" 0 3 pf)
string(REGEX REPLACE "^0+(.)" "\\1" pf "${pf}")
math(EXPR pptm "${pi} * 1000 + ${pf}")
function(xat tick out)
  math(EXPR v "${x0} + ${tick} * ${pptm} / 1000")
  set(${out} ${v} PARENT_SCOPE)
endfunction()
math(EXPR y "${py0} + (127 - 60) * ${rowH} + ${rowH} / 2")
xat(240 x)
math(EXPR xs "${x} + 4")

# The notes printed at frame `fr`, as a list of start ticks.
function(starts tag fr out)
  if(NOT ${tag}_out MATCHES "notes frame ${fr}:[^\n]*\n((  note [^\n]*\n)*)")
    message(FATAL_ERROR "FAIL (${tag}): no notes at frame ${fr}\n${${tag}_out}")
  endif()
  string(REGEX MATCHALL "note [0-9]+ [^\n]*" rows "${CMAKE_MATCH_1}")
  set(r "")
  foreach(n IN LISTS rows)
    if(NOT n MATCHES "start ([0-9]+)")
      message(FATAL_ERROR "FAIL (${tag}): no start in: ${n}")
    endif()
    list(APPEND r ${CMAKE_MATCH_1})
  endforeach()
  set(${out} "${r}" PARENT_SCOPE)
endfunction()
function(want tag got expect what)
  if(NOT "${got}" STREQUAL "${expect}")
    message(FATAL_ERROR "FAIL (${tag}): ${what}\n  want: ${expect}\n  got:  ${got}\n${${tag}_out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

set(focus "12 move ${x} ${y}\n14 move ${x} ${y}\n")
run(hold "${make}${focus}16 down 1\n40 up 1\n44 notes\n46 quit discard\n")
starts(hold 44 s)
want(hold "${s}" "0;96" "a held right press deletes only the top note")

run(slip "${make}${focus}16 down 1\n20 move ${xs} ${y}\n24 move ${x} ${y}\n28 move ${xs} ${y}\n40 up 1\n44 notes\n46 quit discard\n")
starts(slip 44 s)
want(slip "${s}" "0;96" "the pointer slipping while held deletes only the top note")

run(again "${make}${focus}16 down 1\n24 up 1\n30 down 1\n38 up 1\n42 notes\n44 undo\n48 notes\n50 quit discard\n")
starts(again 42 s)
want(again "${s}" "0" "a second right-click deletes the next note")
starts(again 48 s)
want(again "${s}" "0;96" "undo brings that note back")

# Two notes: 0..384 and 192..576 (step 192). From tick 240 to tick 100, which only the first note covers.
xat(100 xl)
run(sweep "2 mknotes 2 192 384\n3 typing off\n${focus}16 down 1\n24 move ${xl} ${y}\n32 up 1\n36 notes\n38 quit discard\n")
if(NOT sweep_out MATCHES "notes frame 36:[^\n]*\n  note")
  message(STATUS "ok - a drag off the deleted note deletes the note beneath where nothing covered it")
else()
  message(FATAL_ERROR "FAIL (sweep): the note beneath stayed under a deliberate drag\n${sweep_out}")
endif()

run(tool "${make}${focus}15 key d\n16 down 0\n40 up 0\n44 notes\n46 quit discard\n")
starts(tool 44 s)
want(tool "${s}" "0;96" "the delete tool held on the overlap deletes only the top note")

message(STATUS "piano_erase_overlap: all checks passed")
