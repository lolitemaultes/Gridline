# Gridline: J-193, stretched clips follow the tempo (hidden, isolated configs, 1440x900, the playlist
# shown, a fixed 16 ms frame time). A copy of the demo song (124 BPM) gets a sample channel ("Take")
# and two audio clips cut to 5 beats: clip 70 on track 8, clip 71 on track 9.
#   1. stretch mode on (the saved switch), clip 70's right edge dragged: it is stretched and tempo-locked;
#   2. the tempo set to 100 BPM: clip 70 keeps its bars (same ticks, same pixels); clip 71 (never
#      stretched) keeps its audio and its border follows: 5 beats at 124 BPM are 4.03 beats at 100;
#      one undo step;
#   3. undo: the tempo and clip 71 as they were.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j193_tempo_follow.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
set(chan "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clips "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 480, \"pattern\": 0}, ")
string(APPEND clips "{\"id\": 71, \"kind\": \"audio\", \"source\": 60, \"track\": 8, \"start\": 0, \"length\": 480, \"pattern\": 0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chan}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clips}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}/gridline")
  # Stretch mode on (the playlist's "resizing stretches" switch, Shift+M), as saved by the user.
  file(WRITE "${OUT}/cfg_${tag}/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {\"playlist.stretchResize\": true}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --show playlist
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    fail("${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
function(int var v)  # "123.4" -> 123
  string(REGEX REPLACE "\\..*" "" i "${v}")
  set(${var} ${i} PARENT_SCOPE)
endfunction()
# Geometry of clip <id> at <frame>: <id>_x0 _x1 _y0 _y1 _head (integers).
function(clipgeom frame id)
  string(REGEX MATCH "plgeom frame ${frame}:[^\n]*\n(  [^\n]*\n)*" d "${out}")
  string(REGEX MATCH "  clip ${id} x ([-0-9.]+) ([-0-9.]+) y ([-0-9.]+) ([-0-9.]+) head ([0-9.]+)" _ "${d}")
  if(CMAKE_MATCH_1 STREQUAL "")
    fail("no geometry for clip ${id} at frame ${frame}")
  endif()
  set(k 1)
  foreach(n x0 x1 y0 y1 head)
    int(v "${CMAKE_MATCH_${k}}")
    set(${id}_${n} ${v} PARENT_SCOPE)
    math(EXPR k "${k} + 1")
  endforeach()
endfunction()
# The "dump" of <frame> -> clips_<frame> (the block) and undo_<frame>.
function(dump frame)
  string(REGEX MATCH "dump frame ${frame}: [0-9]+ clips, [0-9]+ selected, undo ([0-9]+)\n(  clip [^\n]*\n)*" d "${out}")
  if(d STREQUAL "")
    fail("no clip dump at frame ${frame}")
  endif()
  set(clips_${frame} "${d}" PARENT_SCOPE)
  set(undo_${frame} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
# Field <key> of clip <id> in dump <frame> -> <var>.
function(field var frame id key)
  string(REGEX MATCH "  clip ${id} kind [^\n]*" line "${clips_${frame}}")
  string(REGEX MATCH " ${key} ([^ ]+)" _ "${line}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# ---- geometry ----
run(geo 14 "8 plgeom\n")
clipgeom(8 70)
clipgeom(8 71)
math(EXPR ex "${70_x1} - 3")
math(EXPR ey "(${70_y0} + ${70_head} + ${70_y1}) / 2")
math(EXPR ex1 "${ex} + 40")
math(EXPR ex2 "${ex} + 80")
math(EXPR fx "${71_x1} + 200")

run(tempo 80 "2 typing off
3 move ${fx} ${ey}
10 dump
12 move ${ex} ${ey}
14 down 0
16 move ${ex1} ${ey}
18 move ${ex2} ${ey}
20 move ${ex2} ${ey}
22 up 0
26 dump
27 plgeom
30 tempo 100
36 dump
37 plgeom
40 undo
44 dump
")
foreach(f 10 26 36 44)
  dump(${f})
endforeach()

# 1. stretched and locked.
field(st 26 70 stretch)
field(lock 26 70 tempoLock)
field(len26 26 70 len)
check("clip 70 stretched by its right edge (stretch ${st}, ${len26} ticks)" st GREATER 1.05 AND len26 GREATER 480)
check("the stretch locks clip 70 to the tempo (tempoLock ${lock})" lock EQUAL 1)
field(l71 26 71 len)
check("clip 71 untouched (${l71} ticks)" l71 EQUAL 480)

# 2. tempo 124 -> 100.
field(len36 36 70 len)
check("clip 70 keeps its ticks at 100 BPM (${len26} -> ${len36})" len36 EQUAL len26)
clipgeom(27 70)
set(a70x1 ${70_x1})
clipgeom(37 70)
check("clip 70 keeps its bars on screen (${a70x1} -> ${70_x1} px)" 70_x1 EQUAL a70x1)
field(m36 36 71 len)
field(e36 36 71 audioEnd)
field(s36 36 71 audioStart)
# 5 beats at 124 BPM = 2.4194 s = 4.032 beats at 100 BPM = 387 ticks.
check("clip 71 (never stretched) keeps its audio: border 480 -> ${m36} ticks (want 387)" m36 EQUAL 387)
check("clip 71 keeps its cut (audioEnd ${e36}, audioStart ${s36})" e36 STREQUAL "2.419355" AND s36 STREQUAL "0.000000")
math(EXPR want "${undo_26} + 1")
check("one undo step for the tempo change (${undo_26} -> ${undo_36})" undo_36 EQUAL want)

# 3. undo.
field(u71 44 71 len)
field(ue71 44 71 audioEnd)
check("undo: clip 71 back to 480 ticks, uncut (${u71}, ${ue71})" u71 EQUAL 480 AND ue71 STREQUAL "0.000000")
