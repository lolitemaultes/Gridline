# Gridline — J-183: Kerf's editor layout in the real app (hidden, isolated config).
# The editor's own layout audit (GRIDLINE_KERF_TRACE) lists every control box and text run of a
# frame: no two controls may overlap, controls of one row share their top and height, no text is
# cut off or smaller than the minimum size. Checked on every frame it reports:
#   1. 1440 x 960: empty, a loop dropped in, each view tab, 32 even slices, the compact display.
#   2. 1280 x 800: the same, and the whole form fits in the window at no less than 0.86 scale.
#   3. Interface scale 1.5 (a 2160 x 1440 window: the same room as 1440 x 960).
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P j183_kerf_layout.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_KERF_TRACE} "1")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(run tag size frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --plugin gl.slicer --size ${size} --frames ${frames} --screenshot "${OUT}/${tag}.ppm"
                          --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 280)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.log" "${out}")
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# Every audit line of a run is clean; `want` audit lines at least (one per layout state).
function(audit tag text want)
  string(REGEX MATCHALL "kerf-audit:[^\n]*" lines "${text}")
  list(LENGTH lines n)
  if(n LESS want)
    message(FATAL_ERROR "FAIL ${tag}: ${n} audit lines, expected at least ${want}. Output:\n${text}")
  endif()
  foreach(l IN LISTS lines)
    if(NOT l MATCHES " overlaps=0 rows=0 clipped=0 small=0 ")
      message(FATAL_ERROR "FAIL ${tag}: layout audit: ${l}")
    endif()
    if(NOT l MATCHES " boxes=([0-9]+) " OR CMAKE_MATCH_1 LESS 20)
      message(FATAL_ERROR "FAIL ${tag}: the audit saw too few controls: ${l}")
    endif()
  endforeach()
endfunction()

function(lastline text out)
  string(REGEX MATCHALL "kerf-ui:[^\n]*" lines "${text}")
  list(LENGTH lines n)
  if(n EQUAL 0)
    message(FATAL_ERROR "FAIL: the editor printed no trace line. Output:\n${text}")
  endif()
  math(EXPR i "${n} - 1")
  list(GET lines ${i} l)
  set(${out} "${l}" PARENT_SCOPE)
endfunction()

function(centre l name ox oy)
  if(NOT l MATCHES " ${name}=([0-9]+),([0-9]+),([0-9]+),([0-9]+)")
    message(FATAL_ERROR "FAIL: no ${name} in the trace: ${l}")
  endif()
  math(EXPR x "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
  math(EXPR y "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
  set(${ox} ${x} PARENT_SCOPE)
  set(${oy} ${y} PARENT_SCOPE)
endfunction()

# One size: the empty editor's places, then load, every tab, 32 slices, compact.
function(check tag size)
  run(${tag}_empty ${size} 40 "# nothing\n" ${ARGN})
  audit(${tag}_empty "${${tag}_empty_out}" 1)
  lastline("${${tag}_empty_out}" l)
  centre("${l}" dropzone zx zy)
  centre("${l}" tab_amp ax ay)
  centre("${l}" tab_region rx ry)
  centre("${l}" tab_detect tx ty)
  if(NOT l MATCHES " slmode=([0-9]+),([0-9]+),([0-9]+),([0-9]+)")
    message(FATAL_ERROR "FAIL ${tag}: no slicing mode selector: ${l}")
  endif()
  # The fifth of six cells is 32 even slices.
  math(EXPR sx "${CMAKE_MATCH_1} + (${CMAKE_MATCH_3} - ${CMAKE_MATCH_1}) * 9 / 12")
  math(EXPR sy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
  string(REGEX MATCH " display=([0-9]+),([0-9]+),([0-9]+),([0-9]+)" _ "${l}")
  math(EXPR dx "${CMAKE_MATCH_1} + (${CMAKE_MATCH_3} - ${CMAKE_MATCH_1}) * 92 / 108")
  math(EXPR dy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
  run(${tag}_full ${size} 150 "10 dragenter ${zx} ${zy} \"${OUT}/Break 96.wav\"
42 dragmove ${zx} ${zy}
44 dragmove ${zx} ${zy}
46 drop
60 move ${rx} ${ry}
62 down 0
64 up 0
74 move ${tx} ${ty}
76 down 0
78 up 0
88 move ${ax} ${ay}
90 down 0
92 up 0
102 move ${sx} ${sy}
104 down 0
106 up 0
116 move 10 10
126 move ${dx} ${dy}
128 down 0
130 up 0
140 move 10 10
" ${ARGN})
  audit(${tag}_full "${${tag}_full_out}" 4)
  string(REGEX MATCHALL "kerf-ui:[^\n]*" lines "${${tag}_full_out}")
  set(saw "")
  foreach(x IN LISTS lines)
    if(x MATCHES " tab=1 ")
      string(APPEND saw "r")
    endif()
    if(x MATCHES " tab=2 ")
      string(APPEND saw "d")
    endif()
    if(x MATCHES " regions=32 ")
      string(APPEND saw "s")
    endif()
    if(x MATCHES " display=3 ")
      string(APPEND saw "c")
    endif()
  endforeach()
  foreach(c r d s c)
    if(NOT saw MATCHES "${c}")
      message(FATAL_ERROR "FAIL ${tag}: a click did not respond (${c} missing in '${saw}'). Output:\n${${tag}_full_out}")
    endif()
  endforeach()
  set(${tag}_l "${l}" PARENT_SCOPE)
  set(${tag}_full_out "${${tag}_full_out}" PARENT_SCOPE)
endfunction()

check(big 1440x960)
check(small 1280x800)
if(NOT small_l MATCHES " form=([0-9]+),([0-9]+),([0-9]+),([0-9]+)")
  message(FATAL_ERROR "FAIL: no form rectangle: ${small_l}")
endif()
if(CMAKE_MATCH_4 GREATER 780 OR CMAKE_MATCH_3 GREATER 1280)
  message(FATAL_ERROR "FAIL: at 1280 x 800 the editor is cut off (form ${CMAKE_MATCH_1},${CMAKE_MATCH_2} to ${CMAKE_MATCH_3},${CMAKE_MATCH_4})")
endif()
string(REGEX MATCHALL "kerf-audit:[^\n]* scale=([0-9.]+)" sl "${small_full_out}")
foreach(x IN LISTS sl)
  string(REGEX MATCH " scale=([0-9.]+)" _ "${x}")
  if(CMAKE_MATCH_1 LESS 0.86)
    message(FATAL_ERROR "FAIL: at 1280 x 800 the form is scaled below 0.86: ${x}")
  endif()
endforeach()
check(ui150 2160x1440 --ui-scale 1.5 --fixed-dt 16)
message(STATUS "j183 Kerf layout: no overlaps, rows aligned, no clipped or small text at 1440x960, 1280x800 and UI scale 1.5 (2160x1440); fits at 1280x800")
