# Gridline - MIDI-1118: hidden and scripted runs (tests, tools) open no system MIDI ports, so a
# connected keyboard or stray device traffic never writes into a test; tests inject MIDI through the
# dev commands ("tracker midi on|off <pitch>"). In the real app (hidden, an isolated config folder
# and HOME, SDL's dummy audio driver):
#   1. a hidden run with a saved MIDI setup (an enabled input, a selected output): the service reads
#      as answered with no devices, no input, output or controller output is open, stand-in devices
#      listed by the dev check are never opened, and an injected note still reaches the app;
#   2. GRIDLINE_TEST_MIDI brings the system MIDI service back (here "fail", so the real service is
#      still never asked): the run says system ports are on.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P midi1118_hidden_ports_app.cmake
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
       "{\"inputs\": [{\"name\": \"Test Keyboard\", \"enabled\": 1, \"port\": 0, \"controller\": \"\", \"notes\": 1}], \"output\": \"Test Out\"}\n")
  configure_file("${DEMO}" "${OUT}/${tag}/song.gridline.json" COPYONLY)
  file(WRITE "${OUT}/${tag}/input.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/${tag}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --layout tracker
                          --dev-input "${OUT}/${tag}/input.txt" --frames 200
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e TIMEOUT 120)
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

# 1. A hidden run never opens a port.
run(hidden "4 waitfor midi-settled 5
6 midistate
6 midiports
8 tracker mididevs 4
10 midiports
12 tracker midi on 60 90
14 tracker midi off 60
16 midiports
20 quit discard
")
expect("the MIDI service reads as answered" "midistate frame 6: state ready attempts 1 inputs 0 outputs 0")
expect("a hidden run opens no MIDI port" "midiports frame 6: system 0 inputs 0 output 0 device-outputs 0")
expect("the stand-in devices are listed" "tracker mididevs frame 8: 4 inputs")
expect("and never opened" "midiports frame 10: system 0 inputs 0 output 0 device-outputs 0")
expect("still none after injected notes" "midiports frame 16: system 0 inputs 0 output 0 device-outputs 0")

# 2. GRIDLINE_TEST_MIDI asks the system service again (fail: simulated, the real one is not asked).
set(ENV{GRIDLINE_TEST_MIDI} "fail")
run(asked "4 midistate
6 midiports
10 quit discard
")
unset(ENV{GRIDLINE_TEST_MIDI})
expect("GRIDLINE_TEST_MIDI brings the service back" "midistate frame 4: state failed")
expect("and system ports on" "midiports frame 6: system 1 inputs 0 output 0")
