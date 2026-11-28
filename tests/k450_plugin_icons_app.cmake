# Gridline: every built-in plugin shows its own icon wherever a plugin is listed or opened.
# Hidden app runs (isolated config and home) of the test song in the four layouts, Dark and Light,
# at 100% and 200%, with an instrument window and an effect window open, the mixer's slot view,
# the browser's plugin lists, the command palette and the plugin picker; plugin windows docked as
# tabs (none selected), a second effect window in Smooth, the Device chain's chips, the Tracker's
# effect menu and its instrument menu's search; the picker's header (no part overlapping another
# or outside it, also at the smallest main window). Each run asks the app
# (dev input "plugicons") how many plugin icons each surface drew, how many times a surface fell
# back to a generic icon, and how many of those fallbacks were for a plugin that has its own icon
# (a mistake). Every surface must make no mistakes, and every surface a layout shows must draw icons.
#   cmake -DAPP=<gridline-app> -DDEMO=<song.gridline.json> -DOUT=<dir> -P k450_plugin_icons_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_BROWSER_EXPAND} "Plugin database,Generators,Effects,Instruments,Audio effects,Synth,Drums,Sampler,Dynamics")
set(ENV{GRIDLINE_PICKER_GLYPHS} "1")
set(surfaces browser picker rack window mixer deviceband smooth tracker palette drag menu)

