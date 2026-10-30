# Gridline: J-195, "Route to a free mixer track" in the channel window header (hidden, isolated
# config and HOME, 1440x900, a fixed 16 ms frame time). A copy of the test song gets an audio clip
# channel "Take", a synth channel "Lead" and an installed-plugin channel that is not installed,
# all on the master; inserts 1-8 are named and used, 9 onwards are free.
#   1. audio clip channel: the header button routes it to insert 9, named "Take", the track
#      selected, one undo step; a second click keeps the track (no new undo step); undo goes back
#      to the master;
#   2. instrument: the first item of the route list does the same (insert 9, "Lead");
#   3. installed plugin channel (placeholder): the button is in its header too and works.
# Screenshots of each header: <OUT>/audio.ppm, <OUT>/list.ppm, <OUT>/plugin.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P j195_route_free.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)  # a copy: the test song is never opened (or saved over) in place
set(chan "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 2, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"audioClip\": true, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
string(APPEND chan "{\"color\": 14251782, \"gain\": 0.8, \"id\": 61, \"mixerTrack\": 2, \"muted\": false, \"name\": \"Lead\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.synth\", \"params\": []}}, ")
string(APPEND chan "{\"color\": 4359668, \"gain\": 0.8, \"id\": 62, \"mixerTrack\": 2, \"muted\": false, \"name\": \"Ext Synth\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"vst3:0123456789ABCDEF0123456789ABCDEF\", \"params\": []}}, ")
set(clips "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chan}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clips}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag chan script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --chan ${chan}
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(check what regex)
  if(NOT out MATCHES "${regex}")
    message(FATAL_ERROR "FAIL: ${what}\n(looked for: ${regex})\n${out}")
  endif()
  message(STATUS "ok: ${what}")
endfunction()

# The "routes" block of a frame.
function(routes frame var)
  string(REGEX MATCH "routes frame ${frame}:[^\n]*\n(  route [^\n]*\n)*" b "${out}")
  set(${var} "${b}" PARENT_SCOPE)
endfunction()

# 1. Audio clip channel (channel 0), the header button.
run(audio 0 "8 routes
10 sdlat chroutefree
12 sdldown 0
13 sdlup 0
18 routes
20 sdldown 0
21 sdlup 0
26 routes
28 undo
30 routes
34 sdlat chroutefree
40 shot \"${OUT}/audio.ppm\"
44 quit discard
")
check("audio: the button is in the header" "sdlat frame 10: chroutefree at ")
routes(8 r8)
routes(18 r18)
routes(26 r26)
routes(30 r30)
if(NOT r8 MATCHES "route Take -> 0 \"Master\"")
  message(FATAL_ERROR "FAIL: the audio clip channel starts on the master\n${r8}")
endif()
if(NOT r18 MATCHES "route Take -> 9 \"Take\"" OR NOT r18 MATCHES "selected 9, undo [0-9]+ \"route to free mixer tracks\"")
  message(FATAL_ERROR "FAIL: the click routes the clip to insert 9, named after it, selected, one undo step\n${r18}")
endif()
message(STATUS "ok: audio clip channel routed to free insert 9, track named \"Take\" and selected")
string(REGEX MATCH "undo ([0-9]+)" _ "${r18}")
set(u18 "${CMAKE_MATCH_1}")
string(REGEX MATCH "undo ([0-9]+)" _ "${r8}")
set(u8 "${CMAKE_MATCH_1}")
math(EXPR step "${u18} - ${u8}")
if(NOT step EQUAL 1)
  message(FATAL_ERROR "FAIL: one undo step expected, got ${step}\n${r8}\n${r18}")
endif()
string(REGEX MATCH "undo ([0-9]+)" _ "${r26}")
set(u26 "${CMAKE_MATCH_1}")
if(NOT r26 MATCHES "route Take -> 9 \"Take\"" OR NOT u26 EQUAL u18)
  message(FATAL_ERROR "FAIL: a second click keeps the channel's own track without a new undo step\n${r26}")
endif()
message(STATUS "ok: a second click keeps the track (no new undo step)")
if(NOT r30 MATCHES "route Take -> 0 \"Master\"" OR r30 MATCHES "\"Take\"\n")
  message(FATAL_ERROR "FAIL: undo puts the clip back on the master\n${r30}")
endif()
message(STATUS "ok: undo restores the route in one step")

# 2. Instrument (channel 1), the first item of the route list.
run(list 1 "8 routes
10 sdlat chroute
12 sdldown 0
13 sdlup 0
18 sdlat chroutelist
20 shot \"${OUT}/list.ppm\"
24 sdldown 0
25 sdlup 0
30 routes
34 quit discard
")
check("list: the item is the first of the route list" "sdlat frame 18: chroutelist at ")
routes(30 r30)
if(NOT r30 MATCHES "route Lead -> 9 \"Lead\"" OR NOT r30 MATCHES "selected 9, undo [0-9]+ \"route to free mixer tracks\"")
  message(FATAL_ERROR "FAIL: the list item routes the instrument to insert 9, named after it\n${r30}")
endif()
message(STATUS "ok: instrument routed to free insert 9 from the route list")

# 3. Installed plugin channel that is not installed here (Gridline's panel stands in).
run(plugin 2 "10 sdlat chroutefree
12 sdldown 0
13 sdlup 0
18 routes
22 sdlat chroutefree
28 shot \"${OUT}/plugin.ppm\"
32 quit discard
")
check("plugin: the button is in the plugin channel's header" "sdlat frame 10: chroutefree at ")
routes(18 r18)
if(NOT r18 MATCHES "route Ext Synth -> 9 \"Ext Synth\"")
  message(FATAL_ERROR "FAIL: the plugin channel is routed to insert 9, named after it\n${r18}")
endif()
message(STATUS "ok: plugin channel routed to free insert 9")

foreach(shot audio list plugin)
  if(NOT EXISTS "${OUT}/${shot}.ppm")
    message(FATAL_ERROR "FAIL: no screenshot ${OUT}/${shot}.ppm")
  endif()
endforeach()
message(STATUS "ok: header screenshots in ${OUT}")
