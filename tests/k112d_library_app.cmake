# Gridline - K-112D: Smooth's Library. A project opened in Smooth shows the Library (the reference
# opens a new project with it); a click on a patch loads it onto the selected track in one undo step;
# B hides it, the save notes that, and the saved project opens again with it hidden.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112d_library_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"logic\", \"keys.profile\": \"logic\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
file(WRITE "${OUT}/input.txt" "5 toasts clear
20 parts lib state
22 parts lib nav 1
24 parts lib load 0
28 parts lib state
30 undo
34 parts lib state
36 key b
42 parts lib state
44 save ${OUT}/closed.gridline.json
48 open ${OUT}/closed.gridline.json
60 parts lib state
62 quit discard
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --layout-dev --layout logic --size 1920x1080
                        --dev-input "${OUT}/input.txt" --fixed-dt 16 --frames 70
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app failed (${rc})")
endif()
string(REGEX MATCHALL "smoothlib state: shown [01] saved \"[a-z]*\" nav [0-9]+ track -?[0-9]+ sound \"[^\"]*\"" states "${out}")
list(LENGTH states n)
if(NOT n EQUAL 5)
  message(FATAL_ERROR "FAIL: expected 5 state lines, got ${n}: ${states}")
endif()
list(GET states 0 s0)
list(GET states 1 s1)
list(GET states 2 s2)
list(GET states 3 s3)
list(GET states 4 s4)
if(NOT s0 MATCHES "shown 1 saved \"\"")
  message(FATAL_ERROR "FAIL: the Library is not open on a project opened in Smooth: ${s0}")
endif()
string(REGEX REPLACE ".*sound \"([^\"]*)\"" "\\1" before "${s0}")
string(REGEX REPLACE ".*sound \"([^\"]*)\"" "\\1" loaded "${s1}")
string(REGEX REPLACE ".*sound \"([^\"]*)\"" "\\1" undone "${s2}")
if(loaded STREQUAL before)
  message(FATAL_ERROR "FAIL: the click loaded nothing onto the track (${before})")
endif()
if(NOT undone STREQUAL before)
  message(FATAL_ERROR "FAIL: one undo did not bring the track's sound back (${before} / ${undone})")
endif()
if(NOT s3 MATCHES "shown 0 saved \"closed\"")
  message(FATAL_ERROR "FAIL: B did not hide the Library and note it in the project: ${s3}")
endif()
file(READ "${OUT}/closed.gridline.json" saved)
if(NOT saved MATCHES "\"smooth\"")
  message(FATAL_ERROR "FAIL: the save does not carry the Library state")
endif()
if(NOT s4 MATCHES "shown 0 saved \"closed\"")
  message(FATAL_ERROR "FAIL: the saved project did not open with the Library hidden: ${s4}")
endif()

# R09-02: a track made with New tracks > Instrument (Tercet, an instrument that can also play a sample)
# is the Library's target: a patch replaces its instrument (no new track, no new channel), in one undo step.
file(WRITE "${OUT}/input2.txt" "5 toasts clear
10 parts lib newtrack
14 parts lib tracks
16 parts lib state
18 parts lib nav 1
20 parts lib load 0
24 parts lib state
26 parts lib tracks
28 undo
32 parts lib state
34 parts lib tracks
36 quit discard
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --layout-dev --layout logic --size 1920x1080
                        --dev-input "${OUT}/input2.txt" --fixed-dt 16 --frames 40
                RESULT_VARIABLE rc2 OUTPUT_VARIABLE out2 ERROR_VARIABLE err2 TIMEOUT 240)
file(WRITE "${OUT}/run2.log" "${out2}\n---- stderr ----\n${err2}")
if(NOT rc2 EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app failed on the new-track run (${rc2})")
endif()
string(REGEX MATCH "smoothlib newtrack: tracks [0-9]+ channels [0-9]+ selected (-?[0-9]+)" nt "${out2}")
set(newTrack "${CMAKE_MATCH_1}")
string(REGEX MATCHALL "smoothlib state: [^\n]*" st2 "${out2}")
string(REGEX MATCHALL "smoothlib tracks: channels [0-9]+" ch2 "${out2}")
string(REGEX MATCHALL "smoothlib track [0-9]+ [^\n]*" tr2 "${out2}")
list(LENGTH st2 n2)
list(LENGTH ch2 c2)
if(NOT nt OR NOT n2 EQUAL 3 OR NOT c2 EQUAL 3)
  message(FATAL_ERROR "FAIL: the new-track run did not report (${nt} / ${st2} / ${ch2})")
endif()
list(GET st2 0 a0)
list(GET st2 1 a1)
list(GET st2 2 a2)
if(NOT a0 MATCHES "track ${newTrack} sound \"Tercet\" type gl.synth")
  message(FATAL_ERROR "FAIL: the Library does not see the new instrument track ${newTrack} (R09-02): ${a0}")
endif()
if(NOT a1 MATCHES "track ${newTrack} sound \"[^\"]+\"" OR a1 MATCHES "sound \"Tercet\" type gl.synth")
  message(FATAL_ERROR "FAIL: the patch did not load onto the selected new track: ${a1}")
endif()
list(GET ch2 0 b0)
list(GET ch2 1 b1)
list(GET ch2 2 b2)
if(NOT b0 STREQUAL b1)
  message(FATAL_ERROR "FAIL: the patch made a new channel instead of loading onto the track (${b0} / ${b1})")
endif()
if(NOT a2 STREQUAL a0 OR NOT b2 STREQUAL b0)
  message(FATAL_ERROR "FAIL: one undo did not bring the track's Tercet back (${a0} / ${a2})")
endif()
set(nTracks0 0)
set(nTracks1 0)
foreach(l IN LISTS tr2)
  if(l MATCHES "^smoothlib track ${newTrack} ")
    math(EXPR nTracks1 "${nTracks1} + 1")
  endif()
endforeach()
if(NOT nTracks1 EQUAL 3)
  message(FATAL_ERROR "FAIL: the new track did not stay the one instrument track loaded (${tr2})")
endif()
message(STATUS "ok - Library open by default, a patch loads in one undo step (a new Tercet track too), its state saved with the project")
