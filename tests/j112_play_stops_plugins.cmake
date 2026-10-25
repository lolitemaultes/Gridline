# Gridline — J-112: Play while the transport is stopped and Seltmann's pattern plays stops the
# pattern instead of starting the song; the next Play plays the song. In the real app (hidden,
# isolated config, SDL's dummy audio driver: the engine runs in real time, nothing is heard, no
# audio device needed). A synthetic break is dropped on a Seltmann editor and analysed; then, each
# time after one click on the editor's Play button ("seltmann-playing": the button lit and a step
# played; "plugin-playback": the engine reports it):
#   1. another window focused, Space: the pattern stops ("seltmann-stopped": the button off and
#      the instrument's flag clear), the transport stays stopped; Space again: the song plays;
#   2. the Seltmann window focused (the click focused it), Space: the same;
#   3. the transport's Play button (a real SDL click): the same;
#   4. the Play / Pause remote action (MIDI controller): the same;
#   5. the song started another way (as a playlist click): the pattern keeps playing (J-110).
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P j112_play_stops_plugins.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{XDG_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_SELTMANN_TRACE} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --plugin gl.seltmann --size 1440x960 --frames ${frames} --screenshot "${OUT}/${tag}.ppm"
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 280)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# ---- where the drop zone and (once loaded) the Play button are
run(empty 20 "# nothing\n")
if(NOT empty_out MATCHES "seltmann: zone ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+)")
  message(FATAL_ERROR "FAIL: no drop zone. Output:\n${empty_out}")
