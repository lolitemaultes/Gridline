# Gridline — J-97: FL-style drop-to-replace on the channel rack. Runs the app
# hidden (off-screen, isolated config) on a copy of the demo song and drags
# files and plugins onto rack rows through the J-75 drag hooks (the same ImGui
# drag a browser item or a file from another application makes):
#   audio onto a synth row (gl.kick "Kick")    -> a sampler holding the file;
#   audio onto that row again (now a sampler)  -> its sample is replaced;
#   audio onto a row with a typed name ("Perc", drum pads with piano-roll notes) -> a sampler, the name and notes stay;
#   an instrument (plugin) onto a row ("Clap")  -> its instrument is replaced;
#   an instrument preset onto a row ("Snare")   -> its instrument from the preset, named after it;
#   audio onto the free area under the rows    -> a new channel (as before);
#   audio onto the edge between two rows       -> a new channel inserted there;
#   undo                                        -> one step per drop.
# Every replaced channel keeps its mixer track, volume, pan, steps and notes.
# Screenshots of the replace highlight and the insertion line: <OUT>/replace.ppm, <OUT>/insert.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j97_rack_drop.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x (CMP0054: a quoted "keep" is that string, never a variable)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/open.wav" "${OUT}/Snap.wav" COPYONLY)
configure_file("${src}/../../assets/sounds/classic/save.wav" "${OUT}/Tick.wav" COPYONLY)
file(WRITE "${OUT}/presets/gl.synth/Warm Bass.json" "{\"format\": \"gridline-preset\", \"type\": \"gl.synth\", \"params\": []}\n")

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames ${frames}
                          --rack steps16,w1300,h560 --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${drops_out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# The "channels" dump of frame <frame> -> <var> (the whole block).
function(dump var frame)
  string(REGEX MATCH "channels frame ${frame}:[^\n]*\n(  channel [^\n]*\n)*" d "${drops_out}")
  if(d STREQUAL "")
    message(FATAL_ERROR "FAIL: no channels dump at frame ${frame}. Output:\n${drops_out}")
  endif()
  set(${var} "${d}" PARENT_SCOPE)
endfunction()
# Field <key> of the channel with id <id> in dump <d> -> <var>.
function(chan var d id key)
  string(REGEX MATCH "  channel ${id} [^\n]*" line "${d}")
  if(line STREQUAL "")
    message(FATAL_ERROR "FAIL: channel ${id} missing in:\n${d}")
  endif()
  if(key STREQUAL "name")
    string(REGEX MATCH "  channel ${id} (.*) type " _ "${line}")
  else()
    string(REGEX MATCH " ${key} ([^ ]*)" _ "${line}")
  endif()
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
function(count var d)
  string(REGEX MATCH "frame [0-9]+: ([0-9]+) channels, [0-9]+ patterns, undo ([0-9]+)" _ "${d}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${var}_undo "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# ---- rack geometry ----
set(drops_out "")
run(geo 12 "8 rack\n")
string(REGEX MATCH "geom originX ([0-9.]+) row0Y ([0-9.]+) viewX0 [-0-9.]+ graphY [-0-9.]+ rowPitch ([0-9.]+)" _ "${geo_out}")
set(ox "${CMAKE_MATCH_1}")
set(oy "${CMAKE_MATCH_2}")
set(rowPitch "${CMAKE_MATCH_3}")
if(ox STREQUAL "" OR oy STREQUAL "")
  message(FATAL_ERROR "FAIL: no rack geometry. Output:\n${geo_out}")
endif()
string(REGEX REPLACE "\\..*" "" ox "${ox}")
string(REGEX REPLACE "\\..*" "" oy "${oy}")
string(REGEX REPLACE "\\..*" "" rowPitch "${rowPitch}")
math(EXPR x "${ox} + 160")            # over the row (between the name button and the steps)
math(EXPR yKick "${oy}")               # row 0: Kick (gl.kick, named "Kick" = its instrument: an automatic name)
math(EXPR yClap "${oy} + ${rowPitch}")  # row 1: Clap
math(EXPR ySnare "${oy} + 4 * ${rowPitch}")  # row 4: Snare (gl.snare "Snare": an automatic name)
math(EXPR yBass "${oy} + 7 * ${rowPitch}")  # row 7: Perc (gl.pads "Abacus": a typed name, piano-roll notes)
math(EXPR yGap "${oy} + ${rowPitch} / 2")  # the edge between row 0 and row 1: insert between Kick and Clap
math(EXPR yFree "${oy} + 9 * ${rowPitch} + 40")  # under the last row (9 rows by then)
math(EXPR xFree "${ox} + 20")          # left of the channel settings window the insert opens

set(snap "${OUT}/Snap.wav")
set(tick "${OUT}/Tick.wav")
set(preset "${OUT}/presets/gl.synth/Warm Bass.json")
run(drops 120 "9 channels
12 dragenter ${x} ${yKick} \"${snap}\"
14 dragmove ${x} ${yKick}
16 shot \"${OUT}/replace.ppm\"
17 dragmove ${x} ${yKick}
18 drop
24 channels
26 dragenter ${x} ${yKick} \"${tick}\"
28 dragmove ${x} ${yKick}
30 drop
36 channels
38 dragenter ${x} ${yBass} \"${snap}\"
40 dragmove ${x} ${yBass}
42 drop
48 channels
50 dragenter ${x} ${yClap} plugin:gl.synth
52 dragmove ${x} ${yClap}
53 shot \"${OUT}/replace_plugin.ppm\"
54 dragmove ${x} ${yClap}
56 drop
62 channels
63 undo
64 channels
66 dragenter ${x} ${yGap} \"${tick}\"
68 dragmove ${x} ${yGap}
69 shot \"${OUT}/insert.ppm\"
70 dragmove ${x} ${yGap}
72 drop
76 channels
78 dragenter ${xFree} ${yFree} \"${snap}\"
80 dragmove ${xFree} ${yFree}
82 dragmove ${xFree} ${yFree}
84 drop
90 channels
92 undo
93 undo
94 undo
95 undo
96 undo
97 undo
98 channels
100 dragenter ${x} ${ySnare} \"${preset}\"
102 dragmove ${x} ${ySnare}
104 drop
110 channels
111 undo
113 channels
")

dump(d0 9)
dump(d1 24)
dump(d2 36)
dump(d3 48)
dump(d4 62)
dump(d5 64)
dump(d6 76)
dump(d7 90)
dump(d8 98)
count(n0 "${d0}")
foreach(k mixer vol pan steps notes)
  chan(kick0_${k} "${d0}" 20 ${k})
  chan(bass0_${k} "${d0}" 27 ${k})
  chan(clap0_${k} "${d0}" 21 ${k})
endforeach()

# Audio onto a synth row: a sampler with the file, renamed (automatic name), routing / mix / steps kept.
chan(t "${d1}" 20 type)
chan(dat "${d1}" 20 data)
chan(nm "${d1}" 20 name)
count(n1 "${d1}")
check("audio onto a synth row: it becomes a sampler holding Snap.wav (${t}, ${dat})" t STREQUAL "gl.sampler" AND dat STREQUAL "Snap.wav")
check("... renamed after the file (its name was automatic): ${nm}" nm STREQUAL "Snap")
foreach(k mixer vol pan steps notes)
  chan(v "${d1}" 20 ${k})
  check("... keeps its ${k} (${kick0_${k}} -> ${v})" v STREQUAL kick0_${k})
endforeach()
math(EXPR want "${n0_undo} + 1")
check("... one undo step, no new channel (${n0} -> ${n1}, undo ${n0_undo} -> ${n1_undo})" n1 EQUAL n0 AND n1_undo EQUAL want)

# Audio onto the sampler row: the sample is replaced, the name follows (it was the old sample's).
chan(t "${d2}" 20 type)
chan(dat "${d2}" 20 data)
chan(nm "${d2}" 20 name)
count(n2 "${d2}")
check("audio onto a sampler row: its sample is replaced (${dat}), still a sampler" t STREQUAL "gl.sampler" AND dat STREQUAL "Tick.wav")
check("... renamed after the new file (${nm})" nm STREQUAL "Tick")
chan(v "${d2}" 20 mixer)
chan(st "${d2}" 20 steps)
check("... routing and steps kept" v STREQUAL kick0_mixer AND st STREQUAL kick0_steps)
math(EXPR want "${n1_undo} + 1")
check("... one undo step" n2_undo EQUAL want)

# A typed name stays.
chan(t "${d3}" 27 type)
chan(nm "${d3}" 27 name)
chan(v "${d3}" 27 mixer)
chan(nt "${d3}" 27 notes)
check("audio onto a drum-pad row with a typed name: a sampler (${t}), the name stays (${nm})" t STREQUAL "gl.sampler" AND nm STREQUAL "Perc")
check("... routing and piano-roll notes kept (mixer ${v}, ${nt} notes)" v STREQUAL bass0_mixer AND nt STREQUAL bass0_notes AND nt GREATER 0)

# An instrument from the browser onto a row: its instrument is replaced (FL "Replace").
chan(t "${d4}" 21 type)
chan(nm "${d4}" 21 name)
count(n4 "${d4}")
check("instrument onto a row: Clap's instrument is replaced (${t}), renamed ${nm}" t STREQUAL "gl.synth" AND nm STREQUAL "Tercet")
foreach(k mixer vol pan steps notes)
  chan(v "${d4}" 21 ${k})
  check("... keeps its ${k} (${clap0_${k}} -> ${v})" v STREQUAL clap0_${k})
endforeach()
check("... no new channel" n4 EQUAL n0)
chan(t "${d5}" 21 type)
chan(nm "${d5}" 21 name)
check("undo: the instrument drop is undone in one step (${t}, ${nm})" t STREQUAL "gl.clap" AND nm STREQUAL "Clap")

# Between two rows: a new channel inserted there (FL).
count(n6 "${d6}")
math(EXPR want "${n0} + 1")
check("audio onto the edge between Kick and Clap: a new channel (${n0} -> ${n6})" n6 EQUAL want)
string(REGEX MATCH "\n  channel 20 [^\n]*\n  channel [0-9]+ Tick type gl.sampler data Tick.wav[^\n]*\n  channel 21 " ins "${d6}")
check("... inserted between them" ins MATCHES ".")

# The free area under the rows still adds a channel.
count(n7 "${d7}")
math(EXPR want "${n6} + 1")
check("audio onto the free area: a new channel (${n6} -> ${n7})" n7 EQUAL want)
string(REGEX MATCH "  channel [0-9]+ Snap type gl.sampler data Snap.wav mixer 0 " newch "${d7}")
check("... a sampler named after the file" newch MATCHES ".")

# Undo every drop: the song is back as it was.
count(n8 "${d8}")
chan(t "${d8}" 20 type)
chan(nm "${d8}" 20 name)
chan(t5 "${d8}" 27 type)
check("undo x6: back to ${n0} channels, Kick (${t}, ${nm}) and Perc (${t5}) restored" n8 EQUAL n0 AND t STREQUAL "gl.kick" AND nm STREQUAL "Kick" AND t5 STREQUAL "gl.pads")
check("... at the starting undo position (${n8_undo})" n8_undo EQUAL n0_undo)
# An instrument preset onto a row: the channel's instrument from the preset (FL "Replace").
dump(d9 110)
dump(d10 113)
foreach(k mixer vol pan steps)
  chan(snare0_${k} "${d0}" 24 ${k})
endforeach()
chan(t "${d9}" 24 type)
chan(nm "${d9}" 24 name)
count(n9 "${d9}")
check("instrument preset onto a row: Snare's instrument from the preset (${t}), named ${nm}" t STREQUAL "gl.synth" AND nm STREQUAL "Warm Bass")
foreach(k mixer vol pan steps)
  chan(v "${d9}" 24 ${k})
  check("... keeps its ${k} (${snare0_${k}} -> ${v})" v STREQUAL snare0_${k})
endforeach()
check("... no new channel, one undo step (${n9_undo})" n9 EQUAL n0 AND n9_undo EQUAL 1)
chan(t "${d10}" 24 type)
check("... undone in one step (${t})" t STREQUAL "gl.snare")
foreach(f replace replace_plugin insert)
  check("screenshot ${f}.ppm written" EXISTS "${OUT}/${f}.ppm")
endforeach()
