# Gridline - K-147: every built-in plugin's window fits its editor, in the real app (hidden,
# off-screen, isolated configs, SDL's dummy audio driver).
#
# The app opens every built-in instrument, effect and note processor in turn ("fitall": effects
# in an effect slot window, instruments and note processors in the channel window) and reports,
# for each, what the editor drew against its window ("edfit"): how far anything runs past the
# window's edges (clipped) and how much room is left inside the padding (spare). For every
# plugin, at the default plugin window size (75 %), 100 % and 150 %, at interface scale 1
# and 2 (where the work area is half as large and big editors are reduced to fit):
#   * nothing is clipped (more than 1 px), unless the editor is taller than the work area even
#     at the smallest fit size (then the window scrolls; reported, allowed only at scale 2);
#   * no spare room on the right or at the bottom beyond kSpare px (the window is the editor's
#     size), and the editor starts at the padding on the left;
#   * the editor's own drawing (under the header) is as wide as the width it was given, and
#     nothing (the header row included) is laid out past the window's right edge;
#   * the window fits the work area.
#   cmake -DAPP=<gridline-app> -DDEMO=<song.gridline.json> -DOUT=<dir> -P k147_plugin_fit_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
unset(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE})  # the real plugin window size setting
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
set(kSpare 6)
set(kRunLimit 150)  # s per sweep (a sweep takes 15 to 60 s; the test's own limit covers all five)
set(failures "")

function(sweep tag ui size)
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}/home")
  set(script "")
  if(NOT "${size}" STREQUAL "default")
    string(APPEND script "1 setting ui.pluginWindowSize ${size}\n")
  endif()
  # Each plugin is reported once its window keeps its size for two frames (at most 12 frames),
  # and the app quits after the last one. The frame count and the time limit only bound a run
  # that goes wrong (on a slow, software-rendered machine a frame of the full window costs about
  # 100 ms: the main windows are closed, they play no part here).
  string(APPEND script "4 fitall 12\n")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  string(TIMESTAMP t0 "%s")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1920x1080 --ui-scale ${ui} --frames 2400 --screenshot "${OUT}/${tag}.ppm"
                          --hide playlist --hide browser --hide piano --hide mixer --hide rack --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT ${kRunLimit})
  string(TIMESTAMP t1 "%s")
  math(EXPR took "${t1} - ${t0}")
  file(WRITE "${OUT}/${tag}.out" "${out}")
  file(REMOVE "${OUT}/${tag}.ppm")
  string(REGEX MATCHALL "edfit plugin [^ ]+" opened "${out}")
  list(LENGTH opened nopened)
  set(last "none")
  if(nopened GREATER 0)
    list(GET opened -1 last)
  endif()
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed after ${took} s (${rc}), ${nopened} plugins reported, the last: ${last}\n${err}")
  endif()
  if(NOT out MATCHES "edfit done ([0-9]+)")
    message(FATAL_ERROR "${tag}: the sweep did not finish in ${took} s: ${nopened} plugins reported, the last: ${last}")
  endif()
  set(n ${CMAKE_MATCH_1})
  if(n LESS 150)
    message(FATAL_ERROR "${tag}: only ${n} plugins were opened")
  endif()
  string(REGEX MATCHALL "edfit [a-z]+: type [^ ]+ zoom [0-9.]+ inner [0-9]+x[0-9]+ ink [0-9]+x[0-9]+ clipped [-0-9,]+ spare [-0-9,]+ body [0-9]+ at [-0-9]+ of [0-9]+ region [0-9]+ overflow [0-9]+" lines "${out}")
  list(LENGTH lines got)
  if(NOT got EQUAL n)
    message(FATAL_ERROR "${tag}: ${got} fit reports for ${n} plugins")
  endif()
  set(bad "")
  foreach(l IN LISTS lines)
    # (CMake keeps nine groups per match: one match per part of the line)
    string(REGEX MATCH "type ([^ ]+) zoom ([0-9.]+) .* clipped ([-0-9]+),([-0-9]+),([-0-9]+),([-0-9]+)" _ "${l}")
    set(type ${CMAKE_MATCH_1})
    set(z ${CMAKE_MATCH_2})
    set(cL ${CMAKE_MATCH_3})
    set(cT ${CMAKE_MATCH_4})
    set(cR ${CMAKE_MATCH_5})
    set(cB ${CMAKE_MATCH_6})
    string(REGEX MATCH "spare ([-0-9]+),([-0-9]+),([-0-9]+)" _ "${l}")
    set(sL ${CMAKE_MATCH_1})
    set(sR ${CMAKE_MATCH_2})
    set(sB ${CMAKE_MATCH_3})
    string(REGEX MATCH "body ([0-9]+) at ([-0-9]+) of ([0-9]+) region ([0-9]+) overflow ([0-9]+)" _ "${l}")
    set(bw ${CMAKE_MATCH_1})
    set(left ${CMAKE_MATCH_2})
    set(given ${CMAKE_MATCH_3})
    set(region ${CMAKE_MATCH_4})
    set(overflow ${CMAKE_MATCH_5})
    # kSpare design px: in screen px at this size (at least kSpare)
    string(REPLACE "." "" z100 "${z}")
    math(EXPR z100 "${z100} + 0")
    if(z100 LESS 100)
      set(z100 100)
    endif()
    math(EXPR lim "(${kSpare} * ${z100} + 99) / 100")
    set(why "")
    if(cL GREATER 1 OR cT GREATER 1 OR cR GREATER 1 OR cB GREATER 1)
      set(why "clipped ${cL},${cT},${cR},${cB}")
    endif()
    if(why AND z STREQUAL "0.60" AND ui EQUAL 2 AND cL LESS 2 AND cT LESS 2 AND cR LESS 2)
      set(why "")  # taller than a half-size work area even at the smallest size: the window scrolls
    endif()
    if(sL GREATER ${lim} OR sR GREATER ${lim} OR sB GREATER ${lim})
      set(why "${why} spare ${sL},${sR},${sB}")
    endif()
    # The editor's own drawing (bw wide, at its left offset) in the width it was given (the
    # smaller of its own width and the region): no strip left empty on one side only (a drawing
    # centred in its width is fine), nothing drawn past the window's padding.
    if(region LESS given)
      set(given ${region})
    endif()
    math(EXPR right "${given} - ${left} - ${bw}")
    math(EXPR lopsided "${right} - ${left}")
    if(lopsided GREATER ${lim} OR lopsided LESS -${lim} OR left LESS -8 OR right LESS -8)
      set(why "${why} body ${bw} at ${left} of ${given}")
    endif()
    if(overflow GREATER 1)  # laid out past the region (the header row too): cut off at the edge
      set(why "${why} overflow ${overflow}")
    endif()
    if(why)
      list(APPEND bad "${type} at ${z}: ${why}")
    endif()
  endforeach()
  list(LENGTH bad nbad)
  if(nbad GREATER 0)
    string(REPLACE ";" "\n  " bad "${bad}")
    set(failures "${failures}\n${tag} (interface scale ${ui}, size ${size}): ${nbad} windows do not fit\n  ${bad}" PARENT_SCOPE)
  endif()
  message(STATUS "${tag}: ${n} plugin windows checked in ${took} s, ${nbad} do not fit")
endfunction()

sweep(def1 1 default)  # the default is 75 %
sweep(s100 1 1)
sweep(s150 1 1.5)
sweep(def2 2 default)
sweep(s150x2 2 1.5)
if(failures)
  message(FATAL_ERROR "FAIL:${failures}")
endif()
message(STATUS "ok - every built-in plugin window fits its editor at every size and interface scale")