endif()
math(EXPR zx "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
math(EXPR zy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
set(drop "10 dragenter ${zx} ${zy} \"${OUT}/Break 96.wav\"
12 dragmove ${zx} ${zy}
14 dragmove ${zx} ${zy}
16 drop
17 waitfor seltmann-ready 120
")
run(ready 30 "${drop}")
string(REGEX MATCH "seltmann: play ([0-9]+) ([0-9]+)" _ "${ready_out}")
set(px "${CMAKE_MATCH_1}")
set(py "${CMAKE_MATCH_2}")
if(NOT px MATCHES "[0-9]")
  message(FATAL_ERROR "FAIL: the editor shows no Play button. Output:\n${ready_out}")
endif()

# One click on Seltmann's Play, then the engine reports plugin playback.
function(click f out)
  math(EXPR f1 "${f} + 1")
  math(EXPR f2 "${f} + 3")
  math(EXPR f3 "${f} + 5")
  math(EXPR f4 "${f} + 6")
  math(EXPR f5 "${f} + 7")
  set(${out} "${f} forget seltmann-playing
${f} forget plugin-playback
${f1} move ${px} ${py}
${f2} down 0
${f3} up 0
${f4} waitfor seltmann-playing 60
${f5} waitfor plugin-playback 5
" PARENT_SCOPE)
endfunction()
# Play at frame f (how = the script lines doing it at f, f+2, f+3): the pattern must stop, the
# transport stay stopped; the second Play (same way, at f+20) plays the song; then Stop.
function(press f how out)
  math(EXPR f1 "${f} + 4")
  math(EXPR f2 "${f} + 5")
  math(EXPR g "${f} + 20")
  math(EXPR g1 "${g} + 4")
  math(EXPR g2 "${g} + 5")
  math(EXPR h "${g} + 8")
  math(EXPR h1 "${g} + 9")
  math(EXPR f02 "${f} + 2")
  math(EXPR f03 "${f} + 3")
  string(REPLACE "@2" "${f02}" first "${how}")
  string(REPLACE "@3" "${f03}" first "${first}")
  string(REPLACE "@" "${f}" first "${first}")
  math(EXPR g02 "${g} + 2")
  math(EXPR g03 "${g} + 3")
  string(REPLACE "@2" "${g02}" second "${how}")
  string(REPLACE "@3" "${g03}" second "${second}")
  string(REPLACE "@" "${g}" second "${second}")
  set(${out} "${f} forget seltmann-stopped
${f} playstate
${first}${f1} waitfor seltmann-stopped 1.5
${f2} playstate
${g} forget transport-playing
${second}${g1} waitfor transport-playing 1.5
${g2} playstate
${h} stopvia button
${h1} forget transport-playing
" PARENT_SCOPE)
endfunction()

set(space "@ key space\n")
set(button "@ sdlat play\n@2 sdldown 0\n@3 sdlup 0\n")
set(remote "@ playvia remote\n")

click(30 c1)
press(40 "${space}" p1)
set(p1 "38 focus \"Channel rack\"\n39 playstate\n${p1}")  # 1. another window has the focus
click(80 c2)
press(90 "${space}" p2)                                # 2. the Seltmann window (the click focused it)
click(130 c3)
press(140 "${button}" p3)                              # 3. the transport's Play button
click(180 c4)
press(190 "${remote}" p4)                              # 4. the remote / MIDI Play
click(230 c5)
set(p5 "240 forget seltmann-stopped
241 startsong
243 waitfor seltmann-stopped 0.6
244 playstate
246 stopvia button
248 waitfor seltmann-stopped 1.5
")                                                     # 5. the song started another way
run(flow 270 "${drop}${c1}${p1}${c2}${p2}${c3}${p3}${c4}${p4}${c5}${p5}")
file(WRITE "${OUT}/flow.log" "${flow_out}")
if(flow_out MATCHES "sdlat frame [0-9]+: play not shown")
  message(FATAL_ERROR "FAIL: the transport's Play button is not shown. Output:\n${flow_out}")
endif()

string(REGEX MATCHALL "waitfor (seltmann-playing|seltmann-stopped|transport-playing|plugin-playback): (reached|timed out)" waits "${flow_out}")
set(want)
set(names)
foreach(way "another window focused, Space" "Seltmann focused, Space" "the Play button" "the remote Play")
  list(APPEND want "waitfor seltmann-playing: reached" "waitfor plugin-playback: reached" "waitfor seltmann-stopped: reached"
       "waitfor transport-playing: reached")
  list(APPEND names "${way}: one click plays the pattern" "${way}: the engine reports plugin playback"
       "${way}: Play stops the pattern (the button shows stopped)" "${way}: Play again plays the song")
endforeach()
list(APPEND want "waitfor seltmann-playing: reached" "waitfor plugin-playback: reached" "waitfor seltmann-stopped: timed out"
     "waitfor seltmann-stopped: reached")
list(APPEND names "5: one click plays the pattern" "5: plugin playback" "5: the song started another way: the pattern keeps playing"
     "5: Stop stops it")
list(LENGTH want n)
list(LENGTH waits got)
if(NOT got EQUAL n)
  message(FATAL_ERROR "FAIL: ${got} waits reported, want ${n}. Output:\n${flow_out}")
endif()
math(EXPR last "${n} - 1")
foreach(i RANGE ${last})
  list(GET want ${i} w)
  list(GET waits ${i} g)
  list(GET names ${i} nm)
  if(NOT g STREQUAL w)
    message(FATAL_ERROR "FAIL: ${nm} (got \"${g}\"). Output:\n${flow_out}")
  endif()
  message(STATUS "ok - ${nm}")
endforeach()

# playstate lines: per way, before the press (stopped, plugin playback), after it (stopped, none),
# after the second press (playing); then 5. (playing, the pattern still running).
string(REGEX MATCHALL "playstate frame [0-9]+: playing [01] own [0-9]+ epoch [0-9]+ focus \"[^\"]*\"" ps "${flow_out}")
list(LENGTH ps nps)
if(NOT nps EQUAL 14)
  message(FATAL_ERROR "FAIL: ${nps} playstate lines, want 14. Output:\n${flow_out}")
endif()
list(GET ps 0 focus0)
list(GET ps 1 before1)
list(GET ps 4 before2)
if(NOT focus0 MATCHES "focus \"Channel rack" OR before1 MATCHES "focus \"Seltmann")
  message(FATAL_ERROR "FAIL: 1. was meant to run with another window focused (${focus0}; ${before1}). Output:\n${flow_out}")
endif()
if(NOT before2 MATCHES "focus \"Seltmann")
  message(FATAL_ERROR "FAIL: 2. was meant to run with the Seltmann window focused (${before2}). Output:\n${flow_out}")
endif()
message(STATUS "ok - 1. ran with another window focused, 2. with the Seltmann window focused")
foreach(k 0 1 2 3)
  math(EXPR i0 "1 + ${k} * 3")
  math(EXPR i1 "${i0} + 1")
  math(EXPR i2 "${i0} + 2")
  list(GET ps ${i0} a)
  list(GET ps ${i1} b)
  list(GET ps ${i2} c)
  if(NOT a MATCHES "playing 0 own 1 ")
    message(FATAL_ERROR "FAIL: way ${k}: before Play the transport is stopped and Seltmann plays (${a}). Output:\n${flow_out}")
  endif()
  if(NOT b MATCHES "playing 0 own 0 ")
    message(FATAL_ERROR "FAIL: way ${k}: Play left the transport stopped and nothing plays (${b}). Output:\n${flow_out}")
  endif()
  if(NOT c MATCHES "playing 1 ")
    message(FATAL_ERROR "FAIL: way ${k}: the second Play plays the song (${c}). Output:\n${flow_out}")
  endif()
endforeach()
message(STATUS "ok - each time: stopped with Seltmann playing -> Play: still stopped, nothing playing -> Play: the song plays")
list(GET ps 13 s5)
if(NOT s5 MATCHES "playing 1 own 1 ")
  message(FATAL_ERROR "FAIL: 5. the song started another way should play alongside the pattern (${s5}). Output:\n${flow_out}")
endif()
message(STATUS "ok - a song start other than Play leaves the pattern playing")
string(REGEX MATCHALL "play: stopped plugin playback \\(Seltmann\\)" hints "${flow_out}")
list(LENGTH hints nh)
if(NOT nh EQUAL 4)
  message(FATAL_ERROR "FAIL: ${nh} plugin stops reported, want 4. Output:\n${flow_out}")
endif()
message(STATUS "ok - four Plays stopped Seltmann (the hint names it)")
