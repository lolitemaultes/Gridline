# Gridline — J-208: the mouse wheel over a block of Seltmann's sequence pitches that one step (not its
# region: the region's other steps keep their pitch), driven like a user (hidden window, isolated
# config and HOME, fixed frame time, SDL's dummy audio):
#   1. two notches up over block D (step 4): that step plays 2 semitones higher, region D itself and
#      every other step are unchanged; one undo step takes both notches back;
#   2. Shift + a notch down: a tenth of a semitone lower;
#   3. the wheel away from the editor changes nothing.
# Positions come from the editor's GRIDLINE_SELTMANN_TRACE geometry lines. Screenshot <OUT>/wheel.ppm.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P j208_seltmann_wheel.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_SELTMANN_TRACE} "1")
set(ENV{GRIDLINE_SELTMANN_DEMO} "${OUT}/Break 96.wav")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "5 waitfor seltmann-ready 120\n${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.seltmann --size 1440x960 --frames ${frames}
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

function(last_state var text)
  string(REGEX MATCHALL "seltmann: state ready[^\n]*" lines "${text}")
  list(GET lines -1 l)
  set(${var} "${l}" PARENT_SCOPE)
endfunction()

run(ready 30 "")
set(last_out "${ready_out}")
check("... in order, nothing pitched" ready_out MATCHES "order \"A B C D [^\"]*\" pitch \"\" start A")
string(REGEX MATCH "seltmann: seq 3 ([0-9]+) ([0-9]+)" _ "${ready_out}")
set(sx "${CMAKE_MATCH_1}")
set(sy "${CMAKE_MATCH_2}")
check("block D is traced (${sx} ${sy})" sx MATCHES "^[0-9]+$")

# ---- 1. two notches up over block D
set(up "10 move ${sx} ${sy}
12 wheel 1
14 wheel 1
18 move ${sx} ${sy}
")
run(wheel 30 "${up}")
set(last_out "${wheel_out}")
last_state(st "${wheel_out}")
check("two wheel notches up over block D pitch step 4 by 2 semitones" st MATCHES "stepst \"4:\\+2\"")
check("... region D itself (its other steps, its pad) keeps its pitch" st MATCHES "pitch \"\" ")
run(wheelundo 34 "${up}24 undo
")
set(last_out "${wheelundo_out}")
last_state(st "${wheelundo_out}")
check("... one undo step takes both notches back" st MATCHES "stepst \"\"")

# ---- 2. Shift: a tenth of a semitone
run(fine 34 "${up}20 mod shift 1
22 wheel -1
24 mod shift 0
")
set(last_out "${fine_out}")
last_state(st "${fine_out}")
check("Shift + a notch down: +1.9" st MATCHES "stepst \"4:\\+1.9\"")

# ---- 3. the wheel away from the editor (top left of the window) leaves the pitches alone
run(strip 30 "10 move 6 6
12 wheel 1
14 wheel 1
")
set(last_out "${strip_out}")
last_state(st "${strip_out}")
check("the wheel away from the blocks pitches nothing" st MATCHES "pitch \"\" " AND st MATCHES "stepst \"\"")
check("screenshot wheel.ppm written" EXISTS "${OUT}/wheel.ppm")
