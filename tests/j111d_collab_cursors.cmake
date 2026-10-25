# Gridline - J-111d: collaborators' cursors land on the same thing on screens of other sizes and
# interface scales, fade where that thing is not shown here, and draw on top of the window they point
# into (J-111h: windows open here in front of it cover them; a modal dims them).
#   1. one hidden window with a simulated collaborator (--show collab-cursors, in-process network),
#      no mixer: Sam's cursor on a playlist spot under the floating channel rack maps back to the same
#      track and time and is behind the rack (J-111h: the pixel read back is the rack's, not Sam's
#      colour); in the rack, in Settings it is on top; under Settings it is covered, at the rack's edge
#      cut there, behind a modal dimmed;
#      scrolled out of view, in the closed mixer, in the piano roll of another pattern it fades out;
#      in the piano roll of the pattern shown here it shows, on the same key and time;
#   2. two hidden windows (own configs, real ICE on this computer): Alex at 1500x920, scale 1; Sam at
#      1100x720, scale 1.5, no mixer. Alex points at a playlist spot (track, time), a channel rack
#      button and a mixer strip; Sam's copy of the cursor is over the same track and time, over the
#      same button, and hidden for the mixer.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j111d_collab_cursors.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

# A JSON number as an integer in thousandths (cmake has no float math).
function(milli out v)
  if(v MATCHES "e-")
    set(${out} 0 PARENT_SCOPE)
    return()
  endif()
  if(NOT v MATCHES "^(-?)([0-9]+)(\\.([0-9]*))?$")
    message(FATAL_ERROR "FAIL: not a number: ${v}")
  endif()
  set(sign "${CMAKE_MATCH_1}")
  set(int "${CMAKE_MATCH_2}")
  set(frac "${CMAKE_MATCH_4}000")
  string(SUBSTRING "${frac}" 0 3 frac)
  string(REGEX REPLACE "^0+([0-9])" "\\1" frac "${frac}")
  math(EXPR m "${int} * 1000 + ${frac}")
  if(sign STREQUAL "-")
    math(EXPR m "0 - ${m}")
  endif()
  set(${out} ${m} PARENT_SCOPE)
endfunction()

# |a - b| <= tol (all JSON numbers).
function(expectNear what a b tol)
  milli(ma "${a}")
  milli(mb "${b}")
  milli(mt "${tol}")
  math(EXPR d "${ma} - ${mb}")
  if(d LESS 0)
    math(EXPR d "0 - ${d}")
  endif()
  if(d GREATER mt)
    message(FATAL_ERROR "FAIL (${what}): ${a} vs ${b} (tolerance ${tol})")
  endif()
endfunction()

function(expectEq what a b)
  if(NOT "${a}" STREQUAL "${b}")
    message(FATAL_ERROR "FAIL (${what}): '${a}' vs '${b}'")
  endif()
endfunction()

# The first peer of a cursor report: <prefix>_<key> for its fields and under_<key>.
function(readPeer file prefix)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "FAIL: no report ${file}")
  endif()
  file(READ "${file}" j)
  string(JSON n LENGTH "${j}" peers)
  if(n LESS 1)
    message(FATAL_ERROR "FAIL: no collaborator cursor in ${file}")
  endif()
  foreach(k window kind state why vis drawn a b cx cy topWindow)
    string(JSON v GET "${j}" peers 0 ${k})
    set(${prefix}_${k} "${v}" PARENT_SCOPE)
  endforeach()
  string(JSON ok GET "${j}" peers 0 under ok)
  set(${prefix}_under_ok "${ok}" PARENT_SCOPE)
  if(ok EQUAL 1)
    foreach(k kind a b cx cy)
      string(JSON v GET "${j}" peers 0 under ${k})
      set(${prefix}_under_${k} "${v}" PARENT_SCOPE)
    endforeach()
  endif()
  string(JSON bt GET "${j}" barTicks)
  set(${prefix}_barTicks "${bt}" PARENT_SCOPE)
endfunction()

function(readMe file prefix)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "FAIL: no report ${file}")
  endif()
  file(READ "${file}" j)
  foreach(k window kind a b cx cy)
    string(JSON v GET "${j}" me ${k})
    set(${prefix}_${k} "${v}" PARENT_SCOPE)
  endforeach()
