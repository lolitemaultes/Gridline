# Gridline: K-186, the channel rack's headphones button (hidden app runs, isolated config and home):
#   * every row has a headphones button at the far left, left of the pan knob, inside the row,
#     and the step grid still starts under the header's step lamps (100% and 200%);
#   * click mutes (one undo step), again unmutes;
#   * right-click solos: every other channel muted, the dump reports "solo" for it;
#   * right-click again restores the exact earlier states, including a channel muted before the solo;
#   * each solo and unsolo is one undo step; a moved solo still restores the first states;
#   * Ctrl+click solos and unsolos; the light between the name and the steps agrees;
#   * another channel unmuted by hand during a solo ends it: the solo ring goes, and a right-click
#     on the formerly soloed channel starts a new solo.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k186_rack_headphones_app.cmake
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
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1440x900 --frames ${frames} --screenshot "${d}/end.ppm"
                          --dev-input "${d}/input.txt" ${ARGN}
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

# The text of the n-th "rack" dump (0 based) -> out.
function(dump tag n out)
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
  set(${out} "${rest}" PARENT_SCOPE)
endfunction()

# Every channel's mute state in dump n as "m-m...".
function(mutes tag n out)
  dump(${tag} ${n} d)
  string(REGEX MATCHALL "  channel [0-9]+ [^\n]* muted [01]" lines "${d}")
  set(s "")
  foreach(l ${lines})
    if(l MATCHES "muted 1$")
      string(APPEND s "m")
    else()
      string(APPEND s "-")
    endif()
  endforeach()
  set(${out} "${s}" PARENT_SCOPE)
endfunction()