# SEED (optional, set before the call): the run's settings.json values.
function(run tag script)
  set(d "${OUT}/${tag}")
  file(MAKE_DIRECTORY "${d}/cfg/gridline" "${d}/home")
  if(SEED)
    file(WRITE "${d}/cfg/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {${SEED}}}\n")
  endif()
  set(ENV{GRIDLINE_CONFIG_HOME} "${d}/cfg")
  set(ENV{HOME} "${d}/home")
  file(WRITE "${d}/input.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --size 1440x900 --frames 400
                          --screenshot "${d}/end.ppm" --dev-input "${d}/input.txt" "${OUT}/fixture.gridline.json" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${d}/run.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (${tag}): gridline-app exited with ${rc}:\n${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# Reads "plugicons frame <f>: browser o/g/m picker o/g/m ..." into c_<surface>_own / _mistakes.
function(counts tag f)
  string(REGEX MATCH "plugicons frame ${f}:[^\n]*" line "${${tag}_out}")
  if(line STREQUAL "")
    message(FATAL_ERROR "FAIL (${tag}): no plugicons report at frame ${f}. Output:\n${${tag}_out}")
  endif()
  foreach(s IN LISTS surfaces)
    if(NOT line MATCHES " ${s} ([0-9]+)/([0-9]+)/([0-9]+)")
      message(FATAL_ERROR "FAIL (${tag}): no ${s} count in: ${line}")
    endif()
    set(c_${s}_own ${CMAKE_MATCH_1} PARENT_SCOPE)
    set(c_${s}_mistakes ${CMAKE_MATCH_3} PARENT_SCOPE)
  endforeach()
  message(STATUS "${tag}: ${line}")
endfunction()

set(checks 0)
function(expect tag cond what)  # cond: a condition as a list ("a;EQUAL;0")
  math(EXPR n "${checks} + 1")
  set(checks ${n} PARENT_SCOPE)
  if(NOT ${cond})
    message(FATAL_ERROR "FAIL (${tag}): ${what}")
  endif()
endfunction()

# No surface showed the generic icon for a plugin that has its own (the last counts read).
macro(clean tag)
  foreach(s IN LISTS surfaces)
    expect(${tag} "c_${s}_mistakes;EQUAL;0" "${s} showed the generic icon ${c_${s}_mistakes} times for plugins that have their own")
  endforeach()
endmacro()

# The picker's header ("pickerhead"): every part (title, count, search, filter, thumbnail buttons,
# guide, close) inside the window and the header, no two overlapping, at every scale and size.
macro(header tag)
  if(NOT ${tag}_out MATCHES "pickerhead frame [0-9]+: width ([0-9]+) head ([0-9]+) parts ([0-9]+) overlaps ([0-9]+) outside ([0-9]+)")
    message(FATAL_ERROR "FAIL (${tag}): no pickerhead report. Output:\n${${tag}_out}")
  endif()
  set(h_parts ${CMAKE_MATCH_3})
  set(h_overlaps ${CMAKE_MATCH_4})
  set(h_outside ${CMAKE_MATCH_5})
  string(REGEX MATCH "pickerhead frame[^\n]*" h_line "${${tag}_out}")
  message(STATUS "${tag}: ${h_line}")
  expect(${tag} "h_parts;GREATER;5" "the picker's header reported ${h_parts} parts")
  expect(${tag} "h_overlaps;EQUAL;0" "the picker's header has ${h_overlaps} overlapping parts: ${h_line}")
  expect(${tag} "h_outside;EQUAL;0" "the picker's header has ${h_outside} parts outside it: ${h_line}")
endmacro()

# What each layout shows: the surfaces that must draw icons (the window titles and the palette in all).
set(want_studio browser rack mixer)
set(want_live browser deviceband)
set(want_logic smooth)
set(want_tracker tracker)

foreach(theme dark light)
  foreach(scale 1 2)
    foreach(lay studio live logic tracker)
      set(args --layout ${lay} --ui-scale ${scale})
      if(theme STREQUAL "light")
        list(APPEND args --light)
      endif()
      set(tag "${lay}_${theme}_${scale}x")
      run(${tag} "5 toasts clear\n20 plugicons reset\n40 plugicons\n50 palette open\n56 text comp\n70 palette close\n72 plugicons\n80 quit discard\n"
          ${args} --chan 5 --fx 0 0 --mixer-view ext,sel2)
      counts(${tag} 72)
      clean(${tag})
      set(want ${want_${lay}} window palette)
      if(lay STREQUAL "studio" AND scale EQUAL 2)
        list(REMOVE_ITEM want rack)  # at 200% the rack window sits behind the plugin windows
      endif()
      foreach(s IN LISTS want)
        expect(${tag} "c_${s}_own;GREATER;0" "${s} drew no plugin icons")
      endforeach()
      # The surfaces that need a window or menu opened first (20 frames counted: frames 40 to 60).
      if(lay STREQUAL "studio")
        # Plugin windows docked as tabs beside the playlist, neither tab selected: both tabs show
        # their icon every frame (2 a frame); the Device chain's chips.
        set(SEED "\"input.dockPlugins\": \"on\"")
        run(${tag}_more "5 toasts clear\n10 openwin Device chain\n40 plugicons reset\n60 plugicons\n66 quit discard\n" ${args} --chan 5 --fx 0 0)
        unset(SEED)
        counts(${tag}_more 60)
        clean(${tag}_more)
        expect(${tag}_more "c_window_own;GREATER;35" "the docked plugin tabs drew ${c_window_own} icons in 20 frames (both tabs: 40)")
        expect(${tag}_more "c_deviceband_own;GREATER;0" "the Device chain's chips drew no plugin icons")
      elseif(lay STREQUAL "logic")
        # A second effect window kept open (Smooth keeps the earlier effect in its own window): three
        # plugin windows a frame; the Device chain's chips.
        run(${tag}_more "5 toasts clear\n10 fxopen 0 1\n12 openwin Device chain\n40 plugicons reset\n60 plugicons\n66 quit discard\n" ${args} --chan 5 --fx 0 0)
        counts(${tag}_more 60)
        clean(${tag}_more)
        expect(${tag}_more "c_window_own;GREATER;55" "the plugin windows drew ${c_window_own} title icons in 20 frames (three windows: 60)")
        expect(${tag}_more "c_deviceband_own;GREATER;0" "the Device chain's chips drew no plugin icons")
      elseif(lay STREQUAL "tracker")
        # The Mix page's effect menu (its slots), then the add-instrument menu searched.
        run(${tag}_more "5 toasts clear\n10 tracker page Mix\n20 menu open trackerfx0\n40 plugicons reset\n60 plugicons\n66 quit discard\n" ${args})
        counts(${tag}_more 60)
        clean(${tag}_more)
        expect(${tag}_more "c_menu_own;GREATER;19" "the effect menu drew ${c_menu_own} slot icons in 20 frames")
        run(${tag}_add "5 toasts clear\n20 menu open trackeradd\n30 text synth\n40 plugicons reset\n60 plugicons\n66 quit discard\n" ${args})
        counts(${tag}_add 60)
        clean(${tag}_add)
        expect(${tag}_add "c_menu_own;GREATER;100" "the instrument menu's search drew ${c_menu_own} plugin icons")
      endif()
      # The plugin picker (its own page) in the same layout and theme.
      run(${tag}_picker "5 toasts clear\n20 plugicons reset\n40 plugicons\n41 pickerhead\n48 quit discard\n" ${args} --picker)
      counts(${tag}_picker 40)
      clean(${tag}_picker)
      expect(${tag}_picker "c_picker_own;GREATER;100" "the picker drew ${c_picker_own} plugin icons")
      header(${tag}_picker)
      # The picker's header at the smallest main window (960 x 600): its parts wrap, none overlap.
      if(lay STREQUAL "studio" OR lay STREQUAL "tracker")
        run(${tag}_small "5 toasts clear\n40 pickerhead\n48 quit discard\n" ${args} --picker --size 960x600)
        header(${tag}_small)
      endif()
    endforeach()
  endforeach()
endforeach()
message(STATUS "k450_plugin_icons_app: ${checks} checks passed")
