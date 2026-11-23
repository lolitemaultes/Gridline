# Gridline - K-245: macro knobs in the real app (hidden, isolated config and HOME, SDL's dummy audio).
#   1. in each of the four layouts, a channel's window opens on its Misc page with the macro panel,
#      a link made as the menu makes it shows under the knob, and the panel fits its window;
#   2. an effect window shows the track's macro strip, open by itself once a macro is in use;
#   3. dragging a macro knob with the real mouse moves it (one change), the link stays.
# Screenshots: <OUT>/misc_<layout>.ppm, <OUT>/fx.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<song.gridline.json> -DOUT=<dir> -P k245_macros_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home" "${OUT}/cfg/gridline")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_CACHE_DIR} "${OUT}/cache")
set(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE} "1")
file(WRITE "${OUT}/cfg/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true}}\n")
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 ${ARGN}
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
# panel(<p> <n>): the n-th "macros frame" report with a box into <p>_drawn _owner _x0 _y0 _x1 _y1 _assigned _k0x _k0y.
function(panel p n)
  string(REGEX MATCHALL "macros frame [0-9]+: drawn [^\n]*(\n  [^\n]*)*" reps "${out}")
  list(LENGTH reps count)
  if(NOT n LESS count)
    message(FATAL_ERROR "FAIL: ${p}: only ${count} macro reports\n${out}")
  endif()
  list(GET reps ${n} r)
  string(REGEX MATCH "drawn ([-0-9]+) owner ([0-9]+) selected [-0-9]+ learning [-0-9]+ box ([-0-9]+) ([-0-9]+) ([-0-9]+) ([-0-9]+) assigned ([0-9]+)" m "${r}")
  set(${p}_drawn "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${p}_owner "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${p}_x0 "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(${p}_y0 "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(${p}_x1 "${CMAKE_MATCH_5}" PARENT_SCOPE)
  set(${p}_y1 "${CMAKE_MATCH_6}" PARENT_SCOPE)
  set(${p}_assigned "${CMAKE_MATCH_7}" PARENT_SCOPE)
  string(REGEX MATCH "knob 0 ([-0-9]+) ([-0-9]+)" m "${r}")
  set(${p}_k0x "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${p}_k0y "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${p}_text "${r}" PARENT_SCOPE)
endfunction()

# ---- 1. the Misc page in every layout
foreach(lay studio live logic tracker)
  run(misc_${lay} 50 "4 macros open 0\n6 macros link c 0 100 0\n40 macros\n48 quit discard\n" --layout ${lay})
  string(FIND "${out}" "linked 1" pos)
  check("${lay}: a plugin control links to the channel's macro" pos GREATER -1)
  panel(m 0)
  math(EXPR w "${m_x1} - ${m_x0}")
  math(EXPR h "${m_y1} - ${m_y0}")
  check("${lay}: the macro panel is drawn on the Misc page (${w} x ${h}), one macro in use" m_drawn GREATER 0 AND w GREATER 400 AND h GREATER 120 AND m_assigned EQUAL 1)
  string(FIND "${m_text}" "Macro 1 ->" lk)
  check("${lay}: the link is listed with its range" lk GREATER -1)
endforeach()

# ---- 2. an effect window: the track's macro strip opens by itself once a macro is in use
run(fx 50 "4 macros link m 0 1000 3\n8 plugwin fx 0 0\n40 macros\n48 quit discard\n")
string(FIND "${out}" "linked 1" pos)
check("an effect control links to the track's macro" pos GREATER -1)
panel(f 0)
check("the effect window shows the track's macros (owner ${f_owner})" f_drawn GREATER 0 AND f_assigned EQUAL 1)

# ---- 3. a real drag on a macro knob moves it
run(drag 40 "6 macros link c 0 100 0\n20 macros\n36 macros\n38 quit discard\n" --chan 0:misc)
panel(d 0)
check("knob 0 is at ${d_k0x} ${d_k0y}" d_k0x GREATER 0 AND d_k0y GREATER 0)
math(EXPR y1 "${d_k0y} - 60")
run(drag2 60 "6 macros link c 0 100 0\n20 macros\n24 move ${d_k0x} ${d_k0y}\n26 down 0\n28 move ${d_k0x} ${d_k0y}\n32 move ${d_k0x} ${y1}\n36 up 0\n40 macros\n58 quit discard\n" --chan 0:misc)
panel(e 1)
check("after the drag the macro link is still there" e_assigned EQUAL 1)
message(STATUS "done")
