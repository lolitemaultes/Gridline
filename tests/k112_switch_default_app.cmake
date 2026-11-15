# Gridline - K-284: a layout switch always opens that layout's designed arrangement.
#   A config holds a deliberately odd saved arrangement for every layout (each dock file with its
#   splits turned the other way, the regions of Clips and Smooth hidden and shown against the
#   design) and version keys that accept them. The app starts in Studio and switches
#   Studio -> Clips -> Smooth -> Tracker -> Studio -> Smooth -> Studio (the region shells, as
#   development layouts).
#   After each switch the dock tree, the regions and the open windows equal those of a run from a
#   clean config doing the same switches, and the dock tree has the shape of a clean start in that
#   layout. The start itself keeps the last session's arrangement (the odd Studio one).
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112_switch_default_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(failures 0)
macro(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR failures "${failures} + 1")
endmacro()

set(base "\"ui.layoutChosen\": true, \"ui.layoutDev\": true")
set(switches --switch-layout live@20 --switch-layout logic@40 --switch-layout tracker@60 --switch-layout studio@80
    --switch-layout logic@100 --switch-layout studio@120)
set(script "5 toasts clear\n")
foreach(f 15 35 55 75 95 115 135)
  string(APPEND script "${f} layout docks\n${f} layout dump\n${f} layout state\n")
endforeach()

# run(<tag> <settings values> <files dir or ""> <frames> args...): a run in its own config folder;
# the dock files in <files dir> are put in it first. Sets <tag>_log.
function(run tag values files frames)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {${values}}}\n")
  if(files)
    file(GLOB inis "${files}/*.ini")
    file(COPY ${inis} DESTINATION "${cfg}/gridline")
  endif()
  file(WRITE "${cfg}/input.txt" "${script}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${OUT}/${tag}.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${OUT}/${tag}.gridline.json" --hidden --no-ui-sounds --size 1440x900 --dev-input "${cfg}/input.txt"
                          --frames ${frames} ${ARGN} --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "FAIL: ${tag}: gridline-app failed (${rc})")
  endif()
  set(${tag}_log "${out}" PARENT_SCOPE)
endfunction()

# between(<out var> <log> <begin text> <end text>): the text from <begin text> to <end text>.
function(between var log first last)
  set(${var} "" PARENT_SCOPE)
  string(FIND "${log}" "${first}" a)
  if(a LESS 0)
    return()
  endif()
  string(SUBSTRING "${log}" ${a} -1 rest)
  string(FIND "${rest}" "${last}" b)
  if(b LESS 0)
    return()
  endif()
  string(SUBSTRING "${rest}" 0 ${b} got)
  set(${var} "${got}" PARENT_SCOPE)
endfunction()

# The shape of a dock tree: splits, central node and windows, without the rectangles.
function(shape var text)
  string(REGEX REPLACE " rect [-0-9]+ [-0-9]+ [-0-9]+ [-0-9]+" "" s "${text}")
  string(REGEX REPLACE "DOCKS BEGIN frame [0-9]+" "DOCKS" s "${s}")
  set(${var} "${s}" PARENT_SCOPE)
endfunction()

# ---- each layout's own dock file, saved by a run that visits them all ----
set(ENV{GRIDLINE_SAVE_LAYOUT} "1")
run(prep "${base}" "" 100 ${switches})
unset(ENV{GRIDLINE_SAVE_LAYOUT})
set(odd "${OUT}/odd")
file(MAKE_DIRECTORY "${odd}")
foreach(f layout.ini layout-live-shell.ini layout-logic-shell.ini layout-tracker.ini)
  set(src "${OUT}/cfg_prep/gridline/${f}")
  if(NOT EXISTS "${src}")
    fail("the prep run saved no ${f}")
    continue()
  endif()
  file(READ "${src}" ini)
  # Every split the other way round: a valid but deliberately odd arrangement.
  string(REPLACE "Split=X" "Split=@" ini "${ini}")
  string(REPLACE "Split=Y" "Split=X" ini "${ini}")
  string(REPLACE "Split=@" "Split=Y" ini "${ini}")
  if(NOT ini MATCHES "Split=")
    fail("${f} has no dock splits")
  endif()
  file(WRITE "${odd}/${f}" "${ini}")
endforeach()

