# Gridline: K-349, audio clips show in the channel rack. Each way audio reaches the playlist is done
# in the app (hidden, isolated config and HOME, silent audio, fixed frame time), then the channel rack
# is asked what it drew. The new channel must be drawn in view (the rack's row geometry has it, and
# on this version its "rackrow" line says visible 1), pass the display filter, and be listed as an
# audio clip channel (a row of steps like any other channel's, see k078_clips_rack). The project has 29 channels, so the rack
# scrolls, and a display group, so a filter can hide a new channel.
#   Ways in: the playlist drop (the browser's drop path) with the rack scrolled to its end; a file
#   from outside the app dropped on the playlist with the rack showing one group; a file dropped on
#   the rack itself (a sampler, not an audio clip) with the rack scrolled; audio dropped in Clips,
#   Smooth and the Tracker, then Studio; an older project opened. Recording, renders, Chop and stems
#   are covered by the engine test (tests/test_k349_audio_rack.cpp).
#   Then on an audio clip channel's row: a click on its name selects it and opens its settings, rename and delete are
#   one undo step each, and deleting the channel takes its clip with it (undo brings both back).
# Every failing way in is listed before the test stops.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DWAV=<a wav> -DOUT=<dir> -P k349_audio_rack_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home" "${OUT}/media")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
configure_file("${WAV}" "${OUT}/media/loop one.wav" COPYONLY)
configure_file("${WAV}" "${OUT}/media/loop two.wav" COPYONLY)
configure_file("${WAV}" "${OUT}/media/rack drop.wav" COPYONLY)
configure_file("${WAV}" "${OUT}/media/old take.wav" COPYONLY)

# The project: the test song with twenty more instruments in a display group ("Pads", group 0) before
# its own channels, so the rack scrolls and a filter can show one group.
file(READ "${DEMO}" demo)
set(extra "")
foreach(i RANGE 1 20)
  math(EXPR id "300 + ${i}")
  string(APPEND extra "{\"color\": 5219296, \"gain\": 0.78, \"id\": ${id}, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Pad ${i}\", "
         "\"pan\": 0, \"pitch\": 0, \"group\": 0, \"plugin\": {\"type\": \"gl.synth\", \"params\": []}}, ")
endforeach()
string(REPLACE "\"channels\": [" "\"channels\": [${extra}" big "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"channelGroups\": [\"Pads\"], \"nextId\": 400" big "${big}")
file(WRITE "${OUT}/big.gridline.json" "${big}")
# An older project: the same, plus an audio clip channel saved before any mark (a plain sampler) after
# the twenty pads (out of view while the rack shows its top) and its clip on track 5.
string(CONCAT old_ch "{\"color\": 9083816, \"gain\": 0.78, \"id\": 350, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Old take\", \"pan\": 0, \"pitch\": 0, "
           "\"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/media/old take.wav\", \"params\": []}}")
set(old_clip "{\"id\": 351, \"kind\": \"audio\", \"source\": 350, \"track\": 5, \"start\": 0, \"length\": 384}, ")
string(REPLACE "\"channels\": [${extra}" "\"channels\": [${extra}${old_ch}, " old "${big}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${old_clip}" old "${old}")
file(WRITE "${OUT}/old.gridline.json" "${old}")
# The same older project with the audio clip channel first (the rack's top row: the clicks below).
string(REPLACE "\"channels\": [" "\"channels\": [${old_ch}, " first "${big}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${old_clip}" first "${first}")
file(WRITE "${OUT}/first.gridline.json" "${first}")

set(failures "")
macro(fail what)
  list(APPEND failures "${what}")
  message(STATUS "FAIL - ${what}")
endmacro()

