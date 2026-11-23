# Gridline: Loupe's spectrogram in the real app (hidden, isolated config and HOME, SDL's dummy audio
# driver). Loupe in a mixer slot with the demo phrase (a 440 Hz tone from 1.0 to 3.2 s over kicks):
#   1. the spectrogram view gets an image made for its pixels (one column per pixel of the lanes'
#      width, one row per pixel of their height) at 1x and at 2x (two pixels per point);
#   2. the tone reads its level (-13 dB) at 440 Hz and the empty highs stay low;
#   3. zooming in makes a new image for the frames in view, with the same level;
#   4. the View menu's range and resolution are settings, and a new resolution redraws the image.
# Screenshots: <OUT>/spec1x.ppm, <OUT>/spec2x.ppm.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k078_loupe_spectrogram_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home" "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_CACHE_DIR} "${OUT}/cache")
set(ENV{GRIDLINE_EDISON_DEMO} "1")
set(ENV{GRIDLINE_EDISON_VIEW} "spectral")

function(run tag scale script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  file(REMOVE_RECURSE "${OUT}/cfg")  # each run from the default settings
  file(MAKE_DIRECTORY "${OUT}/cfg")
  if(scale STREQUAL "2")
    set(size 2880x1800)
  else()
    set(size 1440x900)
  endif()
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --size ${size} --ui-scale ${scale} --plugin gl.edison --frames 3000
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
# spec(<p> <n>): the n-th "loupe spec" line into <p>_from _to _cols _rows _cur _lw _lh _range _res _l1 _l2
function(spec p n)
  string(REGEX MATCHALL "loupe spec: [^\n]*" lines "${out}")
  list(LENGTH lines count)
  if(NOT n LESS count)
    message(FATAL_ERROR "FAIL: ${p}: no loupe spec #${n} (${count})\nOutput:\n${out}")
  endif()
  list(GET lines ${n} l)
  string(REGEX MATCH "view ([0-9]+) ([0-9]+) cols ([0-9]+) rows ([0-9]+) current ([01]) lanes ([0-9]+) ([0-9]+) range ([0-9]+) resolution ([0-9])" m "${l}")
  if(m STREQUAL "")
    message(FATAL_ERROR "FAIL: ${p}: cannot read: ${l}")
  endif()
  set(${p}_from ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${p}_to ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${p}_cols ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(${p}_rows ${CMAKE_MATCH_4} PARENT_SCOPE)
  set(${p}_cur ${CMAKE_MATCH_5} PARENT_SCOPE)
  set(${p}_lw ${CMAKE_MATCH_6} PARENT_SCOPE)
  set(${p}_lh ${CMAKE_MATCH_7} PARENT_SCOPE)
  set(${p}_range ${CMAKE_MATCH_8} PARENT_SCOPE)
  set(${p}_res ${CMAKE_MATCH_9} PARENT_SCOPE)
  string(REGEX MATCHALL "level [0-9.]+ [0-9.]+ (-?[0-9.]+)" lv "${l}")
  set(i 1)
  foreach(e ${lv})
    string(REGEX MATCH "(-?[0-9.]+)$" v "${e}")
    # whole dB, as an integer for the comparisons
    string(REGEX REPLACE "\\.[0-9]+$" "" v "${v}")
    set(${p}_l${i} ${v} PARENT_SCOPE)
    math(EXPR i "${i} + 1")
  endforeach()
endfunction()

foreach(scale 1 2)
  run(spec${scale}x ${scale} "10 waitfor loupe-spectrogram 30
12 loupe spec 2.0 440 2.0 9000
14 forget loupe-spectrogram
16 loupe zoom 1.9 0.2
18 waitfor loupe-spectrogram 30
20 loupe spec 2.0 440
22 forget loupe-spectrogram
24 loupe specset 60 2
26 waitfor loupe-spectrogram 30
28 loupe spec 2.0 440
30 shot \"${OUT}/spec${scale}x.ppm\"
40 quit discard
")
  check("${scale}x: the spectrogram image was made" NOT out MATCHES "waitfor loupe-spectrogram: timed out")
  spec(a 0)
  # (at 2x the lanes are measured in points and drawn with two pixels per point)
  math(EXPR pw "${a_lw} * ${scale}")
  math(EXPR ph "${a_lh} * ${scale}")
  check("${scale}x: one column per pixel of the lanes (${a_cols} for ${pw})" a_cur EQUAL 1 AND a_cols EQUAL pw)
  check("${scale}x: one row per pixel of the lanes (${a_rows} for ${ph})" a_rows EQUAL ph)
  check("${scale}x: the 440 Hz tone reads about -13 dB (${a_l1})" a_l1 GREATER_EQUAL -15 AND a_l1 LESS_EQUAL -11)
  check("${scale}x: the empty highs stay low (${a_l2} dB at 9 kHz)" a_l2 LESS -45)
  check("${scale}x: default range 100 dB, balanced" a_range EQUAL 100 AND a_res EQUAL 1)
  spec(b 1)
  math(EXPR span "${b_to} - ${b_from}")
  check("${scale}x: zoomed in, a new image covers just the view (${span} frames)" b_cur EQUAL 1 AND span GREATER 8000 AND span LESS 9700 AND b_cols EQUAL pw)
  check("${scale}x: ... with the same level (${b_l1})" b_l1 GREATER_EQUAL -15 AND b_l1 LESS_EQUAL -11)
  spec(c 2)
  check("${scale}x: range and resolution are settings, and the image is made again for them" c_range EQUAL 60 AND c_res EQUAL 2 AND c_cur EQUAL 1 AND c_l1 GREATER_EQUAL -15)
  check("${scale}x: screenshot" EXISTS "${OUT}/spec${scale}x.ppm")
endforeach()
