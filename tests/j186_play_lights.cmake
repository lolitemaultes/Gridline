# Gridline - J-186: play lights in the real app (hidden, isolated config, dummy audio).
#   1. Channel rack: the demo song's "Beat" pattern plays (steps on Kick, Clap, Hat and Open hat;
#      Open hat muted here). The rows of Kick, Clap and Hat light up (each reports "rack-lit:<row>"
#      the frame its light comes on); the muted Open hat and the Snare / Bass / Chords rows (no
#      steps in this pattern) stay dark. The rack dump shows each row's light.
#   2. Kerf: a loop dropped into the editor, a note on region 3's key: the editor reports region 3
#      lit ("kerf-lit:3" and lit=3 in its trace line); after the note ends it goes dark again.
# Waits use "waitfor" (frames stop counting until the light is reported), so slow machines pass; after the
# drop the script holds 5 s of real time so the loop is loaded and sliced before the note (a slow runner
# reached the note while the drop was still loading).
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P j186_play_lights.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
get_filename_component(src "${CMAKE_CURRENT_LIST_DIR}/fixtures/test_song.gridline.json" ABSOLUTE)
file(READ "${src}" song)
# Open hat (id 23) muted: its row must stay dark while its steps play.
string(REPLACE "\"id\": 23,\n      \"mixerTrack\": 6,\n      \"muted\": false" "\"id\": 23,\n      \"mixerTrack\": 6,\n      \"muted\": true" song2 "${song}")
if(song2 STREQUAL song)
  message(FATAL_ERROR "FAIL: could not mute the Open hat channel in the fixture")
endif()
file(WRITE "${OUT}/lights.gridline.json" "${song2}")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --size 1440x900 --frames ${frames} --fixed-dt 16 --screenshot "${OUT}/${tag}.ppm"
                          --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 280)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.log" "${out}")
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# ---------------------------------------------------------------- 1. channel rack
run(rack 60 "4 channels
5 key l
7 transport
8 startsong
10 waitfor rack-lit:0 60
11 waitfor rack-lit:1 60
12 waitfor rack-lit:2 60
13 shot \"${OUT}/rack_lit.ppm\"
14 waitfor never-reported 2
16 rack
40 waitfor never-reported 1
41 rack
" "${OUT}/lights.gridline.json")
check("rack: the pattern plays in pattern mode (L)" rack_out MATCHES "transport frame [0-9]+: [^\n]* mode 0")
foreach(row 0 1 2)
  check("rack: row ${row} lit while the pattern plays" rack_out MATCHES "waitfor rack-lit:${row}: reached")
endforeach()
check("rack: the pattern plays" rack_out MATCHES "waitfor never-reported: timed out")
foreach(row 3 4 5 6)
  check("rack: row ${row} (muted / no steps here) never lit" NOT rack_out MATCHES "rack-lit:${row}")
endforeach()
string(REGEX MATCHALL "  channel 3 [^\n]* light ([0-9.]+)" m3 "${rack_out}")
check("rack: the dump reports each row's light" m3)
string(REGEX MATCHALL "  channel [0-9]+ volume[^\n]*light ([0-9.]+)" lines "${rack_out}")
set(any_lit 0)
foreach(l IN LISTS lines)
  if(l MATCHES "  channel ([0-9]+) [^\n]*light ([0-9.]+)")
    set(idx ${CMAKE_MATCH_1})
    set(v ${CMAKE_MATCH_2})
    if(idx GREATER_EQUAL 3 AND idx LESS_EQUAL 6 AND NOT v STREQUAL "0.000")
      message(FATAL_ERROR "FAIL: rack row ${idx} is lit (${v}) but it is muted or has no steps in this pattern:\n${l}")
    endif()
    if(idx LESS 3 AND NOT v STREQUAL "0.000")
      set(any_lit 1)
    endif()
  endif()
endforeach()
check("rack: a playing row is lit in the dump" any_lit EQUAL 1)

# ---------------------------------------------------------------- 2. Kerf
set(ENV{GRIDLINE_KERF_TRACE} "1")
run(kerf_empty 40 "30 channels\n" --plugin gl.slicer)
string(REGEX MATCHALL "kerf-ui:[^\n]*" kl "${kerf_empty_out}")
list(LENGTH kl n)
check("kerf: the editor reports its places" n GREATER 0)
math(EXPR i "${n} - 1")
list(GET kl ${i} l)
if(NOT l MATCHES " dropzone=([0-9]+),([0-9]+),([0-9]+),([0-9]+)")
  message(FATAL_ERROR "FAIL: no drop zone: ${l}")
endif()
math(EXPR zx "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
math(EXPR zy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
string(REGEX MATCHALL "\n  channel [0-9]+ [^\n]*" chs "${kerf_empty_out}")
set(kch -1)
set(idx 0)
foreach(c IN LISTS chs)  # Kerf's channel (added after the selected one)
  if(c MATCHES " type gl\\.slicer " AND kch EQUAL -1)
    set(kch ${idx})
  endif()
  math(EXPR idx "${idx} + 1")
endforeach()
check("kerf: its channel is in the rack" kch GREATER -1)
run(kerf 120 "10 dragenter ${zx} ${zy} \"${OUT}/Break 96.wav\"
42 dragmove ${zx} ${zy}
44 dragmove ${zx} ${zy}
46 drop
60 waitfor never-reported 5
70 noteon ${kch} 62
71 waitfor kerf-lit:3 30
74 shot \"${OUT}/kerf_lit.ppm\"
76 noteoff ${kch} 62
78 waitfor never-reported 2
100 move 10 10
" --plugin gl.slicer)
check("kerf: region 3 lit while its note plays" kerf_out MATCHES "waitfor kerf-lit:3: reached")
check("kerf: the trace lists region 3 lit" kerf_out MATCHES "kerf-ui:[^\n]* lit=3[\n ]")
string(REGEX MATCHALL "kerf-ui:[^\n]*" kl "${kerf_out}")
list(LENGTH kl n)
math(EXPR i "${n} - 1")
list(GET kl ${i} last)
check("kerf: dark again after the note ends" last MATCHES " lit=-")
message(STATUS "j186 play lights: rack rows light with their channel (muted / silent rows dark); Kerf lights the region a note plays")