# Row i of dump n: g_<part>_x0 y0 x1 y1 for phones pan steps lamp, g_state (the headphones button).
function(row tag n i)
  dump(${tag} ${n} d)
  if(NOT d MATCHES "  rowgeom ${i} lights [0-9]+ selected [01]([^\n]*)")
    fail(${tag} "no row geometry for row ${i} in rack dump ${n}")
  endif()
  set(line "${CMAKE_MATCH_1}")
  foreach(part phones pan steps lamp name)
    if(NOT line MATCHES " ${part} ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) ([-0-9.]+)")
      fail(${tag} "row ${i}: no ${part} rectangle")
    endif()
    set(raw "${CMAKE_MATCH_1};${CMAKE_MATCH_2};${CMAKE_MATCH_3};${CMAKE_MATCH_4}")  # a REGEX REPLACE resets CMAKE_MATCH_*
    foreach(k 1 2 3 4)
      math(EXPR j "${k} - 1")
      list(GET raw ${j} r)
      string(REGEX REPLACE "\\..*" "" v${k} "${r}")
    endforeach()
    set(g_${part}_x0 "${v1}" PARENT_SCOPE)
    set(g_${part}_y0 "${v2}" PARENT_SCOPE)
    set(g_${part}_x1 "${v3}" PARENT_SCOPE)
    set(g_${part}_y1 "${v4}" PARENT_SCOPE)
  endforeach()
  if(NOT line MATCHES " phonestate ([a-z]+)")
    fail(${tag} "row ${i}: no headphones state")
  endif()
  set(g_state "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# ---- 1. geometry: the button sits left of the pan knob; the steps line up with the header ----
foreach(spec "1;w520" "1;w1300" "2;w900")
  list(GET spec 0 scale)
  list(GET spec 1 w)
  set(tag geom_${scale}_${w})
  run(${tag} 16 "5 toasts clear\n6 closeplugins\n12 rack\n" --ui-scale ${scale} --rack ${w},h420)
  dump(${tag} 0 d)
  string(REGEX MATCH "originX ([-0-9.]+) row0Y [-0-9.]+ viewX0 ([-0-9.]+)" _ "${d}")
  set(vx "${CMAKE_MATCH_2}")
  string(REGEX REPLACE "\\..*" "" ox "${CMAKE_MATCH_1}")
  string(REGEX REPLACE "\\..*" "" vx "${vx}")
  string(REGEX MATCH " rows ([0-9]+)" _ "${d}")
  math(EXPR last "${CMAKE_MATCH_1} - 1")
  foreach(i RANGE ${last})
    row(${tag} 0 ${i})
    math(EXPR pw "${g_phones_x1} - ${g_phones_x0}")
    math(EXPR ph "${g_phones_y1} - ${g_phones_y0}")
    if(g_phones_x0 LESS ox OR NOT g_phones_x1 LESS g_pan_x0 OR pw LESS 16 OR NOT pw EQUAL ph)
      fail(${tag} "row ${i}: headphones ${g_phones_x0}..${g_phones_x1} (${pw}x${ph}), row from ${ox}, pan from ${g_pan_x0}")
    endif()
    if(g_phones_y0 LESS g_name_y0 OR g_phones_y1 GREATER g_name_y1)
      math(EXPR slack "${g_name_y0} - ${g_phones_y0}")
      if(slack GREATER 1)
        fail(${tag} "row ${i}: the headphones button leaves the row's height")
      endif()
    endif()
    if(NOT g_steps_x0 EQUAL vx)
      fail(${tag} "row ${i}: the steps start at ${g_steps_x0}, the header's step lamps at ${vx}")
    endif()
    if(NOT g_state STREQUAL "on")
      fail(${tag} "row ${i}: the headphones state is ${g_state} at the start, wanted on")
    endif()
  endforeach()
endforeach()
message(STATUS "k186: headphones at the far left of every row, steps aligned with the header (100% / 200%)")

# ---- 2. behaviour ----
row(geom_1_w1300 0 0)
math(EXPR x0 "(${g_phones_x0} + ${g_phones_x1}) / 2")
math(EXPR y0 "(${g_phones_y0} + ${g_phones_y1}) / 2")
row(geom_1_w1300 0 1)
math(EXPR x1 "(${g_phones_x0} + ${g_phones_x1}) / 2")
math(EXPR y1 "(${g_phones_y0} + ${g_phones_y1}) / 2")
row(geom_1_w1300 0 2)
math(EXPR x2 "(${g_phones_x0} + ${g_phones_x1}) / 2")
math(EXPR y2 "(${g_phones_y0} + ${g_phones_y1}) / 2")

# dumps: 0 start, 1 click row 1, 2 undo, 3 click row 2 (muted by hand), 4 right-click row 0 (solo),
# 5 right-click row 0 again (normal), 6 undo (soloed), 7 undo (normal), 8 right-click row 0,
# 9 right-click row 1 (moved), 10 right-click row 1 again, 11 Ctrl+click row 1, 12 Ctrl+click again
# 13 right-click row 0 (solo), 14 click row 1 (unmuted by hand: the solo ends), 15 right-click row 0 (a new solo)
run(act 170 "5 toasts clear
6 closeplugins
9 focus \"Channel rack\"
12 rack
14 move ${x1} ${y1}
16 down 0
17 up 0
22 rack
24 undo
28 rack
30 move ${x2} ${y2}
32 down 0
33 up 0
38 rack
40 move ${x0} ${y0}
42 down 1
43 up 1
48 rack
50 down 1
51 up 1
56 rack
58 undo
62 rack
64 undo
68 rack
70 down 1
71 up 1
76 rack
78 move ${x1} ${y1}
80 down 1
81 up 1
86 rack
88 down 1
89 up 1
94 rack
96 mod ctrl 1
98 down 0
99 up 0
100 mod ctrl 0
104 rack
106 mod ctrl 1
108 down 0
109 up 0
110 mod ctrl 0
114 rack
116 move ${x0} ${y0}
118 down 1
119 up 1
124 rack
126 move ${x1} ${y1}
128 down 0
129 up 0
134 rack
136 move ${x0} ${y0}
138 down 1
139 up 1
144 rack
" --rack w1300,h420)

mutes(act 0 m0)
string(LENGTH "${m0}" nch)
if(nch LESS 3 OR NOT m0 MATCHES "^-+$")
  fail(act "the fixture should start with 3 or more playing channels (${m0})")
endif()
math(EXPR rest "${nch} - 3")
string(REPEAT "m" ${rest} allM)
string(REPEAT "-" ${rest} allP)
# n | wanted mutes | what
set(cases
    "1|-m-${allP}|click: mute row 1"
    "2|---${allP}|undo of the mute"
    "3|--m${allP}|click: mute row 2 (by hand, before the solo)"
    "4|-mm${allM}|right-click row 0: solo"
    "5|--m${allP}|right-click row 0 again: the exact earlier states (row 2 stays muted)"
    "6|-mm${allM}|undo of the unsolo: soloed again"
    "7|--m${allP}|undo of the solo"
    "8|-mm${allM}|right-click row 0: solo"
    "9|m-m${allM}|right-click row 1: the solo moves"
    "10|--m${allP}|right-click row 1 again: the states from before the first solo"
    "11|m-m${allM}|Ctrl+click row 1: solo"
    "12|--m${allP}|Ctrl+click row 1 again: back to normal"
    "13|-mm${allM}|right-click row 0: solo"
    "14|--m${allM}|click row 1 during the solo: unmuted by hand"
    "15|-mm${allM}|right-click row 0 after the hand change: a new solo")
set(seen "")
foreach(i RANGE 14)
  list(GET cases ${i} c)
  string(REGEX MATCH "^([0-9]+)[|]([-m]+)[|](.*)$" _ "${c}")
  set(n "${CMAKE_MATCH_1}")
  set(w "${CMAKE_MATCH_2}")
  set(why "${CMAKE_MATCH_3}")
  mutes(act ${n} m)
  string(APPEND seen " ${m}")
  if(NOT m STREQUAL w)
    fail(act "${why}: mutes ${m}, wanted ${w}")
  endif()
endforeach()
message(STATUS "k186: mutes through the clicks:${seen}")

# The undo history: one step per click, solo and unsolo.
function(undoAt n out)
  dump(act ${n} d)
  string(REGEX MATCH " undo ([0-9]+)" _ "${d}")
  set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
undoAt(0 u0)
undoAt(1 u1)
undoAt(3 u3)
undoAt(4 u4)
undoAt(5 u5)
math(EXPR d1 "${u1} - ${u0}")
math(EXPR d4 "${u4} - ${u3}")
math(EXPR d5 "${u5} - ${u4}")
if(NOT d1 EQUAL 1 OR NOT d4 EQUAL 1 OR NOT d5 EQUAL 1)
  fail(act "undo steps: mute ${d1}, solo ${d4}, unsolo ${d5} (wanted one each)")
endif()

# The headphones states as drawn: muted, solo, on.
row(act 1 1)
if(NOT g_state STREQUAL "muted")
  fail(act "a muted channel's headphones show '${g_state}'")
endif()
foreach(pair "4;0;solo" "4;1;muted" "4;2;muted" "5;0;on" "5;2;muted" "9;1;solo" "9;0;muted" "13;0;solo" "14;0;on" "14;1;on" "15;0;solo")
  list(GET pair 0 n)
  list(GET pair 1 i)
  list(GET pair 2 want)
  row(act ${n} ${i})
  if(NOT g_state STREQUAL want)
    fail(act "dump ${n} row ${i}: the headphones show '${g_state}', wanted ${want}")
  endif()
endforeach()
message(STATUS "k186: all checks passed")
