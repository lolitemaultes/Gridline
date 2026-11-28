# Gridline: K-446, automated controls move on screen while the song plays.
# The fixture's playlist automates a channel's volume (channel rack knob), a mixer track's volume
# (fader), one of that track's effect parameters (effect window), an instrument parameter
# (instrument window) and a send level, each a ramp up over two bars and back down.
# In each of the four layouts the app runs hidden (isolated config and HOME, SDL's dummy audio),
# plays, and reports every control's drawn value ("knobs", with the parameter it shows) twice,
# 1.5 s of playback apart:
#   1. every automated control on screen shows a moving value; the stored value stays where it was;
#   2. each automated parameter is seen moving in at least one layout;
#   3. stopped, the controls show the stored values again (no live value left over);
#   4. a grab wins: a knob held and moved shows the user's value, which no longer follows the
#      automation while it is held.
# Screenshots: <OUT>/play_<layout>.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<k446_automated.gridline.json> -DOUT=<dir> -P k446_automated_knobs_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home" "${OUT}/cfg/gridline")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_CACHE_DIR} "${OUT}/cache")
file(WRITE "${OUT}/cfg/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 ${ARGN}
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.out" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
# report(<frame> <var>): the knob lines of the "knobs" report of that frame.
function(report f var)
  string(FIND "${out}" "knobs frame ${f} " at)
  if(at LESS 0)
    message(FATAL_ERROR "FAIL: no knob report at frame ${f}\n${out}")
  endif()
  string(SUBSTRING "${out}" ${at} -1 rest)
  string(FIND "${rest}" "knobs end" e)
  string(SUBSTRING "${rest}" 0 ${e} b)
  set(${var} "${b}" PARENT_SCOPE)
endfunction()
# values(<block> <owner> <index> <var>): the drawn values of the controls showing that parameter.
function(values b owner index var)
  string(REGEX MATCHALL "value [-0-9.e+]+ norm [-0-9.e+]+ [^\n]* pid ${owner} ${index}\n" ls "${b}")
  set(vs "")
  foreach(l ${ls})
    string(REGEX MATCH "norm ([-0-9.e+]+)" _ "${l}")
    list(APPEND vs "${CMAKE_MATCH_1}")
  endforeach()
  set(${var} "${vs}" PARENT_SCOPE)
endfunction()

# The automated parameters: "<label>|<owner>|<index>".
set(params "channel volume|20|0" "mixer fader|4|0" "effect parameter|4|1001" "instrument parameter|25|101" "send level|4|200")
set(required "channel volume" "mixer fader" "effect parameter" "instrument parameter")  # (a send knob shows only for a selected route)
set(seen "")
foreach(lay studio live logic tracker)
  # Open the effect and the instrument windows, play, wait for the transport, report, play 1.5 s, report.
  run(play_${lay} 60 "4 plugwin fx 2 0\n6 plugwin chan 5\n10 waitfor transport-moved 20\n20 knobs\n22 waitfor k446-never 1.5\n30 knobs\n40 docstate\n58 quit discard\n"
      --layout ${lay} --play)
  report(20 b0)
  report(30 b1)
  set(moving 0)
  foreach(p ${params})
    string(REPLACE "|" ";" f "${p}")
    list(GET f 0 label)
    list(GET f 1 owner)
    list(GET f 2 index)
    values("${b0}" ${owner} ${index} v0)
    values("${b1}" ${owner} ${index} v1)
    list(LENGTH v0 n0)
    list(LENGTH v1 n1)
    if(n0 GREATER 0 AND n1 EQUAL n0)
      math(EXPR last "${n0} - 1")
      foreach(i RANGE ${last})
        list(GET v0 ${i} a)
        list(GET v1 ${i} c)
        if(a STREQUAL c)
          message(FATAL_ERROR "FAIL: ${lay}: the ${label} control ${i} stays at ${a} while its automation plays\n${b0}\n${b1}")
        endif()
      endforeach()
      message(STATUS "ok - ${lay}: the ${label} moves with its automation (${v0} -> ${v1})")
      list(APPEND seen "${label}")
      math(EXPR moving "${moving} + 1")
    endif()
  endforeach()
  check("${lay}: automated controls on screen move (${moving})" moving GREATER 0)
endforeach()
foreach(label ${required})
  list(FIND seen "${label}" k)
  check("the ${label} is seen moving in at least one layout" k GREATER -1)
endforeach()

# ---- 3. stopped: the stored values (a run that never plays, then one that plays and stops)
run(stopped 40 "4 plugwin fx 2 0\n6 plugwin chan 5\n20 knobs\n38 quit discard\n")
report(20 bs)
values("${bs}" 20 0 vs)
list(LENGTH vs ns)
check("stopped: the rack's volume knob is drawn (${ns})" ns GREATER 0)
list(GET vs 0 stored)
run(playstop 60 "4 plugwin fx 2 0\n6 plugwin chan 5\n10 waitfor transport-moved 20\n20 waitfor k446-never 1.0\n24 stopvia button\n26 waitfor transport-stopped 10\n36 knobs\n58 quit discard\n"
    --play)
report(36 bp)
values("${bp}" 20 0 vp)
list(GET vp 0 after)
check("played and stopped: the knob shows the stored volume again (${after}, stored ${stored})" after STREQUAL stored)

# ---- 4. a grab wins (Studio: the rack's volume knob of the automated channel)
string(REGEX MATCH "item ([0-9a-f]+) win \"Channel rack\" [^\n]* at ([0-9.]+) ([0-9.]+) [^\n]* pid 20 0\n" _ "${bs}")
set(kx "${CMAKE_MATCH_2}")
set(ky "${CMAKE_MATCH_3}")
check("the automated channel's rack knob is at ${kx} ${ky}" kx MATCHES "[0-9]")
string(REGEX REPLACE "\\..*" "" kx "${kx}")
string(REGEX REPLACE "\\..*" "" ky "${ky}")
math(EXPR ky1 "${ky} - 30")
run(grab 70 "10 waitfor transport-moved 20\n20 move ${kx} ${ky}\n22 down 0\n24 move ${kx} ${ky1}\n26 move ${kx} ${ky1}\n30 knobs\n32 waitfor k446-never 1.0\n40 knobs\n44 up 0\n50 docstate\n68 quit discard\n"
    --layout studio --play)
report(30 g0)
report(40 g1)
values("${g0}" 20 0 h0)
values("${g1}" 20 0 h1)
list(GET h0 0 a)
list(GET h1 0 c)
check("held after a move, the knob keeps the user's value while the automation plays on (${a} then ${c})" a STREQUAL c)
message(STATUS "done")
