# Gridline - K-333: the Patch Player in the real app (hidden, isolated config and HOME, SDL's dummy
# audio driver), in the Studio, Clips, Smooth and Tracker layouts, with an SF2 bank the pack test writes:
#   1. a patch of the bank dragged from the browser onto the rack makes a Patch Player channel playing
#      it (named after it), and its window opens on the Patches tab;
#   2. the patch list's search, a row picked in the list, the Sound tab;
#   3. a drum kit dropped on that channel changes its patch, one undo step takes it back;
#   4. Sends to mixer routes the reverb and chorus sends to a reverb track and a chorus track.
# Screenshots of each layout's editor stay in OUT.
#   cmake -DAPP=<gridline-app> -DBANK=<gridline_test_patchplayer> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k333_patchplayer_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/lib" "${OUT}/hm")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{HOME} "${OUT}/hm")
set(SF "${OUT}/lib/Test Bank.sf2")
execute_process(COMMAND "${BANK}" --write "${SF}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${SF}")
  message(FATAL_ERROR "the test bank was not written (${rc})")
endif()
configure_file("${DEMO}" "${OUT}/fixture.gridline.json" COPYONLY)

function(run tag layout script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900
                          --frames 90 --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.out" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

foreach(layout studio live logic tracker)
  set(tag pp_${layout})
  run(${tag} ${layout} "4 browser drag \"${SF}#patch=0:48\"
6 patchplayer open
8 instview
10 patchplayer state
14 shot \"${OUT}/${tag}_patches.ppm\"
18 patchplayer find strings
22 shot \"${OUT}/${tag}_search.ppm\"
24 patchplayer pick 1
28 patchplayer state
30 patchplayer find
32 patchplayer tab 1
36 shot \"${OUT}/${tag}_sound.ppm\"
38 patchplayer tab 0
40 browser drag \"${SF}#patch=128:40\" sel
44 patchplayer state
46 undo
50 patchplayer state
52 patchplayer sends
56 patchplayer state
60 quit discard
")
  check("${layout}: the patch dragged onto the rack makes a Patch Player channel playing it"
        out MATCHES "browserdrag frame 4: [^\n]* -> New Patch Player channel\n" AND out MATCHES "PATCHPLAYER channel=Warm Strings bank=0 program=48 name=Warm Strings reverb=0.35")
  check("${layout}: its window opens" out MATCHES "instview frame 8: open \"Warm Strings\" type gl.patchplayer")
  check("${layout}: a row of the searched list plays (Slow Strings)" out MATCHES "PATCHPLAYER channel=[^\n]* bank=0 program=49 name=Slow Strings [^\n]*undo=\"change patch\"")
  check("${layout}: a drum kit dropped on the channel changes its patch"
        out MATCHES "browserdrag frame 40: [^\n]* -> Play this patch\n" AND out MATCHES "PATCHPLAYER channel=[^\n]* bank=128 program=40 name=Brush Kit")
  string(REGEX MATCHALL "PATCHPLAYER channel=[^\n]* program=([0-9]+) name=" all "${out}")
  list(LENGTH all n)
  check("${layout}: five state lines (${n})" n EQUAL 5)
  list(GET all 3 afterUndo)
  check("${layout}: undo takes the drop back in one step (${afterUndo})" afterUndo MATCHES "program=49 name=")
  check("${layout}: Sends to mixer routes reverb and chorus to tracks with Gridline's effects"
        out MATCHES "PATCHPLAYER [^\n]*sends=gl.reverb,gl.chorus undo=\"send reverb and chorus to mixer tracks\"")
endforeach()
message(STATUS "k333: the Patch Player from the browser, its editor, patch drops, undo and sends in every layout")
