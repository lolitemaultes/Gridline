# Gridline - K-112: in the Live layout the notes editor's envelope is drawn over the notes.
# Runs the app hidden (isolated config, fixed frame time) with the demo pattern lane (`--show autolane`)
# in the Live and Studio layouts, and checks:
#   A. Live: the envelope is drawn over the notes grid, with its choosers and draw-curve chip in a band
#      under it (not in the key column);
#   B. Live: a drag across the grid with the draw tool redraws the envelope's points in one undo step
#      (the notes are not edited: no second step), and the envelope keeps the keyboard (focus);
#   C. Studio: the lane stays a strip under the grid, with the chip in the key column beside it.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k112_envelope_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag layout frames script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames ${frames}
                          --show autolane --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
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

# The "alane" line printed at <frame>: <prefix>_points, _focus, _x0 _y0 _x1 _y1 (the lane rect),
# _cx0 _cy0 _cx1 _cy1 (the draw-curve chip), _undo, _pts (whole pixels).
function(alane out frame prefix)
  string(REGEX MATCH "alane frame ${frame}: target [0-9]+ [0-9]+ points [0-9]+ sel [0-9]+ focus [0-9] shape [0-9]+ lane [-0-9. ]+ chip [-0-9. ]+ undo [0-9]+ pts[^\n]*" line "${out}")
  if(NOT line)
    message(FATAL_ERROR "FAIL: no alane line at frame ${frame}\n${out}")
  endif()
  string(REGEX MATCH " pts([^\n]*)" _ "${line}")
  string(STRIP "${CMAKE_MATCH_1}" pts)
  string(REGEX REPLACE " pts[^\n]*" "" head "${line}")
  string(REGEX REPLACE "^alane frame [0-9]+: target [0-9]+ [0-9]+ " "" head "${head}")
  string(REGEX MATCHALL "-?[0-9.]+" m "${head}")
  # points sel focus shape, the lane rect (4), the chip rect (4), undo
  set(names points sel focus shape x0 y0 x1 y1 cx0 cy0 cx1 cy1 undo)
  foreach(i RANGE 12)
    list(GET names ${i} n)
    list(GET m ${i} v)
    string(REGEX REPLACE "\\..*" "" v "${v}")
    set(${prefix}_${n} ${v} PARENT_SCOPE)
  endforeach()
  set(${prefix}_pts "${pts}" PARENT_SCOPE)
endfunction()

# ---- A. Live: the envelope over the whole grid
run(live live 24 "4 clipsel 39\n20 alane\n")  # (C44-26: Clips' clip band shows the selected clip only: clip 39 plays the current pattern)
set(last_out "${live_out}")
alane("${live_out}" 20 L)
check("Live: the demo envelope has points (${L_points})" L_points GREATER 2)
check("Live: the envelope is drawn over the notes grid, above the band of its choosers (${L_y1} <= ${L_cy0})" L_y1 LESS_EQUAL L_cy0)
check("Live: ... and the draw-curve chip sits in that band under the grid, not in the key column (${L_cx0} > ${L_x0})" L_cx0 GREATER L_x0)

# ---- B. Live: a drag across the grid redraws the envelope, in one undo step, and keeps the keyboard
math(EXPR xa "${L_x0} + (${L_x1} - ${L_x0}) / 8")
math(EXPR xb "${L_x0} + (${L_x1} - ${L_x0}) / 3")
math(EXPR ya "${L_y0} + (${L_y1} - ${L_y0}) / 5")
run(draw live 44 "4 clipsel 39\n20 alane\n22 pianotool draw\n24 move ${xa} ${ya}\n26 down 0\n28 move ${xb} ${ya}\n30 move ${xb} ${ya}\n32 up 0\n36 alane\n")
set(last_out "${draw_out}")
alane("${draw_out}" 20 D0)
alane("${draw_out}" 36 D1)
math(EXPR steps "${D1_undo} - ${D0_undo}")
check("Live: the drag redrew the envelope's points" NOT "${D0_pts}" STREQUAL "${D1_pts}")
check("Live: ... in one undo step, the notes untouched (${steps})" steps EQUAL 1)
check("Live: the envelope keeps the keyboard after a click on the grid (${D1_focus})" D1_focus EQUAL 1)

# ---- C. Studio: the lane stays a strip under the grid
run(studio studio 24 "20 alane\n")
set(last_out "${studio_out}")
alane("${studio_out}" 20 S)
check("Studio: the draw-curve chip stays in the key column beside the lane (${S_cx1} <= ${S_x0})" S_cx1 LESS_EQUAL S_x0)
check("Studio: ... with the lane beside it, not above it (${S_cy0} < ${S_y1})" S_cy0 LESS S_y1)
