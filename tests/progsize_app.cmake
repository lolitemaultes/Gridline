# Gridline - 0.7.8: progress windows keep one size and one place for their whole life, in the real
# app (hidden, isolated config and HOME, dummy audio), in all four layouts (Studio, Clips, Smooth,
# Tracker). Each progress window is driven by a stand-in state (dev-input "progsize show") through
# 0% to 100%, short, long, empty and path-like names, a stalled job's warning on and off; every frame
# its rect is printed ("progsize trace on") and must never change while it shows:
#   filejob        saving the zipped project (and every file job: data files, bones, render to audio, ...)
#   render         audio export progress
#   projectload    opening a project
#   flpexport      FL Studio project export
#   samplerrender  render to sampled instrument (busy footer)
#   stems          split stems (while splitting)
#   trackreceipt   Track Receipt measuring
#   urlimport      import from URL, downloading
#   urlsetup       import from URL, setting up the downloader
#   update         update download (a made-up release, no network)
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> [-DSHOTS=<dir>] -P progsize_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{XDG_CACHE_HOME} "${OUT}/xdgcache")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)

set(popups filejob render projectload flpexport samplerrender stems trackreceipt urlimport urlsetup)
# frac|stalled|name
set(states
    "0|0|Kick.wav"
    "0.004|0|A really long sample file name that goes on and on past any window width, take 12 final mix (copy 3).wav"
    "0.07|0|Snare 2"
    "0.5|1|/Volumes/Samples Drive/Projects/2026/A very long folder name/with sub folders/and more/loop 120 bpm.wav"
    "0.5|0|"
    "0.88|0|W"
    "0.999|1|Hat"
    "1|0|Another long name: the master bus bounce with every effect and the long reverb tail, version 9.wav")
set(kStep 6)

# A script driving `list` popups from frame `start`; sets <out>_script and the frame ranges.
function(script out start list)
  set(s "5 progsize trace on\n")
  set(f ${start})
  set(ranges "")
  foreach(p IN LISTS list)
    set(first ${f})
    foreach(st IN LISTS states)
      string(REGEX MATCH "^([^|]*)\\|([^|]*)\\|(.*)$" m "${st}")
      set(frac "${CMAKE_MATCH_1}")
      set(stall "${CMAKE_MATCH_2}")
      set(name "${CMAKE_MATCH_3}")
      string(APPEND s "${f} progsize show ${p} ${frac} ${stall} \"${name}\"\n")
      if(DEFINED SHOTS AND p MATCHES "^(filejob|render|projectload)$" AND frac STREQUAL "0.5" AND stall STREQUAL "1")
        math(EXPR sf "${f} + 4")
        string(APPEND s "${sf} shot \"${OUT}/shot_${p}_TAG.ppm\"\n")
      endif()
      math(EXPR f "${f} + ${kStep}")
    endforeach()
    string(APPEND s "${f} progsize off\n")
    # the window's frames: from its third (an auto-sized window finds its size on its first two) to the last
    math(EXPR from "${first} + 3")
    math(EXPR to "${f}")
    list(APPEND ranges "${p}:${from}:${to}")
    math(EXPR f "${f} + 6")
  endforeach()
  math(EXPR q "${f} + 4")
  string(APPEND s "${q} quit discard\n")
  set(${out}_script "${s}" PARENT_SCOPE)
  set(${out}_ranges "${ranges}" PARENT_SCOPE)
endfunction()

function(run tag layout scale script)
  string(REPLACE "TAG" "${tag}" script "${script}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  # 2x: the interface at twice its size (fonts, widths, padding) in a window twice as large
  set(size 1280x800)
  if(scale EQUAL 2)
    set(size 2560x1600)
  endif()
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --no-update-check --size ${size} --ui-scale ${scale}
                          --layout-dev --layout ${layout} --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.out" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# Every rect `popup` printed in frames [from, to) is the same (and there are enough of them).
function(steady tag ranges)
  string(REGEX MATCHALL "progsize frame [0-9]+ [a-z]+ [-0-9]+ [-0-9]+ [0-9]+ [0-9]+[^\n]*" lines "${${tag}_out}")
  foreach(r IN LISTS ranges)
    string(REPLACE ":" ";" r "${r}")
    list(GET r 0 popup)
    # The interface's frames (the trace counts those, not the script's): from the first "show" of
    # this window to its "off", less the frames it settles in after it opens (a dialog measures its
    # body and centres itself over its first few frames; a real job starts in a window already open).
    if(NOT "${${tag}_out}" MATCHES "progsize at ([0-9]+) show ${popup}\n")
      message(FATAL_ERROR "FAIL (${tag}): ${popup} was never shown\n${${tag}_out}")
    endif()
    math(EXPR from "${CMAKE_MATCH_1} + 6")
    if(NOT "${${tag}_out}" MATCHES "progsize at ([0-9]+) off ${popup}\n")
      message(FATAL_ERROR "FAIL (${tag}): ${popup} was never switched off\n${${tag}_out}")
    endif()
    set(to ${CMAKE_MATCH_1})
    set(first "")
    set(count 0)
    set(changes "")
    foreach(l IN LISTS lines)
      if(NOT l MATCHES "^progsize frame ([0-9]+) ${popup} ([-0-9]+ [-0-9]+ [0-9]+ [0-9]+)( appearing)?$")
        continue()
      endif()
      set(fr ${CMAKE_MATCH_1})
      set(rect "${CMAKE_MATCH_2}")
      if(fr LESS from OR NOT fr LESS to OR CMAKE_MATCH_3)
        continue()
      endif()
      math(EXPR count "${count} + 1")
      if(first STREQUAL "")
        set(first "${rect}")
      elseif(NOT rect STREQUAL first)
        string(APPEND changes "  frame ${fr}: ${rect} (first ${first})\n")
      endif()
    endforeach()
    if(count LESS 30)
      message(FATAL_ERROR "FAIL (${tag}): ${popup} showed on only ${count} frames of ${from}..${to}\n${${tag}_out}")
    endif()
    if(NOT changes STREQUAL "")
      message(FATAL_ERROR "FAIL (${tag}): the ${popup} window changed its rect while its progress and names changed:\n${changes}")
    endif()
    message(STATUS "ok - ${tag}: ${popup} kept ${first} over ${count} frames")
  endforeach()
endfunction()

foreach(layout studio live logic tracker)
  script(main 20 "${popups}")
  run(${layout} ${layout} 1 "${main_script}")
  steady(${layout} "${main_ranges}")
  # The update download: a made-up newer release is offered after the start (no network).
  script(upd 90 "update")
  run(${layout}_update ${layout} 1 "${upd_script}" --fake-update 99.0.0)
  steady(${layout}_update "${upd_ranges}")
endforeach()
# At 2x too (Studio, interface scale 2): the fixed widths scale with it.
script(main 20 "${popups}")
run(studio2x studio 2 "${main_script}")
steady(studio2x "${main_ranges}")
if(DEFINED SHOTS)
  file(MAKE_DIRECTORY "${SHOTS}")
  file(GLOB shots "${OUT}/shot_*.ppm")
  file(COPY ${shots} DESTINATION "${SHOTS}")
endif()
message(STATUS "progsize: every progress window kept its size and place")
