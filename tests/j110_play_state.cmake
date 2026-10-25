# Gridline — J-110: Seltmann's pattern Play button follows Stop, in the real app (hidden,
# isolated config, SDL's dummy audio driver: the engine runs in real time, nothing is heard).
# A synthetic break is dropped on a Seltmann editor and analysed; then, each time with a single
# click on the editor's Play button and the engine's own report (the trace hook's
# "seltmann-playing" = the button lit and a pattern step played; "seltmann-stopped" = the button
# off and the instrument's pattern flag clear):
#   1. Play, then the transport Stop button's action: stopped at once; one click plays again;
#   2. the song started from elsewhere (as a playlist click) while the pattern plays (it keeps playing;
#      J-112: Play / Space with the transport stopped would stop the pattern instead), Space stops: stopped;
#   3. Stop twice (the second one is the panic): stopped; one click plays again;
#   4. Tools > Stop all sound; 5. a MIDI controller's (remote) Stop; 6. the window losing focus;
#   7. a recorded take dragged from the playlist picker loads into Seltmann (analysed).
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P j110_play_state.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
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

# One click on Play: "<f> forget seltmann-playing / move / down / up / waitfor seltmann-playing".
function(click f out)
  math(EXPR f1 "${f} + 1")
  math(EXPR f2 "${f} + 3")
  math(EXPR f3 "${f} + 5")
  math(EXPR f4 "${f} + 6")
  set(${out} "${f} forget seltmann-playing
${f1} move ${px} ${py}
${f2} down 0
${f3} up 0
${f4} waitfor seltmann-playing 60
" PARENT_SCOPE)
endfunction()
# A way of stopping at frame f, then "waitfor seltmann-stopped" (the button must follow at once).
function(stopby f how out)
  math(EXPR f1 "${f} + 1")
  math(EXPR f2 "${f} + 2")
  set(${out} "${f} forget seltmann-stopped
${f1} ${how}
${f2} waitfor seltmann-stopped 1.5
" PARENT_SCOPE)
endfunction()

click(30 c1)
stopby(40 "stopvia button" s1)
click(50 c2)
# The song starts (not by Play: J-112) while the pattern plays (the pattern keeps playing), Space stops.
set(sp "60 startsong
62 epoch
70 forget seltmann-stopped
72 waitfor seltmann-stopped 0.5
74 epoch
80 forget seltmann-stopped
81 key space
82 waitfor seltmann-stopped 1.5
")
click(90 c3)
set(panic "100 stopvia button
104 forget seltmann-stopped
105 stopvia button
106 waitfor seltmann-stopped 1.5
")
click(110 c4)
stopby(120 "stopvia menu" s5)
click(130 c5)
stopby(140 "stopvia remote" s6)
click(150 c6)
stopby(160 "focuslost" s7)
click(170 c7)
run(flow 190 "${drop}${c1}${s1}${c2}${sp}${c3}${panic}${c4}${s5}${c5}${s6}${c6}${s7}${c7}")
file(WRITE "${OUT}/flow.log" "${flow_out}")

# "waitfor <name>: reached|timed out after <s> s", in script order.
string(REGEX MATCHALL "waitfor seltmann-(playing|stopped): (reached|timed out)" waits "${flow_out}")
set(want
  "waitfor seltmann-playing: reached"   # 1. one click plays
  "waitfor seltmann-stopped: reached"   #    Stop: stopped at once
  "waitfor seltmann-playing: reached"   #    one click plays again
  "waitfor seltmann-stopped: timed out" # 2. the song starts: the pattern keeps playing
  "waitfor seltmann-stopped: reached"   #    Space stops: stopped
  "waitfor seltmann-playing: reached"   #    one click plays
  "waitfor seltmann-stopped: reached"   # 3. Stop twice (panic): stopped
  "waitfor seltmann-playing: reached"   #    one click plays after the panic
  "waitfor seltmann-stopped: reached"   # 4. Tools > Stop all sound
  "waitfor seltmann-playing: reached"
  "waitfor seltmann-stopped: reached"   # 5. a MIDI controller's Stop
  "waitfor seltmann-playing: reached"
  "waitfor seltmann-stopped: reached"   # 6. focus lost
  "waitfor seltmann-playing: reached")
set(names "first click plays" "Stop: the button shows stopped" "one click plays again after Stop"
          "the song starts: the pattern keeps playing" "Space stops: stopped" "one click plays"
          "Stop twice (panic): stopped" "one click plays after the panic" "Tools > Stop all sound: stopped"
          "one click plays" "MIDI / remote Stop: stopped" "one click plays" "focus lost: stopped" "one click plays")
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
# The song really played (the transport ran while the pattern did).
string(REGEX MATCHALL "epoch frame [0-9]+: [0-9]+ playing [01]" ep "${flow_out}")
list(GET ep 0 e0)
if(NOT e0 MATCHES "playing 1")
  message(FATAL_ERROR "FAIL: the song did not start (${e0}). Output:\n${flow_out}")
endif()
message(STATUS "ok - the song started while the pattern played")

# ---- 7. A recorded take (the playlist picker's audio clip source of a recording: a 32-bit float
# WAV in the "Recorded" folder, as the recorder writes it) dragged onto the empty Seltmann: it
# loads into Seltmann and is analysed, like a browser audio file (the picker's drag used to be
# refused by plugin windows and channel rows).
file(MAKE_DIRECTORY "${OUT}/cfg/Recorded")
set(take "${OUT}/cfg/Recorded/Insert1_rec-0123456789abcdef.wav")
execute_process(COMMAND "${TESTBIN}" writetake "${take}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${take}")
  message(FATAL_ERROR "FAIL: could not write the recorded take (${rc})")
endif()
run(take 30 "5 audio \"${take}\" 0 1
7 channels
10 dragenter ${zx} ${zy} clipsrc:last
12 dragmove ${zx} ${zy}
14 dragmove ${zx} ${zy}
16 drop
17 waitfor seltmann-ready 120
20 channels
")
file(WRITE "${OUT}/take.log" "${take_out}")
if(NOT take_out MATCHES "channel [0-9]+ Insert1_rec-0123456789abcdef type gl.sampler data Insert1_rec-0123456789abcdef.wav")
  message(FATAL_ERROR "FAIL: the take did not become an audio clip source. Output:\n${take_out}")
endif()
message(STATUS "ok - the recorded take is an audio clip source in the playlist picker")
if(NOT take_out MATCHES "waitfor seltmann-ready: reached")
  message(FATAL_ERROR "FAIL: the take dragged from the picker did not load into Seltmann. Output:\n${take_out}")
endif()
if(NOT take_out MATCHES "type gl.seltmann data Insert1_rec-0123456789abcdef.wav")
  message(FATAL_ERROR "FAIL: Seltmann does not hold the take. Output:\n${take_out}")
endif()
if(NOT take_out MATCHES "seltmann: state ready kind break bpm 9[56]\\.[0-9]+ slices 16")
  message(FATAL_ERROR "FAIL: the take was not analysed as the 96 BPM break it is. Output:\n${take_out}")
endif()
message(STATUS "ok - a recorded take dragged from the picker loads into Seltmann and is analysed")
