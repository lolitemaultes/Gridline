# Gridline: K-124 to K-127, hidden app runs (isolated config and home):
#   K-124: Esc in the piano roll, channel rack or mixer brings the playlist to the front (also in the
#          Live and Logic layouts); Esc that closes a prompt (a popup with a text field) leaves the
#          focus where it was; Esc on the playlist changes nothing.
#   K-125: an instrument added from the rack's add menu, "insert below", "replace", a drop on the
#          rack or the playlist and the plugin picker opens its editor, focused; undo / redo, a
#          project load and a collaborator's new channel open nothing.
#   K-126: the channel light: right-click solos (one undo step), again unsolos; undo; click mutes;
#          Ctrl+click solos; Shift+click selects.
#   K-127: every row has exactly one light, right of the name button and left of the steps, the
#          left section's controls (K-186: the headphones button first) do not overlap, at 100% and 200% and in a narrow and a wide rack;
#          screenshots of the playing rack at 100% and 200%.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k124_rack_and_keys.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

function(run tag frames script)
  set(d "${OUT}/${tag}")
  file(MAKE_DIRECTORY "${d}/cfg" "${d}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${d}/cfg")
  set(ENV{HOME} "${d}/home")
  file(WRITE "${d}/input.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --size 1440x900 --frames ${frames}
                          --screenshot "${d}/end.ppm" --dev-input "${d}/input.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${d}/run.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (${tag}): gridline-app exited with ${rc}:\n${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(fail tag what)
  message(FATAL_ERROR "FAIL (${tag}): ${what}. Output:\n${${tag}_out}")
endfunction()

# "instview frame <f>: open "<name>" type <type> focus "<window>" channels <n> undo <u> popup <0|1> text <0|1>"
function(view tag f)
  if(NOT ${tag}_out MATCHES "instview frame ${f}: open \"([^\"]*)\" type ([^ ]+) focus \"([^\"]*)\" channels ([0-9]+) undo ([0-9]+) popup ([01]) text ([01])")
    fail(${tag} "no instview report at frame ${f}")
  endif()
  set(v_open "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(v_type "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(v_focus "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(v_channels "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(v_undo "${CMAKE_MATCH_5}" PARENT_SCOPE)
  set(v_popup "${CMAKE_MATCH_6}" PARENT_SCOPE)
  set(v_text "${CMAKE_MATCH_7}" PARENT_SCOPE)
endfunction()

function(expectFocus tag f want)
  view(${tag} ${f})
  if(NOT v_focus STREQUAL want)
    fail(${tag} "frame ${f}: focus '${v_focus}', wanted '${want}'")
  endif()
endfunction()

# ---------------------------------------------------------------- K-124 ----
run(esc 70 "5 toasts clear
6 closeplugins
10 focus \"Piano roll\"
14 instview
16 key escape
22 instview
24 focus \"Channel rack\"
28 instview
30 key f2
36 instview
38 key escape
44 instview
46 key escape
52 instview
54 focus Mixer
57 key escape
62 instview
64 key escape
68 instview
" "${OUT}/fixture.gridline.json")
expectFocus(esc 14 "Piano roll")
expectFocus(esc 22 "Playlist")
expectFocus(esc 28 "Channel rack")
view(esc 36)
if(NOT v_popup STREQUAL "1")
  fail(esc "F2 in the rack did not open the rename prompt")
endif()
view(esc 44)
if(NOT v_popup STREQUAL "0" OR v_focus STREQUAL "Playlist")
  fail(esc "Esc in the rename prompt: popup ${v_popup}, focus '${v_focus}' (wanted the prompt closed and the focus kept)")
endif()
expectFocus(esc 52 "Playlist")
expectFocus(esc 62 "Playlist")
expectFocus(esc 68 "Playlist")
message(STATUS "k124: Esc from the piano roll, rack and mixer focuses the playlist; Esc in the prompt only closes it")

foreach(layout live logic)
  run(esc_${layout} 40 "5 toasts clear
6 closeplugins
12 focus \"Piano roll\"
16 instview
18 key escape
24 instview
26 key escape
32 instview
" "${OUT}/fixture.gridline.json" --layout ${layout} --pane piano)  # the Logic layout starts with its bottom pane closed
  expectFocus(esc_${layout} 16 "Piano roll")
  expectFocus(esc_${layout} 24 "Playlist")
  expectFocus(esc_${layout} 32 "Playlist")
endforeach()
message(STATUS "k124: Live and Logic layouts: Esc from the piano roll focuses the playlist")

# ---------------------------------------------------------------- K-125 ----
run(add 150 "5 toasts clear
6 closeplugins
12 instview
14 focus Playlist
16 addvia rack gl.fm
22 instview
24 closeplugins
28 focus Playlist
30 addvia below gl.synth
36 instview
38 closeplugins
42 focus Playlist
44 addvia replace gl.kick
50 instview
52 closeplugins
56 focus Playlist
58 addvia droprack gl.fm
64 instview
66 closeplugins
70 focus Playlist
72 addvia dropplaylist gl.synth
78 instview
80 closeplugins
84 focus Playlist
86 addvia picker gl.kick
92 instview
94 closeplugins
100 instview
102 undo
106 instview
108 redo
112 instview
114 open \"${OUT}/fixture.gridline.json\"
140 instview
" "${OUT}/fixture.gridline.json")
view(add 12)
if(NOT v_open STREQUAL "-")
  fail(add "the editor was open at the start ('${v_open}')")
endif()
set(n0 ${v_channels})
foreach(pair "22;gl.fm;rack menu" "36;gl.synth;insert below" "50;gl.kick;replace" "64;gl.fm;drop on the rack"
             "78;gl.synth;drop on the playlist" "92;gl.kick;plugin picker")
  list(GET pair 0 f)
  list(GET pair 1 t)
  list(GET pair 2 how)
  view(add ${f})
  if(NOT v_type STREQUAL t OR NOT v_focus STREQUAL "channelsettings")
    fail(add "${how}: editor ${v_type} '${v_open}' focus '${v_focus}', wanted ${t} open and focused")
  endif()
endforeach()
view(add 100)
set(n1 ${v_channels})
if(NOT v_open STREQUAL "-")
  fail(add "the editor did not close")
endif()
view(add 106)
if(NOT v_open STREQUAL "-" OR NOT v_channels LESS n1)
  fail(add "undo of the last add: open '${v_open}', ${v_channels} channels (had ${n1})")
endif()
view(add 112)
if(NOT v_open STREQUAL "-" OR NOT v_channels EQUAL n1)
  fail(add "redo of the last add: open '${v_open}', ${v_channels} channels (wanted ${n1}, closed)")
endif()
view(add 140)
if(NOT v_open STREQUAL "-" OR NOT v_channels EQUAL n0)
  fail(add "project load: open '${v_open}', ${v_channels} channels (wanted ${n0}, closed)")
endif()
message(STATUS "k125: every add path opens the editor focused; undo, redo and a project load open nothing")

run(peer 200 "5 toasts clear
6 closeplugins
12 instview
14 waitfor never 6
190 channels
192 instview
" "${OUT}/fixture.gridline.json" --show collab-demo)
if(NOT peer_out MATCHES "channel [0-9]+ Sam's loop type gl.sampler")
  fail(peer "the simulated collaborator's channel did not arrive")
endif()
view(peer 192)
if(NOT v_open STREQUAL "-")
  fail(peer "a collaborator's new channel opened '${v_open}'")
endif()
message(STATUS "k125: a collaborator's new channel opens nothing")

# ---------------------------------------------------------------- K-126 ----
# Row <i> of the n-th "rack" dump: its light count, selection, mute state and the rectangles of
# phones pan vol mix name lamp steps (4 numbers each) -> g_lights, g_selected, g_muted, g_<part>_x0 ...
function(rowgeom tag n i)
  string(REGEX MATCHALL "rack frame [^\n]*" dumps "${${tag}_out}")
  list(LENGTH dumps count)
  if(count LESS_EQUAL n)
    fail(${tag} "no rack dump ${n}")
  endif()
  set(rest "${${tag}_out}")
  foreach(k RANGE ${n})
    string(FIND "${rest}" "rack frame " at)
    math(EXPR at "${at} + 10")
    string(SUBSTRING "${rest}" ${at} -1 rest)
  endforeach()
  string(FIND "${rest}" "rack frame " end)
  if(end GREATER -1)
    string(SUBSTRING "${rest}" 0 ${end} rest)
  endif()
  if(NOT rest MATCHES "  rowgeom ${i} lights ([0-9]+) selected ([01])([^\n]*)")
    fail(${tag} "no row geometry for row ${i} in rack dump ${n}")
  endif()
  set(g_lights "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(g_selected "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(line "${CMAKE_MATCH_3}")
  if(NOT rest MATCHES "  channel ${i} [^\n]* light ([0-9.]+) muted ([01])")
    fail(${tag} "no channel ${i} line in rack dump ${n}")
  endif()
  set(g_glow "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(g_muted "${CMAKE_MATCH_2}" PARENT_SCOPE)
  foreach(part phones pan vol mix name lamp steps)
    if(NOT line MATCHES " ${part} ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) ([-0-9.]+)")
      fail(${tag} "row ${i}: no ${part} rectangle")
    endif()
    set(g_${part}_x0 "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(g_${part}_y0 "${CMAKE_MATCH_2}" PARENT_SCOPE)
    set(g_${part}_x1 "${CMAKE_MATCH_3}" PARENT_SCOPE)
    set(g_${part}_y1 "${CMAKE_MATCH_4}" PARENT_SCOPE)
  endforeach()
endfunction()

# The mute states of rows 0..k-1 in dump n as "m-m...", and row 1's selection after "/".
function(mutes tag n k out)
  set(s "")
  math(EXPR last "${k} - 1")
  foreach(i RANGE ${last})
    rowgeom(${tag} ${n} ${i})
    if(g_muted STREQUAL "1")
      string(APPEND s "m")
    else()
      string(APPEND s "-")
    endif()
  endforeach()
  rowgeom(${tag} ${n} 1)
  set(${out} "${s}/${g_selected}" PARENT_SCOPE)
endfunction()

run(solo_probe 14 "5 toasts clear
6 closeplugins
9 focus \"Channel rack\"
12 rack
" "${OUT}/fixture.gridline.json")
# Centre of a rectangle, in whole pixels (CMake's math is integer only).
function(centre out x0 y0 x1 y1)
  foreach(v x0 y0 x1 y1)
    string(REGEX REPLACE "\\..*" "" ${v} "${${v}}")
  endforeach()
  math(EXPR cx "(${x0} + ${x1}) / 2")
  math(EXPR cy "(${y0} + ${y1}) / 2")
  set(${out} "${cx} ${cy}" PARENT_SCOPE)
endfunction()
rowgeom(solo_probe 0 1)
centre(lampAt ${g_lamp_x0} ${g_lamp_y0} ${g_lamp_x1} ${g_lamp_y1})
string(REGEX MATCH "rack frame 12: [^\n]* rows ([0-9]+)" _ "${solo_probe_out}")
set(rows "${CMAKE_MATCH_1}")
if(rows LESS 3)
  fail(solo_probe "the fixture shows fewer than 3 rack rows (${rows})")
endif()
set(k 3)

# Row 1's light: right-click solo, again unsolo, undo twice; click mutes, again unmutes;
# Ctrl+click solo and unsolo; Shift+click selects and deselects (mutes unchanged).
run(solo 110 "5 toasts clear
6 closeplugins
9 focus \"Channel rack\"
12 rack
14 move ${lampAt}
16 down 1
17 up 1
22 rack
24 down 1
25 up 1
30 rack
32 undo
36 rack
38 undo
42 rack
44 down 0
45 up 0
50 rack
52 down 0
53 up 0
58 rack
60 mod ctrl 1
62 down 0
63 up 0
64 mod ctrl 0
68 rack
70 mod ctrl 1
72 down 0
73 up 0
74 mod ctrl 0
78 rack
80 mod shift 1
82 down 0
83 up 0
84 mod shift 0
88 rack
90 mod shift 1
92 down 0
93 up 0
94 mod shift 0
98 rack
" "${OUT}/fixture.gridline.json")
set(want "---" "m-m" "---" "m-m" "---" "-m-" "---" "m-m" "---" "---" "---")
set(what "start" "right-click: solo" "right-click again: unsolo" "undo" "undo" "click: mute" "click again: unmute"
         "Ctrl+click: solo" "Ctrl+click again: unsolo" "Shift+click: select" "Shift+click again: deselect")
mutes(solo 0 ${k} first)
string(REGEX REPLACE ".*/" "" sel0 "${first}")
set(seen "")
foreach(n RANGE 10)
  mutes(solo ${n} ${k} got)
  list(GET want ${n} w)
  list(GET what ${n} why)
  string(REGEX REPLACE "/.*" "" m "${got}")
  string(REGEX REPLACE ".*/" "" sel "${got}")
  string(APPEND seen " ${m}/${sel}")
  if(NOT m STREQUAL w)
    fail(solo "${why}: mutes ${m}, wanted ${w}")
  endif()
  if(n EQUAL 9 AND sel EQUAL sel0)
    fail(solo "Shift+click on the light did not change row 1's selection (${sel})")
  endif()
  if(n EQUAL 10 AND NOT sel EQUAL sel0)
    fail(solo "a second Shift+click did not restore row 1's selection (${sel})")
  endif()
endforeach()
message(STATUS "k126: the light's mute, solo and select paths:${seen}")

# ---------------------------------------------------------------- K-127 ----
function(checkRows tag n)
  string(REGEX MATCH "rack frame [0-9]+: [^\n]* rows ([0-9]+)" _ "${${tag}_out}")
  set(rows "${CMAKE_MATCH_1}")
  math(EXPR last "${rows} - 1")
  foreach(i RANGE ${last})
    rowgeom(${tag} ${n} ${i})
    if(NOT g_lights EQUAL 1)
      fail(${tag} "row ${i} draws ${g_lights} lights, wanted exactly one")
    endif()
    set(prev "")
    foreach(part phones pan vol mix name lamp steps)
      if(prev)
        if(g_${prev}_x1 GREATER g_${part}_x0)
          fail(${tag} "row ${i}: ${prev} (to ${g_${prev}_x1}) overlaps ${part} (from ${g_${part}_x0})")
        endif()
      endif()
      set(prev ${part})
    endforeach()
    foreach(v lamp_x0 name_x1 steps_x0 lamp_x1)
      string(REGEX REPLACE "\\..*" "" i_${v} "${g_${v}}")
    endforeach()
    math(EXPR gapL "${i_lamp_x0} - ${i_name_x1}")
    math(EXPR gapR "${i_steps_x0} - ${i_lamp_x1}")
    if(gapL LESS 2 OR gapR LESS 2)
      fail(${tag} "row ${i}: the light is too close to the name (${gapL}) or the steps (${gapR})")
    endif()
    if(g_lamp_y0 LESS g_name_y0 OR g_lamp_y1 GREATER g_name_y1)
      fail(${tag} "row ${i}: the light is not inside the row's height")
    endif()
  endforeach()
  set(rows_${tag} ${rows} PARENT_SCOPE)
endfunction()

foreach(spec "1;w520;narrow" "1;w1300;wide" "2;w520;narrow" "2;w900;wide")
  list(GET spec 0 scale)
  list(GET spec 1 w)
  list(GET spec 2 name)
  set(tag geom_${scale}x_${name})
  run(${tag} 16 "5 toasts clear
6 closeplugins
12 rack
" "${OUT}/fixture.gridline.json" --ui-scale ${scale} --rack ${w},h420)
  checkRows(${tag} 0)
  # The steps line up under the playing-step lamps of the header (both start at the step viewport).
  string(REGEX MATCH "viewX0 ([-0-9.]+)" _ "${${tag}_out}")
  rowgeom(${tag} 0 0)
  if(NOT CMAKE_MATCH_1 EQUAL g_steps_x0)
    fail(${tag} "the steps start at ${g_steps_x0}, the header's step lamps at ${CMAKE_MATCH_1}")
  endif()
endforeach()
message(STATUS "k127: one light per row, between the name and the steps, no overlaps (100% / 200%, narrow / wide)")

# Screenshots: the rack while the song plays (lights on), at 100% and 200%.
foreach(scale 1 2)
  set(d "${OUT}/shot_${scale}x")
  file(MAKE_DIRECTORY "${d}/cfg" "${d}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${d}/cfg")
  set(ENV{HOME} "${d}/home")
  file(WRITE "${d}/input.txt" "5 toasts clear
6 closeplugins
9 focus \"Channel rack\"
10 waitfor rack-lit:0 10
12 shot \"${d}/rack.ppm\"
13 rack
")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1440x900 --ui-scale ${scale}
                          --frames 16 --play --screenshot "${d}/end.ppm" --dev-input "${d}/input.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  if(NOT rc EQUAL 0 OR NOT EXISTS "${d}/rack.ppm")
    message(FATAL_ERROR "FAIL (shot ${scale}x): ${rc}\n${err}")
  endif()
  string(REGEX MATCHALL "  channel [0-9]+ [^\n]* light ([0-9.]+) muted 0" lit "${out}")
  set(glowing 0)
  foreach(l ${lit})
    if(l MATCHES " light ([0-9.]+) " AND CMAKE_MATCH_1 GREATER 0.2)
      math(EXPR glowing "${glowing} + 1")
    endif()
  endforeach()
  if(glowing LESS 1)
    message(FATAL_ERROR "FAIL (shot ${scale}x): no channel light glows while the song plays:\n${out}")
  endif()
endforeach()
message(STATUS "k124: all checks passed")
