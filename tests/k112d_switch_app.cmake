# Gridline - K-112D: Smooth's window state survives every other layout. In Smooth the Library is
# hidden (B); then Studio changes the tempo and saves; the save is opened again and goes through Clips
# and Tracker, which rename nothing of it; back in Smooth the Library is still hidden and the edit is
# there; the last save holds both. A project never opened in Smooth stays without the key.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112d_switch_app.cmake
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
22 key b
26 parts lib state
40 tempo 133
44 save ${OUT}/a.gridline.json
48 open ${OUT}/a.gridline.json
96 parts lib state
98 tempo
100 save ${OUT}/b.gridline.json
104 quit discard
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --layout-dev --layout logic --size 1440x900
                        --dev-input "${OUT}/input.txt" --fixed-dt 16 --frames 110
                        --switch-layout studio@30 --switch-layout live@60 --switch-layout tracker@75 --switch-layout logic@90
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app failed (${rc})")
endif()
string(REGEX MATCHALL "smoothlib state: shown [01] saved \"[a-z]*\"" states "${out}")
list(LENGTH states n)
if(NOT n EQUAL 3)
  message(FATAL_ERROR "FAIL: expected 3 state lines, got ${n}: ${states}")
endif()
list(GET states 0 s0)
list(GET states 1 s1)
list(GET states 2 s2)
if(NOT s0 MATCHES "shown 1" OR NOT s1 MATCHES "shown 0 saved \"closed\"")
  message(FATAL_ERROR "FAIL: B did not hide the Library in Smooth: ${s0} / ${s1}")
endif()
if(NOT s2 MATCHES "shown 0 saved \"closed\"")
  message(FATAL_ERROR "FAIL: after Studio, a reopen, Clips and Tracker the Library is not hidden in Smooth: ${s2}")
endif()
foreach(f a b)
  file(READ "${OUT}/${f}.gridline.json" saved)
  string(JSON lib ERROR_VARIABLE e1 GET "${saved}" smooth library)
  string(JSON bpm ERROR_VARIABLE e2 GET "${saved}" tempo)
  if(NOT lib STREQUAL "closed")
    message(FATAL_ERROR "FAIL: ${f}: the save lost the Smooth state (${lib} ${e1})")
  endif()
  if(NOT bpm MATCHES "^133")
    message(FATAL_ERROR "FAIL: ${f}: the save lost the edit made in Studio (tempo ${bpm} ${e2})")
  endif()
endforeach()
file(READ "${DEMO}" demo)
if(demo MATCHES "\"smooth\"")
  message(FATAL_ERROR "FAIL: the fixture already carries the key; the test needs a project without it")
endif()
message(STATUS "ok - Smooth's Library state and an edit made in Studio survive Studio, a reopen, Clips and Tracker")
