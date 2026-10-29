# Gridline — J-181: Kerf's editor in the real app (hidden, isolated config).
#   1. Empty: the wave view shows its drop zone and the region column its Load button.
#   2. A synthetic break dropped on the drop zone loads (regions appear); the DETECT tab and the
#      compact display mode respond to a click (the editor's trace line reports them).
#   3. At 1280 x 800 the whole editor fits in the window (it scales down instead of being cut off).
#   4. The editor layout audit of Kerf alone: no inset issues and no alignment findings.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P j181_kerf_ui.cmake
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
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 280)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.log" "${out}")
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# The last trace line of a run.
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

# ---- 1. empty
run(empty 1440x960 20 "# nothing\n")
lastline("${empty_out}" l)
if(NOT l MATCHES " dropzone=([0-9]+),([0-9]+),([0-9]+),([0-9]+)")
  message(FATAL_ERROR "FAIL: no drop zone in the empty wave view: ${l}")
endif()
math(EXPR zx "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
math(EXPR zy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
if(NOT l MATCHES " loadbig=" OR NOT l MATCHES " regions=0 ")
  message(FATAL_ERROR "FAIL: the empty editor shows no Load button: ${l}")
endif()
string(REGEX MATCH " tab_detect=([0-9]+),([0-9]+),([0-9]+),([0-9]+)" _ "${l}")
math(EXPR tx "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
math(EXPR ty "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
string(REGEX MATCH " display=([0-9]+),([0-9]+),([0-9]+),([0-9]+)" _ "${l}")
# The compact mode is the fourth segment: form x 606 + 3 * 26 + 12 of the selector at 604 (108 wide).
math(EXPR dx "${CMAKE_MATCH_1} + (${CMAKE_MATCH_3} - ${CMAKE_MATCH_1}) * 92 / 108")
math(EXPR dy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")

# ---- 2. drop a loop, then the DETECT tab, then the compact display
run(loaded 1440x960 90 "10 dragenter ${zx} ${zy} \"${OUT}/Break 96.wav\"
12 dragmove ${zx} ${zy}
14 dragmove ${zx} ${zy}
16 drop
40 move ${tx} ${ty}
42 down 0
44 up 0
50 move 10 10
60 move ${dx} ${dy}
62 down 0
64 up 0
70 move 10 10
")
string(REGEX MATCHALL "kerf-ui:[^\n]*" lines "${loaded_out}")
set(sawLoad 0)
set(sawTab 0)
set(sawCompact 0)
foreach(x IN LISTS lines)
  if(x MATCHES " regions=([0-9]+) " AND NOT CMAKE_MATCH_1 EQUAL 0)
    set(sawLoad 1)
  endif()
  if(x MATCHES " tab=2 " AND sawLoad)
    set(sawTab 1)
  endif()
  if(x MATCHES " display=3 " AND sawTab)
    set(sawCompact 1)
  endif()
endforeach()
if(NOT sawLoad)
  message(FATAL_ERROR "FAIL: the dropped loop did not load (no regions). Output:\n${loaded_out}")
endif()
if(NOT sawTab)
  message(FATAL_ERROR "FAIL: the DETECT tab did not respond. Output:\n${loaded_out}")
endif()
if(NOT sawCompact)
  message(FATAL_ERROR "FAIL: the compact display mode did not respond. Output:\n${loaded_out}")
endif()

# ---- 3. 1280 x 800: the form ends inside the window
run(small 1280x800 20 "# nothing\n")
lastline("${small_out}" l)
if(NOT l MATCHES " form=([0-9]+),([0-9]+),([0-9]+),([0-9]+)")
  message(FATAL_ERROR "FAIL: no form rectangle: ${l}")
endif()
if(CMAKE_MATCH_4 GREATER 780 OR CMAKE_MATCH_3 GREATER 1280)
  message(FATAL_ERROR "FAIL: at 1280 x 800 the editor is cut off (form ${CMAKE_MATCH_1},${CMAKE_MATCH_2} to ${CMAKE_MATCH_3},${CMAKE_MATCH_4})")
endif()

# ---- 4. layout audit of Kerf alone (insets and alignment)
set(ENV{GRIDLINE_AUDIT_ONLY} "gl.slicer")
execute_process(COMMAND "${APP}" --hidden --layout-audit "${OUT}/audit" --size 2400x1600 --screenshot "${OUT}/audit/frame.ppm" --frames 100000 --no-ui-sounds
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 280)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: the layout audit of Kerf found issues (${rc}): ${out}\n${err}")
endif()
file(READ "${OUT}/audit/alignment.tsv" align)
if(align MATCHES "\ngl\\.slicer\t")
  message(FATAL_ERROR "FAIL: alignment findings in Kerf's editor:\n${align}")
endif()
message(STATUS "j181 Kerf UI: drop zone, load, tabs, display modes, fit at 1280 x 800, layout audit clean")
