# Gridline - the Clips layout's detail spot (0.7.8): the clip view (the selected clip's editor) and the
# device view (the selected track's instrument and effects) share one place under the main view, one
# shown at a time. Runs the app hidden (isolated config and HOME, dummy audio, fixed frame time) in the
# Clips layout on a copy of the test song with two scenes in the clip grid, and clicks with the pointer:
#   1. a new run shows the device view; a click on a clip in the grid shows the clip view in the same
#      rectangle (the device view goes), the status bar's Clip button lit;
#   2. a click on the clip's track title shows the device view in that rectangle (Devices lit);
#   3. a click on the same clip again (its selection unchanged) brings the clip view back;
#   4. Shift+Tab swaps to the device view and gives it the keys; again: the clip view with the keys; the
#      main view stays the clip grid (plain Tab is the main view's switch in Clips);
#   5. the status bar's Devices and Clip buttons switch the spot and give it the keys; the lit one hides
#      the spot, and Shift+Tab brings it back with the other view;
#   6. a double-click on a clip shows the clip view with the keys;
#   7. a click on an empty slot shows that track's devices;
#   8. a layout saved while the two could stack (0.7.7) opens with one of them only;
#   9. a Set saved with the clip view in front opens with it again.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> [-DSHOTS=<dir>] -P k078_clips_detail_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
file(READ "${DEMO}" demo)  # a copy: the fixture is never opened (or saved over) in place
string(REGEX REPLACE "\"playlist\": \\[" "\"live\": {\"areaEnd\": 3072, \"view\": \"grid\"}, \"playlist\": [" demo "${demo}")
string(REGEX REPLACE "\"version\": *[0-9]+" "\"version\": 10" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\n${out}")
endfunction()
function(ok what)
  message(STATUS "ok - ${what}")
endfunction()

# run <tag> <script> [extra settings json]: one hidden run with its own config.
function(run tag script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  set(extra "")
  if(ARGC GREATER 2)
    set(extra ", ${ARGV2}")
  endif()
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"live\", \"keys.profile\": \"live\", \"ui.menuStyle\": \"live\"${extra}}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900
                          --layout-dev --layout live --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.log" "${o}\n---- stderr ----\n${err}")
  set(out "${o}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL ${tag}: gridline-app failed (${rc}): ${err}\n${o}")
  endif()
endfunction()

# detail <frame>: the livedetail report: d_clip d_dev (shown), d_rect (the shown view's rect), d_litclip
# d_litdev (the status bar buttons), d_grid (the clip grid in front), d_focus (the window with the keys).
function(detail f)
  if(NOT out MATCHES "livedetail frame ${f}: clip ([01]) device ([01]) rect ([0-9-]+ [0-9-]+ [0-9-]+ [0-9-]+) lit clip ([01]) device ([01]) grid ([01]) focus \"([^\"]*)\"")
    fail("no livedetail report at frame ${f}")
  endif()
  set(d_clip ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(d_dev ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(d_rect "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(d_litclip ${CMAKE_MATCH_4} PARENT_SCOPE)
  set(d_litdev ${CMAKE_MATCH_5} PARENT_SCOPE)
  set(d_grid ${CMAKE_MATCH_6} PARENT_SCOPE)
  set(d_focus "${CMAKE_MATCH_7}" PARENT_SCOPE)
endfunction()
# click <var> <frame> <x> <y> [double]: a press and release there (a second one two frames on: a double-click).
function(click var f x y)
  math(EXPR f1 "${f} + 1")
  math(EXPR f2 "${f} + 2")
  set(s "${f} sdlmove ${x} ${y}\n${f1} sdldown 0\n${f2} sdlup 0\n")
  if(ARGC GREATER 4)
    math(EXPR f3 "${f} + 3")
    math(EXPR f4 "${f} + 4")
    string(APPEND s "${f3} sdldown 0\n${f4} sdlup 0\n")
  endif()
  set(${var} "${s}" PARENT_SCOPE)
endfunction()

# The grid's geometry: slot centres and the track titles.
run(geo "4 typing off\n10 livegrid\n14 quit discard\n")
foreach(cell "0 1" "1 1" "2 0")
  string(REPLACE " " ";" tk "${cell}")
  list(GET tk 0 t)
  list(GET tk 1 k)
  if(NOT out MATCHES "\n  slot ${t} ${k} x ([0-9]+)[.0-9]* y ([0-9]+)[.0-9]* clip ([0-9]+)")
    fail("no slot ${t} ${k} in the grid")
  endif()
  set(x${t}${k} ${CMAKE_MATCH_1})
  set(y${t}${k} ${CMAKE_MATCH_2})
  set(c${t}${k} ${CMAKE_MATCH_3})
endforeach()
if(NOT c01 EQUAL 35 OR NOT c20 EQUAL 0)
  fail("the fixture's slots changed (${c01} ${c20})")
endif()
if(NOT out MATCHES "livegrid title 0 \"[^\"]*\" ([0-9]+) ([0-9]+)")
  fail("no title of track 0")
endif()
set(tx0 ${CMAKE_MATCH_1})
set(ty0 ${CMAKE_MATCH_2})

# ---- 1 to 7 in one run ----
click(c1 30 ${x01} ${y01})
click(c2 60 ${tx0} ${ty0})
click(c3 90 ${x01} ${y01})
click(c6 300 ${x11} ${y11} double)
click(c7 340 ${x20} ${y20})
set(shot1 "")
set(shot2 "")
if(DEFINED SHOTS)
  set(shot1 "52 shot \"${SHOTS}/after_clip.ppm\"\n")
  set(shot2 "82 shot \"${SHOTS}/after_device.ppm\"\n")
endif()
run(main "4 typing off
20 livedetail
${c1}50 livedetail
${shot1}${c2}80 livedetail
${shot2}${c3}110 livedetail
120 sdlkey tab down shift
121 sdlkey tab up
150 livedetail
160 sdlkey tab down shift
161 sdlkey tab up
190 livedetail
200 sdlat sb_dev
202 sdldown 0
203 sdlup 0
230 livedetail
240 sdlat sb_clip
242 sdldown 0
243 sdlup 0
270 livedetail
272 sdlat sb_clip
274 sdldown 0
275 sdlup 0
285 livedetail
287 sdlkey tab down shift
288 sdlkey tab up
296 livedetail
${c6}330 livedetail
${c7}370 livedetail
380 quit discard
")
detail(20)
if(NOT d_dev EQUAL 1 OR NOT d_clip EQUAL 0 OR NOT d_litdev EQUAL 1 OR NOT d_litclip EQUAL 0)
  fail("a new run shows the device view alone (clip ${d_clip} device ${d_dev}, lit ${d_litclip} ${d_litdev})")
endif()
set(spot "${d_rect}")
ok("a new run shows the device view (${spot})")
detail(50)
if(NOT d_clip EQUAL 1 OR NOT d_dev EQUAL 0 OR NOT d_rect STREQUAL spot OR NOT d_litclip EQUAL 1 OR NOT d_litdev EQUAL 0)
  fail("1. a clip clicked: the clip view alone, in the device view's place (clip ${d_clip} device ${d_dev} rect ${d_rect} vs ${spot}, lit ${d_litclip} ${d_litdev})")
endif()
ok("1. a clip clicked shows the clip view in the same rectangle; the Clip button is lit")
detail(80)
if(NOT d_dev EQUAL 1 OR NOT d_clip EQUAL 0 OR NOT d_rect STREQUAL spot OR NOT d_litdev EQUAL 1 OR NOT d_litclip EQUAL 0)
  fail("2. the track title clicked: the device view in the same place (clip ${d_clip} device ${d_dev} rect ${d_rect}, lit ${d_litclip} ${d_litdev})")
endif()
ok("2. the clip's track title shows the device view in the same rectangle; Devices is lit")
detail(110)
if(NOT d_clip EQUAL 1 OR NOT d_dev EQUAL 0 OR NOT d_rect STREQUAL spot)
  fail("3. the same clip clicked again: the clip view (clip ${d_clip} device ${d_dev} rect ${d_rect})")
endif()
ok("3. a click on the clip selected already brings the clip view back")
detail(150)
if(NOT d_dev EQUAL 1 OR NOT d_clip EQUAL 0 OR NOT d_rect STREQUAL spot OR NOT d_litdev EQUAL 1 OR NOT d_focus MATCHES "Device chain" OR NOT d_grid EQUAL 1)
  fail("4. Shift+Tab: the device view with the keys, the clip grid still in front (clip ${d_clip} device ${d_dev} rect ${d_rect} focus ${d_focus} grid ${d_grid})")
endif()
ok("4. Shift+Tab swaps to the device view in the same place and gives it the keys (${d_focus})")
detail(190)
if(NOT d_clip EQUAL 1 OR NOT d_dev EQUAL 0 OR NOT d_rect STREQUAL spot OR NOT d_litclip EQUAL 1 OR NOT d_focus MATCHES "Piano roll" OR NOT d_grid EQUAL 1)
  fail("4. Shift+Tab again: the clip view with the keys (clip ${d_clip} device ${d_dev} rect ${d_rect} focus ${d_focus} grid ${d_grid})")
endif()
ok("4. Shift+Tab again: the clip view with the keys (${d_focus}); the main view stays")
detail(230)
if(NOT d_dev EQUAL 1 OR NOT d_clip EQUAL 0 OR NOT d_rect STREQUAL spot OR NOT d_litdev EQUAL 1 OR NOT d_focus MATCHES "Device chain")
  fail("5. the Devices button: the device view with the keys (clip ${d_clip} device ${d_dev} rect ${d_rect} focus ${d_focus})")
endif()
ok("5. the Devices button switches the spot and gives it the keys")
detail(270)
if(NOT d_clip EQUAL 1 OR NOT d_dev EQUAL 0 OR NOT d_rect STREQUAL spot OR NOT d_litclip EQUAL 1 OR NOT d_focus MATCHES "Piano roll")
  fail("5. the Clip button: the clip view with the keys (clip ${d_clip} device ${d_dev} rect ${d_rect} focus ${d_focus})")
endif()
ok("5. the Clip button switches back")
detail(285)
if(NOT d_clip EQUAL 0 OR NOT d_dev EQUAL 0 OR NOT d_litclip EQUAL 0 OR NOT d_litdev EQUAL 0)
  fail("5. the lit Clip button hides the spot (clip ${d_clip} device ${d_dev})")
endif()
detail(296)
if(NOT d_dev EQUAL 1 OR NOT d_clip EQUAL 0 OR NOT d_rect STREQUAL spot)
  fail("5. Shift+Tab brings the hidden spot back with the other view (clip ${d_clip} device ${d_dev} rect ${d_rect})")
endif()
ok("5. the lit button hides the spot; Shift+Tab brings it back with the other view")
detail(330)
if(NOT d_clip EQUAL 1 OR NOT d_dev EQUAL 0 OR NOT d_rect STREQUAL spot OR NOT d_focus MATCHES "Piano roll")
  fail("6. a double-click on a clip: the clip view with the keys (clip ${d_clip} device ${d_dev} rect ${d_rect} focus ${d_focus})")
endif()
ok("6. a double-click on a clip shows the clip view with the keys")
detail(370)
if(NOT d_dev EQUAL 1 OR NOT d_clip EQUAL 0 OR NOT d_rect STREQUAL spot)
  fail("7. an empty slot: the track's devices (clip ${d_clip} device ${d_dev} rect ${d_rect})")
endif()
ok("7. a click on an empty slot shows that track's devices")

# ---- 8. a 0.7.7 layout with both shown ----
run(stacked "4 typing off\n30 livedetail\n34 quit discard\n"
    "\"layout.live.state\": \"{\\\"shown\\\":[\\\"live.clip\\\"]}\", \"ui.layoutVersion.live\": 9999, \"ui.layoutVersion.live.shell\": 9999")
detail(30)
if(NOT (d_clip EQUAL 1 AND d_dev EQUAL 0) AND NOT (d_clip EQUAL 0 AND d_dev EQUAL 1))
  fail("8. a layout saved with both shown opens with one of them (clip ${d_clip} device ${d_dev})")
endif()
ok("8. a 0.7.7 layout with both views shown opens with one (clip ${d_clip} device ${d_dev})")

# ---- 9. a Set saved with the clip view opens with it ----
run(perset "4 typing off\n30 livedetail\n34 quit discard\n" "\"clips.clipViewSets\": \"${OUT}/song.gridline.json\"")
detail(30)
if(NOT d_clip EQUAL 1 OR NOT d_dev EQUAL 0 OR NOT d_rect STREQUAL spot)
  fail("9. a Set saved with the clip view opens with it (clip ${d_clip} device ${d_dev} rect ${d_rect})")
endif()
ok("9. a Set saved with the clip view in front opens with it")
message(STATUS "ok - the Clips clip view and device view share one spot; Shift+Tab, the buttons and the selection switch it")
