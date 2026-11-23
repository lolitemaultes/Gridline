# Gridline - k078-songlinks: the "Remote control settings" dialog offers an internal controller only
# where the engine applies one (the real app, Studio layout, hidden, an isolated config folder and
# HOME, SDL's dummy audio driver). The song is the test song plus an internal link on swing from
# channel 1's first macro, as a project saved before this change could hold.
#   1. on swing the dialog shows the note and the old link with its Remove button, no controller choice;
#   2. Remove takes the link away in one undo step and leaves the note;
#   3. on channel 1's volume the controller choice is offered; on tempo only the note; an action
#      target (Play) has no such section.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> [-DSHOTS=<dir>] -P k078_songlinks_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{HOME} "${OUT}/home")

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n(the run log: ${OUT}/run.log)")
endfunction()

file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"studio\"}}\n")
file(READ "${DEMO}" song)
# Channel id 20 is the test song's first channel; owner 1 is the song, index 1 its swing.
string(REPLACE "\"links\": [],"
               "\"links\": [],\n  \"modLinks\": [{\"sourceOwner\": 20, \"sourceSlot\": -2, \"sourceOutput\": 0, \"owner\": 1, \"index\": 1, \"min\": 0, \"max\": 1, \"curve\": 0, \"smoothMs\": 0, \"enabled\": true}],"
               song "${song}")
if(NOT song MATCHES "\"modLinks\": \\[{")
  fail("could not add the swing link to the test song")
endif()
file(WRITE "${OUT}/song.gridline.json" "${song}")
if(NOT SHOTS)
  set(SHOTS "${OUT}")
endif()
file(WRITE "${OUT}/input.txt" "10 linkctl open song swing
13 linkctl state
14 shot \"${SHOTS}/swing-linked.png\"
15 linkctl remove-internal
17 linkctl state
18 shot \"${SHOTS}/swing.png\"
19 linkctl cancel
21 linkctl open channel 0
24 linkctl state
25 shot \"${SHOTS}/channel.png\"
26 linkctl cancel
28 linkctl open song tempo
30 linkctl state
31 linkctl cancel
33 linkctl open action play
35 linkctl state
36 linkctl cancel
38 quit discard
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --layout studio ${EXTRA}
                        --dev-input "${OUT}/input.txt" --frames 300
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE e TIMEOUT 200)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${e}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed (${rc})")
endif()

function(expect what re)
  if(NOT out MATCHES "${re}")
    fail("${what} (wanted /${re}/)")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

expect("the old swing link loads; on swing the dialog shows it with Remove and the note, no controller choice"
       "linkctl frame 13: open 1 [^\n]*target \"Swing\" internal note-linked modlinks 1 undo ([0-9]+)\n")
set(u0 ${CMAKE_MATCH_1})
math(EXPR u1 "${u0} + 1")
expect("Remove takes the link away in one undo step; the note stays"
       "linkctl frame 17: open 1 [^\n]*target \"Swing\" internal note modlinks 0 undo ${u1}\n")
expect("a channel's volume offers the internal controller choice"
       "linkctl frame 24: open 1 [^\n]*target \"[^\"]+ Volume\" internal offered modlinks 0 ")
expect("tempo shows only the note" "linkctl frame 30: open 1 [^\n]*target \"Tempo\" internal note modlinks 0 ")
expect("an action target has no internal controller section" "linkctl frame 35: open 1 [^\n]*internal none ")
foreach(s swing-linked swing channel)
  if(NOT EXISTS "${SHOTS}/${s}.png")
    fail("no ${s}.png")
  endif()
endforeach()
message(STATUS "ok - screenshots")
