# Gridline - K-459 (0.7.9): no "conflicting ID" error in any built-in plugin editor, in any layout,
# nor in the main windows, dialogs and menus. User report: hovering Isoline's OSC A / OSC B
# wavetable graphs showed Dear ImGui's "Programmer error: 2 visible items with conflicting ID!"
# popup (both graphs were one InvisibleButton "##table" under one ID stack).
# Dear ImGui (1.92) finds two items with one ID when one of them is hovered; the app's dev commands
# (IdSweep.h) read that state every frame and sweep the pointer over every visible item of some
# windows ("idsweep <windows>"), also naming one ID drawn twice in a frame without hovering
# ("static"); "idsweep <windows> scan" checks every item that way, the pointer only over what opens. Runs the app hidden (isolated config and HOME, SDL's dummy audio, a fixed frame time,
# frames laid out, not drawn) and checks, every sweep finding no conflicting or doubled ID:
#   0. the list of built-in plugin types ("plugwin types");
#   1. (PART studio, live = Clips, logic = Smooth, tracker; one run) in that layout every built-in
#      instrument's editor (channel 0's instrument replaced, "plugwin inst"), the channel window's
#      Misc and Envelope pages, every note processor ("plugwin notefx"), every effect's editor
#      (mixer track 1, slot 0, "plugwin pick"), every window open, the dialogs (every settings page,
#      about, shortcuts, export, info, plugin manager, groove, master, live pads: "show") and every
#      menu (main and right-click menus, "devmenu", each submenu opened), each sweep named;
#      nothing may be drawn outside any window (Dear ImGui's implicit "Debug##Default" window);
#   2. (studio) Isoline's two graphs hovered for real (Dear ImGui's own check), a scan drag on OSC B
#      is one undo step on OSC B only; the popup never shows with the users' setting.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -DPART=<studio|live|logic|tracker> -P k459_idconflict_editors_app.cmake
cmake_minimum_required(VERSION 3.16)
set(failures "")
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
# The test song with a sampler channel "Take" playing a file and an audio clip of it (clip 70), so
# the sample menus and the audio clip windows have something to show.
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)
set(chanJson "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clipJson "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chanJson}" demo "${demo}")
# (the audio clip goes second: the first clip stays a pattern clip, for the clip menu)
string(FIND "${demo}" "\"playlist\": [" pl)
string(SUBSTRING "${demo}" ${pl} -1 tail)
string(FIND "${tail}" "}," firstEnd)
math(EXPR cut "${pl} + ${firstEnd} + 2")
string(SUBSTRING "${demo}" 0 ${cut} head)
string(SUBSTRING "${demo}" ${cut} -1 tail)
set(demo "${head} ${clipJson}${tail}")
# an automation clip (of Take's first parameter) and a marker, for their menus
string(REPLACE "\"channels\": [" "\"automations\": [{\"id\": 71, \"name\": \"Sweep\", \"color\": 5219296, \"owner\": 60, \"index\": 0, \"points\": [[0, 0.2, 0, 0], [384, 0.8, 0, 0]]}], \"markers\": [{\"pos\": 0, \"name\": \"Intro\"}], \"channels\": [" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
set(SONG "${OUT}/song.gridline.json")
file(WRITE "${SONG}" "${demo}")

if(NOT DEFINED PART)
  set(PART studio)  # studio | live | logic | tracker
endif()

set(ENV{GRIDLINE_QUIET_WINDOWS} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

# run(tag layout script [app args...]): `out` is the app's output
function(run tag layout script)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${SONG}" --hidden --no-ui-sounds --no-draw --fixed-dt 16 --size 1440x900 --frames 400 ${ARGN}
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 1500)
  file(WRITE "${OUT}/${tag}.log" "${o}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

# A sweep of <windows> from frame f named <name> (the script waits for it); f moved on.
macro(sweep var name windows)
  string(JOIN " " more ${ARGN})
  list(APPEND names "${name}")
  string(APPEND ${var} "${f} idsweep \"${windows}\" ${more}\n")
  math(EXPR f "${f} + 1")
  string(APPEND ${var} "${f} waitfor idsweep-done 300\n")
  math(EXPR f "${f} + 2")
endmacro()
macro(finish var)
  string(APPEND ${var} "${f} idconflicts\n")
  math(EXPR f "${f} + 2")
  string(APPEND ${var} "${f} quit discard\n")
endmacro()

# Every sweep of the run (named in `names`, in order) ran to its end and found neither a conflicting
# ID (Dear ImGui's finding while hovering) nor one ID drawn twice; failures name the sweep and the
# labels. `minItems`: the fewest items each sweep must have gone over (it found the window).
function(clean what minItems)
  # (the output is cut at each "idsweep start" by position: CMake lists would not split it reliably)
  set(rest "\n${out}")
  list(LENGTH names nn)
  set(bad "")
  foreach(name ${names})
    string(FIND "${rest}" "\nidsweep start " at)
    if(at LESS 0)
      string(APPEND bad "  ${name}: the sweep never started\n")
      continue()
    endif()
    math(EXPR at "${at} + 15")
    string(SUBSTRING "${rest}" ${at} -1 rest)
    string(FIND "${rest}" "\nidsweep start " next)
    string(SUBSTRING "${rest}" 0 ${next} c)
    if(NOT c MATCHES "idsweep done [^\n]* conflicts ([0-9]+) static ([0-9]+) implicit ([0-9]+) checked ([0-9]+)")
      string(APPEND bad "  ${name}: the sweep did not end\n")
      continue()
    endif()
    set(nconf ${CMAKE_MATCH_1})
    set(nstat ${CMAKE_MATCH_2})
    set(nimpl ${CMAKE_MATCH_3})
    set(items ${CMAKE_MATCH_4})
    if(NOT nconf EQUAL 0 OR NOT nstat EQUAL 0 OR NOT nimpl EQUAL 0)
      string(REGEX MATCHALL "idconflict frame [^\n]*|idsweep static [^\n]*|idsweep   item [^\n]*|idsweep implicit [^\n]*" found "${c}")
      string(REPLACE ";" "\n    " found "${found}")
      string(APPEND bad "  ${name}: ${nconf} conflicting (hovered), ${nstat} drawn twice, ${nimpl} drawn outside any window\n    ${found}\n")
    elseif(items LESS minItems)
      string(APPEND bad "  ${name}: only ${items} items checked (at least ${minItems})\n")
    endif()
  endforeach()
  if(bad STREQUAL "" AND NOT out MATCHES "idconflicts frame [0-9]+ conflicts 0 ids 0")
    string(REGEX MATCH "idconflict frame [^\n]*" first "${out}")
    set(bad "  Dear ImGui reported a conflicting ID outside the sweeps: ${first}\n")
  endif()
  if(NOT bad STREQUAL "")
    message(STATUS "FAIL - ${what}:\n${bad}")
    set(failures "${failures}${what}:\n${bad}" PARENT_SCOPE)  # (every run goes on; the test fails at the end)
  else()
    message(STATUS "ok - ${what}: ${nn} sweeps, no conflicting or doubled ID")
  endif()
endfunction()

# ---- 0: the built-in plugin types
run(types studio "4 plugwin types\n6 quit discard\n")
string(REGEX MATCHALL "plugwin type gen [^ \n]+" gens "${out}")
string(REGEX MATCHALL "plugwin type fx [^ \n]+" fxs "${out}")
string(REGEX MATCHALL "plugwin type notefx [^ \n]+" notefxs "${out}")
list(TRANSFORM notefxs REPLACE "plugwin type notefx " "")
list(TRANSFORM gens REPLACE "plugwin type gen " "")
list(TRANSFORM fxs REPLACE "plugwin type fx " "")
list(LENGTH gens ng)
list(LENGTH fxs nf)
if(ng LESS 20 OR nf LESS 40)
  message(FATAL_ERROR "FAIL: too few built-in types listed (${ng} instruments, ${nf} effects)\n${out}")
endif()
list(FIND gens "gl.wavetable" wt)
if(wt LESS 0)
  message(FATAL_ERROR "FAIL: Isoline (gl.wavetable) is not listed")
endif()
message(STATUS "ok - ${ng} instruments, ${nf} effects")

# Where a channel's instrument shows (the sampler: the Audio clip window, Smooth's lower pane, Tracker's
# device chain; the others: the channel window) and an insert's editor, in every layout.
set(genWin "###channelsettings|Audio clip|Loupe|Device chain")
set(fxWin "###fxeditor|Loupe")

# ---- 1: in one layout, one run: every instrument and its Misc / Envelope pages, every note
# processor, every effect, every window open, the dialogs and every menu. Each is a "scan" sweep:
# every item of the windows checked for a doubled ID (two places, one ID: what Dear ImGui's popup
# reports once one of them is hovered) and for items drawn outside any window, the pointer only over
# what opens something (submenus), a few frames each.
if(PART MATCHES "^(studio|live|logic|tracker)$")
  set(layout ${PART})
  set(names "")
  set(f 10)
  set(s "")
  # (menus and dialogs first: the editors below replace channel 0, the sampler the sample menus need)
  # the dialogs and windows the menus open: about, shortcuts, export, project info, plugin manager,
  # groove, master, live pads, every settings page
  set(dialogs about shortcuts export info plugins groove master livepads)
  foreach(k RANGE 0 12)
    list(APPEND dialogs settings:${k})
  endforeach()
  foreach(d ${dialogs})
    string(APPEND s "${f} show ${d}\n")
    math(EXPR f "${f} + 3")
    sweep(s "${layout} ${d}" "*" scan)
  endforeach()
  string(APPEND s "${f} show none\n")
  math(EXPR f "${f} + 2")
  # every menu (main menu, right-click menus), each submenu opened by the pointer
  set(menus file edit add patterns view layouts options tools help channel chsample rack pattern track clip playlist piano pianotools
            pianoselect marker markers mixer slot knob plugin record typing patclip device arrclip audioclip chop timesel audiosource autosource
            patternsource)
  foreach(m ${menus})
    string(APPEND s "${f} devmenu ${m}\n")
    math(EXPR f "${f} + 5")
    sweep(s "${layout} menu ${m}" "##Popup|###Menu_|##Menu_" scan 4)
  endforeach()
  string(APPEND s "${f} devmenu off\n")
  math(EXPR f "${f} + 2")
  foreach(t ${gens})
    string(APPEND s "${f} plugwin inst 0 ${t}\n")
    math(EXPR f "${f} + 2")
    sweep(s "${layout} instrument ${t}" "${genWin}" scan)
  endforeach()
  string(APPEND s "${f} plugwin inst 0 gl.wavetable\n")
  math(EXPR f "${f} + 2")
  foreach(tab 1 2)
    string(APPEND s "${f} plugwin tab ${tab}\n")
    math(EXPR f "${f} + 2")
    sweep(s "${layout} channel window page ${tab}" "${genWin}" scan)
  endforeach()
  foreach(t ${notefxs})
    string(APPEND s "${f} plugwin notefx 0 ${t}\n")
    math(EXPR f "${f} + 2")
    sweep(s "${layout} note processor ${t}" "${genWin}" scan)
  endforeach()
  string(APPEND s "${f} plugwin tab 0\n")
  math(EXPR f "${f} + 2")
  foreach(t ${fxs})
    string(APPEND s "${f} plugwin pick 1 0 ${t}\n")
    math(EXPR f "${f} + 2")
    sweep(s "${layout} effect ${t}" "${fxWin}" scan)
  endforeach()
  sweep(s "${layout} every window" "*" scan)
  finish(s)
  run(all_${layout} ${layout} "${s}")
  clean("${layout}: every editor, window, dialog and menu" 1)
endif()

# ---- 1b: Studio, Isoline's two wavetable graphs (the report): hovered, no conflict; a drag across
# OSC B's graph scans OSC B's position only (parameter 23; OSC A's is 8), one undo step takes it back.
# (each graph is 62 px above its oscillator's Position knob, 82 high: 40 px above the knob's centre and
# 188 px right of it is inside the graph: "atparam <k> dx dy")
if(PART STREQUAL "studio")
  run(isoline studio "10 plugwin inst 0 gl.wavetable
20 atparam 8 188 -40
22 atparam 23 188 -40
24 plug 8 23
26 down 0
30 atparam 23 260 -40
34 atparam 23 330 -40
36 up 0
40 plug 8 23
42 undo
46 plug 8 23
48 idconflicts
50 quit discard
")
  string(REGEX MATCHALL "plug frame [0-9]+: undo [0-9]+[^\n]*\n  param 8 [0-9.-]+\n  param 23 [0-9.-]+" p "${out}")
  list(LENGTH p np)
  if(NOT np EQUAL 3)
    message(FATAL_ERROR "FAIL: Isoline graph check: ${np} plug reports\n${out}")
  endif()
  foreach(k 0 1 2)
    list(GET p ${k} r)
    string(REGEX MATCH "undo ([0-9]+) of" _ "${r}")
    set(pos${k} ${CMAKE_MATCH_1})
    string(REGEX MATCH "param 8 ([0-9.-]+)" _ "${r}")
    set(a${k} ${CMAKE_MATCH_1})
    string(REGEX MATCH "param 23 ([0-9.-]+)" _ "${r}")
    set(b${k} ${CMAKE_MATCH_1})
  endforeach()
  math(EXPR want "${pos0} + 1")
  if(NOT a1 STREQUAL a0 OR b1 STREQUAL b0 OR NOT pos1 EQUAL want)
    message(FATAL_ERROR "FAIL: a drag on OSC B's graph should move OSC B's position only, in one undo step (A ${a0} -> ${a1}, B ${b0} -> ${b1}, undo ${pos0} -> ${pos1})")
  endif()
  if(NOT b2 STREQUAL b0 OR NOT a2 STREQUAL a0)
    message(FATAL_ERROR "FAIL: undo should restore OSC B's position (${b2}, want ${b0})")
  endif()
  if(NOT out MATCHES "idconflicts frame [0-9]+ conflicts 0 ids 0")
    string(REGEX MATCH "idconflict frame [^\n]*" first "${out}")
    message(FATAL_ERROR "FAIL: Isoline's graphs hovered: conflicting ID: ${first}")
  endif()
  message(STATUS "ok - Isoline: both graphs hovered without a conflict; OSC B's scan drag (${b0} -> ${b1}) is one undo step")
endif()

# ---- 1c: users never see Dear ImGui's popup: with the setting as users get it (GRIDLINE_DEV_IDS=0)
# two buttons of one ID ("idsweep demo") are still found, without the popup; scripted runs show it.
if(PART STREQUAL "studio")
  set(demo "4 idsweep demo 2000\n10 idsweep \"idsweep demo\"\n11 waitfor idsweep-done 200\n13 idconflicts\n15 quit discard\n")
  run(popup_dev studio "${demo}")
  if(NOT out MATCHES "idconflict frame [0-9]+ id 0x[0-9A-F]+ tooltip-count 2 drawn 2 [^\n]*label \"twin##same\"" OR NOT out MATCHES "idconflicts [^\n]* popup-frames [1-9]")
    message(FATAL_ERROR "FAIL: a scripted run should show Dear ImGui's conflict popup (tests and development)\n${out}")
  endif()
  set(ENV{GRIDLINE_DEV_IDS} "0")
  run(popup_user studio "${demo}")
  unset(ENV{GRIDLINE_DEV_IDS})
  if(NOT out MATCHES "idconflict frame [0-9]+ id 0x[0-9A-F]+ tooltip-count 2 drawn 2 [^\n]*label \"twin##same\"" OR NOT out MATCHES "idconflicts [^\n]* popup-frames 0\n")
    message(FATAL_ERROR "FAIL: with the users' setting the conflict should be found but no popup shown\n${out}")
  endif()
  message(STATUS "ok - the conflict popup: shown in scripted runs, never with the users' setting (still found)")
endif()

# ---- 1d: the check for items drawn outside any window finds one: a button drawn at the root of the
# frame ("idsweep demo stray") goes into Dear ImGui's implicit "Debug" window, which then shows.
if(PART STREQUAL "studio")
  run(stray studio "4 idsweep demo stray 2000\n10 idsweep \"*\" scan\n11 waitfor idsweep-done 300\n13 quit discard\n")
  if(NOT out MATCHES "idsweep implicit frame [0-9]+ id 0x[0-9A-F]+ label \"stray##k459\"" OR NOT out MATCHES "idsweep done [^\n]* implicit [1-9]")
    message(FATAL_ERROR "FAIL: a button drawn outside any window should be found\n${out}")
  endif()
  message(STATUS "ok - the check finds an item drawn outside any window (the demo's)")
endif()

if(NOT failures STREQUAL "")
  message(FATAL_ERROR "FAIL: conflicting IDs\n${failures}")
endif()
