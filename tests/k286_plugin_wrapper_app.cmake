# Gridline - K-286: the plugin window's own frame around a built-in editor, in the real app (hidden,
# isolated config folders and HOME, SDL's dummy audio driver, the plugin window size at its default):
#   1. an effect window's bypass button and mix knob end flush with the window's right edge (the
#      editor's), also when they wrap onto their own row (a small effect) and in the Smooth layout;
#   2. an editor narrower than its window (the window's own rows need more room) sits in the middle;
#   3. a factory preset picked from the window menu shows in the header and the header steps on from
#      it; bypass from the header is one undo step;
#   4. moved, closed and opened again, the window comes back where it was;
#   5. a click in the editor gives it the keyboard.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k286_plugin_wrapper_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
unset(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE})  # the real plugin window size (75 %)
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag layout plugin frames script)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --layout ${layout} --size 1440x900
                          --ui-scale 1 --frames ${frames} --dev-input "${OUT}/${tag}.txt" --plugin ${plugin}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.out" "${o}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL ${tag}: gridline-app ended with ${rc}\n${o}\n${err}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
endfunction()

# |a - b| <= tol (decimal strings, compared in hundredths)
function(near what a b tol)
  math(EXPR d "(${a}) - (${b})")
  if(d LESS 0)
    math(EXPR d "0 - ${d}")
  endif()
  if(d GREATER ${tol})
    fail("${what}: ${a} vs ${b}")
  endif()
endfunction()

# hdrmix(<tag>): the mix knob's box ends 1 px from the window's right edge (its ring's edge at it)
function(mixflush tag)
  if(NOT out MATCHES "hdrmix frame [0-9]+: drawn [0-9]+ knob ([-0-9.]+),([-0-9.]+)-([-0-9.]+),([-0-9.]+) content right ([-0-9.]+) gap (-?[0-9]+)\\.([0-9])")
    fail("${tag}: no hdrmix line")
  endif()
  # 1 px: the knob's ring is anti-aliased one pixel past its box, and must not touch the window's edge
  if(NOT CMAKE_MATCH_6 STREQUAL "1" OR NOT CMAKE_MATCH_7 STREQUAL "0")
    fail("${tag}: the mix knob is ${CMAKE_MATCH_6}.${CMAKE_MATCH_7} px from the window's right edge")
  endif()
  message(STATUS "ok - ${tag}: the bypass and mix pair ends at the window's right edge")
endfunction()

# 1. flush right
run(reverb studio gl.reverb 40 "32 hdrmix\n32 edzoom fxeditor\n34 quit discard\n")
mixflush("Studio, Reverb")
string(REGEX MATCH "hdrmix frame [0-9]+: drawn [0-9]+ knob ([-0-9.]+),([-0-9.]+)-([-0-9.]+),([-0-9.]+)" _ "${out}")
set(kx0 ${CMAKE_MATCH_1})
set(ky0 ${CMAKE_MATCH_2})
set(ky1 ${CMAKE_MATCH_4})
string(REGEX MATCH "edzoom fxeditor: type gl.reverb zoom [0-9.]+ design [0-9]+x[0-9]+ origin ([-0-9]+)\\.[0-9],([-0-9]+)\\.[0-9] window ([-0-9]+)\\.[0-9],([-0-9]+)\\.[0-9] ([0-9]+)\\.[0-9]x([0-9]+)" _ "${out}")
if(NOT CMAKE_MATCH_0)
  fail("Reverb: no edzoom line")
endif()
set(ox ${CMAKE_MATCH_1})
set(oy ${CMAKE_MATCH_2})
set(wx ${CMAKE_MATCH_3})
set(wy ${CMAKE_MATCH_4})
set(ww ${CMAKE_MATCH_5})
string(REGEX REPLACE "\\..*" "" kx0 "${kx0}")
string(REGEX REPLACE "\\..*" "" ky0 "${ky0}")
string(REGEX REPLACE "\\..*" "" ky1 "${ky1}")
math(EXPR rowY "(${ky0} + ${ky1}) / 2")

run(mini studio gl.minisieve 40 "32 hdrmix\n32 edzoom fxeditor\n34 quit discard\n")
mixflush("Studio, a small effect (the pair on its own row)")
# 2. centred editor
if(NOT out MATCHES "edregion fxeditor: type gl.minisieve width ([0-9]+) left (-?[0-9]+) right (-?[0-9]+)")
  fail("small effect: no edregion line")
endif()
if(CMAKE_MATCH_2 LESS 4)
  fail("small effect: the window is no wider than its editor (left ${CMAKE_MATCH_2}): the case is not covered")
endif()
near("small effect: room left and right of the editor" ${CMAKE_MATCH_2} ${CMAKE_MATCH_3} 1)
message(STATUS "ok - a small effect's editor sits in the middle of its wider window (${CMAKE_MATCH_2} / ${CMAKE_MATCH_3} px)")

run(smooth logic gl.minisieve 40 "32 hdrmix\n34 quit discard\n")
mixflush("Smooth, a small effect (the mix knob on its own row)")

