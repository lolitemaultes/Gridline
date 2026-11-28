# Gridline - 0.7.8: no "conflicting ID" error in the windows most used. Dear ImGui (1.92) finds two
# items with one ID when one of them is hovered (ItemHoverable counts last frame's hovered ID; more
# than one sets DebugDrawIdConflictsId and draws the "Programmer error: N visible items with
# conflicting ID!" tooltip). The app's dev commands (IdSweep.h) read that state every frame
# ("idconflicts": the count since the start) and sweep the pointer over every visible item of some
# windows, one a frame, scrolling them to their end ("idsweep <windows>"). Runs the app hidden
# (isolated config and HOME, SDL's dummy audio, a fixed frame time; frames laid out, not drawn: the
# finding is Dear ImGui's item state, and a software renderer's long sweeps came to a run's whole
# time limit on a loaded ARM runner) on a copy of the test song with
# a sampler channel "Take" playing an audio clip (clip 70) and checks, each sweep finding no conflict:
#   0. the check finds one: two buttons of one ID ("idsweep demo") are reported, with their label;
#   1. Studio, the Audio clip window's Main view (user report rc11: Time stretching's engine chips
#      and FX space's Room / Hall shared one ID), then its Advanced view, every tab;
#   2. Studio, every window open with the clip (control bar, browser, channel rack, mixer, playlist);
#   3. the LFO board popped out with an LFO, a Chaos and a Random card;
#   4. a knob's right-click menu, its Modulate submenu included;
#   5. channel settings (Clap, Bass) and a mixer slot's menu and effect list;
#   6. Smooth: the lower pane's Audio editor (every section open) and the inspector;
#   7. Tracker: the Advanced clip page (every tab), the classic Sample editor.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k078_idconflict_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)
set(chanJson "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clipJson "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chanJson}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clipJson}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

