# Gridline: the piano roll script window's integer knobs, driven by scripted input in the real app
# (hidden, isolated config and HOME, fixed frame time) on a copy of the test song, with the shipped
# ratchet script open on pattern 0, channel 6. Its "Repeats" knob runs 2..16 from 4.
#   1. a slow drag (1 px a frame, 60 px up) moves the value: the steps add up instead of rounding
#      back every frame;
#   2. a double-click on the knob goes back to the script's own value (4), not to the current one.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P script_knob_drag_app.cmake
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
                          --script ratchet --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# The value and place of "Repeats" in the readout after `frame`.
function(repeats out frame vout xout yout)
  string(REGEX MATCH "scriptform frame ${frame}\n[^\n]*(\n[^\n]*)*" block "${out}")
  if(NOT block MATCHES "knob \"Repeats\" ([-0-9.]+) at ([0-9]+) ([0-9]+)")
    message(FATAL_ERROR "FAIL: no Repeats knob in the script window at frame ${frame}:\n${out}")
  endif()
  set(${vout} ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${xout} ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${yout} ${CMAKE_MATCH_3} PARENT_SCOPE)
endfunction()

run(where "10 scriptform\n12 quit discard\n")
repeats("${where_out}" 10 v0 x y)
if(NOT v0 STREQUAL "4")
  message(FATAL_ERROR "FAIL: Repeats starts at ${v0}, not the script's 4")
endif()
message(STATUS "ok - Repeats starts at 4, at ${x} ${y}")

# 1. Slow drag: press, then 1 px up a frame for 60 frames (about 4 steps at this range).
set(s "10 move ${x} ${y}\n12 down 0\n")
foreach(i RANGE 1 60)
  math(EXPR f "12 + ${i}")
  math(EXPR py "${y} - ${i}")
  string(APPEND s "${f} move ${x} ${py}\n")
endforeach()
string(APPEND s "74 scriptform\n76 up 0\n80 scriptform\n")
# 2. Double-click the knob: back to the script's value.
string(APPEND s "84 move ${x} ${y}\n86 down 0\n87 up 0\n88 down 0\n89 up 0\n92 scriptform\n94 quit discard\n")
run(drag "${s}")
repeats("${drag_out}" 74 vmid mx my)
repeats("${drag_out}" 80 vend ex ey)
if(vend LESS 7 OR vend GREATER 9)
  message(FATAL_ERROR "FAIL: a slow 60 px drag left Repeats at ${vend} (mid-drag ${vmid}); expected about 8")
endif()
message(STATUS "ok - a slow drag moves Repeats from 4 to ${vend}")
repeats("${drag_out}" 92 vreset rx ry)
if(NOT vreset STREQUAL "4")
  message(FATAL_ERROR "FAIL: a double-click left Repeats at ${vreset}, not the script's 4")
endif()
message(STATUS "ok - a double-click goes back to the script's own value")
