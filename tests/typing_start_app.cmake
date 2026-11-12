# Gridline: the typing keyboard when Gridline starts (Settings > MIDI > Typing keyboard > When Gridline
# starts). In the real app (hidden, isolated config and HOME): a fresh config starts with it off; by default
# it opens as it was left at the last quit, on or off; "On" and "Off" start that way whatever was left; a
# settings file from before the choice existed starts with it off.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P typing_start_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

set(step 0)
# Runs the app with `script` after a first "typing state" at frame 4; `want` is that first state line.
function(run_app want script)
  math(EXPR n "${step} + 1")
  set(step ${n} PARENT_SCOPE)
  file(WRITE "${OUT}/run${n}.txt" "4 typing state\n${script}")
  execute_process(COMMAND "${APP}" "${DEMO}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --hide rack
                          --dev-input "${OUT}/run${n}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 100)
  file(WRITE "${OUT}/run${n}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gridline-app failed in run ${n} (${rc}): ${err}")
  endif()
  string(REGEX MATCH "typing state [^\n]*" got "${out}")
  if(NOT got STREQUAL "${want}")
    message(FATAL_ERROR "FAIL: run ${n} starts with \"${got}\", wanted \"${want}\"\n${out}")
  endif()
endfunction()

# 1. A fresh config: off, the choice is "as it was left". Turned on before the quit...
run_app("typing state off atstart last" "6 typing on\n8 quit discard\n")
# 2. ...so it opens on. Turned off before the quit...
run_app("typing state on atstart last" "6 typing off\n8 quit discard\n")
# 3. ...so it opens off. Now the choice "On", with the keyboard left off.
run_app("typing state off atstart last" "6 typing atstart on\n8 typing off\n10 quit discard\n")
# 4. "On": on although it was left off. Now "Off", with the keyboard left on.
run_app("typing state on atstart on" "6 typing atstart off\n8 typing on\n10 quit discard\n")
# 5. "Off": off although it was left on. Back to "as it was left": it was left on.
run_app("typing state off atstart off" "6 typing atstart last\n8 typing on\n10 quit discard\n")
run_app("typing state on atstart last" "6 quit discard\n")
# 6. A settings file from before the choice existed: off.
file(WRITE "${OUT}/cfg/gridline/settings.json" "{\"version\": 1, \"values\": {\"ui.scale\": 1.0, \"general.launcher\": false}}")
run_app("typing state off atstart last" "6 quit discard\n")
message(STATUS "ok - the typing keyboard starts as it was left by default, or on or off as chosen")
