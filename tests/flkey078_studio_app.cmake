# Gridline - k078-flkey: Novation FLkey keyboards in the real app (Studio layout, hidden, an
# isolated config folder and HOME, SDL's dummy audio driver), with GRIDLINE_TEST_MIDI=fake: stand-in
# ports named as the keyboards name theirs, plugged in and out by the "midifake" dev command, and
# the bytes Gridline sends to them read back with "midifake sent".
#   1. an FLkey 49 (first generation) plugged in: its DAW port is found by name, Gridline puts it in
#      DAW mode, sets the pads to the DAW layout, lights the selected channel's steps in its colour
#      and writes the pattern name on the LCD; its MIDI port stays a plain keyboard;
#   2. a pad sets a step (one undo step), Channel Rack down selects the next channel, fader 1 moves
#      mixer insert 1 and the LCD shows its name;
#   3. an FLkey 2 37 at the same time gets its own surface and RGB pad colours;
#   4. unplugged, the surface goes; plugged in again, the handshake runs again.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P flkey078_studio_app.cmake
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
file(WRITE "${OUT}/input.txt" "4 waitfor midi-settled 5
6 midifake in add \"FLkey 49 MIDI Out\"
6 midifake in add \"FLkey 49 DAW Out\"
6 midifake out add \"FLkey 49 MIDI In\"
6 midifake out add \"FLkey 49 DAW In\"
7 waitfor \"midi-open:FLkey 49 MIDI Out|FLkey 49 DAW Out\" 10
10 waitfor never 0.5
12 flkey
12 midifake sent \"FLkey 49 DAW In\" clear
12 midifake sent \"FLkey 49 MIDI In\"
12 rackrow
14 midifake send \"FLkey 49 DAW Out\" 0x90 0x61 100
14 midifake send \"FLkey 49 DAW Out\" 0x90 0x61 0
16 rackrow
17 waitfor never 0.2
18 midifake sent \"FLkey 49 DAW In\" clear
20 midifake send \"FLkey 49 DAW Out\" 0xB0 105 127
20 midifake send \"FLkey 49 DAW Out\" 0xB0 105 0
22 rackrow
22 midifake sent \"FLkey 49 DAW In\" clear
22 midifake send \"FLkey 49 DAW Out\" 0xBF 52 127
22 midifake send \"FLkey 49 DAW Out\" 0xBF 52 0
22 midifake send \"FLkey 49 DAW Out\" 0xBF 52 127
22 midifake send \"FLkey 49 DAW Out\" 0xBF 52 0
22 waitfor never 0.3
23 rackrow
23 midifake sent \"FLkey 49 DAW In\" clear
24 midifake send \"FLkey 49 DAW Out\" 0xBF 0x35 81
24 midifake send \"FLkey 49 DAW Out\" 0xBF 0x35 127
25 waitfor never 0.2
26 mixer
26 flkey
26 midifake sent \"FLkey 49 DAW In\" clear
28 midifake in add \"FLkey 2 37 DAW Out\"
28 midifake out add \"FLkey 2 37 DAW In\"
29 waitfor \"midi-open:FLkey 49 MIDI Out|FLkey 49 DAW Out|FLkey 2 37 DAW Out\" 10
31 waitfor never 0.5
32 flkey
32 midifake sent \"FLkey 2 37 DAW In\" clear
34 midifake in remove \"FLkey 49 DAW Out\"
34 midifake out remove \"FLkey 49 DAW In\"
35 waitfor \"midi-open:FLkey 49 MIDI Out|FLkey 2 37 DAW Out\" 10
37 flkey
37 midifake sent \"FLkey 49 DAW In\" clear
38 midifake in add \"FLkey 49 DAW Out\"
38 midifake out add \"FLkey 49 DAW In\"
39 waitfor \"midi-open:FLkey 49 MIDI Out|FLkey 2 37 DAW Out|FLkey 49 DAW Out\" 10
41 waitfor never 0.5
42 flkey
42 midifake sent \"FLkey 49 DAW In\" clear
44 shot \"${OUT}/studio.png\"
46 quit discard
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --layout studio
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
function(expect_not what re)
  if(out MATCHES "${re}")
    fail("${what} (did not want /${re}/)")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

# 1. Found by name, the handshake, the first picture.
expect("the FLkey 49 DAW port gets a surface (its MIDI port does not)"
       "flkey frame 12: 1\nflkey: \"FLkey 49 DAW Out\" model flkey-49 output \"FLkey 49 DAW In\" connected 1 hui 0 page Steps")
expect("DAW mode on, first" "midifake frame 12: sent \"FLkey 49 DAW In\" [0-9]+\nmidifake out: 9F 0C 7F \n")
expect("the pads are set to the DAW layout (channel 16, CC 3, value 2)" "midifake out: BF 03 02 \n")
expect("step 1 of the kick lights (a palette colour on the pad's note)" "midifake out: 90 60 [0-9A-F][0-9A-F] \n")
expect("the pads show the kick's steps in its colour, empty steps dim" "flkey: [^\n]* pads D8775A [0-9A-F]+ [0-9A-F]+ [0-9A-F]+ D8775A")
expect("the LCD shows the pattern name" "midifake out: F0 00 20 29 02 0F 04 00 42 65 61 74 F7")
expect("nothing goes to the keyboard's MIDI port" "midifake frame 12: sent \"FLkey 49 MIDI In\" 0\n")
# 2. Steps, channels, faders.
expect("the kick before" "rackrow frame 12: 4 steps, undo ([0-9]+), row 120 0 0 0 120")
expect("pad 2 sets step 2 (one undo step)" "rackrow frame 16: 5 steps, undo [0-9]+, row 120 100 0 0 120")
expect("its pad lights in the kick's colour" "midifake frame 18: sent \"FLkey 49 DAW In\" [0-9]+\n(midifake out: [^\n]*\n)*midifake out: 90 61 ")
expect("Channel Rack down selects the clap" "rackrow frame 22: 2 steps, undo [0-9]+, row 0 0 0 0 105")
if(NOT out MATCHES "rackrow frame 22: [0-9]+ steps, undo ([0-9]+),")
  fail("no rackrow at frame 22")
endif()
set(u22 ${CMAKE_MATCH_1})
if(NOT out MATCHES "rackrow frame 23: [0-9]+ steps, undo ([0-9]+),")
  fail("no rackrow at frame 23")
endif()
if(NOT CMAKE_MATCH_1 GREATER u22)
  fail("Preset down loads the clap's next preset as an undo step (undo ${u22} -> ${CMAKE_MATCH_1})")
endif()
message(STATUS "ok - Preset down loads the selected channel's next preset (an undo step)")
expect("the LCD shows the preset's name (Wide, the clap's second)" "midifake frame 23: sent \"FLkey 49 DAW In\" [0-9]+\n(midifake out: [^\n]*\n)*midifake out: F0 00 20 29 02 0F 04 01 57 69 64 65 F7")
expect("fader 1 (picked up, then to the top) moves mixer insert 1" "mixer frame 26:[^\n]*\n  track 0 volume [0-9.]+ [^\n]*\n  track 1 volume 1.2500")
expect("the LCD names fader 1's insert" "flkey frame 26: 1\nflkey: [^\n]*channel 1 knob1 \"[^\"]*\" fader1 \"Kick=")
expect("and the LCD got it as a parameter name (07 50 = fader 1)" "midifake out: F0 00 20 29 02 0F 07 50 4B 69 63 6B F7")
# 3. Two keyboards at once.
expect("an FLkey 2 37 at the same time: two surfaces" "flkey frame 32: 2\n")
expect("it is an FLkey 2 37" "flkey: \"FLkey 2 37 DAW Out\" model flkey2-37 output \"FLkey 2 37 DAW In\" connected 1")
expect("its pads take RGB colours (sysex 0x14, 01 43)" "midifake out: F0 00 20 29 02 14 01 43 60 ")
expect("its stationary display is set up" "midifake out: F0 00 20 29 02 14 04 20 02 F7")
# 4. Unplug and replug.
expect("unplugged: its surface goes, the other stays" "flkey frame 37: 1\nflkey: \"FLkey 2 37 DAW Out\"")
expect("plugged in again: two surfaces" "flkey frame 42: 2\n")
expect("and the handshake runs again" "midifake frame 42: sent \"FLkey 49 DAW In\" [0-9]+\nmidifake out: 9F 0C 7F \n")
expect_not("no unhandled-message warnings for the messages sent" "unhandled message")