# ---- the odd config, and a clean one, through the same switches ----
set(versions "\"ui.layoutVersion\": 9999, \"ui.layoutVersion.live\": 9999, \"ui.layoutVersion.logic\": 9999, \"ui.layoutVersion.tracker\": 9999, \"ui.layoutVersion.live.shell\": 9999, \"ui.layoutVersion.logic.shell\": 9999")
set(states "\"layout.live.state\": \"{\\\"hidden\\\":[\\\"live.browser\\\",\\\"live.clip\\\"],\\\"shown\\\":[\\\"live.side\\\",\\\"live.mixer\\\"]}\", \"layout.logic.state\": \"{\\\"hidden\\\":[\\\"logic.inspector\\\"],\\\"shown\\\":[\\\"logic.library\\\",\\\"logic.right\\\",\\\"logic.grid\\\"]}\", \"layout.logic.panesKept\": true")
run(odd "${base}, \"ui.layout\": \"studio\", ${versions}, ${states}" "${odd}" 140 ${switches})
run(fresh "${base}" "" 140 ${switches})

# The odd arrangement is really there: the start keeps the last session's (odd) Studio dock tree.
between(oddStart "${odd_log}" "DOCKS BEGIN frame 15 " "DOCKS END")
between(freshStart "${fresh_log}" "DOCKS BEGIN frame 15 " "DOCKS END")
if(oddStart STREQUAL "" OR freshStart STREQUAL "")
  fail("no dock tree at the start")
elseif(oddStart STREQUAL freshStart)
  fail("the odd Studio arrangement was not read at the start (the test sets nothing up)")
else()
  message(STATUS "ok - the start keeps the saved Studio arrangement")
endif()

set(i 0)
foreach(pair "35;live" "55;logic" "75;tracker" "95;studio" "115;logic" "135;studio")
  list(GET pair 0 f)
  list(GET pair 1 lay)
  between(a "${odd_log}" "DOCKS BEGIN frame ${f} layout ${lay}" "DOCKS END")
  between(b "${fresh_log}" "DOCKS BEGIN frame ${f} layout ${lay}" "DOCKS END")
  if(a STREQUAL "" OR b STREQUAL "")
    fail("${lay}: no dock tree at frame ${f}")
  elseif(NOT a STREQUAL b)
    fail("${lay}: after the switch the dock tree is not the default one\n--- odd config ---\n${a}\n--- clean config ---\n${b}")
  else()
    message(STATUS "ok - ${lay}: the dock tree after the switch is the default one")
  endif()
  between(a "${odd_log}" "LAYOUT BEGIN frame ${f}" "LAYOUT END")
  between(b "${fresh_log}" "LAYOUT BEGIN frame ${f}" "LAYOUT END")
  if(a STREQUAL "" OR NOT a STREQUAL b)
    fail("${lay}: after the switch the regions are not the default ones\n--- odd config ---\n${a}\n--- clean config ---\n${b}")
  endif()
  string(REGEX MATCH "frame ${f} layoutstate layout ${lay} [^\n]* open \\[([^]]*)\\]" _ "${odd_log}")
  set(oa "${CMAKE_MATCH_1}")
  string(REGEX MATCH "frame ${f} layoutstate layout ${lay} [^\n]* open \\[([^]]*)\\]" _ "${fresh_log}")
  set(ob "${CMAKE_MATCH_1}")
  if(oa STREQUAL "" OR NOT oa STREQUAL ob)
    fail("${lay}: after the switch the open windows are [${oa}], by default [${ob}]")
  endif()
  # The shape of a clean start in that layout (the splits and the windows in them).
  if(NOT DEFINED start_${lay}_log)
    run(start_${lay} "${base}, \"ui.layout\": \"${lay}\"" "" 20)
  endif()
  between(c "${start_${lay}_log}" "DOCKS BEGIN frame 15 layout ${lay}" "DOCKS END")
  between(a "${odd_log}" "DOCKS BEGIN frame ${f} layout ${lay}" "DOCKS END")
  shape(sa "${a}")
  shape(sc "${c}")
  if(sc STREQUAL "" OR NOT sa STREQUAL sc)
    fail("${lay}: the dock tree after the switch differs from a clean start\n--- after the switch ---\n${sa}\n--- clean start ---\n${sc}")
  endif()
  # Studio is not a region shell: its windows after the switch are those of a clean start (no part
  # another layout opened stays open in it).
  if(lay STREQUAL "studio")
    string(REGEX MATCH "frame 15 layoutstate layout studio [^\n]* open \\[([^]]*)\\]" _ "${start_studio_log}")
    if(NOT oa STREQUAL CMAKE_MATCH_1)
      fail("studio (frame ${f}): the open windows are [${oa}], a clean start opens [${CMAKE_MATCH_1}]")
    endif()
  endif()
endforeach()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - every layout switched to opens its designed arrangement, whatever was saved for it")
