# Gridline - the Launchpad Pro [MK3] control surface in the real app (hidden, isolated config and HOME,
# SDL's dummy audio driver, GRIDLINE_TEST_MIDI=fake: a stand-in "LPProMK3 DAW" port the midifake dev
# command plugs in, feeds and unplugs). "surface log" records every byte Gridline sends the unit.
#   Clips:  plugged in, the port is recognised by name and gets the handshake (DAW mode on, the Session
#           layout, the device inquiry); a grid pad launches the clip in that slot and its pad then
#           pulses in the clip's colour; Shift + Track Select 2 opens the Keys page and the unit's own
#           Note report does too; Volume asks for the fader layout and a fader move changes the
#           track's volume in one undo step; unplugged: DAW mode off; plugged in again: a new handshake.
#   Studio: the Session page shows the arrangement: a pad moves the play position to its bar; the Steps
#           page (Shift + Track Select 4) toggles a step of the selected channel in one undo step.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k078_launchpad_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_TEST_MIDI} "fake")
file(READ "${DEMO}" demo)  # a copy with two scenes in the clip grid (as grid1118_clips_app)
string(REGEX REPLACE "\"playlist\": \\[" "\"live\": {\"areaEnd\": 3072, \"view\": \"grid\"}, \"playlist\": [" demo "${demo}")
string(REGEX REPLACE "\"version\": *[0-9]+" "\"version\": 10" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\n(the run logs: ${OUT}/*.log)")
endfunction()

function(run tag layout script)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --no-update-check --video-clock 60 --size 1440x900
                          --layout-dev --layout ${layout} --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 150)
  file(WRITE "${OUT}/${tag}.log" "${o}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    fail("gridline-app failed in run ${tag} (${rc})")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

function(expect what re)
  if(NOT out MATCHES "${re}")
    fail("${what} (wanted /${re}/)")
  endif()
  message(STATUS "ok - ${what}")
endfunction()
function(refuse what re)
  if(out MATCHES "${re}")
    fail("${what} (did not want /${re}/)")
  endif()
  message(STATUS "ok - ${what}")
endfunction()
function(undoAt frame var)
  if(NOT out MATCHES "dump frame ${frame}: [0-9]+ clips, [0-9]+ selected, undo ([0-9]+)\n")
    fail("no dump at frame ${frame}")
  endif()
  set(${var} ${CMAKE_MATCH_1} PARENT_SCOPE)
endfunction()

set(P "\"LPProMK3 DAW\"")
set(H "F0 00 20 29 02 0E")

# ---- Clips ----
run(clips live "4 typing off
4 waitfor midi-settled 5
6 surface log
6 surface
8 midifake in add ${P}
8 midifake out add ${P}
9 waitfor \"midi-open:LPProMK3 DAW\" 10
14 surface
16 surface sent clear
20 key space
30 midifake send ${P} 0x90 71 100
30 midifake send ${P} 0x80 71 0
200 livestate
200 surface sent clear
204 midifake send ${P} 0xB0 90 127
204 midifake send ${P} 0xB0 102 127
204 midifake send ${P} 0xB0 102 0
204 midifake send ${P} 0xB0 90 0
210 surface
212 midifake send ${P} 0xB0 101 127
212 midifake send ${P} 0xB0 101 0
214 surface sent clear
216 midifake send ${P} 0xF0 0x00 0x20 0x29 0x02 0x0E 0x00 0x04 0x00 0x00 0xF7
222 surface
224 surface sent clear
230 midifake send ${P} 0xF0 0x00 0x20 0x29 0x02 0x0E 0x00 0x00 0x00 0x00 0xF7
236 key space
242 midifake send ${P} 0xB0 4 127
242 midifake send ${P} 0xB0 4 0
256 surface
258 surface sent clear
260 forget \"midi-open:\"
260 midifake in remove ${P}
261 waitfor \"midi-open:\" 10
266 surface
268 surface sent clear
270 forget \"midi-open:LPProMK3 DAW\"
270 midifake in add ${P}
271 waitfor \"midi-open:LPProMK3 DAW\" 10
276 surface
278 surface sent clear
290 quit discard
")
expect("nothing runs before the unit is plugged in" "surface frame 6: none")
expect("plugged in: recognised by its port name, answers on its DAW output"
       "surface frame 14: \"LPProMK3 DAW\" type launchpad-pro-mk3 output \"LPProMK3 DAW\" page Session")
expect("handshake: DAW mode on" "surface frame 16: sent \"LPProMK3 DAW\" ${H} 10 01 F7")
expect("handshake: the Session layout" "surface frame 16: sent \"LPProMK3 DAW\" ${H} 00 00 00 00 F7")
expect("handshake: the device inquiry" "surface frame 16: sent \"LPProMK3 DAW\" F0 7E 7F 06 01 F7")
expect("handshake: the four fader banks set up" "surface frame 16: sent \"LPProMK3 DAW\" ${H} 01 03 00 ")
expect("every pad lit once (the clip in scene 2, track 1: pad 71)" "surface frame 16: sent \"LPProMK3 DAW\" 90 47 [0-9A-F][0-9A-F]")
expect("the pad launched the clip in its slot" "\n  live 0 active 35 ")
expect("the launched clip's pad pulses (channel 3)" "surface frame 200: sent \"LPProMK3 DAW\" 92 47 [0-9A-F][0-9A-F]")
expect("Shift + Track Select 2: the Keys page" "surface frame 210: [^\n]* page Keys ")
expect("the unit's Note report: Keys too, and Gridline asks for the Session layout back"
       "surface frame 222: [^\n]* page Keys ")
expect("the Note report is answered with the Session layout" "surface frame 224: sent \"LPProMK3 DAW\" ${H} 00 00 00 00 F7")
expect("Volume: the Faders page" "surface frame 256: [^\n]* page Faders bank 0 ")
expect("Volume asks the unit for its fader layout" "surface frame 258: sent \"LPProMK3 DAW\" ${H} 00 01 00 00 F7")
expect("unplugged: DAW mode off (the unit back to standalone)" "surface frame 268: sent \"LPProMK3 DAW\" ${H} 10 00 F7")
expect("unplugged: nothing runs" "surface frame 266: none")
expect("plugged in again: a new handshake" "surface frame 278: sent \"LPProMK3 DAW\" ${H} 10 01 F7")

# ---- Studio ----
run(studio studio "4 typing off
4 waitfor midi-settled 5
6 surface log
8 midifake in add ${P}
9 waitfor \"midi-open:LPProMK3 DAW\" 10
14 surface
20 midifake send ${P} 0x90 83 100
20 midifake send ${P} 0x80 83 0
24 livestate
26 dump
28 midifake send ${P} 0xB0 90 127
28 midifake send ${P} 0xB0 104 127
28 midifake send ${P} 0xB0 104 0
28 midifake send ${P} 0xB0 90 0
32 surface
34 midifake send ${P} 0x90 81 100
34 midifake send ${P} 0x80 81 0
40 dump
42 undo
46 dump
48 midifake send ${P} 0xB0 4 127
48 midifake send ${P} 0xB0 4 0
50 dump
52 midifake send ${P} 0xB4 20 30
53 midifake send ${P} 0xB4 20 40
54 midifake send ${P} 0xB4 20 50
60 dump
64 quit discard
")
expect("Studio: the same unit runs on the Session page" "surface frame 14: \"LPProMK3 DAW\" type launchpad-pro-mk3 [^\n]* page Session")
if(NOT out MATCHES "livestate frame 24: mode [0-9] playing [0-9] pos ([0-9]+) ")
  fail("no livestate at frame 24")
endif()
set(pos ${CMAKE_MATCH_1})
if(pos EQUAL 0)
  fail("a pad of the arrangement overview moves the play position to its bar (pos ${pos})")
endif()
message(STATUS "ok - Studio: pad 3 of the overview moves the play position to bar 3 (${pos})")
expect("Shift + Track Select 4: the Steps page" "surface frame 32: [^\n]* page Steps ")
undoAt(26 s0)
undoAt(40 s1)
undoAt(46 s2)
math(EXPR d "${s1} - ${s0}")
if(NOT d EQUAL 1)
  fail("a step toggled on the Steps page is one undo step (undo ${s0} -> ${s1})")
endif()
message(STATUS "ok - Studio: a step toggled from the unit is one undo step")
if(NOT s2 EQUAL s0)
  fail("undo takes the step back (undo ${s2}, wanted ${s0})")
endif()
message(STATUS "ok - Studio: undo takes it back")
undoAt(50 f0)
undoAt(60 f1)
math(EXPR d "${f1} - ${f0}")
if(NOT d EQUAL 1)
  fail("a fader move by hand (three positions) is one undo step (undo ${f0} -> ${f1})")
endif()
message(STATUS "ok - Studio: a fader move by hand is one undo step")
