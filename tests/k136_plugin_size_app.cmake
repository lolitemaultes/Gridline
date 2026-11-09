# Gridline - K-136: one size for every built-in plugin window (Settings > Interface, "Plugin
# window size") and a size of its own per plugin, in the real app (hidden, off-screen,
# isolated configs, SDL's dummy audio driver):
#   1. by default (K-147: 75 %) an effect and an instrument window are drawn at 75 %, on a large
#      screen too (they no longer grow by themselves);
#   2. at 75 / 100 / 125 / 150 % the editor and its window scale in proportion;
#   3. a size picked for one plugin (Editor size) wins over the global size and is
#      remembered in the next run;
#   4. at 75 % a knob dragged where it shows changes, and the pointer beside the smaller
#      window (inside the editor's design size) is not over the editor;
#   5. with "Open small plugin windows larger on big screens" on, a small editor grows
#      on a large screen again (the earlier automatic size).
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k136_plugin_size_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
unset(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE})  # K-136: the real plugin window size setting
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag cfg size plugin script)
  if(NOT "${ARGN}" STREQUAL "keep")
    file(REMOVE_RECURSE "${OUT}/cfg_${cfg}")
  endif()
  file(MAKE_DIRECTORY "${OUT}/cfg_${cfg}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${cfg}")
  set(ENV{HOME} "${OUT}/cfg_${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size ${size} --frames 40
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

# zoom(<what> <key> <n>): the n-th (0-based) "edzoom <key>" dump; sets <what>_z (percent),
# _dw, _dh (design size), _x, _y, _w, _h (window), _hov (pointer over the editor).
function(zoom what key n)
  string(REGEX MATCHALL "edzoom ${key}: type [^ ]+ zoom [0-9.]+ design [0-9]+x[0-9]+ origin [-0-9.,]+ window [-0-9.]+,[-0-9.]+ [0-9.]+x[0-9.]+ hovered [01]" all "${out}")
  list(LENGTH all count)
  if(count LESS_EQUAL n)
    fail("${what}: no edzoom ${key} dump ${n}")
  endif()
  list(GET all ${n} d)
  string(REGEX MATCH "zoom ([0-9.]+) design ([0-9]+)x([0-9]+) origin [-0-9.,]+ window ([-0-9]+)[.0-9]*,([-0-9]+)[.0-9]* ([0-9]+)[.0-9]*x([0-9]+)[.0-9]* hovered ([01])" _ "${d}")
  string(REPLACE "." "" zs "${CMAKE_MATCH_1}")
  math(EXPR z100 "${zs}")
  set(${what}_z ${z100} PARENT_SCOPE)
  set(${what}_dw ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${what}_dh ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(${what}_x ${CMAKE_MATCH_4} PARENT_SCOPE)
  set(${what}_y ${CMAKE_MATCH_5} PARENT_SCOPE)
  set(${what}_w ${CMAKE_MATCH_6} PARENT_SCOPE)
  set(${what}_h ${CMAKE_MATCH_7} PARENT_SCOPE)
  set(${what}_hov ${CMAKE_MATCH_8} PARENT_SCOPE)
endfunction()

# near(<a> <b> <tol> <msg>)
function(near a b tol msg)
  math(EXPR d "${a} - ${b}")
  if(d GREATER ${tol} OR d LESS -${tol})
    fail("${msg}: ${a}, expected about ${b}")
  endif()
endfunction()

# ---- 1. the default: 75 %, also on a large screen
run(def_fx d 2560x1440 gl.comp "38 edzoom fxeditor\n")
zoom(dfx fxeditor 0)
if(NOT dfx_z EQUAL 75)
  fail("Compressor by default on 2560x1440: drawn at ${dfx_z} %, expected 75")
endif()
run(def_inst i 1920x1080 gl.synth "38 edzoom channelsettings\n")
zoom(din channelsettings 0)
if(NOT din_z EQUAL 75)
  fail("an instrument window by default: drawn at ${din_z} %, expected 75")
endif()
message(STATUS "ok - by default effect and instrument windows are drawn at 75 % (${dfx_w}x${dfx_h}, ${din_w}x${din_h})")

# ---- 2. every step of the setting: editor and window in proportion
set(w100 0)
foreach(step 75 100 125 150)
  math(EXPR whole "${step} / 100")
  math(EXPR frac "${step} % 100")
  if(frac LESS 10)
    set(frac "0${frac}")
  endif()
  run(step${step} s${step} 1920x1080 gl.comp "1 setting ui.pluginWindowSize ${whole}.${frac}\n38 edzoom fxeditor\n")
  zoom(s${step} fxeditor 0)
  if(NOT s${step}_z EQUAL ${step})
    fail("plugin window size ${step} %: the Compressor is drawn at ${s${step}_z} %")
  endif()
  # The window holds the scaled picture (its chrome is the same at every size), never clipped.
  math(EXPR expectW "${s${step}_dw} * ${step} / 100")
  math(EXPR expectH "${s${step}_dh} * ${step} / 100")
  math(EXPR roomW "${s${step}_w} - 16")
  near(${roomW} ${expectW} 4 "${step} %: window content width")
  if(s${step}_h LESS expectH)
    fail("${step} %: window ${s${step}_h} px tall, the editor needs ${expectH}")
  endif()
  if(step EQUAL 100)
    set(w100 ${s100_w})
  endif()
endforeach()
math(EXPR r150 "(${s150_w} - 16) * 100 / (${w100} - 16)")
math(EXPR r75 "(${s75_w} - 16) * 100 / (${w100} - 16)")
near(${r150} 150 2 "window width at 150 % over 100 %")
near(${r75} 75 2 "window width at 75 % over 100 %")
message(STATUS "ok - 75 / 100 / 125 / 150 %: windows ${s75_w} / ${s100_w} / ${s125_w} / ${s150_w} px wide, in proportion")

# ---- 3. a size of its own wins and is remembered
run(own o 1920x1080 gl.comp "1 setting ui.pluginWindowSize 0.75\n2 edzoomset gl.comp 1.25\n38 edzoom fxeditor\n")
zoom(o fxeditor 0)
if(NOT o_z EQUAL 125)
  fail("Compressor set to 125 % with the global size at 75 %: drawn at ${o_z} %")
endif()
file(READ "${OUT}/cfg_o/gridline/settings.json" settings)
if(NOT settings MATCHES "\"ui.editorZoom\": *\"gl.comp=1.25\"" OR NOT settings MATCHES "\"ui.pluginWindowSize\": *0.75")
  fail("the sizes were not saved: ${settings}")
endif()
run(own_again o 1920x1080 gl.comp "38 edzoom fxeditor\n" keep)
zoom(o2 fxeditor 0)
run(own_other o 1920x1080 gl.delay "38 edzoom fxeditor\n" keep)
zoom(o3 fxeditor 0)
if(NOT o2_z EQUAL 125 OR NOT o3_z EQUAL 75)
  fail("next run: Compressor at ${o2_z} % (expected its own 125), another effect at ${o3_z} % (expected the global 75)")
endif()
message(STATUS "ok - a plugin's own size (125 %) wins over the global 75 % and is remembered; other plugins keep 75 %")

# ---- 4. at 75 %: a knob drag where it shows; the pointer beside the window is not over the editor
run(drag g 1920x1080 gl.reverb "1 setting ui.pluginWindowSize 0.75\n20 plug 1\n21 atparam 1 0 0\n23 down 0\n25 atparam 1 0 -30\n27 atparam 1 0 -60\n29 up 0\n32 plug 1\n33 edzoom fxeditor\n")
string(REGEX MATCH "plug frame 20:[^\n]*\n  param 1 ([-0-9.]+)" _ "${out}")
set(before ${CMAKE_MATCH_1})
string(REGEX MATCH "plug frame 32:[^\n]*\n  param 1 ([-0-9.]+)" _ "${out}")
set(after ${CMAKE_MATCH_1})
if(before STREQUAL "" OR after STREQUAL "" OR before STREQUAL after)
  fail("dragging knob 1 at 75 % did not change it (${before} -> ${after})")
endif()
zoom(g fxeditor 0)
# A point inside the editor's design size but past the reduced window (right of it, same height).
math(EXPR px "${g_x} + ${g_w} + 30")
math(EXPR py "${g_y} + ${g_h} / 2")
math(EXPR inx "${g_x} + ${g_w} / 2")
run(hover g 1920x1080 gl.reverb "1 setting ui.pluginWindowSize 0.75\n20 move ${inx} ${py}\n23 edzoom fxeditor\n26 move ${px} ${py}\n29 edzoom fxeditor\n")
zoom(h1 fxeditor 0)
zoom(h2 fxeditor 1)
if(NOT h1_hov EQUAL 1 OR NOT h2_hov EQUAL 0)
  fail("at 75 %: pointer on the editor ${h1_hov} (expected 1), beside the smaller window ${h2_hov} (expected 0)")
endif()
message(STATUS "ok - at 75 % a knob drag changes it (${before} -> ${after}); beside the window the pointer is not over the editor")

# ---- 5. larger on big screens (the earlier automatic size), when switched on
run(fit f 2560x1440 gl.reverb "1 setting ui.pluginWindowFit on\n38 edzoom fxeditor\n")
zoom(f fxeditor 0)
if(NOT f_z GREATER 100)
  fail("larger on big screens: Reverb on 2560x1440 drawn at ${f_z} %")
endif()
message(STATUS "ok - with larger on big screens on, the Reverb opens at ${f_z} % on 2560x1440")