endfunction()

function(expectHidden tag file why)
  readPeer("${file}" P)
  message(STATUS "${tag}: ${P_window} ${P_kind} -> ${P_state} (${P_why}), vis ${P_vis}")
  expectEq("${tag}: state" "${P_state}" "hidden")
  expectEq("${tag}: why" "${P_why}" "${why}")
  expectEq("${tag}: faded out" "${P_vis}" "0")
  expectEq("${tag}: not drawn" "${P_drawn}" "0")
endfunction()

# ---- 1: one window, a simulated collaborator ----
set(D "${OUT}/demo")
file(MAKE_DIRECTORY "${D}")
file(WRITE "${D}/script.txt" "5 collab demo-cursor Playlist playlist 1 0 2.5bar 0.5
6 waitfor collab-cursor:playlist:shown 30
7 collab cursors \"${D}/shown.json\"
8 collab pixel \"${D}/pixel.json\"
10 forget collab-cursor:playlist:hidden
11 collab demo-cursor Playlist playlist 1 0 400bar 0.5
12 waitfor collab-cursor:playlist:hidden 30
13 collab cursors \"${D}/offview.json\"
15 collab demo-cursor Mixer mixer-strip mx1 0 0.5 0.5
16 waitfor collab-cursor-window:Mixer:hidden 30
17 collab cursors \"${D}/mixer.json\"
19 focus \"Piano roll\"
20 waitfor never 0.5
21 collab demo-cursor \"Piano roll\" piano 99999 99998 1bar 60.5
22 waitfor collab-cursor:piano:hidden 30
23 collab cursors \"${D}/otherpattern.json\"
25 collab demo-cursor \"Piano roll\" piano pat sel 1.5bar 60.5
26 waitfor collab-cursor:piano:shown 30
27 collab cursors \"${D}/piano.json\"
29 focus Playlist
30 collab demo-cursor Playlist playlist 1 0 2.5bar 0.5
31 forget collab-cursor:playlist:shown
32 waitfor collab-cursor:playlist:shown 30
33 collab trace-reset
34 collab demo-cursor Mixer mixer-strip mx1 0 0.5 0.5
35 waitfor never 0.08
36 collab demo-cursor Playlist playlist 1 0 2.5bar 0.5
37 waitfor never 0.6
38 collab cursors \"${D}/brief.json\"
39 collab trace-reset
40 collab demo-cursor Mixer mixer-strip mx1 0 0.5 0.5
41 forget collab-cursor-window:Mixer:hidden
42 waitfor collab-cursor-window:Mixer:hidden 30
43 collab cursors \"${D}/sustained.json\"
50 collab open-editor ch0
51 waitfor never 0.5
52 collab demo-cursor Playlist window 0 0 0.5 0.5
53 waitfor collab-cursor:window:shown 30
54 waitfor never 0.3
55 collab cursors \"${D}/occluded.json\"
56 collab pixel \"${D}/occluded-pixel.json\"
60 collab demo-cursor channelsettings window 0 0 0.5 0.6
61 collab demo-cursor-plugin ch0
62 forget collab-cursor-window:channelsettings:shown
63 waitfor collab-cursor-window:channelsettings:shown 30
64 waitfor never 0.3
65 collab cursors \"${D}/sameplugin.json\"
66 collab pixel \"${D}/sameplugin-pixel.json\"
70 collab demo-cursor-plugin ch1
71 waitfor collab-cursor-window:channelsettings:hidden 30
72 collab cursors \"${D}/otherplugin.json\"
74 collab open-editor none
76 collab demo-cursor \"Channel rack\" rack-row ch0 0 112 0.5
77 forget collab-cursor:rack-row:shown
78 waitfor collab-cursor:rack-row:shown 30
79 waitfor never 0.3
80 collab cursors \"${D}/inrack.json\"
81 collab pixel \"${D}/inrack-pixel.json\"
83 collab demo-cursor-edge Playlist \"Channel rack\" -9
84 forget collab-cursor-window:Playlist:shown
85 waitfor collab-cursor-window:Playlist:shown 30
86 waitfor never 0.3
87 collab cursors \"${D}/edge.json\"
88 collab pixel \"${D}/edge-pixel.json\"
90 collab window settings
91 waitfor never 0.5
92 collab demo-cursor-edge Playlist Settings 200
93 waitfor never 0.6
94 collab cursors \"${D}/settings.json\"
95 collab pixel \"${D}/settings-pixel.json\"
96 collab demo-cursor-edge Settings Settings 200
97 waitfor never 0.6
98 collab cursors \"${D}/insettings.json\"
99 collab pixel \"${D}/insettings-pixel.json\"
101 collab window none
102 collab demo-cursor Playlist playlist 1 0 12bar 0.5
103 waitfor never 0.8
104 collab ui leave
105 waitfor never 1
106 collab pixel \"${D}/modal-pixel.json\"
107 collab cursors \"${D}/modal.json\"
")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfg XDG_CONFIG_HOME=${D}/cfg
          "${APP}" --hidden --no-ui-sounds --show collab-cursors --hide mixer --size 1400x880 --ui-scale 1 --frames 115
          --screenshot "${D}/shot.ppm" --dev-input "${D}/script.txt"
  RESULT_VARIABLE rc OUTPUT_FILE "${D}/out.txt" ERROR_FILE "${D}/err.txt" TIMEOUT 240)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (demo): exit code ${rc}")
