# Gridline: K-232 / K-233, the channel rack's select buttons and clicks (hidden app runs,
# isolated config and home):
#   * a click on a row's select button toggles that channel in the selection and opens nothing;
#   * the typing keyboard plays every selected channel (live notes = selected channels);
#   * a right-click on a select button selects only that channel;
#   * a double-click on a channel's name opens the piano roll for it, and leaves no channel
#     window open from the first click.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k232_rack_select_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)
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
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1440x900 --frames ${frames}
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

# The n-th "rack" dump (0 based) -> out.
function(dump tag n out)
  set(rest "${${tag}_out}")
  foreach(k RANGE ${n})
    string(FIND "${rest}" "rack frame " at)
    if(at EQUAL -1)
      fail(${tag} "no rack dump ${n}")
    endif()
    math(EXPR at "${at} + 10")
    string(SUBSTRING "${rest}" ${at} -1 rest)
  endforeach()
  string(FIND "${rest}" "rack frame " end)
  if(end GREATER -1)
    string(SUBSTRING "${rest}" 0 ${end} rest)
  endif()
  set(${out} "${rest}" PARENT_SCOPE)
endfunction()

# The selected flags of every row in dump n as "1-1...".
function(selected tag n out)
  dump(${tag} ${n} d)
  string(REGEX MATCHALL "  rowgeom [0-9]+ lights [0-9]+ selected [01]" lines "${d}")
  set(s "")
  foreach(l ${lines})
    if(l MATCHES "selected 1$")
      string(APPEND s "1")
    else()
      string(APPEND s "-")
    endif()
  endforeach()
  set(${out} "${s}" PARENT_SCOPE)
endfunction()

# Centre of row i's <part> rectangle in dump n -> cx, cy.
function(centre tag n i part)
  dump(${tag} ${n} d)
  if(NOT d MATCHES "  rowgeom ${i} lights [0-9]+ selected [01][^\n]* ${part} ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) ([-0-9.]+)")
    fail(${tag} "row ${i}: no ${part} rectangle")
  endif()
  set(raw "${CMAKE_MATCH_1};${CMAKE_MATCH_2};${CMAKE_MATCH_3};${CMAKE_MATCH_4}")
  foreach(k 1 2 3 4)
    math(EXPR j "${k} - 1")
    list(GET raw ${j} r)
    string(REGEX REPLACE "\\..*" "" v${k} "${r}")
  endforeach()
  math(EXPR x "(${v1} + ${v3}) / 2")
  math(EXPR y "(${v2} + ${v4}) / 2")
  set(cx "${x}" PARENT_SCOPE)
  set(cy "${y}" PARENT_SCOPE)
endfunction()

function(windowcount tag n out)
  string(REGEX MATCHALL "windows frame [^\n]*(\n  window [^\n]*)*" all "${${tag}_out}")
  list(GET all ${n} w)
  string(REGEX MATCHALL "\n  window " ws "${w}")
  list(LENGTH ws c)
  set(${out} "${c}" PARENT_SCOPE)
endfunction()

run(geom 16 "5 toasts clear\n6 closeplugins\n12 rack\n" --rack w900,h420 --hide piano)
selected(geom 0 s0)
centre(geom 0 1 lamp)
set(l1x ${cx})
set(l1y ${cy})
centre(geom 0 2 lamp)
set(l2x ${cx})
set(l2y ${cy})
centre(geom 0 2 name)
set(n2x ${cx})
set(n2y ${cy})
message(STATUS "k232: start selection ${s0}")

# dumps: 0 start, 1 click lamp 1, 2 click lamp 2, 3 right-click lamp 1 (only), 4 click lamp 1 again (toggle off)
run(act 120 "5 toasts clear
6 closeplugins
9 focus \"Channel rack\"
10 windows
12 rack
14 move ${l1x} ${l1y}
16 down 0
17 up 0
22 rack
24 windows
26 move ${l2x} ${l2y}
28 down 0
29 up 0
34 rack
36 typing on
38 sdlkey z down
46 sound
48 sdlkey z up
50 typing off
52 move ${l1x} ${l1y}
54 down 1
55 up 1
60 rack
62 move ${l2x} ${l2y}
64 down 0
65 up 0
70 rack
72 windows
110 quit discard
" --rack w900,h420 --hide piano)
selected(act 0 a0)
selected(act 1 a1)
selected(act 2 a2)
selected(act 3 a3)
selected(act 4 a4)
string(SUBSTRING "${a1}" 1 1 r1)
if(NOT r1 STREQUAL "1")
  fail(act "a click on row 1's select button did not select it (${a0} -> ${a1})")
endif()
string(SUBSTRING "${a0}" 0 1 r0a)
string(SUBSTRING "${a1}" 0 1 r0b)
if(NOT r0a STREQUAL r0b)
  fail(act "a click on row 1's select button changed row 0 (${a0} -> ${a1}): it must toggle, not replace")
endif()
windowcount(act 0 w0)
windowcount(act 1 w1)
if(NOT w0 EQUAL w1)
  fail(act "a click on a select button opened a window (${w0} -> ${w1})")
endif()
string(REGEX MATCH "sound frame [0-9]+: peak [0-9.]+ live ([0-9]+)" _ "${act_out}")
set(live "${CMAKE_MATCH_1}")
string(REGEX MATCHALL "1" ones "${a2}")
list(LENGTH ones nsel)
if(nsel LESS 2 OR NOT live EQUAL nsel)
  fail(act "typing played ${live} notes with ${nsel} channels selected (${a2})")
endif()
if(NOT a3 MATCHES "^-1-*$")
  fail(act "a right-click on row 1's select button did not select only it (${a3})")
endif()
if(NOT a4 MATCHES "^-11-*$")
  fail(act "a click on row 2's select button did not add it (${a4})")
endif()
message(STATUS "k232: select buttons toggle (${a0} -> ${a1} -> ${a2}), typing plays ${live} channels, right-click selects one (${a3})")

# K-233: a double-click on a channel's name opens the piano roll for it; the first click's window closes.
run(dbl 80 "5 toasts clear
6 closeplugins
9 focus \"Channel rack\"
10 windows
12 plgeom
14 move ${n2x} ${n2y}
16 down 0
17 up 0
19 down 0
20 up 0
40 windows
42 plgeom
70 quit discard
" --rack w900,h420 --hide piano)
string(REGEX MATCHALL " piano ([01]) pattern [0-9]+ channel ([0-9]+)" pg "${dbl_out}")
list(LENGTH pg npg)
if(npg LESS 2)
  fail(dbl "no playlist geometry")
endif()
list(GET pg 0 p0)
list(GET pg 1 p1)
if(NOT p0 MATCHES " piano 0 " OR NOT p1 MATCHES " piano 1 ")
  fail(dbl "a double-click on a channel name did not open the piano roll (${p0} -> ${p1})")
endif()
windowcount(dbl 0 d0)
windowcount(dbl 1 d1)
math(EXPR d0p "${d0} + 1")
if(d1 GREATER d0p)
  fail(dbl "the first click's channel window stayed open (${d0} -> ${d1} windows)")
endif()
message(STATUS "k233: a double-click on a name opens the piano roll (${p1})")
