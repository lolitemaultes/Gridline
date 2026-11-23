# Gridline 0.7.8 - installed plugins' interfaces inside Gridline, in the real app (hidden, an isolated
# config folder and HOME, SDL's dummy audio driver), with the in-tree CLAP test effect's interface:
#   1. by default an installed effect's interface opens inside the main window, in its effect window
#      (no window of its own), and hides while a dialog is over it, then shows again;
#      a tooltip over it is cut out of it;
#   2. closed and opened again many times: one interface at most, never a window of its own;
#   3. the window menu's choice puts it in a window of its own (kept for the plugin) and the bar's
#      button brings it back inside;
#   4. popped out with its window's pop-out button it moves into that window, and back in;
#   5. keys: Space typed into a view of the interface that passes keys on reaches Gridline (the
#      transport), a text field of the interface keeps it; an SDL key typed while the interface's
#      text field has the keyboard is not Gridline's;
#      an interface never keeps the keyboard it took without a click;
#   6. the same in the Clips, Smooth and Tracker layouts;
#   7. "In their own windows" (the setting) opens it in a window of its own, as before.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DCLAP=<bundle> -DOUT=<dir> -P plugin_dock_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_DEV_PLUGIN_WINDOWS} "1")
set(ENV{GRIDLINE_TEST_GAIN_GUI} "1")  # the CLAP test gain gets an interface
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1280x800
                          --dev-input "${OUT}/${tag}.txt" --plugin gl.patcher ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.out" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL ${tag}: gridline-app ended with ${rc}\n${out}\n${err}")
  endif()
  set(out "${out}\n${err}" PARENT_SCOPE)
