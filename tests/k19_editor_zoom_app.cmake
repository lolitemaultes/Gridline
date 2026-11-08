# Gridline - K-19: plugin editors have their own size on top of the interface scale, in the
# real app (hidden, isolated configs, off-screen, SDL's dummy audio driver):
#   1. automatic size (K-136: with "larger on big screens" on): a small editor (Reverb) on a 2560x1440 screen at 130 % opens larger
#      (125 %), on a 1440x900 one at its design size (100 %);
#   2. a size picked for the plugin (Editor size 150 %) is drawn at that zoom, the window is
#      that much larger, inside the work area, and remembered in the next run;
#   3. at 150 % the controls work where they show: dragging a knob (pointer on its magnified
#      place) changes it;
#   4. the zoom never makes a window larger than the work area (200 % on a 1920x1080 screen
#      at 200 %: capped to what fits).
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k19_editor_zoom_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
unset(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE})  # K-136: the real plugin window size setting
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag cfg size scale plugin script)
  if(NOT "${ARGN}" STREQUAL "keep")
    file(REMOVE_RECURSE "${OUT}/cfg_${cfg}")
  endif()
  file(MAKE_DIRECTORY "${OUT}/cfg_${cfg}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${cfg}")
  set(ENV{HOME} "${OUT}/cfg_${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size ${size} --ui-scale ${scale} --frames 40
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" --plugin ${plugin}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
endfunction()

# zoom(<what> <frame>): the "edzoom fxeditor" dump of that frame; sets <what>_z (x100), _w, _h, _x, _y, _dh.
function(zoom what frame)
  string(REGEX MATCH "frame ${frame}[^\n]*\n" _ "${out}")  # (frame markers are not printed for edzoom)
  string(REGEX MATCHALL "edzoom fxeditor: type [^ ]+ zoom ([0-9.]+) design ([0-9]+)x([0-9]+) origin [-0-9.,]+ window ([-0-9]+)[.0-9]*,([-0-9]+)[.0-9]* ([0-9]+)[.0-9]*x([0-9]+)[.0-9]*" all "${out}")
  list(LENGTH all n)
  if(n EQUAL 0)
    fail("${what}: no edzoom dump")
  endif()
  list(GET all -1 last)
  string(REGEX MATCH "zoom ([0-9.]+) design ([0-9]+)x([0-9]+) origin [-0-9.,]+ window ([-0-9]+)[.0-9]*,([-0-9]+)[.0-9]* ([0-9]+)[.0-9]*x([0-9]+)" _ "${last}")
  math(EXPR z100 "0")
  string(REPLACE "." "" zs "${CMAKE_MATCH_1}")
  math(EXPR z100 "${zs}")
  set(${what}_z ${z100} PARENT_SCOPE)
  set(${what}_dh ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(${what}_x ${CMAKE_MATCH_4} PARENT_SCOPE)
  set(${what}_y ${CMAKE_MATCH_5} PARENT_SCOPE)
  set(${what}_w ${CMAKE_MATCH_6} PARENT_SCOPE)
  set(${what}_h ${CMAKE_MATCH_7} PARENT_SCOPE)
endfunction()

# inside(<what> <logical w> <logical h>): the window lies in the work area.
function(inside what mw mh)
  math(EXPR r "${${what}_x} + ${${what}_w}")
  math(EXPR b "${${what}_y} + ${${what}_h}")
  math(EXPR wb "${mh} - 26")
  if(${what}_x LESS 0 OR ${what}_y LESS 40 OR r GREATER ${mw} OR b GREATER wb)
    fail("${what}: window ${${what}_x},${${what}_y} ${${what}_w}x${${what}_h} outside the work area ${mw}x${wb}")
  endif()
endfunction()

# ---- 1. automatic size
run(auto_1440p130 a 2560x1440 1.3 gl.reverb "1 setting ui.pluginWindowFit on\n38 edzoom fxeditor\n")
zoom(a 38)
if(NOT a_z EQUAL 125)
  fail("Reverb at 2560x1440 / 130 %: automatic zoom ${a_z}, expected 125")
endif()
inside(a 1969 1108)
message(STATUS "ok - automatic: Reverb opens at 125 % on 2560x1440 at 130 % (${a_w}x${a_h})")
run(auto_900 b 1440x900 1 gl.reverb "1 setting ui.pluginWindowFit on\n38 edzoom fxeditor\n")
zoom(b 38)
if(NOT b_z EQUAL 100)
  fail("Reverb at 1440x900: automatic zoom ${b_z}, expected 100")
endif()
message(STATUS "ok - automatic: Reverb at its design size on 1440x900 (${b_w}x${b_h})")

# ---- 2. a picked size, remembered
run(pick150 p 2560x1440 1.3 gl.reverb "2 edzoomset gl.reverb 1.5\n38 edzoom fxeditor\n")
zoom(p 38)
if(NOT p_z EQUAL 150)
  fail("Reverb set to 150 %: drawn at ${p_z}")
endif()
inside(p 1969 1108)
math(EXPR expectW "(${b_w} - 16) * 3 / 2 + 16")
math(EXPR dw "${p_w} - ${expectW}")
if(dw GREATER 4 OR dw LESS -4)
  fail("Reverb at 150 %: window ${p_w} wide, expected about ${expectW}")
endif()
file(READ "${OUT}/cfg_p/gridline/settings.json" settings)
if(NOT settings MATCHES "\"ui.editorZoom\": *\"gl.reverb=1.5\"")
  fail("the picked size was not saved: ${settings}")
endif()
run(pick150_again p 2560x1440 1.3 gl.reverb "38 edzoom fxeditor\n" keep)
zoom(p2 38)
if(NOT p2_z EQUAL 150)
  fail("Reverb opened again: drawn at ${p2_z}, expected the remembered 150")
endif()
message(STATUS "ok - picked 150 %: ${p_w}x${p_h} (design ${b_w}x${b_h}), remembered")

# ---- 3. a knob drag at 150 % (the pointer on the knob's magnified place)
run(drag p 2560x1440 1.3 gl.reverb "20 plug 1\n21 atparam 1 0 0\n23 down 0\n25 atparam 1 0 -30\n27 atparam 1 0 -60\n29 up 0\n32 plug 1\n" keep)
string(REGEX MATCH "atparam frame 21: param 1 at ([0-9.]+) ([0-9.]+)" _ "${out}")
if(CMAKE_MATCH_1 STREQUAL "")
  fail("knob 1 not shown")
endif()
string(REGEX MATCH "plug frame 20:[^\n]*\n  param 1 ([-0-9.]+)" _ "${out}")
set(before ${CMAKE_MATCH_1})
string(REGEX MATCH "plug frame 32:[^\n]*\n  param 1 ([-0-9.]+)" _ "${out}")
set(after ${CMAKE_MATCH_1})
if(before STREQUAL "" OR after STREQUAL "" OR before STREQUAL after)
  fail("dragging knob 1 at 150 % did not change it (${before} -> ${after})")
endif()
message(STATUS "ok - a knob dragged at 150 % changes (${before} -> ${after})")

# ---- 4. never larger than the work area
run(cap c 1920x1080 2 gl.reverb "2 edzoomset gl.reverb 2\n38 edzoom fxeditor\n")
zoom(c 38)
if(NOT c_z LESS 200)
  fail("Reverb at 200 % in a 1067x600 workspace: not capped (${c_z})")
endif()
inside(c 1067 600)
message(STATUS "ok - 200 % asked in a 1067x600 workspace: capped to ${c_z} %, ${c_w}x${c_h} inside")
