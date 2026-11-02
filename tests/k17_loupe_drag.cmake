# Gridline: K-17: audio dragged out of Loupe (the audio editor) onto the playlist, and the
# project's edited audio under the browser's PROJECT tab. The real app, hidden, isolated config
# and HOME, 1440x900, the playlist shown, Loupe open on mixer insert 1 slot 1 holding Snap.wav:
#   1. the toolbar's drag handle dragged (real SDL mouse events) onto playlist track 5: an audio
#      clip of the whole file where it was dropped (start snapped to the grid), one undo step;
#   2. a selection made in the waveform, then the handle dragged onto track 6: the clip plays a
#      new file holding just the selection (saved in the project's "Edited audio" folder);
#   3. the browser's PROJECT tab lists that edited file under the current project (Samples used),
#      and the Edited audio folder is there too.
# Screenshot: <OUT>/drag.ppm (the drag over the playlist), <OUT>/project.ppm (the PROJECT tab).
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k17_loupe_drag.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{HOME} "${OUT}/home")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/open.wav" "${OUT}/Snap.wav" COPYONLY)
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
# Loupe on insert 1 (Kick) slot 1, holding Snap.wav (a bare path is a Loupe state too).
string(REGEX REPLACE "(\"name\": \"Kick\",[ \n]*\"pan\": 0,[ \n]*\"sends\": \\[[^]]*\\],[ \n]*\"slots\": \\[[ \n]*{[ \n]*\"enabled\": true,[ \n]*\"mix\": 1)"
                     "\\1, \"plugin\": {\"data\": \"${OUT}/Snap.wav\", \"type\": \"gl.edison\"}" song "${demo}")
if(song STREQUAL demo)
  message(FATAL_ERROR "FAIL: the fixture's Kick mixer track was not found")
