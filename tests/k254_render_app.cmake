# Gridline: K-254, render MIDI to audio in the real app, in each of the four layouts. Runs the app hidden
# (isolated config and HOME, fixed frame time) on a copy of the test song (pattern 0, "Beat", whose first
# channel has steps):
#   1. "Into a slicer channel" on the first channel's row: the render runs as a background job, then a
#      slicer channel lands as one undo step with slices, and its file is in the project's Rendered folder;
#      undo takes it away;
#   2. the render sheet opens on the row (and on the whole pattern), and a screenshot is taken of it
#      (Studio), for the look;
#   3. the whole pattern rendered into an audio clip, one undo step.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k254_render_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(run layout tag script)
  set(cfg "${OUT}/cfg_${layout}_${tag}")
  file(REMOVE_RECURSE "${cfg}" "${OUT}/${layout}_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home" "${OUT}/${layout}_${tag}")
  # each run its own project folder: the Rendered folder lands next to it
  configure_file("${DEMO}" "${OUT}/${layout}_${tag}/song.gridline.json" COPYONLY)
  if(NOT layout STREQUAL "studio")
    file(WRITE "${cfg}/gridline/settings.json"
         "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
    set(extra --layout-dev --layout ${layout})
  else()
    set(extra "")
  endif()
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  file(WRITE "${OUT}/${layout}_${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/${layout}_${tag}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 ${extra}
                          --dev-input "${OUT}/${layout}_${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${layout}_${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${layout} ${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(run_out "${out}" PARENT_SCOPE)
endfunction()

# The undo position from the n-th dump line.
function(undoAt text n out)
  string(REGEX MATCHALL "dump frame [0-9]+: [0-9]+ clips, [0-9]+ selected, undo [0-9]+" dumps "${text}")
  list(LENGTH dumps nd)
  if(nd LESS_EQUAL ${n})
    message(FATAL_ERROR "FAIL: no dump ${n}\n${text}")
  endif()
  list(GET dumps ${n} d)
  string(REGEX REPLACE ".*undo ([0-9]+)" "\\1" u "${d}")
  set(${out} ${u} PARENT_SCOPE)
endfunction()

foreach(layout studio tracker live logic)
  # 1. The first row into a slicer channel (the quick row: the last options, here the defaults).
  run(${layout} slicer "3 typing off\n6 dump\n8 render rows 0 slicer\n60 render state\n120 render state\n200 render state\n202 dump\n204 undo\n206 dump\n208 quit discard\n")
  if(NOT run_out MATCHES "render state job 0 sheet 0 finished 1 last done slicer channel ([0-9]+) clip [0-9]+ slices ([0-9]+) frames ([0-9]+)")
    message(FATAL_ERROR "FAIL ${layout}: the row should render into a slicer channel\n${run_out}")
  endif()
  set(slices ${CMAKE_MATCH_2})
  set(frames ${CMAKE_MATCH_3})
  if(slices LESS 1 OR frames LESS 1000)
    message(FATAL_ERROR "FAIL ${layout}: the slicer should get slices of a real render (${slices} slices, ${frames} frames)\n${run_out}")
  endif()
  undoAt("${run_out}" 0 u0)
  undoAt("${run_out}" 1 u1)
  undoAt("${run_out}" 2 u2)
  math(EXPR steps "${u1} - ${u0}")
  if(NOT steps EQUAL 1 OR NOT u2 EQUAL u0)
    message(FATAL_ERROR "FAIL ${layout}: the render should land as one undo step that undo takes away (${u0} -> ${u1} -> ${u2})\n${run_out}")
  endif()
  file(GLOB rendered "${OUT}/${layout}_slicer/Rendered/*.wav")
  list(LENGTH rendered nr)
  if(nr LESS 1)
    message(FATAL_ERROR "FAIL ${layout}: the rendered file should be in the project's Rendered folder")
  endif()
  message(STATUS "ok - ${layout}: a row rendered into a slicer channel (${slices} slices, ${frames} frames), one undo step, the file in Rendered")

  # 2. The sheet on the row, then on the whole pattern (a screenshot each in Studio).
  run(${layout} sheet "3 typing off\n6 render sheet rows 0\n40 render state\n60 shot \"${OUT}/${layout}_sheet.ppm\"\n62 render sheet pattern\n100 shot \"${OUT}/${layout}_sheet_pattern.ppm\"\n102 render state\n104 quit discard\n")
  if(NOT run_out MATCHES "render state job 0 sheet 1 ")
    message(FATAL_ERROR "FAIL ${layout}: the render sheet should open\n${run_out}")
  endif()
  if(NOT EXISTS "${OUT}/${layout}_sheet.ppm")
    message(FATAL_ERROR "FAIL ${layout}: no screenshot of the sheet")
  endif()
  message(STATUS "ok - ${layout}: the render sheet opens")

  # 3. The whole pattern into an audio clip.
  run(${layout} clip "3 typing off\n6 dump\n8 render pattern audioclip\n80 render state\n160 render state\n240 render state\n242 dump\n244 quit discard\n")
  if(NOT run_out MATCHES "finished 1 last done audioclip channel [0-9]+ clip ([0-9]+) ")
    message(FATAL_ERROR "FAIL ${layout}: the pattern should render into an audio clip\n${run_out}")
  endif()
  undoAt("${run_out}" 0 u0)
  undoAt("${run_out}" 1 u1)
  math(EXPR steps "${u1} - ${u0}")
  if(NOT steps EQUAL 1)
    message(FATAL_ERROR "FAIL ${layout}: the audio clip should be one undo step (${u0} -> ${u1})\n${run_out}")
  endif()
  message(STATUS "ok - ${layout}: the whole pattern rendered into an audio clip, one undo step")
endforeach()
