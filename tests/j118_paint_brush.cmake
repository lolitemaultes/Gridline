# Gridline — J-118: the playlist's clip brush in the real app (hidden, isolated configs, 1440x900,
# the playlist filling the window, a fixed 16 ms frame time). A copy of the demo song gets a
# sample channel ("Take", a 2.5 s file) and a trimmed audio clip of it on track 8 (audio start
# 0.5 s, 3 beats long, hand-set fades, gain 0.5, +150 cents, reversed), plus two clips of it
# overlapping on track 4 (an automatic crossfade, for the screenshot).
#   1. draw tool: a click on the trimmed clip, then a click on empty space on track 9 adds a copy
#      of the clicked clip as it is (length, audio start, fades, gain, pitch, reverse);
#   2. paint tool: dragging on track 10 lays copies of it back to back, one undo step;
#   3. the sample picked in the picker's audio tab paints its natural clip (full length,
#      default settings) on track 11 — the brush is dropped;
#   4. undo removes that last clip in one step.
# Screenshot of the playlist with the crossfade: <OUT>/brush.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j118_paint_brush.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../assets/sounds/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
set(chan "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clips "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 384, \"length\": 288, \"pattern\": 0, \"audioStart\": 0.5, \"fadeIn\": 0.05, \"fadeOut\": 0.12, \"fadeInManual\": true, \"fadeOutManual\": true, \"gain\": 0.5, \"pitch\": 150, \"reverse\": true}, ")
string(APPEND clips "{\"id\": 71, \"kind\": \"audio\", \"source\": 60, \"track\": 3, \"start\": 0, \"length\": 480, \"pattern\": 0}, ")
string(APPEND clips "{\"id\": 72, \"kind\": \"audio\", \"source\": 60, \"track\": 3, \"start\": 384, \"length\": 480, \"pattern\": 0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chan}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clips}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
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

# Geometry (logical px): x of tick t, track tops / heights, picker tabs and rows.
function(geom frame)
  string(REGEX MATCH "plgeom frame ${frame}: valid 1 x0 ([-0-9.]+) pxPerTick ([0-9.]+) tabs ([0-9.]+) ([0-9.]+) ([0-9.]+) tabY ([0-9.]+)" _ "${out}")
  if(CMAKE_MATCH_1 STREQUAL "")
    fail("no playlist geometry at frame ${frame}")
  endif()
  set(gx0 ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(gppt ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(tabAudioX ${CMAKE_MATCH_4} PARENT_SCOPE)
  set(tabY ${CMAKE_MATCH_6} PARENT_SCOPE)
  string(REGEX MATCH "plgeom frame ${frame}:[^\n]*\n(  [^\n]*\n)*" d "${out}")
  foreach(t 3 7 8 9 10)
    string(REGEX MATCH "  track ${t} top ([0-9.]+) h ([0-9.]+)" _ "${d}")
    set(top${t} ${CMAKE_MATCH_1} PARENT_SCOPE)
    set(h${t} ${CMAKE_MATCH_2} PARENT_SCOPE)
  endforeach()
  string(REGEX MATCH "  row 60 x ([0-9.]+) y ([0-9.]+)" _ "${d}")
  set(row60x "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(row60y "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()
# x of tick <t> (+0.5 px), y at <frac> of track <tr>'s height -> <var>_x, <var>_y (integers).
function(at var t tr yfrac)
  math(EXPR tt "${t}")
  set(top ${top${tr}})
  set(h ${h${tr}})
  # CMake math is integer only: pxPerTick ("0.170000") in millionths.
  string(REGEX REPLACE "\\..*" "" x0w "${gx0}")
  string(REGEX MATCH "^([0-9]+)\\.([0-9][0-9][0-9][0-9][0-9][0-9])" _ "${gppt}")
  set(whole ${CMAKE_MATCH_1})
  string(REGEX REPLACE "^0+([0-9])" "\\1" frac "${CMAKE_MATCH_2}")  # no leading zeros
  math(EXPR px "(${tt} * (${whole} * 1000000 + ${frac})) / 1000000")
  math(EXPR x "${x0w} + ${px} + 3")
  string(REGEX REPLACE "\\..*" "" topi "${top}")
  string(REGEX REPLACE "\\..*" "" hi "${h}")
  math(EXPR y "${topi} + (${hi} * ${yfrac}) / 100")
  set(${var}_x ${x} PARENT_SCOPE)
  set(${var}_y ${y} PARENT_SCOPE)
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
# Clips of <d> on track <tr> -> <var> (list of lines).
function(onTrack var d tr)
  string(REGEX MATCHALL "  clip [0-9]+ kind 1 track ${tr} [^\n]*" lines "${d}")
  set(${var} "${lines}" PARENT_SCOPE)
endfunction()
function(field var line key)
  string(REGEX MATCH " ${key} ([^ ]+)" _ "${line}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
# Asserts that <line> is a copy of the trimmed clip (track 7, id 70).
function(isBrushCopy what line)
  field(len "${line}" len)
  field(as "${line}" audioStart)
  field(fi "${line}" fadeIn)
  field(fo "${line}" fadeOut)
  field(g "${line}" gain)
  field(p "${line}" pitch)
  field(r "${line}" reverse)
  field(src "${line}" source)
  check("${what}: a copy of the clicked clip (source ${src}, len ${len}, audioStart ${as}, fades ${fi} / ${fo}, gain ${g}, pitch ${p}, reverse ${r})"
        src EQUAL 60 AND len EQUAL 288 AND as STREQUAL "0.500000" AND fi STREQUAL "0.050000" AND fo STREQUAL "0.120000" AND g STREQUAL "0.5000"
        AND p STREQUAL "150.00" AND r EQUAL 1)
endfunction()

# ---- geometry: the playlist, then the picker's audio tab ----
run(geo 14 "8 plgeom\n")
geom(8)
run(geo2 20 "6 move ${tabAudioX} ${tabY}\n7 down 0\n9 up 0\n14 plgeom\n")
geom(14)
if(row60x STREQUAL "")
  fail("the picker's audio tab shows no row for the Take channel")
endif()
string(REGEX REPLACE "\\..*" "" row60x "${row60x}")
string(REGEX REPLACE "\\..*" "" row60y "${row60y}")
string(REGEX REPLACE "\\..*" "" tabAudioX "${tabAudioX}")
string(REGEX REPLACE "\\..*" "" tabY "${tabY}")

at(trim "384 + 144" 7 70)       # the trimmed clip's body
at(drawAt 1536 8 60)           # empty space, track 9
at(paint0 1536 9 60)           # paint from here on track 10 ...
at(paint1 "1536 + 288 * 2 + 100" 9 60)  # ... to the third slot
at(pickAt 1536 10 60)          # track 11: the picked sample

run(brush 90 "4 typing off
6 dump
10 move ${trim_x} ${trim_y}
11 down 0
13 up 0
16 move ${drawAt_x} ${drawAt_y}
17 down 0
19 up 0
22 dump
24 key b
26 move ${paint0_x} ${paint0_y}
27 down 0
29 move ${paint1_x} ${paint1_y}
31 move ${paint1_x} ${paint1_y}
33 up 0
36 dump
38 key p
40 move ${tabAudioX} ${tabY}
41 down 0
43 up 0
46 move ${row60x} ${row60y}
47 down 0
49 up 0
52 move ${pickAt_x} ${pickAt_y}
53 down 0
55 up 0
58 dump
60 undo
62 dump
")
dump(6)
dump(22)
dump(36)
dump(58)
dump(62)

# 1. draw: a click on empty space adds a copy of the clicked clip.
onTrack(t8 "${clips_22}" 8)
list(LENGTH t8 n)
check("draw after clicking the trimmed clip: one new clip on track 9 (${n})" n EQUAL 1)
isBrushCopy("draw" "${t8}")
math(EXPR want "${undo_6} + 1")
check("... one undo step (${undo_6} -> ${undo_22})" undo_22 EQUAL want)

# 2. paint: copies laid back to back.
onTrack(t9 "${clips_36}" 9)
list(LENGTH t9 n)
check("paint tool: three copies on track 10 (${n})" n EQUAL 3)
set(i 0)
foreach(line IN LISTS t9)
  isBrushCopy("paint copy ${i}" "${line}")
  field(st "${line}" start)
  math(EXPR want "1536 + 288 * ${i}")
  check("... back to back (start ${st})" st EQUAL want)
  math(EXPR i "${i} + 1")
endforeach()
math(EXPR want "${undo_22} + 1")
check("... one undo step (${undo_22} -> ${undo_36})" undo_36 EQUAL want)

# 3. a sample picked in the picker paints its natural clip.
onTrack(t10 "${clips_58}" 10)
list(LENGTH t10 n)
check("draw after picking the sample in the picker: one new clip on track 11 (${n})" n EQUAL 1)
field(len "${t10}" len)
field(as "${t10}" audioStart)
field(fi "${t10}" fadeIn)
field(g "${t10}" gain)
field(r "${t10}" reverse)
field(src "${t10}" source)
check("... the base sample at its natural length (source ${src}, len ${len} > 288, audioStart ${as}, fadeIn ${fi}, gain ${g}, reverse ${r})"
      src EQUAL 60 AND len GREATER 450 AND as STREQUAL "0.000000" AND fi STREQUAL "0.000000" AND g STREQUAL "1.0000" AND r EQUAL 0)

# 4. undo.
onTrack(t10u "${clips_62}" 10)
list(LENGTH t10u n)
check("undo: the picked sample's clip goes (${n} left on track 11)" n EQUAL 0)
onTrack(t9u "${clips_62}" 9)
list(LENGTH t9u n)
check("... the painted copies stay (${n})" n EQUAL 3)
check("screenshot brush.ppm written" EXISTS "${OUT}/brush.ppm")
