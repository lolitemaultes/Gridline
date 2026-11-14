# Gridline - Plasma opens at its full size on a 1920 x 1080 screen, in the real app (hidden,
# off-screen, isolated config, SDL's dummy audio driver), with every setting at its default.
#   1. 1920x1080: the editor shows at 100 %, the window has nothing to scroll and nothing of the
#      editor is cut off; the window's own rows (header, tabs with the A/B row on them, the slim
#      note processor row) and its frame take no more than kRowsMax px of its height;
#   2. 1440x900 and 1280x800: it is reduced to fit, still whole, without a scrollbar;
#   3. a global plugin window size of 60 % leaves Plasma at full size, a size picked for Plasma
#      itself wins, and another instrument keeps the global size (75 % by default).
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k223_plasma_fullsize_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/no-library")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GL_PLASMA_SERUM_ROOTS} "${OUT}/no-library")
unset(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE})  # the real plugin window size setting (the suite pins 100 %)
set(kRowsMax 186)  # 1080 less the top and status bars and margins (990), 800 for the editor and 4 to spare

# Runs the app with `plugin` open at `size` and `script`; sets z, scroll, wy, wh, dh, clipped, cut.
function(run tag size plugin script)
  file(MAKE_DIRECTORY "${OUT}/${tag}/cfg" "${OUT}/${tag}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/${tag}/cfg")
  set(ENV{HOME} "${OUT}/${tag}/home")
  file(WRITE "${OUT}/${tag}/in.txt" "${script}38 edzoom channelsettings\n")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --plugin ${plugin} --size ${size} --frames 42
                          --dev-input "${OUT}/${tag}/in.txt" --screenshot "${OUT}/${tag}/shot.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}/out.txt" "${out}")
  file(WRITE "${OUT}/${tag}/err.txt" "${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL: ${tag}: gridline-app failed (${rc}):\n${err}")
  endif()
  if(NOT out MATCHES "edzoom channelsettings: type ${plugin} zoom ([0-9.]+) design ([0-9]+)x([0-9]+) origin [-0-9.,]+ window ([-0-9]+)[.0-9]*,([-0-9]+)[.0-9]* ([0-9]+)[.0-9]*x([0-9]+)[.0-9]* hovered [01] scroll ([01])")
    message(FATAL_ERROR "FAIL: ${tag}: no window report for ${plugin}:\n${out}")
  endif()
  set(z ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(dh ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(wy ${CMAKE_MATCH_5} PARENT_SCOPE)
  set(wh ${CMAKE_MATCH_7} PARENT_SCOPE)
  set(scroll ${CMAKE_MATCH_8} PARENT_SCOPE)
  if(NOT out MATCHES "edfit channelsettings: type ${plugin} zoom [0-9.]+ inner [0-9]+x[0-9]+ ink [0-9]+x[0-9]+ clipped ([-0-9]+),([-0-9]+),([-0-9]+),([-0-9]+)")
    message(FATAL_ERROR "FAIL: ${tag}: no fit report for ${plugin}:\n${out}")
  endif()
  set(clipped "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(c 0)
  foreach(v ${CMAKE_MATCH_1} ${CMAKE_MATCH_2} ${CMAKE_MATCH_3} ${CMAKE_MATCH_4})
    if(v GREATER 1)
      set(c 1)
    endif()
  endforeach()
  set(cut ${c} PARENT_SCOPE)
endfunction()

# 1. full size on 1920 x 1080
run(full 1920x1080 gl.plasma "")
if(NOT z STREQUAL "1.00")
  message(FATAL_ERROR "FAIL: 1920x1080: Plasma opened at ${z}, not at its full size")
endif()
if(NOT scroll EQUAL 0 OR NOT cut EQUAL 0)
  message(FATAL_ERROR "FAIL: 1920x1080: Plasma's window scrolls (${scroll}) or cuts its editor (clipped ${clipped})")
endif()
math(EXPR rows "${wh} - ${dh}")
if(rows GREATER ${kRowsMax})
  message(FATAL_ERROR "FAIL: 1920x1080: the window's own rows take ${rows} px (at most ${kRowsMax})")
endif()
math(EXPR bottom "${wy} + ${wh}")
if(wy LESS 0 OR bottom GREATER 1080)
  message(FATAL_ERROR "FAIL: 1920x1080: the window (${wy} to ${bottom}) runs past the screen")
endif()
message(STATUS "1920x1080: Plasma at ${z}, rows ${rows} px, window ${wy} to ${bottom}")

# 2. smaller screens: reduced to fit, whole, no scrollbar
foreach(size 1440x900 1280x800)
  run(small_${size} ${size} gl.plasma "")
  if(NOT z MATCHES "^0\\.[6-9][0-9]$")
    message(FATAL_ERROR "FAIL: ${size}: Plasma opened at ${z} (reduced to fit expected)")
  endif()
  if(NOT scroll EQUAL 0 OR NOT cut EQUAL 0)
    message(FATAL_ERROR "FAIL: ${size}: Plasma's window scrolls (${scroll}) or cuts its editor (clipped ${clipped})")
  endif()
  message(STATUS "${size}: Plasma at ${z}")
endforeach()

# 3. the global size does not shrink it; its own size wins; other instruments keep the global size
run(global 1920x1080 gl.plasma "1 setting ui.pluginWindowSize 0.6\n")
if(NOT z STREQUAL "1.00")
  message(FATAL_ERROR "FAIL: a global size of 60 % shrank Plasma to ${z}")
endif()
run(own 1920x1080 gl.plasma "1 edzoomset gl.plasma 0.75\n")
if(NOT z STREQUAL "0.75")
  message(FATAL_ERROR "FAIL: Plasma's own size of 75 % gave ${z}")
endif()
run(other 1920x1080 gl.synth "")
if(NOT z STREQUAL "0.75")
  message(FATAL_ERROR "FAIL: another instrument opened at ${z}, not at the global 75 %")
endif()
message(STATUS "ok - Plasma opens at full size on 1920x1080, reduced to fit on smaller screens")
