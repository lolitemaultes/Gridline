# Gridline: Loupe (the audio editor) workflow in the real app: hidden, isolated config and HOME,
# SDL's dummy audio driver, real SDL mouse events. Loupe in a mixer slot with the demo phrase
# (selection 1.5 .. 2.5 s):
#   1. a selection edge dragged moves only that edge; Shift-click extends the selection;
#   2. a new selection made while playing (loop on) is played at once: the old loop stops;
#   3. a click on the waveform while playing jumps there; the song transport Loupe follows starts
#      from the play position set by a ruler click (not where Play was last pressed);
#   4. a drag along the ruler scrubs, and the play position stays where the scrub ends;
#   5. the auto-scroll toggle is a setting of its own (loupe.autoScroll), kept for the next run.
# Loupe on a channel's sample (opened as from the channel rack):
#   6. Alt-drag scrubs there too (it did nothing outside a mixer slot);
#   7. the drag handle drops the audio onto a playlist track and onto a channel rack row that
#      lies under Loupe's window (Loupe turns see-through while dragging; it never takes its own drop).
# A file opened on its own (the browser's Edit in Loupe):
#   8. opens with its audio, adds nothing to the project, and the file is never changed.
# Screenshots: <OUT>/edge.ppm, <OUT>/scrub.ppm (1440x900), <OUT>/file1080.ppm (1920x1080).
#   cmake -DAPP=<gridline-app> -DDEMO=<song.gridline.json> -DOUT=<dir> -P k166_loupe_workflow.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home" "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_CACHE_DIR} "${OUT}/cache")
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/open.wav" "${OUT}/Snap.wav" COPYONLY)

