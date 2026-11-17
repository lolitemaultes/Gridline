# Gridline - K-112 (R3): every editor body has exactly one host at a time, in every layout.
# With the Device chain open, the instrument, an effect, the master chain and Loupe are opened
# from their windows and from the chain's chips. Each frame's body draws ("layout bodies") must
# never show a body twice, nor a Loupe effect both as an effect editor and in the Loupe panel, and
# the device last opened is drawn exactly once: by the chain where the layout homes it there
# (Live, Tracker), by its own window elsewhere (Studio, Logic: the chain's chips open the window).
# The Tracker's instrument is its lower band's own screen (K-106 cp18-cp20): no channel settings body.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112_hosts_app.cmake
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

function(run tag layout script)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true}}\n")
  file(WRITE "${cfg}/input.txt" "${script}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${OUT}/${tag}.gridline.json" COPYONLY)
  set(sw "")
  if(NOT layout STREQUAL "studio")
    set(sw --switch-layout ${layout}@3)
  endif()
  execute_process(COMMAND "${APP}" "${OUT}/${tag}.gridline.json" --hidden --no-ui-sounds --size 1440x900 --fixed-dt 16
                          --dev-input "${cfg}/input.txt" ${sw} ${ARGN} --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "FAIL: ${tag}: gridline-app failed (${rc})")
  endif()
  set(log "${out}" PARENT_SCOPE)
endfunction()

# The bodies line of a frame (fails when missing), checked for doubles.
function(bodies tag f var)
  if(NOT log MATCHES "frame ${f} bodies \\[([^]\n]*)\\]")
    fail("${tag}: no bodies line at frame ${f}")
    set(${var} "" PARENT_SCOPE)
    set(failures ${failures} PARENT_SCOPE)
    return()
  endif()
  set(b "${CMAKE_MATCH_1}")
  if(b MATCHES " x([2-9]|[1-9][0-9])")
    fail("${tag} frame ${f}: a body is drawn twice (${b})")
  endif()
  string(REGEX MATCHALL "effect editor [0-9]+/[0-9]+" fx "${b}")
  foreach(e ${fx})
    string(REPLACE "effect editor " "" target "${e}")
    if(b MATCHES "Loupe ${target} ")
      fail("${tag} frame ${f}: a Loupe effect is drawn as an effect editor and in the Loupe panel (${b})")
    endif()
  endforeach()
  set(${var} "${b}" PARENT_SCOPE)
  set(failures ${failures} PARENT_SCOPE)
endfunction()

# `what` drawn exactly once, by `host` ("chain": the Device chain; "window": any other window).
macro(expect tag f b what host)
  if(NOT "${b}" MATCHES "${what} [0-9-]+/[0-9-]+ x1 @([^;]*)")
    fail("${tag} frame ${f}: ${what} is not drawn once (${b})")
  elseif("${host}" STREQUAL "chain" AND NOT CMAKE_MATCH_1 STREQUAL "Device chain")
    fail("${tag} frame ${f}: ${what} is drawn by ${CMAKE_MATCH_1}, not the Device chain (${b})")
  elseif("${host}" STREQUAL "window" AND CMAKE_MATCH_1 STREQUAL "Device chain")
    fail("${tag} frame ${f}: ${what} is drawn by the Device chain, not its window (${b})")
  endif()
endmacro()

foreach(lay studio live logic tracker)
  if(lay STREQUAL "live" OR lay STREQUAL "tracker")
    set(h chain)
  else()
    set(h window)
  endif()
  # ---- instrument, effect, chips, master chain ----
  run(${lay} ${lay} "5 toasts clear
20 layout open Device chain
24 layout open ###channelsettings
34 layout bodies
40 layout open ###fxeditor
50 layout bodies
56 layout close ###fxeditor
60 parts chain fx 0
70 layout bodies
74 parts chain instrument
84 layout bodies
88 parts chain master
98 layout bodies
" --frames 104)
  bodies(${lay} 34 b)
  if(lay STREQUAL "tracker")
    # K-106 cp18-cp20: the Tracker's lower band shows its own screen for the instrument (the
    # Instrument editor or the Channel frame), never the channel settings body; that opens floating
    # from the frame's "Open plugin window" (k316_tracker_editors_app, the inventory).
    if("${b}" MATCHES "channel settings")
      fail("${lay} frame 34: the channel settings body is drawn in the Tracker (${b})")
    endif()
  else()
    expect(${lay} 34 "${b}" "channel settings" ${h})
  endif()
  bodies(${lay} 50 b)
  expect(${lay} 50 "${b}" "effect editor" ${h})
  if(h STREQUAL "window")  # their windows: both stay open, each drawn once
    expect(${lay} 50 "${b}" "channel settings" window)
  endif()
  bodies(${lay} 70 b)
  expect(${lay} 70 "${b}" "effect editor" ${h})
  bodies(${lay} 84 b)
  if(lay STREQUAL "tracker")  # (as at frame 34)
    if("${b}" MATCHES "channel settings")
      fail("${lay} frame 84: the channel settings body is drawn in the Tracker (${b})")
    endif()
  else()
    expect(${lay} 84 "${b}" "channel settings" ${h})
  endif()
  bodies(${lay} 98 b)
  expect(${lay} 98 "${b}" "master chain" window)

  # ---- Loupe: as an effect, then pinned into the layout ----
  run(${lay}_loupe ${lay} "5 toasts clear
12 layout open Device chain
30 layout bodies
34 loupe pin below
50 layout bodies
" --frames 56 --plugin gl.edison)
  bodies(${lay}_loupe 30 b)
  expect(${lay}_loupe 30 "${b}" "effect editor" ${h})
  bodies(${lay}_loupe 50 b)
  expect(${lay}_loupe 50 "${b}" "Loupe" ${h})  # Live, Tracker: the chain hosts Loupe and the panel steps aside
endforeach()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - every body has one host, in every layout")