endif()
file(WRITE "${OUT}/fixture.gridline.json" "${song}")

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 ${ARGN}
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
function(geom)  # x0, pxPerTick and the track rows of the first plgeom block
  string(REGEX MATCH "plgeom frame [0-9]+: valid 1 x0 ([0-9.]+) pxPerTick ([0-9.]+)" _ "${out}")
  set(x0 "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(ppt "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()
# The audio clip on track <t> of the dump of frame <frame>: start, length, channel.
function(clipon frame t)
  string(REGEX MATCH "dump frame ${frame}: [^\n]*\n(  clip [^\n]*\n)*" blk "${out}")
  string(REGEX MATCH "  clip [0-9]+ kind 1 track ${t} start ([0-9]+) len ([0-9]+) [^\n]* source ([0-9]+) " _ "${blk}")
  set(cstart "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(clen "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(csrc "${CMAKE_MATCH_3}" PARENT_SCOPE)
  string(REGEX MATCH "dump frame ${frame}: ([0-9]+) clips, [0-9]+ selected, undo ([0-9]+)" _ "${blk}")
  set(nclips "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(undo "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()
function(chandata var frame id)
  string(REGEX MATCH "channels frame ${frame}: [^\n]*\n(  channel [^\n]*\n)*" blk "${out}")
  string(REGEX MATCH "  channel ${id} [^\n]* type gl.sampler data ([^\n]*) mixer " _ "${blk}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# Playlist lane points right of the Loupe window (the window spans x 155..1285 at this size).
set(dropX 1352)
set(y5 362)   # track index 4 (the fifth track): top 340, height 46
set(y6 408)   # track index 5: top 386
run(drag 70 "6 plgeom
8 dump
10 sdlat loupedrag
12 sdldown 0
14 sdlmove 1225 200
16 sdlmove 1260 260
18 sdlmove 1320 340
20 sdlmove ${dropX} ${y5}
22 sdlmove ${dropX} ${y5}
23 shot \"${OUT}/drag.ppm\"
24 sdlmove ${dropX} ${y5}
26 sdlup 0
30 dump
31 channels
34 sdlmove 400 500
36 sdldown 0
38 sdlmove 500 500
40 sdlmove 700 500
42 sdlup 0
44 sdlat loupedrag
46 sdldown 0
48 sdlmove 1225 200
50 sdlmove 1260 260
52 sdlmove 1320 380
54 sdlmove ${dropX} ${y6}
56 sdlmove ${dropX} ${y6}
58 sdlmove ${dropX} ${y6}
60 sdlup 0
64 dump
65 channels
66 browser refs
" --show playlist --fx 1 0)
geom()
check("playlist geometry (x0 ${x0}, ${ppt} px per tick)" x0 MATCHES "^[0-9]")
clipon(8 4)
set(n0 "${nclips}")
set(u0 "${undo}")
clipon(30 4)
check("whole file: an audio clip on track 5 (start ${cstart}, length ${clen}, channel ${csrc})" cstart MATCHES "^[0-9]+$")
string(REGEX REPLACE "\\..*" "" x0i "${x0}")
string(REPLACE "." "" pptDigits "${ppt}")  # pxPerTick to an integer ratio (6 decimals)
string(REGEX REPLACE "^0+" "" pptDigits "${pptDigits}")
math(EXPR raw "(${dropX} - ${x0i}) * 1000000 / ${pptDigits}")
math(EXPR off "${cstart} - ${raw}")
math(EXPR grid "${cstart} % 96")
check("... where it was dropped, snapped to the beat (start ${cstart}, pointer at tick ${raw})" off GREATER -97 AND off LESS 97 AND grid EQUAL 0)
math(EXPR want "${n0} + 1")
math(EXPR uwant "${u0} + 1")
check("... one clip more, one undo step (${n0} -> ${nclips}, undo ${u0} -> ${undo})" nclips EQUAL want AND undo EQUAL uwant)
chandata(d1 31 ${csrc})
check("... playing the whole file (${d1})" d1 STREQUAL "Snap.wav")
set(len1 "${clen}")
clipon(64 5)
check("selection: an audio clip on track 6 (start ${cstart}, length ${clen})" cstart MATCHES "^[0-9]+$")
check("... shorter than the whole file (${clen} < ${len1})" clen LESS len1)
chandata(d2 65 ${csrc})
check("... playing a new file of the selection (${d2})" d2 MATCHES "selection.*\\.wav$")
check("... saved in the project's Edited audio folder" EXISTS "${OUT}/Edited audio/${d2}")
check("screenshot drag.ppm" EXISTS "${OUT}/drag.ppm")

# ---- 3. PROJECT tab: a project that uses the edited file (as Send to playlist / the drop left it)
string(REGEX REPLACE "(\"name\": \"Clap\",[^}]*\"plugin\": {)" "\\1\"data\": \"${OUT}/Edited audio/${d2}\", " song2 "${song}")
string(REGEX REPLACE "(\"data\": \"[^\"]*Edited audio[^\"]*\", [ \n]*\"params\": \\[[^]]*\\],[ \n]*\"type\": )\"gl.clap\"" "\\1\"gl.sampler\"" song2 "${song2}")
check("fixture: Clap plays the edited file" song2 MATCHES "Edited audio/${d2}\", [ \n]*\"params\": \\[[^]]*\\],[ \n]*\"type\": \"gl.sampler\"")
file(WRITE "${OUT}/fixture.gridline.json" "${song2}")
set(ENV{GRIDLINE_BROWSER_EXPAND} "Samples used,Edited audio")
run(project 20 "6 browser rows on\n8 browser tab 1\n14 browser rows\n16 shot \"${OUT}/project.ppm\"\n")
string(REGEX MATCH "  row \"Current project\" [^\n]* path [^\n]*/Edited audio/${d2}\n" inProject "${out}")
check("PROJECT tab: the edited file is listed under the current project" inProject MATCHES ".")
string(REGEX MATCH "  row \"Edited audio\" [^\n]* path [^\n]*/Edited audio/${d2}\n" inFolder "${out}")
check("... and in the Edited audio folder there" inFolder MATCHES ".")
check("screenshot project.ppm" EXISTS "${OUT}/project.ppm")
