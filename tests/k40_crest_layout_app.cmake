# Gridline - K-40: Crest's editor as drawn, in the real app (hidden, isolated config). The editor's
# layout audit (GL_CREST_TRACE, AlignCheck.h over the frame's controls and text runs) reports every
# overlap of controls or text, knob or control off its row or column, uneven gap in a row of knobs,
# caption off its knob or baseline, and text cut off. Checked in every view, band, mode and engine,
# at interface scales 100 % and 200 %, in both themes, at a small and a large window.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k40_crest_layout_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GL_CREST_TRACE} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")

# Parameter indices (dynamics.h): MsRef0 = 193; MrRaw0 = 194, a band's word = 201 + band * 13 + tag;
# solo 253, target 254, linear phase 258, oversampling 293; the older engine's master band curve-mode flag 167.
# name | GL_CREST_DEV | parameters and flags (comma separated)
set(states
    "monitor||"
    "bands|view=1|"
    "low||--param,254,0"
    "mid||--param,254,1"
    "high||--param,254,2"
    "master||--param,254,3"
    "nocomp||--param,254,1,--param,214,1"
    "mute||--param,254,1,--param,214,2"
    "off||--param,254,1,--param,214,3"
    "rms||--param,254,1,--param,226,1"
    "linear||--param,258,1"
    "oversampled||--param,293,3"
    "solo||--param,254,1,--param,253,1"
    "paused|pause=1|"
    "signal|demo=1|"
    "signalbands|view=1,demo=1|"
    "points|menu=2|"
    "options|menu=1|"
    "linmenu|menu=3|--param,258,1"
    "older||--param,193,0"
    "olderdrawn||--param,193,0,--param,167,1"
    "preset||--preset,1")
set(count 0)
function(check name dev)
  set(ENV{GL_CREST_DEV} "${dev}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --plugin gl.master --frames 20 --screenshot "${OUT}/frame.ppm" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL ${name}: gridline-app failed (${rc}):\n${out}\n${err}")
  endif()
  string(REGEX MATCHALL "crest-audit:[^\n]*" lines "${out}")
  list(LENGTH lines n)
  if(n EQUAL 0)
    message(FATAL_ERROR "FAIL ${name}: the editor did not report its layout:\n${out}\n${err}")
  endif()
  foreach(l IN LISTS lines)
    if(NOT l MATCHES " findings=0 " OR NOT l MATCHES " inset=0 ")
      string(REGEX MATCHALL "crest-finding:[^\n]*" found "${out}")
      string(REPLACE ";" "\n" found "${found}")
      message(FATAL_ERROR "FAIL ${name}: ${l}\n${found}")
    endif()
    if(NOT l MATCHES "controls=([0-9]+) texts=([0-9]+)" OR CMAKE_MATCH_1 LESS 30 OR CMAKE_MATCH_2 LESS 20)
      message(FATAL_ERROR "FAIL ${name}: the audit saw too little: ${l}")
    endif()
  endforeach()
endfunction()

foreach(s ${states})
  string(REPLACE "|" ";" f "${s};")
  list(GET f 0 name)
  list(GET f 1 dev)
  list(GET f 2 extra)
  string(REPLACE "," ";" extra "${extra}")
  check("${name}@100" "${dev}" --size 1440x900 ${extra})
  check("${name}@200" "${dev}" --ui-scale 2 --size 2880x1800 ${extra})
  check("${name}@light" "${dev}" --light --size 1440x900 ${extra})
  math(EXPR count "${count} + 3")
endforeach()
check("small window" "" --size 800x600)
check("large window" "" --size 2560x1400)
check("large window, bands" "view=1" --size 2560x1400)
math(EXPR count "${count} + 3")

# The controls answer where they are drawn: each click changes what it should (read back from the state).
set(ENV{GL_CREST_DEV} "")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --plugin gl.master --size 1440x900 --frames 12 --screenshot "${OUT}/frame.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
string(REGEX MATCH "crest-ui:[^\n]*" l "${out}")
if(NOT rc EQUAL 0 OR NOT l)
  message(FATAL_ERROR "FAIL: no place report (${rc}):\n${out}\n${err}")
endif()
function(place name out)
  if(NOT l MATCHES " ${name}=([0-9]+),([0-9]+)")
    message(FATAL_ERROR "FAIL: no ${name} in ${l}")
  endif()
  set(${out} "${CMAKE_MATCH_1} ${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()
place(tab1 pTab)
place(mode2 pMode)
place(solo pSolo)
place(lin pLin)
place(det1 pDet)
place(slope0 pSlope)
place(bandsview pView)
place(pause pPause)
set(script "")
set(fr 20)
foreach(p "${pTab}" "${pMode}" "${pSolo}" "${pLin}" "${pDet}" "${pSlope}" "${pView}" "${pPause}")
  math(EXPR f1 "${fr} + 2")
  math(EXPR f2 "${fr} + 4")
  string(APPEND script "${fr} move ${p}\n${f1} down 0\n${f2} up 0\n")
  math(EXPR fr "${fr} + 12")
endforeach()
file(WRITE "${OUT}/clicks.txt" "${script}")
math(EXPR last "${fr} + 16")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --plugin gl.master --size 1440x900 --frames ${last} --screenshot "${OUT}/frame.ppm" --dev-input "${OUT}/clicks.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
string(REGEX MATCHALL "crest-ui:[^\n]*" lines "${out}")
list(LENGTH lines n)
if(NOT rc EQUAL 0 OR n EQUAL 0)
  message(FATAL_ERROR "FAIL: the click run (${rc}):\n${out}\n${err}")
endif()
math(EXPR n "${n} - 1")
list(GET lines ${n} final)
foreach(want "band=1" "view=1" "paused=1" "mode=2" "solo=1" "lin=1" "detector=1" "low=0")
  if(NOT final MATCHES " ${want} ")
    message(FATAL_ERROR "FAIL: a click did not reach its control (${want}): ${final}")
  endif()
endforeach()
message(STATUS "ok - ${count} Crest layouts as drawn: no overlaps, rows and columns aligned, no clipped text")
