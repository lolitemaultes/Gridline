# Gridline 0.7.8 - installed plugins in the real app (hidden, an isolated config folder and HOME,
# SDL's dummy audio driver), with the in-tree CLAP test plugins:
#   1. an effect picked into a mixer slot draws "loading" (never "could not be loaded") until its
#      instance is up, then its interface;
#   2. a notice and a menu that open over a plugin window are drawn in front of it: each is its own
#      top-most window, stacked above the plugin window (macOS: a higher window level);
#   3. an effect moved up the chain keeps its instance and its window follows it.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DCLAP=<bundle> -DOUT=<dir> -P plugin_fixes_078_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_PLUGIN_WINDOWS} "own")  # 0.7.8: installed plugins in windows of their own, as before (inside Gridline: plugin_dock_app)
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_DEV_PLUGIN_WINDOWS} "1")
set(ENV{GRIDLINE_TEST_GAIN_GUI} "1")
set(ENV{GRIDLINE_DEV_SHOW_OVERLAYS} "1")  # lifted menus and notices really show in hidden runs (their windows are checked)  # the CLAP test gain gets an interface (an effect window)
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1280x800
                          --dev-input "${OUT}/${tag}.txt" --plugin gl.patcher
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

# 1. Loading, not failed.
run(loading "10 junction scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
60 plugwin drawn
70 quit discard
")
must(loading "plugwin frame 60: drawn [^\n]*\\[[0-9]+ loading\\]" "the panel shows the effect loading while its instance is made")
mustnot(loading "plugwin frame 60: drawn [^\n]*failed" "it never shows 'could not be loaded' on the way")

# 3. A move keeps the instance, and its window follows.
run(follow "10 junction scan clap ${CLAP}
14 plugwin pick 1 1 clap:org.gridline.test.gain
40 plugwin info
50 plugwin step 1 1 -1
80 plugwin info
90 quit discard
")
must(follow "plugwin frame 40: 1 windows" "the effect's window opens")
must(follow "plugwin frame 80: 1 windows" "its window stays open after the move")
if(NOT out MATCHES "plugwin frame 40: [^\n]*\n[^\n]*\n  plugwin [^\n]* slot [0-9]+/1 [^\n]* instance (0x[0-9a-f]+)")
  message(FATAL_ERROR "FAIL (follow): no instance line at frame 40\n${out}")
endif()
set(inst "${CMAKE_MATCH_1}")
must(follow "plugwin frame 80: [^\n]*\n[^\n]*\n  plugwin [^\n]* slot [0-9]+/0 [^\n]* instance ${inst}\n" "the window now shows slot 0 and the same instance: none was made again")

# 2. Overlays in front of a plugin window.
if(APPLE)
  run(overlays "10 junction scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
40 plugwin info
44 plugwin moveto 940 520
60 plugwin notify \"A notice over a plugin window\"
80 overlays
82 plugwin info
90 quit discard
")
  if(out MATCHES "plugwin frame 40: 0 windows")
    message(STATUS "SKIP: no plugin window on this machine\n${out}")
    return()
  endif()
  must(overlays "overlays frame 80: covers [1-9]" "the plugin window is known as a cover")
  must(overlays "overlay \"##notecard[0-9]+\"[^\n]* lifted 1 topmost 1 level ([1-9]|[1-9][0-9]+)\n" "the notice is its own top-most window")

  # A dialog under a plugin window: the window steps aside while it is open, and comes back after.
  run(dialog "10 junction scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
40 plugwin moveto 400 200
50 plugwin dialog
80 plugwin info
84 plugwin nodialog
120 plugwin info
130 quit discard
")
  must(dialog "plugwin frame 80: [^\n]*\n[^\n]*\n  plugwin [^\n]* shown 0 aside 4 " "a dialog under the plugin window: it steps aside")
  must(dialog "plugwin frame 120: [^\n]*\n[^\n]*\n  plugwin [^\n]* shown 1 aside 0 " "and comes back when the dialog closes")
endif()