endif()
file(READ "${D}/out.txt" log)
if(log MATCHES "waitfor collab[^\n]*timed out")
  message(FATAL_ERROR "FAIL (demo): a waitfor timed out:\n${log}")
endif()
# shown: on the spot it names (same track, same time); J-111h: that spot is under the floating channel
# rack here, so the rack covers it (it is behind the rack, still shown, not faded)
readPeer("${D}/shown.json" S)
message(STATUS "demo shown: ${S_kind} track ${S_a} tick ${S_cx} -> under ${S_under_kind} track ${S_under_a} tick ${S_under_cx}, top window ${S_topWindow}")
expectEq("demo: state" "${S_state}" "shown")
expectEq("demo: faded in (not faded out)" "${S_vis}" "1")
expectEq("demo: maps to the playlist" "${S_under_kind}" "playlist")
expectEq("demo: same track" "${S_under_a}" "${S_a}")
expectNear("demo: same time" "${S_under_cx}" "${S_cx}" 1)
expectNear("demo: same height in the track" "${S_under_cy}" "${S_cy}" 0.01)
expectEq("demo: the spot is under the floating channel rack" "${S_topWindow}" "Channel rack")
file(READ "${D}/pixel.json" px)
string(JSON pRead GET "${px}" read)
string(JSON pWant GET "${px}" want)
string(JSON pDiff GET "${px}" diff)
string(JSON pTop GET "${px}" topWindow)
string(JSON occ GET "${px}" occluded)
message(STATUS "demo pixel under the floating channel rack: read ${pRead}, Sam's colour ${pWant}, over ${pTop}, occluded ${occ}")
expectEq("demo under the channel rack: occluded" "${occ}" "1")
if(pDiff LESS 40)
  message(FATAL_ERROR "FAIL (demo): the cursor pointing at the playlist is drawn over the channel rack in front of it: read ${pRead}")