endfunction()
function(must tag regex what)
  if(NOT out MATCHES "${regex}")
    message(FATAL_ERROR "FAIL (${tag}): ${what}\n${out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()
function(mustnot tag regex what)
  if(out MATCHES "${regex}")
    message(FATAL_ERROR "FAIL (${tag}): ${what}\n${out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

if(NOT APPLE)
  message(STATUS "SKIP: the CLAP test effect has an interface on macOS only")
  return()
endif()

# 1. Inside by default; a dialog over it hides it, closing the dialog shows it again.
unset(ENV{GRIDLINE_PLUGIN_WINDOWS})
run(inside "10 junction scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
40 plugwin info
50 plugwin dialog
70 plugwin info
74 plugwin nodialog
100 plugwin info
110 quit discard
")
must(inside "plugwin frame 40: 1 windows\n  plugwin [^\n]* size 0x0 [^\n]*\n  plugwin [^\n]* inside 1 host 0x[0-9a-f]+ at [0-9]+,[0-9]+ size [0-9]+x[0-9]+ visible [0-9]+,[0-9]+ [1-9][0-9]*x[1-9][0-9]* holes 0"
     "the effect's interface opens inside the main window, shown, with no window of its own")
must(inside "plugwin frame 70: [^\n]*\n[^\n]*\n  plugwin [^\n]* inside 1 [^\n]* visible 0,0 0x0 " "a dialog over it: the interface hides")
must(inside "plugwin frame 100: [^\n]*\n[^\n]*\n  plugwin [^\n]* inside 1 [^\n]* visible [0-9]+,[0-9]+ [1-9][0-9]*x[1-9]" "the dialog closed: it shows again")

# 1b. A tooltip over its middle: cut out of it (a hole), not hidden; gone again after.
run(cover "10 junction scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
40 plugwin cover 30
50 plugwin info
90 plugwin info
100 quit discard
")
must(cover "plugwin frame 50: [^\n]*\n[^\n]*\n  plugwin [^\n]* inside 1 [^\n]* visible [0-9]+,[0-9]+ [1-9][0-9]*x[1-9][0-9]* holes 1 " "a tooltip over it is cut out of it, the rest still shows")
must(cover "plugwin frame 90: [^\n]*\n[^\n]*\n  plugwin [^\n]* inside 1 [^\n]* visible [0-9]+,[0-9]+ [1-9][0-9]*x[1-9][0-9]* holes 0 " "the tooltip gone: whole again")

# 2. Opened and closed again and again.
set(script "10 junction scan clap ${CLAP}\n14 plugwin pick 1 0 clap:org.gridline.test.gain\n")
set(f 40)
foreach(i RANGE 1 10)
  math(EXPR c "${f} + 8")
  math(EXPR o "${f} + 12")
  math(EXPR n "${f} + 30")
  string(APPEND script "${f} plugwin info\n${c} plugwin close\n${o} plugwin fx 1 0\n")
  set(f ${n})
endforeach()
string(APPEND script "${f} plugwin info\n")
math(EXPR q "${f} + 4")
string(APPEND script "${q} quit discard\n")
run(again "${script}")
mustnot(again "plugwin frame [0-9]+: [2-9] windows" "never two interfaces for one effect")
mustnot(again "container 0x[0-9a-f]+ at -?[0-9]+,-?[0-9]+ size [1-9]" "never a window of its own")
must(again "plugwin frame ${f}: 1 windows\n[^\n]*\n  plugwin [^\n]* inside 1 [^\n]* visible [0-9]+,[0-9]+ [1-9]" "open and shown after ten closes and opens")

# 3. Put in a window of its own (its window menu), then back inside (its bar's button).
run(place "10 junction scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
40 plugwin info
44 plugwin place own 1 0
70 plugwin info
74 plugwin place inside 1 0
110 plugwin info
120 quit discard
")
must(place "plugwin frame 70: 1 windows\n  plugwin [^\n]* external 0 window 0x[1-9a-f][0-9a-f]* container 0x[0-9a-f]+ at -?[0-9]+,-?[0-9]+ size [1-9]" "the window menu's choice opens it in a window of its own")
mustnot(place "plugwin frame 70: [^\n]*\n[^\n]*\n  plugwin [^\n]* inside 1" "no interface inside meanwhile")
must(place "plugwin frame 110: 1 windows\n  plugwin [^\n]* size 0x0 [^\n]*\n  plugwin [^\n]* inside 1 [^\n]* visible [0-9]+,[0-9]+ [1-9]" "back inside")
if(NOT EXISTS "${OUT}/gridline/plugin-manager.json")
  message(FATAL_ERROR "FAIL (place): no plugin settings saved")
endif()

# 4. Popped out of the main window with its effect window, then docked back.
run(popout "10 junction scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
40 plugwin info
44 popout fxeditor
90 plugwin info
94 dockback fxeditor
140 plugwin info
150 quit discard
")
if(out MATCHES "plugwin frame 40: 1 windows\n[^\n]*\n  plugwin [^\n]* inside 1 host (0x[0-9a-f]+) ")
  set(mainhost "${CMAKE_MATCH_1}")
else()
  message(FATAL_ERROR "FAIL (popout): not inside at frame 40\n${out}")
endif()
if(out MATCHES "plugwin frame 90: 1 windows\n[^\n]*\n  plugwin [^\n]* inside 1 host (0x[0-9a-f]+) ")
  if("${CMAKE_MATCH_1}" STREQUAL "${mainhost}")
    message(FATAL_ERROR "FAIL (popout): the interface stayed in the main window\n${out}")
  endif()
  message(STATUS "ok - popped out: the interface moved into its window's own window")
else()
  message(STATUS "SKIP: windows cannot be popped out here\n${out}")
  return()
endif()
must(popout "plugwin frame 140: 1 windows\n[^\n]*\n  plugwin [^\n]* inside 1 host ${mainhost} " "docked back: the interface is in the main window again")

# 5. Keys.
run(keys "10 junction scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
40 plugwin playing
42 plugwin dockkey 0 space
60 plugwin playing
62 plugwin dockkey 2 space
80 plugwin playing
82 sdlkeyin main space down
83 sdlkeyin main space up
100 plugwin playing
110 quit discard
")
must(keys "plugwin frame 40: playing 0" "stopped at first")
must(keys "plugwin frame 60: playing 1" "Space in a view of the interface that passes it on reaches Gridline")
must(keys "plugwin frame 80: playing 1" "Space in the interface's text field stays there")
must(keys "plugwin frame 100: playing 1" "a key typed while the interface's text field has the keyboard is not Gridline's")

# 5b. An interface that takes the keyboard on its own (no click) never keeps it: Gridline's keys work.
run(grab "10 junction scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
40 plugwin playing
42 plugwin dockkey 3
50 sdlkeyin main space down
51 sdlkeyin main space up
70 plugwin playing
80 quit discard
")
must(grab "plugwin frame 40: playing 0" "stopped at first")
must(grab "plugwin frame 70: playing 1" "an interface that took the keyboard on its own gives it back: Space reaches Gridline")

# 6. In every layout the effect's interface opens inside, in that layout's effect window.
foreach(lay live logic tracker)
  run(layout-${lay} "10 plugwin scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
60 plugwin info
64 plugwin close
70 plugwin fxtoggle 1 0
100 plugwin info
110 quit discard
" --layout ${lay})
  must(layout-${lay} "plugwin frame 60: 1 windows\n[^\n]* size 0x0 [^\n]*\n  plugwin [^\n]* inside 1 [^\n]* visible [0-9]+,[0-9]+ [1-9]" "layout ${lay}: inside")
  must(layout-${lay} "plugwin frame 100: 1 windows\n[^\n]* size 0x0 [^\n]*\n  plugwin [^\n]* inside 1 [^\n]* visible [0-9]+,[0-9]+ [1-9]" "layout ${lay}: closed and opened again, inside")
endforeach()

# 6b. The header and bar buttons pressed again and again while the song plays (GitHub #46: a
# crash was reported from them): inside Gridline (presets, the window menu's choice, popped out and
# back) and in a window of its own (every bar button, the sandbox switch on and off). The app must
# end cleanly with the interface open.
set(script "10 junction scan clap ${CLAP}\n14 plugwin pick 1 0 clap:org.gridline.test.gain\n30 sdlkeyin main space down\n31 sdlkeyin main space up\n")
set(f 40)
foreach(i RANGE 1 3)
  foreach(c "pickpreset 0" "pickpreset 1" "plugwin place own 1 0" "plugwin bar prev" "plugwin bar next" "plugwin bar preset" "key escape"
            "plugwin bar bypass" "plugwin bar bypass" "plugwin bar settings" "plugwin bar settings" "plugwin bar sandbox" "plugwin bar sandbox"
            "plugwin bar dockin" "popout fxeditor" "dockback fxeditor" "plugwin info")
    string(APPEND script "${f} ${c}\n")
    math(EXPR f "${f} + 40")
  endforeach()
endforeach()
string(APPEND script "${f} plugwin playing\n")
math(EXPR f "${f} + 60")
string(APPEND script "${f} plugwin info\n")
math(EXPR q "${f} + 4")
string(APPEND script "${q} quit discard\n")
run(buttons "${script}")
must(buttons "plugwin frame ${f}: 1 windows\n[^\n]* size 0x0 [^\n]*\n  plugwin [^\n]* inside 1 [^\n]* visible [0-9]+,[0-9]+ [1-9]" "header and bar buttons pressed again and again while playing: no crash, the interface is inside at the end")
mustnot(buttons "plugwin frame [0-9]+: [2-9] windows" "never two interfaces")

# 7. The setting "In their own windows": as before.
set(ENV{GRIDLINE_PLUGIN_WINDOWS} "own")
run(own "10 junction scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
40 plugwin info
50 quit discard
")
must(own "plugwin frame 40: 1 windows\n  plugwin [^\n]* external 0 window 0x[1-9a-f][0-9a-f]* container 0x[0-9a-f]+ at -?[0-9]+,-?[0-9]+ size [1-9]" "In their own windows: a window of its own, as before")
mustnot(own "inside 1" "In their own windows: nothing inside")
unset(ENV{GRIDLINE_PLUGIN_WINDOWS})
