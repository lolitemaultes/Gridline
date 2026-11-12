# Gridline — K-72 (issue 15): menus, popups and hints of the main window stay inside it, in the
# real app (hidden, isolated configs, off-screen at 1440x900, SDL's dummy audio driver). The
# reported bug: with the channel rack docked at the right edge, adding instruments from its "+"
# menu made the whole app flicker. The menu and each submenu hovered ran past the main window's
# edge onto the rest of the monitor, so each got an OS window of its own, made and destroyed as
# the pointer moved down the list; some window managers blank the main window for a frame each
# time. Here the main window sits inside a larger monitor ("vmonitors": as a window that is not
# maximised), the rack is docked at the right edge and the pointer opens its "+" menu and moves
# down the instrument kinds:
#   1. every popup and menu is in the main window (viewport 0) and inside its rectangle, and no
#      OS window was made for any of them (popup-windows 0);
#   2. an instrument added from a submenu is added (a new channel, as before);
#   3. a popped-out (detached) rack still opens its "+" menu in OS windows of their own where it
#      runs past the rack's small window (popups of detached windows may stick out).
# Where the platform cannot create windows outside the main one the test is skipped.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k72_popups_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# run(<tag> <frames> <script>): a fresh config ${OUT}/cfg_<tag> (HOME inside it).
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

# The main window inside a larger monitor: 240 px of it to the left, 480 to the right.
set(setup "2 vmonitors -240 -120 2160 1140
6 dock \"Channel rack\" Mixer
10 focus \"Channel rack\"
")
run(geo 24 "${setup}20 rack\n22 viewports\n")
if(out MATCHES "available 0")
  string(REGEX MATCH "unavailable: [^\n]*" why "${out}")
  message(STATUS "SKIP: windows cannot be detached on this platform (${why})")
  return()
endif()
if(NOT out MATCHES " addbutton ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* ([0-9]+)")
  fail("no rack add button reported")
endif()
math(EXPR bx "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
math(EXPR by "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
if(bx LESS 1300)
  fail("the docked rack's + button should be near the main window's right edge (x ${bx})")
endif()
ok("rack docked at the right edge, its + button at ${bx} ${by}")

# Open the menu, then move the pointer down its items (each but the last a submenu of
# instruments) and dump every few frames.
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

# Every popup line of every dump: in the main window, inside its rectangle.
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
  if(NOT vp EQUAL 0)
    fail("menu: popup ${name} should be in the main window, not in an OS window of its own (viewport ${vp}, ${x} ${y} to ${r} ${b})")
  endif()
  if(x LESS 0 OR y LESS 0 OR r GREATER 1440 OR b GREATER 900)
    fail("menu: popup ${name} runs past the main window (${x} ${y} to ${r} ${b})")
  endif()
endforeach()
if(subs LESS 4)
  fail("menu: the instrument submenus should have opened while the pointer went down the list (${subs} lines)")
endif()
string(REGEX MATCHALL "popup-windows [0-9]+" made "${out}")
list(GET made -1 last)
if(NOT last STREQUAL "popup-windows 0")
  fail("menu: no OS window should be made for the main window's menus (${last})")
endif()
ok("the + menu and ${subs} submenu frames stay inside the main window; no OS window made for them")

# 2. An instrument from a submenu is still added: the first item of the first submenu (as it
# was with the pointer on the first item, frame 36).
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
56 viewports
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
  fail("add: an instrument picked from the submenu should add a channel (${nBefore} -> ${nAfter})")
endif()
ok("an instrument picked from the docked rack's submenu is added (${nBefore} -> ${nAfter} channels)")

# 3. A popped-out rack (a small window on the monitor's right part): its menu may still stick out
#    (where popups of detached windows may: "popupsin off", the macOS and Windows default).
run(detached 60 "2 vmonitors -240 -120 2160 1140
6 detach \"Channel rack\" 1500 100 300 200
20 rack
22 viewports
")
if(NOT out MATCHES " addbutton ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* ([0-9]+)")
  fail("detached: no rack add button reported")
endif()
math(EXPR dx "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
math(EXPR dy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
run(detached 50 "2 vmonitors -240 -120 2160 1140
3 popupsin off
6 detach \"Channel rack\" 1500 100 300 200
20 move ${dx} ${dy}
22 down 0
23 up 0
40 viewports
")
if(NOT out MATCHES "window \"Channel rack\" pos [-0-9.]+ [-0-9.]+ size [0-9.]+ [0-9.]+ viewport ([1-9][0-9]*)")
  fail("detached: the rack should be popped out")
endif()
set(rackVp "${CMAKE_MATCH_1}")
if(NOT out MATCHES "  popup \"[^\"]*\" pos [-0-9.]+ [-0-9.]+ size [0-9.]+ [0-9.]+ viewport ([0-9]+)")
  fail("detached: the popped-out rack's + menu did not open")
endif()
set(menuVp "${CMAKE_MATCH_1}")
string(REGEX MATCHALL "popup-windows [0-9]+" made "${out}")
list(GET made -1 last)
if(menuVp EQUAL 0 OR menuVp EQUAL rackVp OR last STREQUAL "popup-windows 0")
  fail("detached: the popped-out rack's menu should get an OS window where it runs past the rack (viewport ${menuVp}, rack ${rackVp}, ${last})")
endif()
ok("a popped-out rack's menu still opens outside its small window")
