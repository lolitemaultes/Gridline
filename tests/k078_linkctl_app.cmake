# Gridline - k078-linkctl: "Link to controller" in the real app (Studio layout, hidden, an isolated
# config folder and HOME, SDL's dummy audio driver), with GRIDLINE_TEST_MIDI=fake: stand-in ports for
# an FLkey 49 (its MIDI port and its DAW port, which Gridline runs as a control surface) and a
# generic controller ("Studio Knobs") that keeps sending one CC value on its own.
#   1. the dialog opens listening; the stream, a key and messages waiting from before capture nothing;
#   2. a knob turned is captured with its device, channel and port; a second knob turned wins;
#   3. Apply links it in one undo step, and the knob then moves channel 1's volume at once;
#   4. an FLkey pot (DAW port) is captured as that port's control; after Apply it moves mixer
#      insert 1's volume instead of what the FLkey page gives it; Cancel links nothing;
#   5. an NRPN knob is captured as one NRPN and drives its link; Remove link deletes a link.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> [-DSHOTS=<dir>] -P k078_linkctl_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_TEST_MIDI} "fake")

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n(the run log: ${OUT}/run.log)")
endfunction()

file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"studio\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
if(NOT SHOTS)
  set(SHOTS "${OUT}")
endif()
set(K "midifake send \"Studio Knobs\"")
set(F "midifake send \"FLkey 49 DAW Out\"")
set(M "midifake send \"FLkey 49 MIDI Out\"")
file(WRITE "${OUT}/input.txt" "4 waitfor midi-settled 5
6 midifake in add \"FLkey 49 MIDI Out\"
6 midifake in add \"FLkey 49 DAW Out\"
6 midifake out add \"FLkey 49 MIDI In\"
6 midifake out add \"FLkey 49 DAW In\"
6 midifake in add \"Studio Knobs\"
7 waitfor \"midi-open:FLkey 49 MIDI Out|FLkey 49 DAW Out|Studio Knobs\" 10
9 waitfor never 0.5
10 midiopen
10 linkctl links
12 ${K} 0xB0 20 64
12 linkctl open channel 0
13 ${K} 0xB0 20 64
14 waitfor never 0.25
15 ${K} 0xB0 20 64
15 ${M} 0x90 60 100
15 ${M} 0x80 60 0
16 ${K} 0xB0 20 64
16 waitfor never 0.2
17 ${K} 0xB0 20 64
17 linkctl state
18 shot \"${SHOTS}/listening.png\"
20 ${K} 0xB0 74 10
20 ${K} 0xB0 74 14
20 ${K} 0xB0 74 20
21 linkctl state
22 ${K} 0xB0 75 40
22 ${K} 0xB0 75 50
22 ${K} 0xB0 75 70
23 ${K} 0xB0 20 64
23 linkctl state
24 shot \"${SHOTS}/captured.png\"
26 linkctl apply
26 linkctl state
27 ${K} 0xB0 75 127
28 waitfor never 0.1
29 linkctl links
31 linkctl open mixer 1
32 waitfor never 0.25
33 ${F} 0xBF 0x15 30
33 ${F} 0xBF 0x15 40
33 ${F} 0xBF 0x15 50
34 linkctl state
35 shot \"${SHOTS}/surface.png\"
36 linkctl apply
37 ${F} 0xBF 0x15 127
38 waitfor never 0.1
39 linkctl links
41 linkctl open mixer 2
42 waitfor never 0.25
43 ${K} 0xB0 76 10
43 ${K} 0xB0 76 90
44 linkctl cancel
45 linkctl links
47 linkctl open mixer 2
48 waitfor never 0.25
49 ${K} 0xB3 99 1
49 ${K} 0xB3 98 2
49 ${K} 0xB3 6 10
49 ${K} 0xB3 6 30
49 ${K} 0xB3 6 50
50 linkctl state
51 linkctl apply
52 ${K} 0xB3 99 1
52 ${K} 0xB3 98 2
52 ${K} 0xB3 6 0
53 waitfor never 0.1
54 linkctl links
56 linkctl open channel 0
57 linkctl remove
58 linkctl links
60 quit discard
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --layout studio ${EXTRA}
                        --dev-input "${OUT}/input.txt" --frames 400
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE e TIMEOUT 200)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${e}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed (${rc})")
endif()
if(REPRO)
  return()
