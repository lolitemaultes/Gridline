# Gridline - K-297 (round 9): each layout keeps its own window and region state, in the real app
# (hidden, isolated config folder and HOME, SDL's dummy audio driver, the development layouts):
#   1. switching layouts in one run: Smooth with its lower pane (the mixer) and library shown; then
#      Clips with the channel rack and the piano roll in its clip band, the piano roll in front, and
#      its mixer band open; back to Smooth, its regions and the tab in front of each are as they were
#      left; back to Clips, the same for Clips. Nothing of one layout leaks into the other (Clips'
#      first visit is a clean start's).
#   2. plugin windows across a restart (Studio and Smooth): the instrument and effect windows open
#      at save are written in the project (pluginWindows) and open again when it is opened in a new
#      run; a project saved with none open writes nothing new (an older project reads as before).
#   3. Settings (a dialog) is not reopened by a restart in Smooth, as in Studio.
#   cmake -DAPP=<gridline-app> -DDEMO=<song.gridline.json> -DOUT=<dir> -P k297_layout_state_app.cmake
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

# The regions of the layout dump at <frame>: "<region> <visible> <front>" lines, in `var`.
function(regions var frame)
  string(FIND "${out}" "LAYOUT BEGIN frame ${frame}\n" at)
  set(body "")
  if(at GREATER -1)
    string(SUBSTRING "${out}" ${at} -1 rest)
    string(FIND "${rest}" "LAYOUT END" end)
    string(SUBSTRING "${rest}" 0 ${end} body)
  endif()
  if(body STREQUAL "")
    fail("no layout dump at frame ${frame}")
  endif()
  string(REGEX MATCHALL "region [^\n]*" lines "${body}")
  set(v "")
  foreach(l IN LISTS lines)
    string(REGEX MATCH "^region ([^ ]+) visible ([01]) .* front (.*)$" _ "${l}")
    list(APPEND v "${CMAKE_MATCH_1} ${CMAKE_MATCH_2} ${CMAKE_MATCH_3}")
  endforeach()
  set(${var} "${v}" PARENT_SCOPE)
endfunction()

set(song "${OUT}/song.gridline.json")
configure_file("${DEMO}" "${song}" COPYONLY)

# ---- 1. A -> B -> A -> B ----
# Smooth: the lower pane shown with the mixer in it, the library shown. Clips: the channel rack and
# the piano roll in the clip band, the piano roll in front (the channel rack opened first would be); the mixer band open.
set(s "5 toasts clear\n10 layout show logic.lower\n14 layout open Mixer\n18 layout show logic.library\n44 layout dump\n")
string(APPEND s "56 layout dump\n60 layout open Channel rack\n62 layout open Piano roll\n66 focus \"Piano roll\"\n68 layout open Mixer\n72 layout dump\n96 layout dump\n")
string(APPEND s "126 layout dump\n156 layout dump\n")
run(switch sw logic "${song}" "${s}" --frames 170 --switch-layout live@50 --switch-layout logic@100 --switch-layout live@130)
regions(smoothBefore 44)
regions(clipsFirst 56)
regions(clipsLeft 96)
regions(smoothBack 126)
regions(clipsBack 156)
foreach(want "logic.lower 1 Mixer")
  list(FIND smoothBefore "${want}" at)
  if(at LESS 0)
    fail("the Smooth set-up was not made (${want}): ${smoothBefore}")
  endif()
endforeach()
foreach(want "live.clip 1 Piano roll" "live.mixer 1 Mixer")
  list(FIND clipsLeft "${want}" at)
  if(at LESS 0)
    fail("the Clips set-up was not made (${want}): ${clipsLeft}")
  endif()
endforeach()
if(NOT smoothBack STREQUAL smoothBefore)
  fail("back in Smooth its regions and tabs in front changed:\n  left  ${smoothBefore}\n  found ${smoothBack}")
endif()
if(NOT clipsBack STREQUAL clipsLeft)
  fail("back in Clips its regions and tabs in front changed:\n  left  ${clipsLeft}\n  found ${clipsBack}")
endif()
# Clips' first visit is a clean one's (nothing of Smooth's set-up came with it).
run(clipsclean cl live "${song}" "5 toasts clear\n22 layout dump\n" --frames 30)
regions(clipsClean 22)
if(NOT clipsFirst STREQUAL clipsClean)
  fail("Clips' first visit after Smooth differs from a clean start in Clips:\n  clean ${clipsClean}\n  found ${clipsFirst}")
endif()

# ---- 2. plugin windows across a restart ----
foreach(lay studio logic)
  set(p "${OUT}/plug_${lay}.gridline.json")
  configure_file("${DEMO}" "${p}" COPYONLY)
  # The second channel's instrument and the master's first effect (the demo song has a compressor there).
  run(plug_${lay}_1 pw_${lay} ${lay} "${p}" "20 instview\n30 save ${p}\n" --chan 1 --fx 0 0 --frames 40)
  file(READ "${p}" saved)
  if(NOT saved MATCHES "\"pluginWindows\"")
    fail("${lay}: the open plugin windows were not saved in the project")
  endif()
  run(plug_${lay}_2 pw_${lay} ${lay} "${p}" "30 instview\n" --frames 40)
  if(NOT out MATCHES "instview frame 30: open \"[^\"]+\" type [^ ]+ [^\n]* fx 0")
    fail("${lay}: after a restart the instrument and effect windows did not open again:\n${out}")
  endif()
  # Closed and saved: nothing is written, and the next run opens none.
  run(plug_${lay}_3 pw_${lay} ${lay} "${p}" "10 closeplugins\n20 save ${p}\n" --frames 30)
  file(READ "${p}" saved)
  if(saved MATCHES "\"pluginWindows\"")
    fail("${lay}: a project saved with no plugin window open still lists some")
  endif()
  run(plug_${lay}_4 pw_${lay} ${lay} "${p}" "30 instview\n" --frames 40)
  if(NOT out MATCHES "instview frame 30: open \"-\" [^\n]* fx -1")
    fail("${lay}: a project saved with none open opened plugin windows:\n${out}")
  endif()
endforeach()

# ---- 3. dialogs after a restart; Settings' minimum size ----
set(ENV{GRIDLINE_SAVE_LAYOUT} "1")  # these runs keep their layout (their own config folder)
foreach(lay studio logic)
  run(dlg_${lay}_1 dlg_${lay} ${lay} "${song}" "5 toasts clear\n10 layout open Settings\n20 layout state\n" --frames 30)
  if(NOT out MATCHES "open \\[[^]]*Settings")
    fail("${lay}: Settings did not open")
  endif()
  run(dlg_${lay}_2 dlg_${lay} ${lay} "${song}" "30 layout state\n" --frames 40)
  if(out MATCHES "open \\[[^]]*Settings")
    fail("${lay}: a restart opened Settings again")
  endif()
endforeach()
unset(ENV{GRIDLINE_SAVE_LAYOUT})
foreach(lay studio live logic)
  run(min_${lay} min_${lay} ${lay} "${song}" "10 layout open Settings\n30 windows\n" --frames 40 --settings-size 100x100)
  if(NOT out MATCHES "window \"Settings\" pos [-0-9.,]+ size 660\\.0x360\\.0")
    fail("${lay}: Settings' smallest size is not 660 x 360:\n${out}")
  endif()
endforeach()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "k297_layout_state: ok")
