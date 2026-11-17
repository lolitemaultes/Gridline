# Gridline - K-123 core A (0.7.7) in the real app (hidden, isolated config and HOME, the dummy sound device
# with a simulated input: never a microphone). ROW picks the row:
#   quick     : quick punch: punching out and in again while the transport plays gives pieces of one take
#               (not take lanes), crossfaded at the punches.
#   recover   : capture while armed: a pass played without Record comes back as a clip (one undo step).
#   startnote : start on the first sound: Play waits, the input starts the transport and the recording.
#   latency   : the loopback check: a simulated cable that many frames long is measured exactly, and the
#               measurement is used for recordings (kept after a quit).
#   layouts   : the Record window's new controls and the latency check in Studio, Smooth, Clips and Tracker.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -DROW=<row> -P k123_core_a_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true}}\n")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_NO_UI_SOUNDS} "1")
if(ROW STREQUAL "latency" OR ROW STREQUAL "layouts")
  set(ENV{GRIDLINE_LOOPBACK_INPUT} "1234")
else()
  set(ENV{GRIDLINE_SYNTH_INPUT} "1")
endif()

set(step 0)
function(run_app script)
  math(EXPR n "${step} + 1")
  set(step ${n} PARENT_SCOPE)
  file(WRITE "${OUT}/run${n}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --layout-dev --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900
                          --dev-input "${OUT}/run${n}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 200)
  file(WRITE "${OUT}/run${n}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gridline-app failed in run ${n} (${rc}): ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

# The first line of `out` matching `re`, which must match `want` (a regex).
function(expect_match what re want)
  string(REGEX MATCH "${re}" got "${out}")
  if(NOT got MATCHES "${want}")
    message(FATAL_ERROR "FAIL: ${what}: \"${got}\" does not match \"${want}\"\n${out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

if(ROW STREQUAL "quick")
  run_app("3 recmore quick 1
4 liverec arm 1
6 liverec start
7 waitfor never 1.0
8 recmore show
9 recmore punch
10 waitfor never 0.6
11 recmore show
12 recmore punch
13 waitfor never 0.6
14 recmore show
15 liverec stop
16 waitfor never 1.0
17 recmore show
18 recmore clips
20 quit discard
")
  expect_match("recording while the transport plays" "recmore frame 8:[^\n]*" "playing 1 recording 1 session 1")
  expect_match("punched out: still playing, the session open" "recmore frame 11:[^\n]*" "playing 1 recording 0 session 1")
  expect_match("punched in again" "recmore frame 14:[^\n]*" "playing 1 recording 1 session 1")
  expect_match("stopped: the session closed" "recmore frame 17:[^\n]*" "playing 0 recording 0 session 0")
  # Two pieces of one pass, not take lanes: the first fades out at the punch out, the second in at the punch in.
  expect_match("two clips, one undo step" "recmore frame 18:[^\n]*" "clips 2 undo \"[^\"]+\": \\[t[0-9]+ [0-9]+\\+[0-9]+ in 0\\.0000 out 0\\.0[0-9]+\\] \\[t[0-9]+ [0-9]+\\+[0-9]+ in 0\\.0[0-9]+ out 0\\.0000\\]$")
elseif(ROW STREQUAL "recover")
  # K-326: a slow tempo (a 24 s bar), so the pass is one continuous stretch however long a slow machine
  # takes over the wait: at the default tempo the one-bar song looped there (1.7 s), splitting the pass.
  run_app("2 tempo 10
3 recmore capture 30
4 liverec arm 1
6 recopt play
7 waitfor never 1.5
8 liverec stop
9 waitfor never 0.3
10 recmore show
11 recmore clips
12 recmore recover
13 recmore clips
14 undo
15 recmore clips
20 quit discard
")
  expect_match("the armed track keeps what was played" "recmore frame 10:[^\n]*" "recording 0 session 0 held [1-9][0-9.]* tracks 1")
  expect_match("nothing recorded" "recmore frame 11:[^\n]*" "clips 0 ")
  expect_match("recover: one clip, one undo step" "recmore frame 12:[^\n]*" "recovered 1 undo \"Recover audio\"")
  expect_match("the clip is on the playlist, from the start" "recmore frame 13:[^\n]*" "clips 1 [^\n]*\\[t[0-9]+ 0\\+[1-9][0-9]* ")
  expect_match("undo takes it back" "recmore frame 15:[^\n]*" "clips 0 ")
elseif(ROW STREQUAL "startnote")
  # 1. Notes: Play waits; the first key played starts the transport and the recording, and that note
  #    goes in at the start (pattern mode, a slow tempo so the pass outlasts the frames).
  run_app("3 recmore startnote 1
4 recmore startlevel 0
5 playmode pattern
6 typing on
7 recopt qlen 0
8 tempo 20
9 recopt audio 0
10 recopt list
11 recopt arm 1
12 recopt play
14 recmore show
20 sdlkey z down
22 recmore show
40 sdlkey z up
50 recopt stop
54 recopt list
56 recmore show
60 quit discard
")
  expect_match("Play waits for the first note" "recmore frame 14:[^\n]*" "startnote 1 level 0 waiting 1 starts 0 playing 0")
  expect_match("the first note started the transport" "recmore frame 22:[^\n]*" "waiting 0 starts 0 playing 1")
  expect_match("that note is recorded at the start" "recnotes frame 54:[^\n]*" " 48@0\\+[1-9][0-9]* ")
  expect_match("stopped" "recmore frame 56:[^\n]*" "waiting 0 starts 0 playing 0")
  # 2. A sound on an armed track's input above the level starts it (the simulated input alternates
  #    -14 and -3 dBFS each second: a -12 dB level starts within a second).
  run_app("3 recmore startnote 1
4 recmore startlevel -12
5 liverec arm 1
6 recmore show
8 liverec start
10 waitfor never 1.3
11 recmore show
12 liverec stop
13 waitfor never 0.5
14 recmore clips
20 quit discard
")
  expect_match("the setting" "recmore frame 6:[^\n]*" "startnote 1 level -12 waiting 0 starts 0 playing 0")
  expect_match("the input started the transport and the recording" "recmore frame 11:[^\n]*" "waiting 0 starts 1 playing 1 recording 1")
  expect_match("the take" "recmore frame 14:[^\n]*" "clips 1 ")
elseif(ROW STREQUAL "latency")
  run_app("4 recmore latency
5 recmore latency measure 0
6 waitfor latency-measured 20
7 recmore latency
8 recmore latency use
9 recmore latency
20 quit
")
  expect_match("not measured: the estimate" "recmore frame 4:[^\n]*" "phase 0 ok 0 [^\n]*measured 0")
  # The record latency is the round trip plus the song's own delay compensation (as with the estimate).
  string(REGEX MATCH "recmore frame 4:[^\n]* auto ([0-9]+) record ([0-9]+)" m "${out}")
  math(EXPR pdc "${CMAKE_MATCH_2} - ${CMAKE_MATCH_1}")
  math(EXPR want "1234 + ${pdc}")
  expect_match("the simulated cable, measured to the sample (three runs agree)" "recmore frame 7:[^\n]*" "phase 2 ok 1 frames 1234 agree 3 of 3")
  expect_match("used for recordings: the round trip replaces the estimate" "recmore frame 9:[^\n]*" "measured 1234 auto [0-9]+ record ${want}$")
  run_app("4 recmore latency
5 recmore latency clear
6 recmore latency
20 quit
")
  expect_match("kept after a quit (same devices and buffer)" "recmore frame 4:[^\n]*" "measured 1234 auto [0-9]+ record ${want}$")
  expect_match("back to the estimate" "recmore frame 6:[^\n]*" "measured 0 auto [0-9]+ record [0-9]+$")
elseif(ROW STREQUAL "layouts")
  # Each layout: the Record window's Mode page (quick punch, crossfade, capture while armed), Start page,
  # Inputs page (latency) and the latency check after a measurement.
  set(script "3 liverec arm 1\n4 recmore quick 1\n")
  set(f 10)
  foreach(lay studio smooth clips tracker)
    set(id "${lay}")
    if(lay STREQUAL "smooth")
      set(id "logic")
    elseif(lay STREQUAL "clips")
      set(id "live")
    endif()
    string(APPEND script "${f} recmore layout ${id}\n")
    math(EXPR f "${f} + 10")
    foreach(pg 1 2 4 6)
      math(EXPR a "${f}")
      math(EXPR b "${f} + 6")
      string(APPEND script "${a} recmore window ${pg}\n${b} shot \"${OUT}/${lay}_page${pg}.ppm\"\n")
      math(EXPR f "${f} + 10")
    endforeach()
    math(EXPR a "${f}")
    math(EXPR b "${f} + 1")
    math(EXPR c "${f} + 2")
    math(EXPR d "${f} + 8")
    math(EXPR e "${f} + 9")
    string(APPEND script "${a} recmore latency measure 0\n${b} waitfor latency-measured 20\n${c} forget latency-measured\n${d} shot \"${OUT}/${lay}_latency.ppm\"\n${e} recmore latency close\n")
    math(EXPR f "${f} + 30")
  endforeach()
  string(APPEND script "${f} quit discard\n")
  run_app("${script}")
  foreach(lay studio smooth clips tracker)
    foreach(pg page1 page2 page4 page6 latency)
      if(NOT EXISTS "${OUT}/${lay}_${pg}.ppm")
        message(FATAL_ERROR "FAIL: no screenshot ${lay}_${pg}")
      endif()
    endforeach()
  endforeach()
  message(STATUS "ok - the Record window pages and the latency check drawn in every layout")
else()
  message(FATAL_ERROR "unknown ROW ${ROW}")
endif()
