# Gridline K-113: the typing keyboard's layout choice in the real app (hidden, isolated config and HOME).
# A fresh config names the keys as Standard (QWERTY); a settings file saved before the choice existed does
# too; each layout names the keys its own way, play stays by position, and the choice is kept after a quit.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P typing_layout_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

function(run_app name script)
  file(WRITE "${OUT}/${name}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${DEMO}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --hide rack
                          --dev-input "${OUT}/${name}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 100)
  file(WRITE "${OUT}/${name}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gridline-app failed in ${name} (${rc}): ${err}")
  endif()
  string(REGEX MATCHALL "typing (labels|notes)[^\n]*" lines "${out}")
  string(REPLACE ";" "|" lines "${lines}")  # CMake lists split on ';': a ';' label and the line breaks both read '|'
  set(got "${lines}" PARENT_SCOPE)
endfunction()

function(check what want)
  if(NOT "${got}" STREQUAL "${want}")
    message(FATAL_ERROR "FAIL: ${what}\n--- got\n${got}\n--- wanted\n${want}\n")
  endif()
endfunction()

set(std "typing labels standard: Z S X D C V G B H N J M , L . | / Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P [ ] - = \\ '")

# 1. Fresh config: Standard.
run_app(fresh "4 typing labels\n6 quit discard\n")
check("a fresh config is Standard" "${std}")

# 2. Every layout's names; play by position on each (the key in the Z position typing ';' as on Dvorak plays C4).
run_app(layouts "4 typing on
6 typing layout qwertz
8 typing labels
10 typing layout azerty
12 typing labels
14 typing layout colemak
16 typing labels
18 typing layout dvorak
20 typing labels
22 sdlkey ;@z down
24 typing show
26 sdlkey ;@z up
28 typing layout computer
30 typing labels
32 typing layout dvorak
34 quit discard
")
set(joined "${got}")
foreach(want
    "typing labels qwertz: Y S X D C V G B H N J M , L . Ö - Q 2 W 3 E R 5 T 6 Z 7 U I 9 O 0 P Ü + ß ´ # Ä"
    "typing labels azerty: W S X D C V G B H N J , | L : M ! A É Z \" E R ( T - Y È U I Ç O À P ^ $ ) = * Ù"
    "typing labels colemak: Z R X S C V D B H K N M , I . O / Q 2 W 3 F P 5 G 6 J 7 L U 9 Y 0 | [ ] - = \\ '"
    "typing labels dvorak: | O Q E J K I X D B H M W N V S Z ' 2 , 3 . P 5 Y 6 F 7 G C 9 R 0 L / = [ ] \\ -"
    "typing notes 48 octave 4 velocity 100 chord 0")
  string(FIND "${joined}" "${want}" at)
  if(at LESS 0)
    message(FATAL_ERROR "FAIL: missing \"${want}\"\n--- got\n${joined}\n")
  endif()
endforeach()
# Follow the computer: whatever the system's layout names (logged for the report, not checked).
string(REGEX MATCH "typing labels computer:[^|]*" sys "${joined}")
message(STATUS "this computer: ${sys}")

# 3. The choice made before the quit (Dvorak) is kept.
run_app(kept "4 typing labels\n6 typing layout standard\n8 quit discard\n")
set(joined "${got}")
string(FIND "${joined}" "typing labels dvorak:" at)
if(NOT at EQUAL 0)
  message(FATAL_ERROR "FAIL: the chosen layout was not kept\n${joined}\n")
endif()

# 4. A settings file from before the choice existed (no keys.typingLayout): Standard.
file(WRITE "${OUT}/cfg/gridline/settings.json" "{\"version\": 1, \"values\": {\"ui.scale\": 1.0, \"general.launcher\": false}}")
run_app(old "4 typing labels\n6 quit discard\n")
check("an old settings file is Standard" "${std}")
message(STATUS "ok - the typing keyboard layout is Standard by default and names keys per layout")
