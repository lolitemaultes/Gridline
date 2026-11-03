# Gridline: the piano roll keyboard's hovered key has exactly the accent colour chosen in Settings, in the
# real app (hidden, isolated config and HOME, fixed frame time) on a copy of the test song, editing pattern 0
# on channel 6, with the accent set to #2EA8FF and to #E0409A. Reads the frame's pixels: a white key across
# its width, also beside the black keys, and a black key.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P piano_key_accent_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)

function(run tag accent extra script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/cfg_${tag}/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {\"theme.accent\": \"${accent}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --edit 0 6 --hide rack
                          --dev-input "${OUT}/${tag}.txt" --screenshot "${OUT}/${tag}.ppm" --frames 30 ${extra}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# The pixel at (x, y) of a 1440x900 binary PPM, as lower case rrggbb.
function(pixel file x y out)
  file(READ "${file}" head LIMIT 16)
  if(NOT head MATCHES "^P6\n1440 900\n255\n")
    message(FATAL_ERROR "unexpected frame header in ${file}")
  endif()
  math(EXPR at "16 + (${y} * 1440 + ${x}) * 3")
  file(READ "${file}" hex OFFSET ${at} LIMIT 3 HEX)
  set(${out} "${hex}" PARENT_SCOPE)
endfunction()
function(want got expect what)
  if(NOT "${got}" STREQUAL "${expect}")
    message(FATAL_ERROR "FAIL: ${what}\n  want: ${expect}\n  got:  ${got}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

run(geo "" "" "6 lane\n8 quit discard\n")
if(NOT geo_out MATCHES "lane frame [0-9]+: x0 ([-0-9]+)[.0-9]* [^\n]* pitchY0 ([-0-9]+)[.0-9]* rowH ([0-9]+)")
  message(FATAL_ERROR "FAIL: no piano roll geometry\n${geo_out}")
endif()
set(x0 ${CMAKE_MATCH_1})
set(py0 ${CMAKE_MATCH_2})
set(rowH ${CMAKE_MATCH_3})
# A D in the middle of the view (black keys on both sides), and the D sharp above it.
set(ymid 400)
math(EXPR base "127 - (${ymid} - ${py0}) / ${rowH}")
math(EXPR d "${base} - ${base} % 12 + 2")
math(EXPR yw "${py0} + (127 - ${d}) * ${rowH} + ${rowH} / 2")
math(EXPR ys "${yw} - ${rowH} / 2 - 2")   # the strip beside the black key above
math(EXPR yb "${yw} - ${rowH}")
math(EXPR xr "${x0} - 12")                # near the key's right end, beside the black keys
math(EXPR xl "${x0} - 58")                # near the tip, left
math(EXPR xb "${x0} - 50")                # on the black key

foreach(c "2ea8ff" "e0409a")
  foreach(t dark light)
    set(ex "")
    if(t STREQUAL "light")
      set(ex "--light")
    endif()
    run(w_${c}_${t} "#${c}" "${ex}" "5 move ${xr} ${yw}\n")
    pixel("${OUT}/w_${c}_${t}.ppm" ${xr} ${yw} p)
    want("${p}" "${c}" "${t} #${c}: white key, right")
    pixel("${OUT}/w_${c}_${t}.ppm" ${xl} ${yw} p)
    want("${p}" "${c}" "${t} #${c}: white key, tip")
    pixel("${OUT}/w_${c}_${t}.ppm" ${xr} ${ys} p)
    want("${p}" "${c}" "${t} #${c}: white key, the strip beside the black key")
    run(b_${c}_${t} "#${c}" "${ex}" "5 move ${xb} ${yb}\n")
    pixel("${OUT}/b_${c}_${t}.ppm" ${xb} ${yb} p)
    want("${p}" "${c}" "${t} #${c}: black key")
  endforeach()
endforeach()