endif()

function(expect what re)
  if(NOT out MATCHES "${re}")
    fail("${what} (wanted /${re}/)")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

if(NOT out MATCHES "inputs \"FLkey 49 MIDI Out\\|FLkey 49 DAW Out\\|Studio Knobs\" ports \"([0-9]+),([0-9]+),([0-9]+)\"")
  fail("no midiopen line")
endif()
set(dawPort ${CMAKE_MATCH_2})
set(knobPort ${CMAKE_MATCH_3})
expect("no links to begin with" "linkctl frame 10: links 0\n")
# 1. Listening; noise captures nothing.
expect("the dialog opens listening, nothing captured despite the stream, a key and a waiting message"
       "linkctl frame 17: open 1 listening 1 captured 0 [^\n]*target \"[^\"]+ Volume\"")
# 2. The knob, then the latest knob.
expect("the knob turned is captured (CC 74, channel 1, its device and port)"
       "linkctl frame 21: open 1 listening 1 captured 1 device \"Studio Knobs\" source \"CC 74 · ch 1 · port ${knobPort}\" meter 0.157")
expect("a second knob turned wins and the stream does not take it back"
       "linkctl frame 23: open 1 listening 1 captured 2 device \"Studio Knobs\" source \"CC 75 · ch 1 · port ${knobPort}\" meter 0.551")
# 3. Apply: one undo step, the knob moves the control at once.
if(NOT out MATCHES "linkctl frame 21: [^\n]* undo ([0-9]+)\n")
  fail("no undo at frame 21")
endif()
set(u0 ${CMAKE_MATCH_1})
math(EXPR u1 "${u0} + 1")
expect("Apply closes the dialog and links it in one undo step" "linkctl frame 26: open 0 listening 0 [^\n]* undo ${u1}\n")
expect("the knob then moves channel 1's volume to the top"
       "linkctl frame 29: links 1\nlinkctl link: \"CC 75 · ch 1 · port ${knobPort}\" -> \"[^\"]+ Volume\" value 1.2500")
# 4. The FLkey pot.
expect("the FLkey pot is captured on its DAW port and the dialog says the FLkey hands it over"
       "linkctl frame 34: open 1 listening 1 captured 1 device \"FLkey 49 DAW Out\" source \"CC 21 · ch 16 · port ${dawPort}\" meter 0.394 surface 1")
expect("after Apply the pot moves mixer insert 1's volume"
       "linkctl frame 39: links 2\n[^\n]*\nlinkctl link: \"CC 21 · ch 16 · port ${dawPort}\" -> \"[^\"]+\" value 1.2500")
expect("Cancel links nothing" "linkctl frame 45: links 2\n")
# 5. NRPN, Remove.
expect("an NRPN knob is captured as one NRPN" "linkctl frame 50: open 1 listening 1 captured 1 device \"Studio Knobs\" source \"NRPN 130 · ch 4 · port ${knobPort}\"")
expect("the NRPN link drives its target" "linkctl frame 54: links 3\n[^\n]*\n[^\n]*\nlinkctl link: \"NRPN 130 · ch 4 · port ${knobPort}\" -> \"[^\"]+\" value 0.0000")
expect("an existing link opens not listening, with nothing captured from before"
       "linkctl frame 56: open 1 listening 0 captured 0 device \"\" source \"CC 75 · ch 1 · port ${knobPort}\"")
expect("Remove link deletes channel 1's link" "linkctl frame 58: links 2\n")
foreach(s listening captured surface)
  if(NOT EXISTS "${SHOTS}/${s}.png")
    fail("no ${s}.png")
  endif()
endforeach()
message(STATUS "ok - screenshots")
