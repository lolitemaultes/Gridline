# Gridline - K-199: controller buttons and the command pad, in the real app (hidden, isolated
# config and HOME, SteamDeck=1 so the session is a Game mode one; the buttons come from the
# "gamepad button" dev op, as SDL's controller events would):
#   1. Game mode is detected and the controller is listened to;
#   2. A plays and stops;
#   3. Y opens the command pad, the D-pad moves its lit tile, B closes it;
#   4. A on the pad's Play / stop tile plays and closes the pad;
#   5. D-pad down picks the next channel; R1 turns note entry on: A plays (and its release ends)
#      a note of the scale, the stick moves the octave, L1 sharpens, R1 turns it off;
#   6. D-pad right shows a window that is open already without closing it (it comes to the front);
#   7. in Game mode a built-in plugin window opens docked with the main view.
# Screenshots: <OUT>/pad.ppm, notes.ppm, docked.ppm, settings.ppm, keys.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k199_gamepad_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/hm" "${OUT}/cfg/gridline")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{SteamDeck} "1")
set(ENV{HOME} "${OUT}/hm")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
configure_file("${DEMO}" "${OUT}/fixture.gridline.json" COPYONLY)
file(WRITE "${OUT}/input.txt"
     "5 toasts clear\n10 gamepad state\n12 gamepad button a\n20 transport\n22 gamepad button a\n30 transport\n"
     "32 gamepad button y\n34 gamepad state\n36 gamepad button dpright\n38 gamepad state\n40 shot \"${OUT}/pad.ppm\"\n"
     "42 gamepad button b\n44 gamepad state\n"
     "50 gamepad button start\n52 gamepad button dpleft\n54 gamepad button a\n62 transport\n64 gamepad state\n"
     "66 gamepad button a\n68 gamepad button dpdown\n70 gamepad state\n"
     "72 gamepad button rightshoulder\n74 gamepad down a\n76 gamepad state\n78 gamepad up a\n80 gamepad state\n"
     "82 gamepad stick up\n84 gamepad down dpleft l1\n86 gamepad state\n88 gamepad up dpleft\n90 shot \"${OUT}/notes.ppm\"\n"
     "92 gamepad button rightshoulder\n94 gamepad state\n"
     "96 gamepad button dpright\n97 gamepad button dpright\n98 gamepad button dpright\n99 gamepad button dpright\n100 gamepad state\n"
     "110 gamepad chan\n128 toasts clear\n130 gamepad state\n132 shot \"${OUT}/docked.ppm\"\n"
     "140 closepopups\n141 toasts clear\n142 gamepad settings general\n160 shot \"${OUT}/settings.ppm\"\n"
     "162 gamepad settings keys\n180 shot \"${OUT}/keys.ppm\"\n190 quit discard\n")
execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --layout-dev --no-ui-sounds --fixed-dt 16 --size 1440x900
                        --dev-input "${OUT}/input.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
file(WRITE "${OUT}/out.txt" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${out}")
endif()

function(line var prefix frame)
  string(REGEX MATCH "${prefix} frame ${frame}: [^\n]*" m "${out}")
  if(m STREQUAL "")
    message(FATAL_ERROR "FAIL: no ${prefix} line at frame ${frame}\n${out}")
  endif()
  set(${var} "${m}" PARENT_SCOPE)
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

line(g10 gamepad 10)
check("Game mode detected and listening (${g10})" g10 MATCHES "listening 1 game-mode 1 pad 0")
line(t20 transport 20)
check("A plays (${t20})" t20 MATCHES "playing 1")
line(t30 transport 30)
check("A again stops (${t30})" t30 MATCHES "playing 0")
line(g34 gamepad 34)
check("Y opens the command pad (${g34})" g34 MATCHES "pad 1 lit 0")
line(g38 gamepad 38)
check("the D-pad moves the lit tile (${g38})" g38 MATCHES "pad 1 lit 1")
line(g44 gamepad 44)
check("B closes the pad (${g44})" g44 MATCHES "pad 0")
line(t62 transport 62)
check("A on the Play / stop tile plays (${t62})" t62 MATCHES "playing 1")
line(g64 gamepad 64)
check("... and closes the pad (${g64})" g64 MATCHES "pad 0 lit 0 last transport.play")
line(g70 gamepad 70)
check("D-pad down picks the next channel (${g70})" g70 MATCHES "last pad.nextChannel .*channel [^-]")
line(g76 gamepad 76)
check("R1 turns note entry on and A plays the sixth note, 69 (${g76})" g76 MATCHES "notes 1 octave 5 note 69 held 1")
line(g80 gamepad 80)
check("letting A go ends the note (${g80})" g80 MATCHES "notes 1 octave 5 note 69 held 0")
line(g86 gamepad 86)
check("the stick moves the octave up and L1 sharpens: 73 (${g86})" g86 MATCHES "notes 1 octave 6 note 73 held 1")
line(g94 gamepad 94)
check("R1 turns note entry off, and no note hangs (${g94})" g94 MATCHES "notes 0 octave 6 note 73 held 0")
line(g100 gamepad 99)
check("D-pad right four times reaches the mixer: it comes to the front and every window stays open (${g100})"
      g100 MATCHES "last pad.nextView .*focus Mixer dock 1 views 11111")
line(g130 gamepad 130)
check("in Game mode the plugin window opens docked (${g130})" g130 MATCHES "dock 1 views [01]+ plugwin 2")
