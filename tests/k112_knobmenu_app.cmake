# Gridline - K-112c: the Live knob menu's own rows, through the menu's code (dev "knobmenu", on the
# first channel's volume), in the real app (hidden, isolated config and HOME):
#   1. Show automation in a new lane: an automation clip on a playlist track below every used one,
#      the track named after the control, one undo step;
#   2. a second time: another new track, never beside the first clip;
#   3. Re-enable automation: the control's muted automation clips play again;
#   4. Copy the parameter's name: the name on the clipboard.
# (Which menu shows the rows, Live's alone, is pinned by k112_ctxmenus: ctxmenu_<style>_knob.txt.)
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112_knobmenu_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"live\", \"keys.profile\": \"live\", \"ui.menuStyle\": \"live\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
file(WRITE "${OUT}/in.txt" "6 knobmenu state\n8 knobmenu newlane\n10 knobmenu newlane\n12 knobmenu mute\n14 knobmenu reenable\n16 knobmenu copyname\n")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames 20 --screenshot "${OUT}/end.ppm"
                        --dev-input "${OUT}/in.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
file(WRITE "${OUT}/run.out" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${out}")
endif()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
# One frame's "knobmenu" line, its fields as <var>_clips, _muted, _track, _used, _name, _clip, _undo.
function(at var frame)
  string(REGEX MATCH "knobmenu frame ${frame}: clips ([0-9]+) muted ([0-9]+) track (-?[0-9]+) used ([0-9]+) name \"([^\"]*)\" clipboard \"([^\"]*)\" undo \"([^\"]*)\"" m "${out}")
  if(m STREQUAL "")
    message(FATAL_ERROR "FAIL: no knobmenu line at frame ${frame}\nOutput:\n${out}")
  endif()
  set(${var}_clips "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${var}_muted "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${var}_track "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(${var}_used "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(${var}_name "${CMAKE_MATCH_5}" PARENT_SCOPE)
  set(${var}_clip "${CMAKE_MATCH_6}" PARENT_SCOPE)
  set(${var}_undo "${CMAKE_MATCH_7}" PARENT_SCOPE)
endfunction()

at(s0 6)
at(s1 8)
at(s2 10)
at(s3 12)
at(s4 14)
at(s5 16)
math(EXPR c1 "${s0_clips} + 1")
math(EXPR c2 "${s0_clips} + 2")
check("new lane: one automation clip more (${s0_clips} -> ${s1_clips})" s1_clips EQUAL c1)
check("... on the track below every used one (${s1_track}, ${s0_used} used before)" s1_track EQUAL s0_used)
check("... the track named after the control (${s1_name})" s1_name)
set(want "create automation clip")
check("... one undo step (${s1_undo})" s1_undo STREQUAL want)
check("again: another clip (${s2_clips})" s2_clips EQUAL c2)
math(EXPR t2 "${s1_track} + 1")
check("... on a new track again (${s2_track})" s2_track EQUAL t2)
check("muted: the control's clips are off (${s3_muted})" s3_muted EQUAL s3_clips AND s3_muted GREATER 0)
check("re-enable: none muted (${s4_muted})" s4_muted EQUAL 0)
check("copy name: on the clipboard (${s5_clip})" s5_clip STREQUAL s1_name)
