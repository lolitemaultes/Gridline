# Gridline - every dialog fits the screen at 200% interface scale, its buttons on screen.
# On a 1440 x 900 point screen at 200% the Render dialog lost its lower part, Cancel and Start
# with it: Dear ImGui cuts a window that sizes itself to its content (AlwaysAutoResize) to the work
# size of the monitor its viewport is on, while the dialog's body was sized for the work area
# between Gridline's bars. Where the monitor's usable area was the smaller of the two the body did
# not scroll far enough and the footer fell below the window's edge (MIDI import and Render to
# sampled instrument the same). Track Receipt's file list was a fixed 360 px: at 200% on such a
# screen (960 x 600 logical) its Close and Calculate were off the window in every layout.
# Here, in the real app (hidden, isolated config and HOME, SDL's dummy audio driver, laid out but
# not drawn), each dialog opens on
#   - "small":   a 1440 x 900 point window at 200% (960 x 600 logical, the smallest workspace), in
#     each layout it floats in,
#   - "monitor": a 1440 x 900 logical main window at 200% (a 2880 x 1800 frame) on a monitor whose
#     usable area is 1440 x 474 (the case reported), in Studio,
# and the dev command "dialogfit" reports each dialog's rect against the work area and each primary
# button: shown, not clipped by its window, inside the work area and not covered by another window.
# Every dialog must lie inside and every button be visible.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k442_dialog_fit_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{HOME} "${OUT}/home")
file(MAKE_DIRECTORY "${OUT}/home")

# name | how it opens (show:<--show arg> or run:<command palette id>) | its window | buttons (',' separated)
# | the layouts it floats in (Clips docks Track Receipt, and its empty song has nothing to import or
# render to an instrument; the Tracker shows About and Settings as pages)
set(dialogs
# (the Export audio dialog, ###render, ends on Cancel and Render; the older Render dialog had Start)
  "render|run:cmd:file.render|render|Cancel,Render|studio performance tracks tracker"
  "receipt|run:menu:Help/Track Receipt...|trackreceipt|Close,Calculate|studio tracks tracker"
  "midiimport|show:midiimport|midiimport|Cancel,Accept|studio tracks tracker"
  "rendersampler|show:rendersampler|j59render|Cancel,Render|studio tracks tracker"
  "importreport|show:importreport|FL Studio import|Close,Save as...|studio performance tracks tracker"
  "unsaved|show:unsaved|##unsaved|Don't save,Cancel,Save|studio performance tracks tracker"
  "saveas|show:saveas|##filedialog|Cancel|studio performance tracks tracker"
  "projectinfo|run:win:Project info|Project info||studio performance tracks tracker"
  "about|run:win:About Gridline|About Gridline||studio performance tracks"
  "settings|show:settings|Settings||studio performance tracks"
  "pluginmanager|run:win:Plugin manager|Plugin manager||studio performance tracks tracker")
# tag | --size | --ui-scale | monitor (logical px) | layouts: a 1440 x 900 point screen at 200% in every
# layout; the reported case (a monitor smaller than the window's logical size) in Studio.
set(screens "small|1440x900|2||studio performance tracks tracker" "monitor|2880x1800|2|0 0 1440 474|studio")
set(failures "")
set(checked 0)
foreach(screen IN LISTS screens)
  string(REPLACE "|" ";" sc "${screen}")
  list(GET sc 0 stag)
  list(GET sc 1 size)
  list(GET sc 2 scale)
  list(GET sc 3 vmon)
  list(GET sc 4 screenLayouts)
  string(REPLACE " " ";" screenLayouts "${screenLayouts}")
  foreach(layout IN LISTS screenLayouts)
    foreach(d IN LISTS dialogs)
      string(REPLACE "|" ";" f "${d}")
      list(GET f 0 name)
      list(GET f 1 how)
      list(GET f 2 win)
      list(GET f 3 buttons)
      list(GET f 4 floats)
      string(REPLACE " " ";" floats "${floats}")
      if(NOT layout IN_LIST floats)
        continue()
      endif()
      if(NOT buttons STREQUAL "")
        string(REPLACE "," "\" \"" buttons "${buttons}")
        set(buttons "\"${buttons}\"")
      endif()
      set(tag "${name} ${layout} ${stag}")
      set(D "${OUT}/${layout}-${stag}-${name}")
      file(MAKE_DIRECTORY "${D}")
      set(ENV{GRIDLINE_CONFIG_HOME} "${D}")
      set(script "")
      if(NOT vmon STREQUAL "")
        string(APPEND script "2 vmonitors ${vmon}\n")
      endif()
      set(args "")
      if(how MATCHES "^show:(.*)$")
        set(args --show "${CMAKE_MATCH_1}")
      elseif(how MATCHES "^run:(.*)$")
        string(APPEND script "40 palette run ${CMAKE_MATCH_1}\n")
      endif()
      string(APPEND script "100 dialogfit ${buttons}\n110 quit discard\n")
      file(WRITE "${D}/script.txt" "${script}")
      execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-draw --layout ${layout} --size ${size} --ui-scale ${scale} --frames 300
                              --dev-input "${D}/script.txt" ${args}
                      RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 90)
      file(WRITE "${D}/run.log" "${out}\n${err}")
      math(EXPR checked "${checked} + 1")
      if(NOT rc EQUAL 0)
        string(APPEND failures "${tag}: the app ended with ${rc}\n")
        continue()
      endif()
      string(REPLACE "(" "\\(" wre "${win}")
      string(REPLACE ")" "\\)" wre "${wre}")
      string(REPLACE "." "\\." wre "${wre}")
      if(NOT out MATCHES "dialogfit window \"${wre}\" [^\n]*inside ([01])")
        string(APPEND failures "${tag}: the dialog '${win}' was not shown\n")
        continue()
      endif()
      string(REGEX MATCHALL "dialogfit window [^\n]*inside 0" outside "${out}")
      foreach(l IN LISTS outside)
        string(APPEND failures "${tag}: not inside the work area: ${l}\n")
      endforeach()
      string(REGEX MATCHALL "dialogfit item [^\n]*visible 0" hidden "${out}")
      foreach(l IN LISTS hidden)
        string(APPEND failures "${tag}: button not visible: ${l}\n")
      endforeach()
    endforeach()
  endforeach()
endforeach()
if(NOT failures STREQUAL "")
  message(FATAL_ERROR "FAIL:\n${failures}")
endif()
message(STATUS "k442: ${checked} dialog runs fit the work area with their buttons visible")