# (0.7.8: the Sampler's settings are the Audio clip window's now; Finger Bass is an instrument whose
# editor is narrower than its channel window's rows too.)
run(chan studio gl.fingerbass 40 "32 edzoom channelsettings\n34 quit discard\n")
if(NOT out MATCHES "edregion channelsettings: type gl.fingerbass width ([0-9]+) left (-?[0-9]+) right (-?[0-9]+)")
  fail("Finger Bass: no edregion line")
endif()
if(CMAKE_MATCH_2 LESS 4)
  fail("Finger Bass: the channel window is no wider than its editor (left ${CMAKE_MATCH_2}): the case is not covered")
endif()
near("Finger Bass: room left and right of the editor" ${CMAKE_MATCH_2} ${CMAKE_MATCH_3} 1)
message(STATUS "ok - Finger Bass's editor sits in the middle of its channel window (${CMAKE_MATCH_2} / ${CMAKE_MATCH_3} px)")

# 3 to 5, on the Reverb window found above (the menu button at the row's left, the bypass button
# left of the mix knob, the title bar, its close button 17 px from the right).
math(EXPR menuX "${ox} + 11")
math(EXPR presetsX "${menuX} + 55")
math(EXPR presetsY "${rowY} + 43")
math(EXPR viaX "${menuX} + 159")
math(EXPR hallX "${menuX} + 232")
math(EXPR hallY "${rowY} + 84")
math(EXPR bypassX "${kx0} - 6 - 38")
math(EXPR titleX "${wx} + 200")
math(EXPR titleY "${wy} + 12")
math(EXPR movedX "${wx} - 60")
math(EXPR movedY "${wy} - 30")
math(EXPR dragX1 "${titleX} - 20")
math(EXPR dragY1 "${titleY} - 10")
math(EXPR dragX2 "${titleX} - 60")
math(EXPR dragY2 "${titleY} - 30")
math(EXPR closeX "${movedX} + ${ww} - 17")
math(EXPR closeY "${movedY} + 12")
math(EXPR editX "${movedX} + (${ox} - ${wx}) + 40")
math(EXPR editY "${movedY} + (${oy} - ${wy}) + 60")
run(act studio gl.reverb 110 "30 hdrpreset
31 move ${menuX} ${rowY}
32 down 0
33 up 0
36 move ${presetsX} ${presetsY}
37 move ${presetsX} ${presetsY}
45 move ${viaX} ${hallY}
46 move ${hallX} ${hallY}
48 down 0
49 up 0
53 hdrpreset
55 move ${bypassX} ${rowY}
56 down 0
57 up 0
60 pluginview
61 undo
64 pluginview
66 move ${titleX} ${titleY}
67 down 0
68 move ${dragX1} ${dragY1}
69 move ${dragX2} ${dragY2}
70 up 0
74 edzoom fxeditor
76 move ${closeX} ${closeY}
77 down 0
78 up 0
82 edzoom fxeditor
84 fxopen 1 9
90 edzoom fxeditor
92 move ${editX} ${editY}
93 down 0
94 up 0
97 zorder
99 quit discard
")
if(NOT out MATCHES "hdrpreset frame 30: \"Default\" index -1")
  fail("the header does not start on Default")
endif()
if(NOT out MATCHES "hdrpreset frame 53: \"Hall\" index 2 undo 2")
  fail("a preset picked from the window menu (Presets > Hall) does not show in the header, or the header does not step on from it")
endif()
message(STATUS "ok - a preset picked from the window menu shows in the header (and its place in the list)")
if(NOT out MATCHES "pluginview frame 60: [^\n]* undo 3 \"bypass effect\"")
  fail("the header's bypass button is not one undo step")
endif()
if(NOT out MATCHES "pluginview frame 64: [^\n]* undo 2 \"load preset\"")
  fail("undo does not take the bypass back")
endif()
message(STATUS "ok - bypass from the header, and its undo")
if(NOT out MATCHES "edzoom fxeditor: type gl.reverb [^\n]* window ${movedX}\\.0,${movedY}\\.0 ")
  fail("the window did not move to ${movedX},${movedY} by its title bar")
endif()
string(REGEX MATCHALL "edzoom fxeditor: [^\n]*" lines "${out}")
list(GET lines 1 closed)
if(NOT closed MATCHES "not drawn")
  fail("the close button did not close the window (${closed})")
endif()
list(GET lines 2 reopened)
if(NOT reopened MATCHES "window ${movedX}\\.0,${movedY}\\.0 ${ww}\\.0x")
  fail("closed and opened again, the window is not where it was (${movedX},${movedY}, width ${ww}): ${reopened}")
endif()
message(STATUS "ok - moved, closed and opened again, the window comes back where it was, at its size")
if(NOT out MATCHES "zorder frame 97: [^\n]*focused-root \"fxeditor\"")
  fail("a click in the editor does not give its window the keyboard")
endif()
message(STATUS "ok - a click in the editor gives its window the keyboard")
