# Gridline — K-83: on X11 (Linux, BSD) the menus, lists and hints of a popped-out (detached) window
# stay inside it where they fit, in the real app (hidden, isolated configs, off-screen at 1440x900,
# SDL's dummy audio driver). The report: the screen flickered on a GNOME laptop (X11) whenever
# dropdown menus came on screen. Each popup that ran past its window got an undecorated OS window
# of its own, made and destroyed per submenu or hint, which the window manager focused, raised and
# animated. K-72 keeps the main window's popups inside it; this covers a detached window's:
#   1. the default: popups stay in on X11, may stick out on macOS and Windows ("popups-stay-in");
#   2. with popups kept in ("popupsin on"), a popped-out rack's "+" menu and each instrument submenu
#      hovered open inside the rack's own window (its viewport, inside its rectangle) and no OS
#      window is made for any of them (popup-windows 0);
#   3. an instrument picked from such a submenu is still added.
# Where the platform cannot create windows outside the main one the test is skipped.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k83_popups_detached_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()
function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
endfunction()
function(ok msg)
  message(STATUS "ok - ${msg}")
endfunction()

# A popped-out rack, large enough for its menu, on the monitor's right part (outside the main window).
set(setup "2 vmonitors -240 -120 2160 1140
3 popupsin on
6 detach \"Channel rack\" 1100 80 560 620
")
run(geo 24 "2 vmonitors -240 -120 2160 1140\n20 rack\n22 viewports\n")
if(out MATCHES "available 0")
  string(REGEX MATCH "unavailable: [^\n]*" why "${out}")
  message(STATUS "SKIP: windows cannot be detached on this platform (${why})")
  return()
endif()

# 1. The platform default.
if(CMAKE_HOST_APPLE OR CMAKE_HOST_WIN32)
  set(want 0)
else()
  set(want 1)
endif()
if(NOT out MATCHES "popups-stay-in ${want}")
  fail("default: popups of detached windows should stay in = ${want} on this platform")
endif()
ok("default: popups of detached windows stay in = ${want} on this platform")

run(geo 30 "${setup}20 rack\n22 viewports\n")
if(NOT out MATCHES "window \"Channel rack\" pos ([-0-9]+)[.0-9]* ([-0-9]+)[.0-9]* size ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* viewport ([1-9][0-9]*)")
  fail("the rack should be popped out")
endif()
set(rx0 "${CMAKE_MATCH_1}")
set(ry0 "${CMAKE_MATCH_2}")
math(EXPR rx1 "${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}")
math(EXPR ry1 "${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}")
set(rackVp "${CMAKE_MATCH_5}")
if(NOT out MATCHES " addbutton ([-0-9]+)[.0-9]* ([-0-9]+)[.0-9]* ([-0-9]+)[.0-9]* ([-0-9]+)")
  fail("no rack add button reported")
endif()
math(EXPR bx "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
math(EXPR by "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
ok("rack popped out at ${rx0} ${ry0} to ${rx1} ${ry1} (viewport ${rackVp}), its + button at ${bx} ${by}")

# 2. Open the menu, then move the pointer down its items (submenus of instruments).
set(open "${setup}20 move ${bx} ${by}
22 down 0
23 up 0
")
run(open 32 "${open}28 viewports\n29 plugsearch\n")
string(REGEX MATCH "viewports frame 28:[^\n]*\n(  [^\n]*\n)*" d "${out}")
if(NOT d MATCHES "  popup \"[^\"]*\" pos ([-0-9]+)[.0-9]* ([-0-9]+)[.0-9]* size ([0-9]+)[.0-9]* ([0-9]+)")
  fail("open: the + menu did not open")
endif()
math(EXPR mx "${CMAKE_MATCH_1} + ${CMAKE_MATCH_3} / 2")
set(my0 "${CMAKE_MATCH_2}")
math(EXPR mBottom "${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}")
# The picker starts with a search field; its rows (the instrument kinds) start below it, where the
# dev report says, so the pointer goes down the real rows, not the field.
if(NOT out MATCHES "plugsearch frame [0-9]+ x0 [-0-9]+ y0 [-0-9]+ x1 [-0-9]+ y1 [-0-9]+ active [01] rows ([-0-9]+)")
  fail("open: the + menu's search field was not reported")
endif()
math(EXPR fy "${CMAKE_MATCH_1} + 2")
math(EXPR step "(${mBottom} - ${CMAKE_MATCH_1} + 8) / 9")
set(script "${open}28 viewports\n")
set(f 30)
foreach(k 0 1 2 3 4 5 6)
  math(EXPR y "${fy} + ${k} * ${step}")
  math(EXPR f1 "${f} + 1")
  math(EXPR f2 "${f} + 2")
  math(EXPR f6 "${f} + 6")
  string(APPEND script "${f} move ${mx} ${y}\n${f1} viewports\n${f2} viewports\n${f6} viewports\n")
  math(EXPR f "${f} + 8")
endforeach()
math(EXPR fEnd "${f} + 4")
string(APPEND script "${fEnd} viewports\n")
math(EXPR frames "${fEnd} + 4")
run(menu ${frames} "${script}")
string(REGEX MATCHALL "  popup \"[^\"]*\" pos [-0-9.]+ [-0-9.]+ size [0-9.]+ [0-9.]+ viewport [-0-9]+" popups "${out}")
list(LENGTH popups npop)
if(npop LESS 8)
  fail("menu: the + menu and its submenus should have opened (${npop} popup lines)")
endif()
set(subs 0)
foreach(p IN LISTS popups)
  string(REGEX MATCH "popup \"([^\"]*)\" pos ([-0-9]+)[.0-9]* ([-0-9]+)[.0-9]* size ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* viewport ([-0-9]+)" _ "${p}")
  set(name "${CMAKE_MATCH_1}")
  set(x "${CMAKE_MATCH_2}")
  set(y "${CMAKE_MATCH_3}")
  math(EXPR r "${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}")
  math(EXPR b "${CMAKE_MATCH_3} + ${CMAKE_MATCH_5}")
  set(vp "${CMAKE_MATCH_6}")
  if(name MATCHES "Menu_")
    math(EXPR subs "${subs} + 1")
  endif()
  if(NOT vp EQUAL rackVp)
    fail("menu: popup ${name} should be in the rack's window (viewport ${rackVp}), not ${vp} (${x} ${y} to ${r} ${b})")
  endif()
  if(x LESS rx0 OR y LESS ry0 OR r GREATER rx1 OR b GREATER ry1)
    fail("menu: popup ${name} runs past the rack's window (${x} ${y} to ${r} ${b}; rack ${rx0} ${ry0} to ${rx1} ${ry1})")
  endif()
endforeach()
if(subs LESS 4)
  fail("menu: the instrument submenus should have opened while the pointer went down the list (${subs} lines)")
endif()
string(REGEX MATCHALL "popup-windows [0-9]+" made "${out}")
list(GET made -1 last)
if(NOT last STREQUAL "popup-windows 0")
  fail("menu: no OS window should be made for the detached rack's menus (${last})")
endif()
ok("the detached rack's + menu and ${subs} submenu frames stay inside its window; no OS window made for them")

# 3. An instrument from a submenu is still added.
string(REGEX MATCH "viewports frame 36:[^\n]*\n(  [^\n]*\n)*" d "${out}")
string(REGEX MATCH "popup \"[^\"]*###Menu_00\" pos ([-0-9]+)[.0-9]* ([-0-9]+)[.0-9]* size ([0-9]+)" _ "${d}")
if(CMAKE_MATCH_1 STREQUAL "")
  fail("add: no submenu with the pointer on the first item")
endif()
math(EXPR sx "${CMAKE_MATCH_1} + ${CMAKE_MATCH_3} / 2")
math(EXPR sy "${CMAKE_MATCH_2} + 14")
set(y0 "${fy}")
run(add 60 "${open}30 move ${mx} ${y0}
40 move ${sx} ${y0}
44 move ${sx} ${sy}
50 down 0
51 up 0
54 channels
")
string(REGEX MATCH "channels frame 54: ([0-9]+)" _ "${out}")
set(nAfter "${CMAKE_MATCH_1}")
run(count 24 "${setup}20 channels\n")
string(REGEX MATCH "channels frame 20: ([0-9]+)" _ "${out}")
set(nBefore "${CMAKE_MATCH_1}")
if(nBefore STREQUAL "" OR nAfter STREQUAL "")
  fail("add: no channel counts (${nBefore} -> ${nAfter})")
endif()
math(EXPR nWant "${nBefore} + 1")
if(NOT nAfter EQUAL nWant)
  fail("add: an instrument picked from the detached rack's submenu should add a channel (${nBefore} -> ${nAfter})")
endif()
ok("an instrument picked from the detached rack's submenu is added (${nBefore} -> ${nAfter} channels)")
