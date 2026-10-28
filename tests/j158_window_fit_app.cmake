# Gridline — J-158: big plugin editors fit inside the main window, in the real app (hidden,
# isolated configs, off-screen, SDL's dummy audio driver, two virtual monitors side by side).
# Platter (two decks and a mixer) and Seltmann (a fixed 1120 px layout) are opened in a
# 1280x800 main window:
#   1. no OS window is created for them (detached 0), the main window keeps its size, and the
#      editor window lies inside the main window's work area (between the top bar and the
#      status bar), with "Allow windows outside" on and off;
#   2. the reported bug: the editor last saved on a larger main window (1920x1080, layout.ini
#      holds its size and place) opened again in the 1280x800 one popped out into an OS window
#      of its own (ImGui's first-frame viewport fallback); it now opens inside, as in 1;
#   3. at 150 % (a 960x600 workspace) the editors are wider than the work area: the window is
#      capped to it and the content scrolls sideways (and down) instead of running off the
#      main window;
#   4. on a big screen nothing changes: Seltmann has its natural size and does not scroll.
# Where the platform cannot create windows outside the main one the test still checks the fit.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j158_window_fit_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# run(<tag> <cfg> <size> <scale> <plugin> <script> [keep]): config ${OUT}/cfg_<cfg>, fresh unless "keep".
function(run tag cfg size scale plugin script)
  if(NOT "${ARGN}" STREQUAL "keep")
    file(REMOVE_RECURSE "${OUT}/cfg_${cfg}")
  endif()
  file(MAKE_DIRECTORY "${OUT}/cfg_${cfg}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${cfg}")
  set(ENV{HOME} "${OUT}/cfg_${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size ${size} --ui-scale ${scale} --frames 44
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
function(ok msg)
  message(STATUS "ok - ${msg}")
endfunction()

# check(<what> <frame> <main w> <main h>): the "viewports" dump of <frame>: nothing detached, the
# main window (ImGui and OS size) as asked, the channel window inside the work area. Sets
# <what>_w _h _sx _sy (window size, scroll range) in the caller.
function(check what frame mw mh)
  string(REGEX MATCH "viewports frame ${frame}:[^\n]*\n(  [^\n]*\n)*" d "${out}")
  if(d STREQUAL "")
    fail("${what}: no viewports dump at frame ${frame}")
  endif()
  string(REGEX MATCH "available ([01]) enabled ([01]) detached ([0-9]+)" _ "${d}")
  if(NOT CMAKE_MATCH_3 EQUAL 0)
    fail("${what}: ${CMAKE_MATCH_3} OS window(s) created (detached), expected none")
  endif()
  string(REGEX MATCH "viewport 0 window \"\\(main\\)\" pos [-0-9.]+ [-0-9.]+ size ([0-9]+)[.0-9]* ([0-9]+)[.0-9]*" _ "${d}")
  if(NOT CMAKE_MATCH_1 EQUAL ${mw} OR NOT CMAKE_MATCH_2 EQUAL ${mh})
    fail("${what}: main window ${CMAKE_MATCH_1}x${CMAKE_MATCH_2}, expected ${mw}x${mh}")
  endif()
  string(REGEX MATCH "  window \"channelsettings\" pos ([-0-9]+)[.0-9]* ([-0-9]+)[.0-9]* size ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* viewport ([0-9]+) [^\n]* scroll ([0-9]+) ([0-9]+)"
         line "${d}")
  if(line STREQUAL "")
    fail("${what}: no channel window at frame ${frame}")
  endif()
  set(x ${CMAKE_MATCH_1})
  set(y ${CMAKE_MATCH_2})
  set(w ${CMAKE_MATCH_3})
  set(h ${CMAKE_MATCH_4})
  if(NOT CMAKE_MATCH_5 EQUAL 0)
    fail("${what}: the channel window is in viewport ${CMAKE_MATCH_5}, not the main window")
  endif()
  math(EXPR right "${x} + ${w}")
  math(EXPR bottom "${y} + ${h}")
  math(EXPR workBottom "${mh} - 26")  # the status bar
  if(x LESS 0 OR y LESS 48 OR right GREATER ${mw} OR bottom GREATER workBottom)
    fail("${what}: window ${x},${y} ${w}x${h} not inside the work area 0,48 .. ${mw},${workBottom}")
  endif()
  set(${what}_w ${w} PARENT_SCOPE)
  set(${what}_h ${h} PARENT_SCOPE)
  set(${what}_sx ${CMAKE_MATCH_6} PARENT_SCOPE)
  set(${what}_sy ${CMAKE_MATCH_7} PARENT_SCOPE)
  ok("${what}: inside the work area (${x},${y} ${w}x${h}), nothing detached, main ${mw}x${mh}")
endfunction()

# ---- 1. 1280x800: fresh config, detaching on and off ----
foreach(p platter seltmann)
  run(${p}_on ${p}_on 1280x800 1 gl.${p} "40 viewports\n")
  check(${p}_on 40 1280 800)
  run(${p}_off ${p}_off 1280x800 1 gl.${p} "1 detaching off\n40 viewports\n")
  check(${p}_off 40 1280 800)
endforeach()

# ---- 2. saved on a 1920x1080 main window, opened again in a 1280x800 one ----
foreach(p platter seltmann)
  run(${p}_big ${p}_saved 1920x1080 1 gl.${p} "40 viewports\n")
  if(NOT EXISTS "${OUT}/cfg_${p}_saved/gridline/layout.ini")
    fail("${p}: no layout.ini saved")
  endif()
  file(READ "${OUT}/cfg_${p}_saved/gridline/layout.ini" ini)
  if(NOT ini MATCHES "\\[Window\\]\\[channelsettings\\][^[]*Size=")
    fail("${p}: the channel window's size was not saved:\n${ini}")
  endif()
  run(${p}_again ${p}_saved 1280x800 1 gl.${p} "2 viewports\n3 viewports\n40 viewports\n" keep)
  check(${p}_again_open 3 1280 800)
  check(${p}_again 40 1280 800)
  # and closed and opened again in that run
  run(${p}_reopen ${p}_saved 1280x800 1 gl.${p} "10 plugwin close\n20 plugwin chan 1\n22 viewports\n40 viewports\n" keep)
  check(${p}_reopen_open 22 1280 800)
  check(${p}_reopen 40 1280 800)
endforeach()

# ---- 3. 150 %: a 960x600 workspace, narrower than both editors ----
foreach(p platter seltmann)
  run(${p}_150 ${p}_150 1280x800 1.5 gl.${p} "40 viewports\n")
  check(${p}_150 40 960 600)
  if(${p}_150_sx EQUAL 0 OR ${p}_150_sy EQUAL 0)
    fail("${p} at 150 %: the content should scroll sideways and down (scroll ${${p}_150_sx} ${${p}_150_sy})")
  endif()
  ok("${p} at 150 %: capped to ${${p}_150_w}x${${p}_150_h}, the content scrolls (${${p}_150_sx} x ${${p}_150_sy})")
endforeach()

# ---- 4. a big screen: Seltmann at its natural size ----
run(seltmann_big2 seltmann_big2 1920x1080 1 gl.seltmann "40 viewports\n")
check(seltmann_big2 40 1920 1080)
if(NOT seltmann_big2_sx EQUAL 0 OR NOT seltmann_big2_sy EQUAL 0)
  fail("seltmann at 1920x1080 should not scroll (scroll ${seltmann_big2_sx} ${seltmann_big2_sy})")
endif()
if(seltmann_big2_w LESS 1144)
  fail("seltmann at 1920x1080 should keep its natural width (1120 + padding), got ${seltmann_big2_w}")
endif()
ok("seltmann at 1920x1080: natural size ${seltmann_big2_w}x${seltmann_big2_h}, no scrolling")
