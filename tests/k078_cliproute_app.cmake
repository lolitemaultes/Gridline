# Gridline - 0.7.8: the Audio clip window's mixer track box sits in its header, as a plugin window's
# header has it. Runs the app hidden (isolated config, fixed frame time, 1440x900) on a copy of the test
# song with a sampler channel "Take" playing an audio clip (clip 70), and checks in each layout where the
# window's form shows (Studio's window, the Clips band's head card, Smooth's audio editor, the Tracker's
# Advanced Sample editor page):
#   1. the box is there, above the waveform (in the header, not in a card under it);
#   2. a click opens the mixer's tracks; picking track 2 routes Take there in one undo step (the box
#      reads it), and undo puts it back on the master;
#   3. the free track's button beside it routes Take to a free insert in one undo step.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k078_cliproute_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)
set(chanJson "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clipJson "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chanJson}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clipJson}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

function(run tag layout script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\", \"tracker.sampleView\": \"advanced\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames 120
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# The "routes" block of a frame: Take's track and the undo position.
function(routeOf frame track undo)
  string(REGEX MATCH "routes frame ${frame}:[^\n]*undo ([0-9]+)[^\n]*\n(  route [^\n]*\n)*" b "${out}")
  set(u "${CMAKE_MATCH_1}")
  string(REGEX MATCH "route Take -> ([0-9]+)" _ "${b}")
  set(${track} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${undo} "${u}" PARENT_SCOPE)
endfunction()

foreach(layout studio live logic tracker)
  # (Clips: the band shows the clip's first card; Edit audio brings its start, the head card, into view)
  run(${layout} ${layout} "6 clipeditor dbl 70
12 clipeditor chip 10
16 clipeditor geom
18 sdlat ceroute
19 routes
20 sdldown 0
21 sdlup 0
28 sdlat ceroutetrack2
30 sdldown 0
31 sdlup 0
38 routes
40 sdlat ceroute
44 undo
48 routes
52 sdlat ceroutefree
54 sdldown 0
55 sdlup 0
62 routes
64 shot \"${OUT}/${layout}.ppm\"
70 quit discard
")
  # 1. in the header: above the waveform (Clips: the head card, Smooth: the audio editor's local
  # inspector head, both left of the waveform)
  string(REGEX MATCH "sdlat frame 18: ceroute at ([0-9]+),([0-9]+)" _ "${out}")
  set(bx "${CMAKE_MATCH_1}")
  set(by "${CMAKE_MATCH_2}")
  string(REGEX MATCH "clipeditor geom frame [0-9]+ wave ([0-9-]+) ([0-9-]+)" _ "${out}")
  set(wx "${CMAKE_MATCH_1}")
  set(wy "${CMAKE_MATCH_2}")
  check("${layout}: the mixer track box is drawn (${bx},${by})" "x${by}" MATCHES "^x[0-9]+$")
  if(layout STREQUAL "studio" OR layout STREQUAL "tracker")
    check("${layout}: the box sits above the waveform, in the header (${by} < ${wy})" by LESS wy)
  else()
    check("${layout}: the box sits in the head, left of the waveform (${bx} < ${wx})" bx LESS wx)
  endif()
  # 2. pick track 2, one undo step; undo
  routeOf(19 t0 u0)
  routeOf(38 t1 u1)
  routeOf(48 t2 u2)
  check("${layout}: Take starts on the master (${t0})" t0 EQUAL 0)
  math(EXPR step "${u1} - ${u0}")
  check("${layout}: picking track 2 in the box routes Take there (${t1}) in one undo step (${step})" t1 EQUAL 2 AND step EQUAL 1)
  check("${layout}: the box reads the new track" "${out}" MATCHES "sdlat frame 40: ceroute at ")
  check("${layout}: undo puts Take back on the master (${t2})" t2 EQUAL 0)
  # 3. the free track's button
  routeOf(62 t3 u3)
  math(EXPR step "${u3} - ${u2}")
  check("${layout}: the free track's button beside the box routes Take to a free insert (${t3}) in one undo step (${step})"
        "${out}" MATCHES "sdlat frame 52: ceroutefree at " AND NOT t3 EQUAL 0 AND step EQUAL 1)
endforeach()
