# Gridline GitHub #79: "My keys say" in the real app (hidden, isolated config and HOME). A listed layout
# stands in for the computer's ("keys system <id>"), so no real system layout is needed.
#   1. The default names every shortcut as before, also on a Dvorak computer.
#   2. Dvorak typed on keys printed QWERTY: Stop all sound (Ctrl+H) shows Ctrl+J and Copy shows Ctrl+I;
#      the key printed J (the H place on US QWERTY) is the one that gives H; the key that types / gives /
#      (the interface's own rule left / without a key on Dvorak); Narrower ([) shows the key printed -.
#   3. Each choice names Stop all sound (Clips' set: Ctrl+Alt+.) and Help > Keyboard shortcuts (Ctrl+/) its own way.
#   4. The choice is kept after a quit.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P keycaps_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

function(run_app name script)
  file(WRITE "${OUT}/${name}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${DEMO}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900
                          --dev-input "${OUT}/${name}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 100)
  file(WRITE "${OUT}/${name}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gridline-app failed in ${name} (${rc}): ${err}")
  endif()
  string(REGEX MATCHALL "keys (label|state|down)[^\n]*" lines "${out}")
  string(REPLACE ";" "|" lines "${lines}")
  set(got "${lines}" PARENT_SCOPE)
endfunction()

function(expect_lines what)
  math(EXPR last "${ARGC} - 1")
  foreach(i RANGE 1 ${last})
    set(want "${ARGV${i}}")  # (one argument at a time: a "[" would join list items)
    string(FIND "${got}" "${want}" at)
    if(at LESS 0)
      message(FATAL_ERROR "FAIL: ${what}: missing \"${want}\"\n--- got\n${got}\n")
    endif()
    # In order: what follows starts after this one.
    math(EXPR from "${at} + 1")
    string(SUBSTRING "${got}" ${from} -1 rest)
    set(got "${rest}")
  endforeach()
endfunction()

# 1 and 2. Studio's set.
run_app(studio "2 keys label tools.stopSound
3 keys label playlist.copy
4 keys system dvorak
6 keys label tools.stopSound
7 keys label playlist.copy
8 keys label stage.narrower
9 keys state
10 keys say qwerty
12 keys label tools.stopSound
13 keys label playlist.copy
14 keys label edit.undo
15 keys label stage.narrower
16 keys state
18 sdlkey #13 down ctrl
20 keys down
22 sdlkey #13 up ctrl
24 sdlkey #47 down
26 keys down
28 sdlkey #47 up
30 sdlkey #45 down
32 keys down
34 sdlkey #45 up
36 keys say dvorak
38 keys label tools.stopSound
40 keys say computer
42 keys label tools.stopSound
44 quit discard
")
expect_lines("Studio, the default and Dvorak + QWERTY keys"
    "keys label tools.stopSound: Ctrl+H [Ctrl+H]"
    "keys label playlist.copy: Ctrl+C [Ctrl+C]"
    "keys label tools.stopSound: Ctrl+H [Ctrl+H]"
    "keys label playlist.copy: Ctrl+C [Ctrl+C]"
    "keys label stage.narrower: [ [[]"
    "keys state say computer system dvorak bytyped 1 missing|"
    "keys label tools.stopSound: Ctrl+J [Ctrl+J]"
    "keys label playlist.copy: Ctrl+I [Ctrl+I]"
    "keys label edit.undo: Ctrl+/"
    "keys label stage.narrower: - [-]"
    "keys state say qwerty system dvorak bytyped 1 missing|"
    "keys down: H"
    "keys down: /"
    "keys down: ["
    "keys label tools.stopSound: Ctrl+H [Ctrl+H]"
    "keys label tools.stopSound: Ctrl+H [Ctrl+H]")

# 3. Clips' set: Help > Keyboard shortcuts on F1 and Ctrl+/, each choice on a US computer. On macOS
# Clips shows Ctrl as Cmd.
set(M Ctrl)
if(CMAKE_HOST_APPLE)
  set(M Cmd)
endif()
file(WRITE "${OUT}/cfg/gridline/settings.json" "{\"version\": 1, \"values\": {\"general.launcher\": false, \"keys.profile\": \"live\"}}")
run_app(choices "2 keys system qwerty
4 keys label help.shortcuts
6 keys say qwerty
8 keys label help.shortcuts
10 keys say qwertz
12 keys label tools.stopSound
13 keys label help.shortcuts
14 keys say azerty
16 keys label tools.stopSound
17 keys label help.shortcuts
18 keys say dvorak
20 keys label tools.stopSound
21 keys label help.shortcuts
22 keys say colemak
24 keys label tools.stopSound
25 keys label help.shortcuts
26 keys system dvorak
27 keys say qwerty
28 keys label help.shortcuts
30 sdlkey #47 down ctrl
32 keys down
34 sdlkey #47 up ctrl
36 quit discard
")
expect_lines("each choice, US computer"
    "keys label help.shortcuts: F1 [F1 / ${M}+/]"
    "keys label help.shortcuts: F1 [F1 / ${M}+/]"
    "keys label tools.stopSound: ${M}+Alt+."
    "keys label help.shortcuts: F1 [F1 / ${M}+-]"
    "keys label tools.stopSound: ${M}+Alt+:"
    "keys label help.shortcuts: F1 [F1 / ${M}+!]"
    "keys label tools.stopSound: ${M}+Alt+V"
    "keys label help.shortcuts: F1 [F1 / ${M}+Z]"
    "keys label tools.stopSound: ${M}+Alt+."
    "keys label help.shortcuts: F1 [F1 / ${M}+/]"
    "keys label help.shortcuts: F1 [F1 / ${M}+[]"
    "keys down: /")

# 4. The choice made before the quit (QWERTY, a Dvorak computer standing in again) is kept.
run_app(kept "2 keys system dvorak\n4 keys label tools.stopSound\n6 keys state\n8 quit discard\n")
expect_lines("the choice is kept" "keys label tools.stopSound: ${M}+Alt+E [${M}+Alt+E]" "keys state say qwerty system dvorak")
message(STATUS "ok - My keys say names shortcuts by the printed key that runs them; / has a key on Dvorak")
