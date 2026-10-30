# Gridline — J-190: Kerf slices on the beat grid in the real app (hidden, isolated config).
#   1. A synthetic 2-bar break (96 BPM) is dropped on the wave view; the SLICES card's grid
#      field opens the beat grid menu and 1/8 is chosen: the mode turns to the beat grid and,
#      once the analysis is in (waitfor, so slow machines pass), the loop has 16 regions.
#   2. The field again, 1/16: 32 regions, every start within 15 ms of its 16th of the bar
#      (the hits are played 10 ms late; cuts move onto them by default).
#   3. The bars:beats ruler and the layout audit (no overlaps, rows aligned, no clipped or tiny text).
#   4. The same at 1280 x 800. Frames (the menu, the SLICES card) are kept in OUT for review.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P j190_kerf_grid.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
file(MAKE_DIRECTORY "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")  # the browser's default places stay inside OUT
set(ENV{GRIDLINE_KERF_TRACE} "1")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(run tag size frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.slicer --size ${size} --frames ${frames} --screenshot "${OUT}/${tag}.ppm"
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 280)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.log" "${out}")
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# The centre of a traced rectangle "name=x0,y0,x1,y1" (the last one printed): <out>_x / <out>_y.
function(centre text name out)
  string(REGEX MATCHALL " ${name}=[0-9]+,[0-9]+,[0-9]+,[0-9]+" all "${text}")
  list(LENGTH all n)
  if(n EQUAL 0)
    message(FATAL_ERROR "FAIL: the editor never traced '${name}'. Output:\n${text}")
  endif()
  math(EXPR i "${n} - 1")
  list(GET all ${i} r)
  string(REGEX MATCH "=([0-9]+),([0-9]+),([0-9]+),([0-9]+)" _ "${r}")
  math(EXPR cx "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
  math(EXPR cy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
  set(${out}_x ${cx} PARENT_SCOPE)
  set(${out}_y ${cy} PARENT_SCOPE)
endfunction()

# The last trace line whose fields match `cond` (a regex): <out> ("" when none).
function(lastline text cond out)
  string(REGEX MATCHALL "kerf-ui:[^\n]*" lines "${text}")
  set(v "")
  foreach(l IN LISTS lines)
    if(l MATCHES "${cond}")
      set(v "${l}")
    endif()
  endforeach()
  set(${out} "${v}" PARENT_SCOPE)
endfunction()

function(audit text tag)
  string(REGEX MATCHALL "kerf-audit:[^\n]*" lines "${text}")
  list(LENGTH lines n)
  if(n EQUAL 0)
    message(FATAL_ERROR "FAIL (${tag}): no layout audit printed")
  endif()
  foreach(l IN LISTS lines)
    if(NOT l MATCHES "overlaps=0 rows=0 clipped=0 small=0")
      message(FATAL_ERROR "FAIL (${tag}): layout audit: ${l}")
    endif()
  endforeach()
endfunction()

function(flow tag size)
  run(${tag}_probe0 ${size} 20 "# nothing\n")
  centre("${${tag}_probe0_out}" dropzone dz)
  centre("${${tag}_probe0_out}" gridfield gf)
  set(load "10 dragenter ${dz_x} ${dz_y} \"${OUT}/Break 96.wav\"
12 dragmove ${dz_x} ${dz_y}
14 dragmove ${dz_x} ${dz_y}
16 drop
")
  # Probe: the grid menu's items (opened on the loaded loop).
  run(${tag}_probe1 ${size} 50 "${load}30 move ${gf_x} ${gf_y}
32 down 0
34 up 0
40 move ${gf_x} ${gf_y}
")
  centre("${${tag}_probe1_out}" griddiv3 d8)
  centre("${${tag}_probe1_out}" griddiv4 d16)
  # The flow: 1/8 from the menu, wait for the analysis, then 1/16; the menu shot, then the card.
  run(${tag} ${size} 140 "${load}30 move ${gf_x} ${gf_y}
32 down 0
34 up 0
40 move ${d8_x} ${d8_y}
42 down 0
44 up 0
46 waitfor kerf-grid-ready 120
60 move ${gf_x} ${gf_y}
62 down 0
64 up 0
70 move ${d16_x} ${d16_y}
72 shot \"${OUT}/${tag}_menu.ppm\"
74 down 0
76 up 0
90 move 5 5
100 shot \"${OUT}/${tag}_card.ppm\"
")
  set(o "${${tag}_out}")
  if(NOT o MATCHES "waitfor kerf-grid-ready: reached")
    message(FATAL_ERROR "FAIL (${tag}): the beat analysis never came in. Output:\n${o}")
  endif()
  lastline("${o}" " slmode=6 gridsrc=0 analysing=0 [^\n]* gridcell=3 " l8)
  string(REGEX MATCH " regions=([0-9]+)" _ "${l8}")
  if(NOT CMAKE_MATCH_1 STREQUAL "16")
    message(FATAL_ERROR "FAIL (${tag}): 1/8 on the detected grid gave '${CMAKE_MATCH_1}' regions, not 16. Output:\n${o}")
  endif()
  lastline("${o}" " slmode=6 gridsrc=0 analysing=0 [^\n]* gridcell=4 " l16)
  string(REGEX MATCH " regions=([0-9]+)" _ "${l16}")
  if(NOT CMAKE_MATCH_1 STREQUAL "32")
    message(FATAL_ERROR "FAIL (${tag}): 1/16 on the detected grid gave '${CMAKE_MATCH_1}' regions, not 32. Output:\n${o}")
  endif()
  string(REGEX MATCH " gridbpm=([0-9]+)\\." _ "${l16}")
  if(NOT CMAKE_MATCH_1 STREQUAL "96" AND NOT CMAKE_MATCH_1 STREQUAL "95")
    message(FATAL_ERROR "FAIL (${tag}): the detected tempo is ${CMAKE_MATCH_1} BPM, not 96")
  endif()
  # Every start within 15 ms (662 frames) of its 16th: k * 6890.625 frames (96 BPM, 44.1 kHz).
  string(REGEX MATCH " starts=([0-9,]+)" _ "${l16}")
  string(REPLACE "," ";" starts "${CMAKE_MATCH_1}")
  set(k 0)
  foreach(s IN LISTS starts)
    math(EXPR want "(${k} * 6890625 + 500) / 1000")
    math(EXPR d "${s} - ${want}")
    if(d LESS 0)
      math(EXPR d "0 - ${d}")
    endif()
    if(d GREATER 662)
      message(FATAL_ERROR "FAIL (${tag}): region ${k} starts at ${s}, ${d} frames off its 16th (${want})")
    endif()
    math(EXPR k "${k} + 1")
  endforeach()
  audit("${o}" ${tag})
endfunction()

flow(full 1440x960)
flow(small 1280x800)
message(STATUS "j190 Kerf beat grid: grid menu 1/8 -> 16 regions, 1/16 -> 32 regions on the detected 96 BPM grid, starts on the 16ths, layout audit clean (1440x960, 1280x800)")