set(ENV{GRIDLINE_QUIET_WINDOWS} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

# run(tag layout sampleView script [app args...])
function(run tag layout view script)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\", \"tracker.sampleView\": \"${view}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --no-draw --fixed-dt 16 --size 1440x900 --frames 400 ${ARGN}
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${o}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# A sweep of <windows> from frame f (the script waits for it): its lines appended to `var`, f moved on.
# (extra arguments: the most items, frames on each: a menu row opens its submenu)
macro(sweep var windows)
  string(JOIN " " more ${ARGN})
  string(APPEND ${var} "${f} idsweep \"${windows}\" ${more}\n")
  math(EXPR f "${f} + 1")
  string(APPEND ${var} "${f} waitfor idsweep-done 200\n")
  math(EXPR f "${f} + 2")
endmacro()
macro(finish var)
  string(APPEND ${var} "${f} idconflicts\n")
  math(EXPR f "${f} + 2")
  string(APPEND ${var} "${f} quit discard\n")
endmacro()

# Every sweep of the run ended (`n` of them), none found a conflict, nor did Dear ImGui on any frame
# of the run; and the sweep went over `min` items at least of each window matching a pattern of `wins`.
function(clean what n wins)
  string(REGEX MATCHALL "idsweep done [^\n]*" done "${out}")
  list(LENGTH done got)
  check("${what}: every sweep ran to its end (${got} of ${n})" got EQUAL ${n})
  foreach(d ${done})
    check("${what}: no conflicting ID (${d})" d MATCHES " conflicts 0 ")
  endforeach()
  string(REGEX MATCH "idconflict frame [^\n]*" first "${out}")
  check("${what}: Dear ImGui reported no conflicting ID on any frame ${first}" "${out}" MATCHES "idconflicts frame [0-9]+ conflicts 0 ids 0")
  foreach(w ${wins})
    string(REGEX MATCH "^(.+)>=([0-9]+)$" _ "${w}")
    set(pat "${CMAKE_MATCH_1}")
    set(min "${CMAKE_MATCH_2}")
    string(REGEX MATCHALL "idsweep window \"[^\"\n]*${pat}[^\"\n]*\" items [0-9]+" lines "${out}")
    set(most 0)
    foreach(l ${lines})
      string(REGEX MATCH "items ([0-9]+)$" _ "${l}")
      if(CMAKE_MATCH_1 GREATER most)
        set(most ${CMAKE_MATCH_1})
      endif()
    endforeach()
    check("${what}: the sweep went over ${pat} (${most} items, at least ${min})" most GREATER_EQUAL ${min})
  endforeach()
endfunction()

# ---- 0: the check finds a conflict
set(f 10)
set(s "4 idsweep demo 2000\n")
sweep(s "idsweep demo")
finish(s)
run(demo studio advanced "${s}")
check("the check finds two buttons of one ID (the demo's)" "${out}" MATCHES "idconflict frame [0-9]+ id 0x[0-9A-F]+ tooltip-count 2 drawn 2 [^\n]* label \"twin##same\"")
check("and counts it" "${out}" MATCHES "idsweep done [^\n]* conflicts 1 " AND "${out}" MATCHES "idconflicts frame [0-9]+ conflicts [1-9][0-9]* ids 1 ")

# ---- 1: the Audio clip window, Main then Advanced (every tab)
set(f 20)
set(s "6 clipeditor dbl 70\n")
sweep(s "Audio clip")
string(APPEND s "${f} clipeditor view advanced\n")
math(EXPR f "${f} + 4")
foreach(t RANGE 0 11)
  string(APPEND s "${f} clipeditor tab ${t}\n")
  math(EXPR f "${f} + 4")
  sweep(s "Audio clip")
endforeach()
finish(s)
run(clipwindow studio advanced "${s}")
check("the clip opens on the Main view" "${out}" MATCHES "idsweep window \"Audio clip###Audio clip/##cemain")
clean("Studio, Audio clip window" 13 "##cemain>=60;Audio clip###Audio clip>=20")

# ---- 2: Studio, every window open with the clip
set(f 30)
set(s "6 clipeditor dbl 70\n")
sweep(s "*")
finish(s)
run(studio studio advanced "${s}")
clean("Studio, every window" 1 "##topbar>=20;##rows>=100;Mixer###Mixer/##strips>=30;Browser###Browser/##tree>=20;##pltb>=10")

# ---- 3: the LFO board with Chaos and Random cards, popped out
set(f 30)
set(s "2 lfo new\n2 lfo new chaos\n2 lfo new random\n3 lfo link 1 20 1 0.4\n4 lfo open\n12 lfo popout\n")
sweep(s "LFO board")
finish(s)
run(lfo studio advanced "${s}")
# (0.7.9: the rack holds the cards and the selected LFO's inspector as two child windows; the
# sweep goes into both, at least 40 items between them as it went over 40 in the one rack before)
clean("the LFO board" 1 "LFO board/##rack[^/\"]*/##cards>=15;LFO board/##rack[^/\"]*/##inspector>=25;LFO board/##lfotools>=5")

# ---- 4: a knob's right-click menu (Modulate: New chaos, New random, ...)
set(f 30)
set(s "")
sweep(s "##Popup|###Menu_" 3000 4)
finish(s)
run(knobmenu studio advanced "${s}" --menu knob)
clean("the knob menu" 1 "##Popup>=8;Modulate###Menu_>=4;Macro###macro>=4;Chaos###src>=1;Random###src>=1")

# ---- 5: channel settings, a mixer slot's menu and effect list
foreach(c 2 6)
  set(f 30)
  set(s "6 plugwin chan ${c}\n")
  sweep(s "###channelsettings")
  finish(s)
  run(chan${c} studio advanced "${s}")
  clean("channel settings of channel ${c}" 1 "###channelsettings>=10")
endforeach()
set(f 30)
set(s "4 layout open Mixer\n20 sdlat fxslotempty\n22 sdldown 1\n23 sdlup 1\n")
sweep(s "###Menu_|##Popup" 3000 4)
finish(s)
run(slotmenu studio advanced "${s}")
clean("a mixer slot's menu" 1 "###Menu_>=10")
set(f 30)
set(s "4 layout open Mixer\n20 sdlat fxslotempty\n22 sdldown 0\n23 sdlup 0\n")
sweep(s "##Popup")
finish(s)
run(slotpick studio advanced "${s}")
clean("a mixer slot's effect list" 1 "##Popup>=10")

# ---- 6: Smooth, the lower pane's Audio editor and the inspector
set(f 20)
set(s "6 clipeditor dbl 70\n10 clipeditor unfold\n")
sweep(s "Loupe|Inspector")
finish(s)
run(smooth logic advanced "${s}")
clean("Smooth, the Audio editor and inspector" 1 "##celocal>=100;Inspector###Inspector/##part>=40")

# ---- 7: Tracker, the Advanced clip page (every tab) and the classic Sample editor
set(f 20)
set(s "6 clipeditor dbl 70\n")
foreach(t RANGE 0 11)
  string(APPEND s "${f} clipeditor tab ${t}\n")
  math(EXPR f "${f} + 4")
  sweep(s "Device chain")
endforeach()
finish(s)
run(tracker tracker advanced "${s}")
clean("Tracker, the Advanced clip page" 12 "##cetabmods>=10;Device chain>=20")
set(f 20)
set(s "6 clipeditor dbl 70\n")
sweep(s "Device chain")
finish(s)
run(trackerclassic tracker classic "${s}")
clean("Tracker, the classic Sample editor" 1 "Device chain/##part>=30")
