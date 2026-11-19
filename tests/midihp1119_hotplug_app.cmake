# Gridline - MIDIHP-1119: MIDI devices plugged in or out while the app runs. Before, the device
# lists were read at startup (and by Rescan) only, and on macOS even Rescan never saw a device
# plugged in later: a new controller needed a restart. In the real app (hidden, an isolated config
# folder and HOME, SDL's dummy audio driver), with GRIDLINE_TEST_MIDI=fake (stand-in devices that
# the "midifake" dev command plugs in and out; no system MIDI client is ever made):
#   1. a known input and the selected output plugged in after launch open on their own, as at
#      startup; a device never seen before opens too (enabled, the next free port number);
#   2. a note (and the sustain pedal) held on an input that is unplugged: the input and the output
#      close, the held note is released (no stuck note), the other input stays open, and messages
#      from the unplugged device no longer arrive;
#   3. plugged in again, it opens again with its settings kept; Rescan devices still works, and the
#      background rescans are not counted as tries of the service;
#   4. a plain hidden run (no GRIDLINE_TEST_MIDI) never rescans and opens nothing (MIDI-1118).
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P midihp1119_hotplug_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{HOME} "${OUT}/home")
unset(ENV{GRIDLINE_TEST_MIDI})

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n(the run logs: ${OUT}/*.log)")
endfunction()

function(run tag script)
  file(MAKE_DIRECTORY "${OUT}/${tag}/cfg/gridline" "${OUT}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/${tag}/cfg")
  file(WRITE "${OUT}/${tag}/cfg/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"tracker\", \"keys.profile\": \"tracker\", \"ui.menuStyle\": \"tracker\"}}\n")
  file(WRITE "${OUT}/${tag}/cfg/gridline/midi.json"
       "{\"inputs\": [{\"name\": \"Fake Keys\", \"enabled\": 1, \"port\": 3, \"controller\": \"\", \"notes\": 1}], \"output\": \"Fake Synth\"}\n")
  configure_file("${DEMO}" "${OUT}/${tag}/song.gridline.json" COPYONLY)
  file(WRITE "${OUT}/${tag}/input.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/${tag}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --layout tracker
                          --dev-input "${OUT}/${tag}/input.txt" --frames 200
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e TIMEOUT 150)
  file(WRITE "${OUT}/${tag}.log" "${o}\n---- stderr ----\n${e}")
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

# 1-3. Stand-in devices plugged in and out while the app runs.
set(ENV{GRIDLINE_TEST_MIDI} "fake")
run(hotplug "4 waitfor midi-settled 5
6 midiopen
6 midistate
8 midifake in add \"Fake Keys\"
8 midifake out add \"Fake Synth\"
9 waitfor \"midi-open:Fake Keys\" 10
10 waitfor midi-output:1 10
12 midiopen
14 midifake in add \"Fake Pads\"
15 waitfor \"midi-open:Fake Keys|Fake Pads\" 10
16 midiopen
18 midifake send \"Fake Keys\" 0x90 60 100
18 midifake send \"Fake Keys\" 0xB0 64 127
18 midifake send \"Fake Pads\" 0x99 36 90
18 midifake send \"Fake Pads\" 0x89 36 0
20 midiopen
22 forget midi-output:0
22 midifake in remove \"Fake Keys\"
22 midifake out remove \"Fake Synth\"
23 waitfor \"midi-open:Fake Pads\" 10
24 waitfor midi-output:0 10
26 midiopen
27 forget output-silent
27 waitfor output-silent 4
27 sound
26 midifake send \"Fake Keys\" 0x90 62 100
28 midifake in add \"Fake Keys\"
29 forget \"midi-open:Fake Pads|Fake Keys\"
29 waitfor \"midi-open:Fake Pads|Fake Keys\" 10
30 midiopen
30 midifake send \"Fake Keys\" 0x90 64 100
30 midifake send \"Fake Keys\" 0x80 64 0
32 midirescan
33 forget midi-settled
33 waitfor midi-settled 10
34 waitfor never 0.5
36 midiopen
36 midistate
40 quit discard
")
unset(ENV{GRIDLINE_TEST_MIDI})
expect("nothing is listed or open at launch" "midiopen frame 6: inputs \"\" ports \"\" held \"\" output 0 released 0 listed \"\" outputs 0")
expect("the service answered" "midistate frame 6: state ready attempts 1 inputs 0 outputs 0")
expect("a known input plugged in after launch opens, and the selected output"
       "midiopen frame 12: inputs \"Fake Keys\" ports \"3\" held \"0\" output 1 released 0 listed \"Fake Keys\" outputs 1")
expect("a new device plugged in opens too" "midiopen frame 16: inputs \"Fake Keys\\|Fake Pads\" ports \"3,0\" held \"0,0\" output 1 released 0 listed \"Fake Keys\\|Fake Pads\"")
expect("messages from the open inputs arrive" "midifake frame 18: send \"Fake Keys\" delivered")
expect("a note is held on the keys (the pads let theirs go)" "midiopen frame 20: inputs \"Fake Keys\\|Fake Pads\" ports \"3,0\" held \"1,0\"")
expect("unplugged: the keys and the output close, the pads stay, the held note is released"
       "midiopen frame 26: inputs \"Fake Pads\" ports \"0\" held \"0\" output 0 released 1 listed \"Fake Pads\" outputs 0")
expect("the released note goes quiet (a note held on the stand-in sounds for longer)" "waitfor output-silent: reached")
expect("the unplugged device sends nothing more" "midifake frame 26: send \"Fake Keys\" not-open")
expect("plugged in again, it opens again with its port number kept" "midiopen frame 30: inputs \"Fake Pads\\|Fake Keys\" ports \"0,3\" held \"0,0\" output 0 released 1")
expect("and its messages arrive" "midifake frame 30: send \"Fake Keys\" delivered")
expect("Rescan devices keeps them open" "midiopen frame 36: inputs \"Fake Pads\\|Fake Keys\" ports \"0,3\" held \"0,0\"")
expect("the background rescans ran" "midiopen frame 36: [^\n]* rescans [1-9]")
expect("and are not counted as tries (Rescan is one)" "midistate frame 36: state ready attempts 2 inputs 2 outputs 0")

# 4. A plain hidden run: no rescans, nothing opened.
run(hidden "4 waitfor midi-settled 5
6 waitfor never 3
8 midiopen
8 midiports
10 quit discard
")
expect("a hidden run never rescans the devices" "midiopen frame 8: inputs \"\" ports \"\" held \"\" output 0 released 0 listed \"\" outputs 0 rescans 0")
expect("and opens no MIDI port" "midiports frame 8: system 0 inputs 0 output 0 device-outputs 0")