endif()
expectHidden("demo scrolled away" "${D}/offview.json" "off-view")
expectHidden("demo closed mixer" "${D}/mixer.json" "window-hidden")
expectHidden("demo other pattern" "${D}/otherpattern.json" "not-drawn")
readPeer("${D}/piano.json" P)
message(STATUS "demo piano: tick ${P_cx} row ${P_cy} -> under ${P_under_kind} tick ${P_under_cx} row ${P_under_cy}")
expectEq("demo piano: state" "${P_state}" "shown")
expectEq("demo piano: maps to the grid" "${P_under_kind}" "piano")
expectNear("demo piano: same time" "${P_under_cx}" "${P_cx}" 1)
expectNear("demo piano: same key row" "${P_under_cy}" "${P_cy}" 0.01)
# J-111g: hidden for a moment (under the hold time): no fade at all; hidden for good: it fades out
readPeer("${D}/brief.json" P)
file(READ "${D}/brief.json" j)
string(JSON minVis GET "${j}" peers 0 minVis)
message(STATUS "demo brief gap: ${P_state}, lowest visibility ${minVis}")
expectEq("demo brief gap: shown again" "${P_state}" "shown")
expectEq("demo brief gap: never faded" "${minVis}" "1")
file(READ "${D}/sustained.json" j)
string(JSON minVis GET "${j}" peers 0 minVis)
message(STATUS "demo sustained gap: lowest visibility ${minVis}")
expectEq("demo sustained gap: faded out" "${minVis}" "0")
expectHidden("demo sustained gap" "${D}/sustained.json" "window-hidden")
# J-111g: a plugin editor open here covers the cursor of someone pointing at what is under it
# (clipped, not faded); someone in that same plugin's editor is drawn over it; another plugin: hidden
readPeer("${D}/occluded.json" P)
file(READ "${D}/occluded.json" j)
string(JSON occ GET "${j}" peers 0 occluded)
file(READ "${D}/occluded-pixel.json" px)
string(JSON pDiff GET "${px}" diff)
string(JSON pTop GET "${px}" topWindow)
message(STATUS "demo under a plugin editor: ${P_state} (${P_why}), occluded ${occ}, pixel over ${pTop} differs by ${pDiff}")
expectEq("demo under a plugin editor: still shown (no fade)" "${P_state}" "shown")
expectEq("demo under a plugin editor: occluded" "${occ}" "1")
expectEq("demo under a plugin editor: the editor is on top there" "${pTop}" "channelsettings")
if(pDiff LESS 40)
  message(FATAL_ERROR "FAIL (demo): the cursor is drawn over a plugin editor it is not in")
endif()
readPeer("${D}/sameplugin.json" P)
file(READ "${D}/sameplugin.json" j)
string(JSON occ GET "${j}" peers 0 occluded)
file(READ "${D}/sameplugin-pixel.json" px)
string(JSON pDiff GET "${px}" diff)
string(JSON pTop GET "${px}" topWindow)
message(STATUS "demo in the same plugin: ${P_state} (${P_why}), occluded ${occ}, pixel over ${pTop} differs by ${pDiff}")
expectEq("demo in the same plugin: shown" "${P_state}" "shown")
expectEq("demo in the same plugin: not occluded" "${occ}" "0")
if(pDiff GREATER 6)
  message(FATAL_ERROR "FAIL (demo): the cursor in the same plugin's editor is not drawn on top of it")
endif()
expectHidden("demo another plugin in the editor" "${D}/otherplugin.json" "other-plugin")
# J-111h: every window open here covers the cursor of someone pointing at what is behind it; in that
# same window it is on top; partly under a window it is cut at the window's edge; behind a modal it is
# under the modal's dim too
function(pixelCheck tag cursors pixel occWant sameColour)
  readPeer("${cursors}" P)
  file(READ "${cursors}" j)
  string(JSON occ GET "${j}" peers 0 occluded)
  string(JSON cov GET "${j}" peers 0 covered)
  file(READ "${pixel}" px)
  string(JSON pDiff GET "${px}" diff)
  string(JSON pRead GET "${px}" read)
  string(JSON pTop GET "${px}" topWindow)
  message(STATUS "demo ${tag}: ${P_window} ${P_state}, occluded ${occ}, covered ${cov}, pixel ${pRead} over ${pTop} differs by ${pDiff}")
  expectEq("demo ${tag}: shown" "${P_state}" "shown")
  expectEq("demo ${tag}: occluded" "${occ}" "${occWant}")
  if(sameColour AND pDiff GREATER 6)
    message(FATAL_ERROR "FAIL (demo ${tag}): the cursor is not drawn on top there: ${pRead}")
  elseif(NOT sameColour AND pDiff LESS 40)
    message(FATAL_ERROR "FAIL (demo ${tag}): the cursor is drawn over a window in front of what it points at: ${pRead}")
  endif()
  set(P_covered "${cov}" PARENT_SCOPE)
endfunction()
pixelCheck("in the channel rack" "${D}/inrack.json" "${D}/inrack-pixel.json" 0 ON)
expectEq("demo in the channel rack: nothing covers it" "${P_covered}" "0")
pixelCheck("under Settings" "${D}/settings.json" "${D}/settings-pixel.json" 1 OFF)
pixelCheck("in Settings" "${D}/insettings.json" "${D}/insettings-pixel.json" 0 ON)
pixelCheck("at the channel rack's edge" "${D}/edge.json" "${D}/edge-pixel.json" 0 ON)
milli(cov "${P_covered}")
if(cov LESS 100 OR cov GREATER 900)
  message(FATAL_ERROR "FAIL (demo at the channel rack's edge): not cut at the edge (covered ${P_covered})")
