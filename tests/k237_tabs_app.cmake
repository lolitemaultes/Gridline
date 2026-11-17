# Gridline - K-237: pane tabs between panes, the side panel rail, and the workspaces copy in a
# project, in the real app (hidden, isolated config folder and HOME, SDL's dummy audio driver, the
# development layouts):
#   1. a tab dragged with the pointer from Smooth's right pane onto the lower pane moves its window
#      there, and dragged back it goes home (nothing kept);
#   2. each layout keeps its own tab state across a layout switch and a restart: a tab moved in Smooth
#      and the side panel rail's order in Clips;
#   3. an older project (no workspaces copy) opens unchanged and saves without one; with the copy
#      turned on it is written, and a person without workspaces of their own gets them when opening it.
#   cmake -DAPP=<gridline-app> -DDEMO=<song.gridline.json> -DOUT=<dir> -P k237_tabs_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_SAVE_LAYOUT} "1")  # the restart checks read what the run before left
set(failures 0)
macro(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR failures "${failures} + 1")
endmacro()

# run(<tag> <cfg> <layout> <song> <script> [args...]): config ${OUT}/cfg_<cfg> (kept between runs of one cfg).
function(run tag cfg layout song script)
  set(c "${OUT}/cfg_${cfg}")
  if(NOT EXISTS "${c}/gridline/settings.json")
    file(MAKE_DIRECTORY "${c}/gridline" "${c}/home")
    file(WRITE "${c}/gridline/settings.json"
         "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\"}}\n")
  endif()
  set(ENV{GRIDLINE_CONFIG_HOME} "${c}")
  set(ENV{HOME} "${c}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${song}" --hidden --layout-dev --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900
                          --dev-input "${OUT}/${tag}.txt" --screenshot "${OUT}/${tag}.ppm" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL: ${tag}: gridline-app failed (${rc}):\n${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

# startIn(<cfg> <layout>): the next run of config <cfg> starts in <layout> (as a restart there).
function(startIn cfg layout)
  file(READ "${OUT}/cfg_${cfg}/gridline/settings.json" t)
  string(REGEX REPLACE "\"ui.layout\": *\"[a-z]+\"" "\"ui.layout\": \"${layout}\"" t "${t}")
  file(WRITE "${OUT}/cfg_${cfg}/gridline/settings.json" "${t}")
endfunction()

# The region a window is docked in at <frame> ("-" when not drawn), in `var`.
function(where var frame window)
  string(REGEX MATCH "frame ${frame} where ${window} layout [a-z]+ drawn ([01]) docked [01] region ([^ ]+)" _ "${out}")
  if(CMAKE_MATCH_1 STREQUAL "1")
    set(${var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
  else()
    set(${var} "-" PARENT_SCOPE)
  endif()
endfunction()
# A pane's tabs line at <frame> ("order;pinned=..;active=.."), in `var`.
function(tabs var frame region)
  string(REGEX MATCH "frame ${frame} tabs ${region} [a-z]+ \\[([^]\n]*)\\]" _ "${out}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
function(moved var frame)
  string(REGEX MATCH "frame ${frame} tabs moved \\[([^]\n]*)\\]" _ "${out}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

set(song "${OUT}/song.gridline.json")
configure_file("${DEMO}" "${song}" COPYONLY)

# ---- 1. drag A -> B and back ----
# Smooth: the right pane with Lists, the lower pane with the mixer. A pointer drag (SDL events through
# the backend) takes the Lists tab onto the lower pane's strip, then from there back onto the right pane.
set(s "5 toasts clear\n10 layout show logic.right\n12 layout open Lists\n16 layout show logic.lower\n18 layout open Mixer\n")
string(APPEND s "40 layout where Lists\n40 layout tabs\n")
string(APPEND s "42 layout tabtarget Lists|logic.lower\n46 sdlat ptabsrc\n48 sdldown\n52 sdlat ptabdst\n56 sdlat ptabdst\n60 sdlup\n")
string(APPEND s "90 layout where Lists\n90 layout tabs\n90 layout where Mixer\n")
string(APPEND s "92 layout tabtarget Lists|logic.right\n96 sdlat ptabsrc\n98 sdldown\n102 sdlat ptabdst\n106 sdlat ptabdst\n110 sdlup\n")
string(APPEND s "140 layout where Lists\n140 layout tabs\n140 layout where Mixer\n")
run(drag drag logic "${song}" "${s}" --frames 150)
where(a0 40 Lists)
where(a1 90 Lists)
where(a2 140 Lists)
moved(m1 90)
moved(m2 140)
tabs(lower1 90 logic.lower)
if(NOT a0 STREQUAL "logic.right")
  fail("Lists did not start in the right pane (${a0})")
endif()
if(NOT a1 STREQUAL "logic.lower" OR NOT m1 STREQUAL "Lists>logic.lower" OR NOT lower1 MATCHES "Lists")
  fail("the drag A -> B did not move Lists to the lower pane: region ${a1}, moved [${m1}], lower tabs [${lower1}]")
endif()
if(NOT a2 STREQUAL "logic.right" OR NOT m2 STREQUAL "")
  fail("the drag back B -> A did not take Lists home: region ${a2}, moved [${m2}]")
endif()
where(mx2 140 Mixer)
if(NOT mx2 STREQUAL "logic.lower")
  fail("the lower pane did not show the mixer again when Lists left it (${mx2})")
endif()

# ---- 2. per-layout state across a switch and a restart ----
# Smooth: Lists moved to the lower pane (the menu's Move to). Clips: the side panel with Lists and
# Notes on its rail, Notes moved first. Then Smooth again, then Clips again, then a restart in each.
set(s "5 toasts clear\n10 layout show logic.right\n12 layout open Lists\n16 layout tabmove Lists|logic.lower\n40 layout tabs\n")
string(APPEND s "60 layout show live.side\n62 layout open Lists\n64 layout open Notes\n80 layout tabs\n")
string(APPEND s "82 layout tabtarget Notes|tab:Lists\n86 sdlat ptabsrc\n88 sdldown\n92 sdlat ptabdst\n96 sdlat ptabdst\n100 sdlup\n120 layout tabs\n")
string(APPEND s "160 layout tabs\n160 layout where Lists\n200 layout tabs\n")
run(state st logic "${song}" "${s}" --frames 210 --switch-layout live@50 --switch-layout logic@140 --switch-layout live@180)
moved(smoothMoved 40)
tabs(rail0 80 live.side)
tabs(rail1 120 live.side)
moved(smoothBack 160)
where(smoothWhere 160 Lists)
tabs(railBack 200 live.side)
if(NOT smoothMoved STREQUAL "Lists>logic.lower")
  fail("Smooth: the Move to did not move Lists (${smoothMoved})")
endif()
if(NOT rail0 MATCHES "^Lists\\|Notes" AND NOT rail0 MATCHES "^Notes\\|Lists")
  fail("Clips: the side panel rail does not list Lists and Notes: [${rail0}]")
endif()
string(REGEX MATCH "^[^;]*" order1 "${rail1}")
string(REGEX MATCH "^[^;]*" order0 "${rail0}")
if(order1 STREQUAL order0)
  fail("Clips: dragging Notes up the rail did not reorder it: [${rail0}] -> [${rail1}]")
endif()
if(NOT smoothBack STREQUAL "Lists>logic.lower" OR NOT smoothWhere STREQUAL "logic.lower")
  fail("back in Smooth the moved tab was lost: moved [${smoothBack}], Lists in ${smoothWhere}")
endif()
string(REGEX MATCH "^[^;]*" orderBack "${railBack}")
if(NOT orderBack STREQUAL order1)
  fail("back in Clips the rail order changed: [${rail1}] -> [${railBack}]")
endif()
# A restart in each layout (the same config folder).
# (the settings keep the layout a run ended in: each restart starts in the layout it checks)
startIn(st logic)
run(state_smooth st logic "${song}" "30 layout tabs\n30 layout where Lists\n" --frames 40)
moved(afterSmooth 30)
if(NOT afterSmooth STREQUAL "Lists>logic.lower")
  fail("after a restart Smooth lost the moved tab: [${afterSmooth}]")
endif()
startIn(st live)
run(state_clips st live "${song}" "10 layout show live.side\n12 layout open Lists\n14 layout open Notes\n40 layout tabs\n" --frames 80
    --switch-layout logic@50)
tabs(railRestart 40 live.side)
string(REGEX MATCH "^[^;]*" orderRestart "${railRestart}")
if(NOT orderRestart STREQUAL order1)
  fail("after a restart the Clips rail order changed: [${rail1}] -> [${railRestart}]")
endif()
# A restart in Clips, then a first visit to Smooth in that run: the tab moved there is still moved
# (a first visit opens the designed regions, the person's tab places stay).
startIn(st live)
run(state_first st live "${song}" "60 layout tabs\n" --frames 70 --switch-layout logic@30)
moved(firstVisit 60)
if(NOT firstVisit STREQUAL "Lists>logic.lower")
  fail("a first visit to Smooth after a restart lost the moved tab: [${firstVisit}]")
endif()

# ---- 3. an older project opens unchanged; the opt-in copy ----
file(READ "${DEMO}" demo)
if(demo MATCHES "\"workspaces\"")
  fail("the fixture already has a workspaces copy")
endif()
set(old "${OUT}/old.gridline.json")
configure_file("${DEMO}" "${old}" COPYONLY)
run(old_open old logic "${old}" "20 workspace dump\n20 layout state\n30 save ${OUT}/old_resaved.gridline.json\n" --frames 40)
string(REGEX MATCH "WORKSPACES logic active 0: \\[[^]]*\\] copy (on|off)" wsOld "${out}")
if(NOT wsOld MATCHES "copy off$" OR wsOld MATCHES "\\] \\[")
  fail("an older project changed the workspaces (${wsOld})")
endif()
file(READ "${OUT}/old_resaved.gridline.json" resaved)
if(resaved MATCHES "\"workspaces\"")
  fail("an older project saved again gained a workspaces copy")
endif()
# The same project with two workspaces and the copy on: saved with it.
set(withCopy "${OUT}/copy.gridline.json")
configure_file("${DEMO}" "${withCopy}" COPYONLY)
run(copy_write cw logic "${withCopy}" "10 workspace add Mixing\n14 workspace copy on\n20 workspace dump\n30 save ${withCopy}\n" --frames 40)
file(READ "${withCopy}" saved)
if(NOT saved MATCHES "\"workspaces\"" OR NOT saved MATCHES "Mixing")
  fail("the copy turned on was not written into the project")
endif()
# A person with no workspaces of their own opens it: the copy's set comes with it.
run(copy_read cr logic "${withCopy}" "30 workspace dump\n" --frames 40)
if(NOT out MATCHES "WORKSPACES logic active [0-9]+: [^\n]*\\[Mixing\\][^\n]* copy on")
  fail("opening a project with the copy did not give the workspaces:\n${out}")
endif()
# Someone with workspaces of their own keeps theirs.
run(copy_own_1 co logic "${old}" "10 workspace add Mine\n14 workspace add Mine too\n" --frames 30)
run(copy_own_2 co logic "${withCopy}" "30 workspace dump\n" --frames 40)
if(out MATCHES "\\[Mixing\\]" OR NOT out MATCHES "\\[Mine\\]")
  fail("the project's copy replaced the person's own workspaces:\n${out}")
endif()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "k237_tabs: ok")
