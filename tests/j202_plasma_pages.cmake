# Gridline — J-202: every page and view of Plasma's editor renders (hidden, off-screen, isolated
# config, no Serum library). For each page (OSC, MIX, FX, MATRIX, GLOBAL), each overlay (arp,
# clip, table editor, preset browser, oscillator mapping), an LFO and an envelope tab and three
# presets with content (effects, matrix rows, arp / clip slots), at 100 % and 150 % UI scale:
#  - the app exits cleanly (no ImGui assert, no crash) and writes its frame;
#  - Plasma's own layout check ran (GL_PLASMA_LAYOUT_CHECK prints "PLASMA_LAYOUT") and found no
#    text / text or text / knob overlap ("PLASMA_OVERLAP").
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j202_plasma_pages.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home" "${OUT}/no-library")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GL_PLASMA_SERUM_ROOTS} "${OUT}/no-library")
set(ENV{GL_PLASMA_LAYOUT_CHECK} "1")

# name | page | view | preset
set(views
    "osc|0||" "mix|1||" "fx|2||" "matrix|3||" "global|4||" "arp|0|arp|" "clip|0|clip|" "table|0|wt|"
    "browser|0|browser|" "mapping|0|map|" "lfo2|0|lfo2|" "env2|0|env2|" "fx-preset|2||Drawn Wobble"
    "matrix-preset|3||Wobble Bass" "clip-preset|0|clip|Clip Groove")
set(count 0)
foreach(scale 1 1.5)
  foreach(v ${views})
    string(REPLACE "|" ";" f "${v}")
    list(GET f 0 name)
    list(GET f 1 page)
    list(GET f 2 view)
    list(GET f 3 preset)
    set(ENV{GL_PLASMA_PAGE} "${page}")
    set(ENV{GL_PLASMA_VIEW} "${view}")
    set(ENV{GL_PLASMA_PRESET} "${preset}")
    set(ENV{GL_PLASMA_LAYOUT_TAG} "${name}@${scale}")
    set(shot "${OUT}/${name}_${scale}.ppm")
    execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --ui-scale ${scale} --plugin gl.plasma --size 1600x1100 --frames 12
                            --screenshot "${shot}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "FAIL: ${name} at ${scale}x: gridline-app failed (${rc}):\n${out}\n${err}")
    endif()
    if(NOT EXISTS "${shot}")
      message(FATAL_ERROR "FAIL: ${name} at ${scale}x: no frame written:\n${out}\n${err}")
    endif()
    if(NOT err MATCHES "PLASMA_LAYOUT ${name}@${scale}: [0-9]+ items")
      message(FATAL_ERROR "FAIL: ${name} at ${scale}x: the editor did not draw (no layout report):\n${err}")
    endif()
    if(err MATCHES "PLASMA_OVERLAP")
      message(FATAL_ERROR "FAIL: ${name} at ${scale}x: overlapping text:\n${err}")
    endif()
    file(REMOVE "${shot}")
    math(EXPR count "${count} + 1")
  endforeach()
endforeach()
message(STATUS "ok - ${count} Plasma pages and views render without overlaps")