endif()
pixelCheck("behind a modal" "${D}/modal.json" "${D}/modal-pixel.json" 0 OFF)

# ---- 2: two windows of other sizes and scales, real ICE ----
# (Sam quits last: Alex's output goes into Sam's input, the pipeline execute_process makes)
set(A "5 collab profile Alex
6 collab picture #C83CA0
10 collab new \"${OUT}/Alex/Song\"
20 collab invite \"${OUT}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
40 collab point Playlist playlist 0 0 0.75bar 0.35
41 waitfor never 1
42 collab cursors \"${OUT}/a-playlist.json\"
43 waitfor never 6
50 collab point \"Channel rack\" rack-row ch0 0 112 0.5
51 waitfor never 1
52 collab cursors \"${OUT}/a-rack.json\"
53 waitfor never 6
60 collab point Mixer mixer-strip mx1 0 0.5 0.6
61 waitfor never 1
62 collab cursors \"${OUT}/a-mixer.json\"
63 waitfor never 6
64 collab picture #3CA0C8
65 waitfor never 10
70 quit
")
set(B "5 collab profile Sam
10 collab join \"${OUT}/invite.txt\" \"${OUT}/Sam/Song\"
11 waitfor collab-joined 120
40 waitfor collab-cursor:playlist:shown 90
41 waitfor never 0.5
42 collab cursors \"${OUT}/b-playlist.json\"
50 waitfor collab-cursor:item:shown 30
51 waitfor never 0.5
52 collab cursors \"${OUT}/b-rack.json\"
60 waitfor collab-cursor-window:Mixer:hidden 30
61 collab cursors \"${OUT}/b-mixer.json\"
62 collab panel
63 waitfor collab-picture:Alex 30
64 waitfor never 0.5
65 collab pictures \"${OUT}/b-pictures1.json\"
66 waitfor never 9
67 collab pictures \"${OUT}/b-pictures2.json\"
68 waitfor never 10
70 quit
")
file(WRITE "${OUT}/a.txt" "${A}")
file(WRITE "${OUT}/b.txt" "${B}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgA XDG_CONFIG_HOME=${OUT}/cfgA
          "${APP}" --hidden --no-ui-sounds --size 1500x920 --ui-scale 1 --dev-input "${OUT}/a.txt"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgB XDG_CONFIG_HOME=${OUT}/cfgB
          "${APP}" --hidden --no-ui-sounds --size 1100x720 --ui-scale 1.5 --hide mixer --dev-input "${OUT}/b.txt"
  RESULTS_VARIABLE rcs OUTPUT_FILE "${OUT}/pair.out" ERROR_FILE "${OUT}/pair.err" TIMEOUT 400)
message(STATUS "two windows: exit codes ${rcs}")
file(READ "${OUT}/pair.out" log)
if(log MATCHES "waitfor collab[^\n]*timed out")
  message(FATAL_ERROR "FAIL (two windows): a waitfor timed out:\n${log}")
endif()
if(NOT rcs STREQUAL "0;0")
  message(FATAL_ERROR "FAIL (two windows): exit codes ${rcs}")
endif()
# the playlist: the same track and time, the same height in the track
readMe("${OUT}/a-playlist.json" M)
readPeer("${OUT}/b-playlist.json" P)
message(STATUS "playlist: Alex ${M_kind} track ${M_a} tick ${M_cx} y ${M_cy}; Sam sees it over ${P_under_kind} track ${P_under_a} tick ${P_under_cx} y ${P_under_cy}")
expectEq("playlist: Alex's anchor" "${M_kind}" "playlist")
expectEq("playlist: shown" "${P_state}" "shown")
expectEq("playlist: over the playlist" "${P_under_kind}" "playlist")
expectEq("playlist: same track" "${P_under_a}" "${M_a}")
expectNear("playlist: same time" "${P_under_cx}" "${M_cx}" 2)
expectNear("playlist: same height in the track" "${P_under_cy}" "${M_cy}" 0.02)
# the channel rack: the same button (item ID), the same spot in it
readMe("${OUT}/a-rack.json" M)
readPeer("${OUT}/b-rack.json" P)
message(STATUS "rack: Alex ${M_kind} ${M_a} at ${M_cx} ${M_cy}; Sam sees it over ${P_under_kind} ${P_under_a} at ${P_under_cx} ${P_under_cy}")
expectEq("rack: Alex's anchor is a button" "${M_kind}" "item")
expectEq("rack: shown" "${P_state}" "shown")
expectEq("rack: over a button" "${P_under_kind}" "item")
expectEq("rack: the same button" "${P_under_a}" "${M_a}")
expectNear("rack: same spot across" "${P_under_cx}" "${M_cx}" 0.02)
expectNear("rack: same spot down" "${P_under_cy}" "${M_cy}" 0.02)
# the mixer, closed on Sam's side: hidden
readMe("${OUT}/a-mixer.json" M)
expectEq("mixer: Alex is over the mixer" "${M_window}" "Mixer")
expectHidden("mixer closed on Sam's side" "${OUT}/b-mixer.json" "window-hidden")
# J-111g: Alex's picture reaches Sam (the same bytes), is kept on Sam's computer and drawn in the
# People list; Alex's new picture replaces it
function(alexPicture file prefix)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "FAIL: no report ${file}")
  endif()
  file(READ "${file}" j)
  string(JSON n LENGTH "${j}" members)
  math(EXPR last "${n} - 1")
  foreach(i RANGE ${last})
    string(JSON name GET "${j}" members ${i} name)
    if(name STREQUAL "Alex")
      foreach(k announced received kept drawn pixelRead pixelWant pixelDiff)
        string(JSON v ERROR_VARIABLE e GET "${j}" members ${i} ${k})
        set(${prefix}_${k} "${v}" PARENT_SCOPE)
      endforeach()
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "FAIL: Alex is not a member in ${file}")
endfunction()
alexPicture("${OUT}/b-pictures1.json" P1)
message(STATUS "picture: announced ${P1_announced}, received ${P1_received}, kept ${P1_kept}, drawn ${P1_drawn}, pixel ${P1_pixelRead} (picture ${P1_pixelWant})")
if(P1_announced STREQUAL "")
  message(FATAL_ERROR "FAIL (picture): Alex's profile names no picture on Sam's side")