function(run tag size frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size ${size} ${ARGN}
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
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
# st(<p> <n> [kind]): the n-th "loupe state" line (of that kind) into <p>_frames _cursor _sa _sb
# _playing _pos _rf _rt _scrub _auto _lx0 _lx1 _ly0 _ly1 _ry0 _ry1 _vs _vl _channel _committed.
function(st p n)
  set(kind "[a-z]+")
  if(ARGC GREATER 2)
    set(kind "${ARGV2}")
  endif()
  string(REGEX MATCHALL "loupe state: key [0-9a-f]+ kind ${kind} [^\n]*" lines "${out}")
  list(LENGTH lines count)
  if(NOT n LESS count)
    message(FATAL_ERROR "FAIL: ${p}: no loupe state #${n} (${count})\nOutput:\n${out}")
  endif()
  list(GET lines ${n} l)
  # "<label> <v1> [<v2> ...]": the values after each label (CMake keeps only nine matches per regex).
  set(spec "channel:channel" "frames:frames" "cursor:cursor" "sel:sa,sb" "playing:playing" "pos:pos" "range:rf,rt" "scrubbing:scrub"
           "autoscroll:auto" "lanes:lx0,lx1,ly0,ly1" "ruler:ry0,ry1" "view:vs,vl")
  foreach(e ${spec})
    string(REPLACE ":" ";" e "${e}")
    list(GET e 0 label)
    list(GET e 1 names)
    string(REPLACE "," ";" names "${names}")
    list(LENGTH names cnt)
    set(rx " ${label}")
    foreach(_ RANGE 1 ${cnt})
      string(APPEND rx " ([-0-9.]+)")
    endforeach()
    string(REGEX MATCH "${rx}" m "${l}")
    if(m STREQUAL "")
      message(FATAL_ERROR "FAIL: ${p}: no ${label} in: ${l}")
    endif()
    set(got "${CMAKE_MATCH_1};${CMAKE_MATCH_2};${CMAKE_MATCH_3};${CMAKE_MATCH_4}")
    set(i 0)
    foreach(k ${names})
      list(GET got ${i} v)
      string(REGEX REPLACE "\\..*" "" v "${v}")
      set(${p}_${k} "${v}" PARENT_SCOPE)
      math(EXPR i "${i} + 1")
    endforeach()
  endforeach()
  string(REGEX MATCH "committed (.*)$" m "${l}")
  set(${p}_committed "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
# xat(<var> <p> <frame>): the screen x of a frame in the lanes of state <p>.
macro(xat var p f)
  math(EXPR ${var} "${${p}_lx0} + (${f} - ${${p}_vs}) * (${${p}_lx1} - ${${p}_lx0}) / ${${p}_vl}")
endmacro()

# ---- geometry: Loupe in a mixer slot with the demo phrase (selection 1.5 .. 2.5 s)
set(ENV{GRIDLINE_EDISON_DEMO} "1")
run(geo 1440x900 20 "16 loupe state\n" --plugin gl.edison)
st(g 0 slot)
math(EXPR rate "${g_sb} - ${g_sa}")  # the selection is one second long
check("demo phrase in the slot (${g_frames} frames, selection ${g_sa} .. ${g_sb}, ${rate} Hz)" g_frames GREATER 0 AND rate GREATER 20000)
math(EXPR ly "(${g_ly0} + ${g_ly1}) / 2")
math(EXPR ry "(${g_ry0} + ${g_ry1}) / 2")
math(EXPR f075 "${rate} * 3 / 4")
math(EXPR f100 "${rate}")
math(EXPR f200 "${rate} * 2")
math(EXPR f275 "${rate} * 11 / 4")
math(EXPR f300 "${rate} * 3")
math(EXPR f400 "${rate} * 32 / 10")  # 3.2 s (the phrase is 4 s long)
math(EXPR f430 "${rate} * 34 / 10")
math(EXPR f450 "${rate} * 36 / 10")
xat(xSelB g ${g_sb})
xat(x075 g ${f075})
xat(x100 g ${f100})
xat(x200 g ${f200})
xat(x275 g ${f275})
xat(x300 g ${f300})
xat(x400 g ${f400})
xat(x430 g ${f430})
xat(x450 g ${f450})
math(EXPR xEdge "${xSelB} - 2")

# ---- 1. selection edges, Shift-click
run(edge 1440x900 80 "16 loupe state
18 sdlmove ${xEdge} ${ly}
20 sdlmove ${xEdge} ${ly}
22 sdldown 0
24 sdlmove ${x275} ${ly}
26 sdlmove ${x300} ${ly}
28 sdlmove ${x300} ${ly}
29 shot \"${OUT}/edge.ppm\"
30 sdlup 0
34 loupe state
56 mod shift 1
58 sdlmove ${x400} ${ly}
60 sdldown 0
62 sdlup 0
64 mod shift 0
68 loupe state
" --plugin gl.edison)
st(e0 1 slot)
math(EXPR lo "${f300} - ${rate} / 50")
math(EXPR hi "${f300} + ${rate} / 50")
check("edge drag: the start stays (${e0_sa} = ${g_sa}), the end moves to 3 s (${e0_sb})" e0_sa EQUAL g_sa AND e0_sb GREATER lo AND e0_sb LESS hi)
st(e1 2 slot)
math(EXPR lo "${f400} - ${rate} / 50")
math(EXPR hi "${f400} + ${rate} / 50")
check("Shift-click extends the selection to 3.2 s (${e1_sa} .. ${e1_sb})" e1_sa EQUAL g_sa AND e1_sb GREATER lo AND e1_sb LESS hi)
check("screenshot edge.ppm" EXISTS "${OUT}/edge.ppm")

# ---- 2. + 3. playback follows the selection and the clicks
run(play 1440x900 200 "10 loupe loop 1
12 loupe play
40 loupe state
42 sdlmove ${x400} ${ly}
44 sdldown 0
46 sdlmove ${x430} ${ly}
48 sdlmove ${x450} ${ly}
50 sdlup 0
80 loupe state
82 sdlmove ${x100} ${ly}
84 sdldown 0
86 sdlup 0
88 loupe state
92 loupe stop
94 loupe loop 0
96 loupe follow 1
100 sdlmove ${x200} ${ry}
102 sdldown 0
104 sdlup 0
110 loupe state
112 startsong
120 loupe state
140 stopvia
" --plugin gl.edison)
st(p0 0 slot)
check("playing the selection, looped (pos ${p0_pos} in ${g_sa} .. ${g_sb})" p0_playing EQUAL 1 AND NOT p0_pos LESS g_sa AND p0_pos LESS g_sb)
st(p1 1 slot)
math(EXPR lo "${f400} - ${rate} / 50")
math(EXPR hi "${f450} + ${rate} / 50")
check("a new selection while playing plays at once (range ${p1_rf} .. ${p1_rt}, pos ${p1_pos}, selection ${p1_sa} .. ${p1_sb})"
      p1_playing EQUAL 1 AND p1_pos GREATER lo AND p1_pos LESS hi AND p1_rf EQUAL p1_sa AND p1_rt EQUAL p1_sb)
st(p2 2 slot)
math(EXPR lo "${f100} - ${rate} / 50")
math(EXPR hi "${f100} + ${rate} * 3 / 2")
check("a click while playing jumps there (pos ${p2_pos}, cursor ${p2_cursor}, no selection ${p2_sa} .. ${p2_sb})"
      p2_playing EQUAL 1 AND p2_pos GREATER lo AND p2_pos LESS hi AND p2_sa EQUAL p2_sb)
st(p3 3 slot)
math(EXPR lo "${f200} - ${rate} / 50")
math(EXPR hi "${f200} + ${rate} / 50")
check("a ruler click while stopped sets the play position (cursor ${p3_cursor})" p3_playing EQUAL 0 AND p3_cursor GREATER lo AND p3_cursor LESS hi)
st(p4 4 slot)
math(EXPR hi "${f200} + ${rate} * 2")
check("the followed song transport starts there, not where Play was last pressed (pos ${p4_pos})" p4_playing EQUAL 1 AND NOT p4_pos LESS p3_cursor AND p4_pos LESS hi)

# ---- 4. + 5. ruler scrub, auto-scroll toggle
math(EXPR xa "(${x100} * 3 + ${x200}) / 4")
math(EXPR xb "(${x100} + ${x200}) / 2")
math(EXPR xc "(${x100} + ${x200} * 3) / 4")
math(EXPR xStrip "${g_lx1} + 6")
run(scrub 1440x900 90 "10 sdlmove ${x100} ${ry}
12 sdldown 0
14 sdlmove ${x100} ${ry}
16 sdlmove ${xa} ${ry}
20 sdlmove ${xb} ${ry}
24 sdlmove ${xc} ${ry}
28 sdlmove ${x200} ${ry}
30 loupe state
31 shot \"${OUT}/scrub.ppm\"
40 sdlmove ${x200} ${ry}
50 sdlup 0
54 loupe state
60 sdlmove ${xStrip} ${ly}
62 sdldown 0
64 sdlup 0
68 loupe state
" --plugin gl.edison)
st(s0 0 slot)
check("a drag along the ruler scrubs (scrubbing ${s0_scrub}, pos ${s0_pos})" s0_scrub EQUAL 1 AND s0_pos GREATER f075)
st(s1 1 slot)
math(EXPR lo "${f100}")
math(EXPR hi "${f200} + ${rate} / 4")
check("... the play position stays where the scrub ended (cursor ${s1_cursor})" s1_scrub EQUAL 0 AND s1_cursor GREATER lo AND s1_cursor LESS hi)
st(s2 2 slot)
check("the auto-scroll toggle turns it off (${s1_auto} -> ${s2_auto})" s1_auto EQUAL 1 AND s2_auto EQUAL 0)
run(scrub2 1440x900 20 "16 loupe state\n" --plugin gl.edison)
st(s3 0 slot)
check("... and stays off in the next session (setting loupe.autoScroll)" s3_auto EQUAL 0)
check("screenshot scrub.ppm" EXISTS "${OUT}/scrub.ppm")
unset(ENV{GRIDLINE_EDISON_DEMO})

# ---- 6. + 7. Loupe on a channel's sample: Alt scrub, the drag handle onto the playlist and the rack
set(ENV{GRIDLINE_EDISON_STANDALONE} "1")
run(chgeo 1440x900 30 "26 loupe state\n27 rack\n28 plgeom\n29 dump\n30 channels\n")
st(c 0 channel)
string(REGEX MATCH "rack frame [0-9]+: [^\n]*\n  geom originX ([0-9.]+) row0Y ([0-9.]+) [^\n]* rowPitch ([0-9.]+)" _ "${out}")
set(rackX "${CMAKE_MATCH_1}")
set(rackY "${CMAKE_MATCH_2}")
string(REGEX MATCH "plgeom frame [0-9]+: valid 1 x0 ([0-9.]+)" _ "${out}")
set(plX0 "${CMAKE_MATCH_1}")
check("Loupe on the new channel's sample (${c_frames} frames; rack at ${rackX},${rackY}; playlist from ${plX0})" c_frames GREATER 0 AND rackX MATCHES "^[0-9]")
string(REGEX MATCH "channels frame [0-9]+: ([0-9]+) channels" _ "${out}")
set(nch0 "${CMAKE_MATCH_1}")
math(EXPR cy "(${c_ly0} + ${c_ly1}) / 2")
math(EXPR ca "${c_lx0} + (${c_lx1} - ${c_lx0}) / 5")
math(EXPR cb "${c_lx0} + (${c_lx1} - ${c_lx0}) / 3")
math(EXPR cc "${c_lx0} + (${c_lx1} - ${c_lx0}) / 2")
run(chscrub 1440x900 60 "26 mod alt 1
28 sdlmove ${ca} ${cy}
30 sdldown 0
32 sdlmove ${ca} ${cy}
34 sdlmove ${cb} ${cy}
36 sdlmove ${cc} ${cy}
37 loupe state
40 sdlup 0
42 mod alt 0
46 loupe state
")
st(cs0 0 channel)
check("Alt-drag scrubs a channel's sample too (scrubbing ${cs0_scrub}, pos ${cs0_pos})" cs0_scrub EQUAL 1 AND cs0_pos GREATER 0)
st(cs1 1 channel)
check("... and stops on release, the play position where it ended (cursor ${cs1_cursor})" cs1_scrub EQUAL 0 AND cs1_cursor GREATER 0)
run(chdrag 1440x900 50 "20 channels
22 sdlat loupedrag
24 sdldown 0
26 sdlmove 1240 200
28 sdlmove 1300 300
30 sdlmove 1352 362
32 sdlmove 1352 362
34 sdlmove 1352 362
35 shot \"${OUT}/chdrag.ppm\"
36 sdlup 0
40 dump
41 channels
" --show playlist)
string(REGEX MATCH "  clip [0-9]+ kind 1 track 4 [^\n]*" onTrack "${out}")
check("drag handle -> playlist track 5: an audio clip there (${onTrack})" onTrack MATCHES "kind 1")
# The rack lies under Loupe's window: while the audio is dragged out, Loupe lets the pointer
# through, so the rack row under it takes the drop (its first row, Kick, becomes a sampler).
run(chrack0 1440x900 30 "26 rack\n")
string(REGEX MATCH "rowgeom 0 [^\n]* name ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* ([0-9]+)" _ "${out}")
math(EXPR rackDropX "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
math(EXPR rackDropY "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
check("the first rack row lies under Loupe (${rackDropX},${rackDropY} inside ${c_lx0}..${c_lx1})" rackDropX GREATER c_lx0 AND rackDropX LESS c_lx1)
run(chrack 1440x900 50 "20 channels
22 sdlat loupedrag
24 sdldown 0
26 sdlmove 1000 400
28 sdlmove ${rackDropX} 300
30 sdlmove ${rackDropX} ${rackDropY}
32 sdlmove ${rackDropX} ${rackDropY}
34 sdlmove ${rackDropX} ${rackDropY}
35 shot \"${OUT}/chrack.ppm\"
36 sdlup 0
40 channels
")
string(REGEX MATCH "channels frame 40: [^\n]*\n  channel [0-9]+ ([^\n]*)" _ "${out}")
set(row0 "${CMAKE_MATCH_1}")
check("drag handle -> the rack row under Loupe: Kick becomes a sampler playing the audio (${row0})" row0 MATCHES "type gl.sampler data demo_phrase")
unset(ENV{GRIDLINE_EDISON_STANDALONE})

# ---- 8. a file on its own (the browser's Edit in Loupe), at 1920x1080
file(SHA256 "${OUT}/Snap.wav" before)
run(file1080 1920x1080 60 "6 channels
10 loupe file \"${OUT}/Snap.wav\"
40 loupe state
42 channels
50 loupe state
")
check("Edit in Loupe opened the file" out MATCHES "loupe file: opened")
st(f0 0 file)
check("... with its audio (${f0_frames} frames, ${f0_committed})" f0_frames GREATER 0 AND f0_committed STREQUAL "Snap.wav")
string(REGEX MATCH "channels frame 6: ([0-9]+) channels" _ "${out}")
set(n0 "${CMAKE_MATCH_1}")
string(REGEX MATCH "channels frame 42: ([0-9]+) channels" _ "${out}")
set(n1 "${CMAKE_MATCH_1}")
check("... adds nothing to the project (${n0} -> ${n1} channels)" n0 EQUAL n1)
file(SHA256 "${OUT}/Snap.wav" after)
check("... and the file is unchanged" before STREQUAL after)
check("screenshot file1080.ppm" EXISTS "${OUT}/file1080.ppm")