# run(<name> <song> <frames> <script text> [extra args...]): the app's output in out_<name>.
function(run name song frames script)
  file(MAKE_DIRECTORY "${OUT}/${name}")
  file(WRITE "${OUT}/${name}/script.txt" "${script}")
  execute_process(COMMAND "${APP}" "${song}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames ${frames}
                          --dev-input "${OUT}/${name}/script.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e TIMEOUT 240)
  file(WRITE "${OUT}/${name}/app.log" "${o}\n${e}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL: ${name}: the app did not finish (${rc})\n${e}")
  endif()
  set(out_${name} "${o}" PARENT_SCOPE)
endfunction()

# report_block(<out> <marker> <var>): the text from "<marker>" up to the next report line that starts in column 0.
function(report_block out marker var)
  string(FIND "${out}" "${marker}" at)
  if(at LESS 0)
    set(${var} "" PARENT_SCOPE)
    return()
  endif()
  string(SUBSTRING "${out}" ${at} -1 rest)
  string(REGEX MATCH "^[^\n]*\n(  [^\n]*\n)*" b "${rest}")
  set(${var} "${b}" PARENT_SCOPE)
endfunction()

# index_of(<channels block> <name> <var>): the channel's place in the rack (its order in the song).
function(index_of chans name var)
  string(REGEX MATCHALL "  channel [0-9]+ [^\n]* type " lines "${chans}")
  set(i 0)
  set(found -1)
  foreach(l IN LISTS lines)
    if(l MATCHES "^  channel [0-9]+ ${name} type ")
      set(found ${i})
      break()
    endif()
    math(EXPR i "${i} + 1")
  endforeach()
  set(${var} ${found} PARENT_SCOPE)
endfunction()

# check(<way> <out> <channels marker> <rack marker> <name> <audio clip 1/0>): the channel named <name>
# was made, is drawn in view in the rack, passes its filter, and (an audio clip channel) is listed as one.
function(check way out cmarker rmarker name clip)
  report_block("${out}" "${cmarker}" chans)
  report_block("${out}" "${rmarker}" rack)
  index_of("${chans}" "${name}" idx)
  set(f "")
  if(idx LESS 0)
    list(APPEND f "${way}: no channel \"${name}\" was made")
  else()
    if(NOT rack MATCHES "\n  rowgeom ${idx} ")
      list(APPEND f "${way}: the rack never drew the new channel \"${name}\" (row ${idx}): out of view or filtered out")
    endif()
    if(NOT rack MATCHES "\n  rackrow ${idx} id [0-9]+ audioclip ${clip} steps [0-9]+ visible 1 shown 1 name ${name}\n")
      string(REGEX MATCH "\n  rackrow ${idx} [^\n]*" got "${rack}")
      list(APPEND f "${way}: the rack row of \"${name}\" is not in view as an audio clip channel ${clip} (${got})")
    endif()
  endif()
  set(failures "${failures};${f}" PARENT_SCOPE)
  if(f)
    message(STATUS "FAIL - ${f}")
  else()
    message(STATUS "ok - ${way}: \"${name}\" is in the rack, in view")
  endif()
endfunction()

set(media "${OUT}/media")

# 1. Studio: the playlist drop (the path a browser file takes), the rack scrolled to its end.
run(scrolled "${OUT}/big.gridline.json" 70
    "8 sdlmove 700 500\n10 wheel -40\n16 rack\n18 channels\n20 audio \"${media}/loop one.wav\" 3 2\n40 channels\n44 rack\n46 shot \"${OUT}/scrolled/rack.ppm\"\n60 quit discard\n")
report_block("${out_scrolled}" "rack frame 16:" r16)
if(NOT r16 MATCHES "rowsScroll ([0-9.]+) " OR CMAKE_MATCH_1 LESS 50)
  message(FATAL_ERROR "FAIL: the rack did not scroll down before the drop (update the script)\n${r16}")
endif()
check("browser file dropped on the playlist, rack scrolled" "${out_scrolled}" "channels frame 40:" "rack frame 44:" "loop one" 1)

# 2. Studio: a file from outside the app dropped on the playlist while the rack shows one group.
run(filtered "${OUT}/big.gridline.json" 70
    "10 dragenter 712 850 \"${media}/loop two.wav\"\n12 dragmove 714 852\n14 drop 714 852\n40 channels\n41 dump\n44 rack\n46 shot \"${OUT}/filtered/rack.ppm\"\n60 quit discard\n"
    --rack group0)
if(NOT out_filtered MATCHES "\n  clip [0-9]+ kind 1 ")
  message(FATAL_ERROR "FAIL: the file from outside did not land on the playlist (update the drop point)\n${out_filtered}")
endif()
check("file from outside dropped on the playlist, rack filtered to a group" "${out_filtered}" "channels frame 40:" "rack frame 44:" "loop two" 1)

# 3. Studio: a file dropped on the rack itself (a sampler channel, no clip), the rack scrolled.
run(rackdrop "${OUT}/big.gridline.json" 60
    "8 sdlmove 700 500\n10 wheel -40\n20 browser drag \"${media}/rack drop.wav\"\n40 channels\n44 rack\n50 quit discard\n")
check("file dropped on the rack, rack scrolled" "${out_rackdrop}" "channels frame 40:" "rack frame 44:" "rack drop" 0)

# 4. Clips, Smooth and the Tracker: audio dropped there, then Studio's rack.
foreach(lay live logic tracker)
  run(${lay} "${OUT}/big.gridline.json" 90
      "20 audio \"${media}/loop one.wav\" 3 2\n50 layout open Channel rack\n70 channels\n74 rack\n76 shot \"${OUT}/${lay}/rack.ppm\"\n80 quit discard\n"
      --layout ${lay} --switch-layout studio@30)
  check("audio dropped in the ${lay} layout, then Studio" "${out_${lay}}" "channels frame 70:" "rack frame 74:" "loop one" 1)
endforeach()

# 5. An older project: its audio clip channel (a plain sampler, last in the rack) is listed, and is an
# audio clip channel in view once the rack scrolls to it.
run(old "${OUT}/old.gridline.json" 50 "8 sdlmove 700 500\n10 wheel -60\n30 channels\n34 rack\n36 shot \"${OUT}/old/rack.ppm\"\n40 quit discard\n")
check("an older project opened" "${out_old}" "channels frame 30:" "rack frame 34:" "Old take" 1)

# 6. On the audio clip channel's row (the top row): a click on its name, rename, delete, undo.
run(where "${OUT}/first.gridline.json" 20 "10 rack\n12 quit discard\n")
if(NOT out_where MATCHES "\n  rowgeom 0 [^\n]* name ([0-9.]+) ([0-9.]+) ([0-9.]+) ([0-9.]+) ")
  message(FATAL_ERROR "FAIL: no geometry for the rack's top row\n${out_where}")
endif()
set(gx0 "${CMAKE_MATCH_1}")
set(gy0 "${CMAKE_MATCH_2}")
set(gx1 "${CMAKE_MATCH_3}")
set(gy1 "${CMAKE_MATCH_4}")
foreach(v gx0 gy0 gx1 gy1)
  string(REGEX REPLACE "\\..*" "" ${v} "${${v}}")
endforeach()
math(EXPR cx "(${gx0} + ${gx1}) / 2")
math(EXPR cy "(${gy0} + ${gy1}) / 2")
run(edit "${OUT}/first.gridline.json" 110
    "8 rack\n10 sdlmove ${cx} ${cy}\n12 sdldown 0\n13 sdlup 0\n20 rack\n60 pluginview\n\
62 renamechannel \"Old take B\"\n64 channels\n66 undo\n68 channels\n70 rmchan 0\n72 dump\n74 undo\n76 dump\n78 channels\n80 shot \"${OUT}/edit/rack.ppm\"\n90 quit discard\n")
report_block("${out_edit}" "rack frame 8:" r8)
if(NOT r8 MATCHES "\n  rackrow 0 id 350 audioclip 1 steps 0 ")
  fail("the top row is not listed as an audio clip channel")
endif()
report_block("${out_edit}" "rack frame 20:" r20)
if(r20 MATCHES "\n  rowgeom 0 [^\n]* selected 1 ")
  message(STATUS "ok - a click on the audio clip channel's name selects it")
else()
  fail("a click on the audio clip channel's name did not select it")
endif()
if(out_edit MATCHES "pluginview frame 60:[^\n]*Old take")
  message(STATUS "ok - a click on the audio clip channel's name opens its settings")
else()
  string(REGEX MATCH "pluginview frame 60:[^\n]*" pv "${out_edit}")
  fail("a click on the audio clip channel's name did not open its settings (${pv})")
endif()
report_block("${out_edit}" "channels frame 64:" c64)
report_block("${out_edit}" "channels frame 68:" c68)
string(REGEX MATCH "undo ([0-9]+)" u64 "${c64}")
set(u64 "${CMAKE_MATCH_1}")
string(REGEX MATCH "undo ([0-9]+)" u68 "${c68}")
set(u68 "${CMAKE_MATCH_1}")
if(NOT u64)
  set(u64 0)
endif()
math(EXPR u64less "${u64} - 1")
if(c64 MATCHES "\n  channel 350 Old take B type " AND c68 MATCHES "\n  channel 350 Old take type " AND u68 EQUAL u64less)
  message(STATUS "ok - rename is one undo step")
else()
  fail("rename of the audio clip channel is not one undo step (${u64} -> ${u68})")
endif()
report_block("${out_edit}" "dump frame 72:" d72)
report_block("${out_edit}" "dump frame 76:" d76)
if(NOT d72 MATCHES "kind 1 " AND d76 MATCHES "\n  clip 351 kind 1 [^\n]* source 350 " AND out_edit MATCHES "channels frame 78:[^\n]*\n  channel 350 Old take type ")
  message(STATUS "ok - deleting the channel takes its clip; one undo brings both back")
else()
  fail("deleting the audio clip channel and undoing it did not take and restore its clip")
endif()

list(FILTER failures EXCLUDE REGEX "^$")
if(failures)
  string(REPLACE ";" "\n  " text "${failures}")
  message(FATAL_ERROR "FAIL: ${text}")
endif()
message(STATUS "ok - every way in shows its channel in the rack")