endif()
expectEq("picture: the bytes received match the hash" "${P1_received}" "${P1_announced}")
expectEq("picture: kept on Sam's computer" "${P1_kept}" "${P1_announced}")
expectEq("picture: drawn in the People list" "${P1_drawn}" "1")
if(P1_pixelDiff GREATER 24)
  message(FATAL_ERROR "FAIL (picture): the People row does not show the picture: ${P1_pixelRead} vs ${P1_pixelWant}")
endif()
string(REGEX MATCH "^[0-9]+" red1 "${P1_pixelWant}")
expectNear("picture: its colour (the red of #C83CA0)" "${red1}" "200" 12)
alexPicture("${OUT}/b-pictures2.json" P2)
message(STATUS "changed picture: announced ${P2_announced}, received ${P2_received}, drawn ${P2_drawn}, pixel ${P2_pixelRead} (picture ${P2_pixelWant})")
if(P2_announced STREQUAL P1_announced)
  message(FATAL_ERROR "FAIL (picture): Alex's new picture was not announced")
endif()
expectEq("changed picture: received" "${P2_received}" "${P2_announced}")
expectEq("changed picture: drawn" "${P2_drawn}" "1")
if(P2_pixelDiff GREATER 24)
  message(FATAL_ERROR "FAIL (changed picture): the People row does not show it: ${P2_pixelRead} vs ${P2_pixelWant}")
endif()
string(REGEX MATCH "^[0-9]+" red2 "${P2_pixelWant}")
expectNear("changed picture: its colour (the red of #3CA0C8)" "${red2}" "60" 12)
message(STATUS "ok - cursors map across sizes and scales, fade where not shown (not for a moment), draw on top of their own window, behind windows in front of it (plugin editors, Settings, the channel rack, modals), cut at their edges; pictures arrive and update")
