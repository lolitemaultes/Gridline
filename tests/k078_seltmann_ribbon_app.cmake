# Gridline — 0.7.8: Seltmann's Ribbon editor, driven like a user (hidden app, isolated config).
#   1. heights: the floating editor is Full; Compact and Standard by choice; the rows never overlap
#      and stay inside the editor;
#   2. tools: with the Cut tool a double-click in the waveform adds a cut;
#   3. layout preview: holding a card plays it and leaves the sequence as it was; a click keeps it as
#      one undo step;
#   4. the editor's keys (only those free in every key set): Right / Left, S, J, 1 to 6;
#   5. cards: a region click opens the region card, a block click the step card, Esc closes it;
#   6. the four layouts at 100 % and 200 %: the editor's rows fit the room it has.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P k078_seltmann_ribbon_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_SELTMANN_TRACE} "1")
set(ENV{GRIDLINE_SELTMANN_DEMO} "${OUT}/Break 96.wav")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(failures 0)

# run(tag frames script [extra app args...])
function(run tag frames script)
  string(REGEX REPLACE "([0-9]+) quit discard" "\\1 shot \"${OUT}/${tag}.ppm\"\n\\1 quit discard" script "${script}")
  # (the traced points are where they show on screen, at the editor's own size)
  file(WRITE "${OUT}/${tag}.txt" "5 waitfor seltmann-ready 120\n${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.seltmann --size 1440x960 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  file(WRITE "${OUT}/${tag}.log" "${out}")
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg text)
  if(NOT (${ARGN}))
    message(SEND_ERROR "FAIL: ${msg}")
    math(EXPR f "${failures} + 1")
    set(failures ${f} PARENT_SCOPE)
    return()
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# The last "seltmann: state ready ..." line of a run.
function(lastState out var)
  string(REGEX MATCHALL "seltmann: state ready[^\n]*" ls "${out}")
  list(LENGTH ls n)
  if(n EQUAL 0)
    set(${var} "" PARENT_SCOPE)
    return()
  endif()
  list(GET ls -1 l)
  set(${var} "${l}" PARENT_SCOPE)
endfunction()

# The named control's centre ("seltmann: btn <name> x y") into <var>_x / <var>_y.
# (the last one: the window settles where it opens over the first frames)
function(btn out name var)
  pt("${out}" "btn ${name}" ${var})
  set(${var}_x "${${var}_x}" PARENT_SCOPE)
  set(${var}_y "${${var}_y}" PARENT_SCOPE)
endfunction()
# The last "seltmann: <key> X Y" into <var>_x / <var>_y.
function(pt out key var)
  string(REGEX MATCHALL "seltmann: ${key} [0-9-]+ [0-9-]+" ms "${out}")
  list(LENGTH ms n)
  if(n EQUAL 0)
    set(${var}_x "" PARENT_SCOPE)
    set(${var}_y "" PARENT_SCOPE)
    return()
  endif()
  list(GET ms -1 m)
  string(REGEX MATCH "([0-9-]+) ([0-9-]+)$" _ "${m}")
  set(${var}_x "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${var}_y "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# Rows: each "name x0 y0 x1 y1" in the last rows line; they must not overlap (top to bottom in
# the ribbon column) and stay in the room the editor had when hosted.
function(rowsFit out tag)
  string(REPLACE ";" "|" out "${out}")  # (the rows line separates rows with "; ": not a CMake list)
  string(REGEX MATCHALL "seltmann: height [a-z]+ host ([0-9]+) room ([0-9-]+) ([0-9-]+) ([0-9-]+) ([0-9-]+)[^\n]*\nseltmann: rows [^\n]*" hs "${out}")
  list(LENGTH hs n)
  if(n EQUAL 0)
    message(SEND_ERROR "FAIL: ${tag}: no rows traced")
    math(EXPR f "${failures} + 1")
    set(failures ${f} PARENT_SCOPE)
    return()
  endif()
  list(GET hs -1 h)
  string(REGEX MATCH "host ([0-9]+) room ([0-9-]+) ([0-9-]+) ([0-9-]+) ([0-9-]+)" _ "${h}")
  set(host ${CMAKE_MATCH_1})
  set(roomBottom ${CMAKE_MATCH_5})
  string(REGEX MATCH "seltmann: rows ([^\n]*)" _ "${h}")
  string(REPLACE "| " ";" rows "${CMAKE_MATCH_1}")
  set(prev -100000)
  set(bad "")
  set(bottom 0)
  foreach(r IN LISTS rows)
    string(REGEX MATCH "^([a-z]+) ([0-9-]+) ([0-9-]+) ([0-9-]+) ([0-9-]+)$" _ "${r}")
    set(name ${CMAKE_MATCH_1})
    set(y0 ${CMAKE_MATCH_3})
    set(y1 ${CMAKE_MATCH_5})
    if(name STREQUAL "library")
      continue()
    endif()
    if(y0 LESS prev)
      string(APPEND bad " ${name} starts at ${y0} above the row before (ends ${prev})")
    endif()
    set(prev ${y1})
    set(bottom ${y1})
  endforeach()
  if(host GREATER 0 AND bottom GREATER roomBottom)
    math(EXPR over "${bottom} - ${roomBottom}")
    if(over GREATER 2)
      string(APPEND bad " the last row ends ${over} px below the room (${bottom} > ${roomBottom})")
    endif()
  endif()
  string(REGEX MATCH "seltmann: crowded [^\n]*" crowd "${out}")
  if(crowd)
    string(APPEND bad " controls run into each other (${crowd})")
  endif()
  if(bad)
    message(SEND_ERROR "FAIL: ${tag}: rows overlap:${bad}\n  ${h}")
    math(EXPR f "${failures} + 1")
    set(failures ${f} PARENT_SCOPE)
  else()
    message(STATUS "ok - ${tag}: the rows do not overlap (${rows})")
  endif()
endfunction()

# ---- 1. heights
run(full 30 "28 quit discard\n")
check("the floating editor is Full" "" full_out MATCHES "seltmann: height full host 0")
check("... with the layout library" "" full_out MATCHES "seltmann: rows [^\n]*library [^\n]*keys")
rowsFit("${full_out}" full)
lastState("${full_out}" st0)
string(REGEX MATCH "order \"([^\"]*)\"" _ "${st0}")
set(order0 "${CMAKE_MATCH_1}")
string(REGEX MATCH "slices ([0-9]+)" _ "${st0}")
set(slices0 "${CMAKE_MATCH_1}")
check("the break is chopped (${slices0} regions, order ${order0})" "" slices0 GREATER 3)
foreach(hc compact standard)
  set(ENV{GRIDLINE_SELTMANN_HEIGHT} "${hc}")
  run(${hc} 30 "28 quit discard\n")
  check("height ${hc} by choice" "" ${hc}_out MATCHES "seltmann: height ${hc}")
  rowsFit("${${hc}_out}" ${hc})
endforeach()
check("Standard shows the layout cards" "" standard_out MATCHES "seltmann: rows [^\n]*layouts")
check("Compact has one row on top" "" compact_out MATCHES "seltmann: rows compactrow")
unset(ENV{GRIDLINE_SELTMANN_HEIGHT})

# ---- 2. tools: Cut, double-click adds a cut
pt("${full_out}" "cut 2" c2)
set(c2x ${c2_x})
set(c2y ${c2_y})
pt("${full_out}" "cut 3" c3)
set(c3x ${c3_x})
math(EXPR mx "(${c2x} + ${c3x}) / 2")
btn("${full_out}" toolcut tc)
check("the Cut tool is shown" "" tc_x MATCHES "[0-9]")
run(cut 60 "20 move ${tc_x} ${tc_y}
21 down 0
22 up 0
26 move ${mx} ${c2y}
28 down 0
29 up 0
30 down 0
31 up 0
40 key escape
50 quit discard
")
check("the Cut tool is picked" "" cut_out MATCHES "seltmann: height full [^\n]*tool 1")
math(EXPR want "${slices0} + 1")
lastState("${cut_out}" st)
check("a double-click with Cut adds a cut (${slices0} -> ${want})" "" st MATCHES "slices ${want} ")
run(playdbl 60 "26 move ${mx} ${c2y}
28 down 0
29 up 0
30 down 0
31 up 0
40 key escape
50 quit discard
")
lastState("${playdbl_out}" st)
check("... with the Play tool it does not" "" st MATCHES "slices ${slices0} ")

# ---- 3. layout preview: hold plays, leaves the sequence; a click keeps it, one undo step
string(REGEX MATCH "seltmann: card \"([^\"]+)\" " _ "${full_out}")
set(card "${CMAKE_MATCH_1}")
pt("${full_out}" "card \"${card}\"" cc)
set(cx ${cc_x})
set(cy ${cc_y})
check("the library lists layouts (first: ${card})" "" cx MATCHES "[0-9]")
run(hold 90 "20 move ${cx} ${cy}
22 down 0
70 up 0
72 move 5 5
85 quit discard
")
check("holding a layout plays it" "" hold_out MATCHES "seltmann: preview start \"${card}\"")
check("... the render plays" "" hold_out MATCHES "seltmann: preview playing \"${card}\"")
check("... letting go stops it" "" hold_out MATCHES "seltmann: preview stop \"${card}\"")
lastState("${hold_out}" st)
check("... and the sequence is as it was" "" st MATCHES "preset \"\"" AND st MATCHES "order \"${order0}\"")
run(keep 70 "20 move ${cx} ${cy}
22 down 0
24 up 0
40 move 5 5
44 undo
60 quit discard
")
check("a click keeps the layout" "" keep_out MATCHES "preset \"${card}\"")
lastState("${keep_out}" st)
check("... and one undo puts the sequence back" "" st MATCHES "preset \"\"" AND st MATCHES "order \"${order0}\"")

# ---- 4. editor keys (the waveform's region focus: click the first region first)
pt("${full_out}" "seq 0" s0)
set(s0x ${s0_x})
set(s0y ${s0_y})
set(focus "20 move ${s0x} ${s0y}\n21 down 0\n22 up 0\n24 key escape\n")
run(keyright 50 "${focus}28 key right\n32 key right\n36 key left\n45 quit discard\n")
check("Right picks the next region" "" keyright_out MATCHES "seltmann: key Next region")
check("Left the one before" "" keyright_out MATCHES "seltmann: key Previous region")
run(keys 50 "${focus}28 key s\n45 quit discard\n")
math(EXPR want "${slices0} + 1")
lastState("${keys_out}" st)
check("S splits the region (${want} regions)" "" keys_out MATCHES "seltmann: key Split the region in two" AND st MATCHES "slices ${want} ")
run(keyj 50 "${focus}28 key right\n30 key j\n45 quit discard\n")
math(EXPR want "${slices0} - 1")
lastState("${keyj_out}" st)
check("J joins the region with the one before (${want} regions)" "" keyj_out MATCHES "seltmann: key Join with the previous region" AND st MATCHES "slices ${want} ")
foreach(k 1 2 3 4 5 6)
  run(key${k} 50 "${focus}28 key ${k}\n45 quit discard\n")
  check("${k} sets the chop size" "" key${k}_out MATCHES "seltmann: key Chop: ")
endforeach()
lastState("${key2_out}" st)
check("... 2 is one region per bar" "" st MATCHES "slices [12] ")

# ---- 5. cards: a region click opens the region card, Esc closes it
string(REGEX MATCHALL "seltmann: rows [^\n]*wave [0-9-]+ [0-9-]+ [0-9-]+ [0-9-]+" wl "${full_out}")
string(REPLACE ";" "|" wl "${wl}")
string(REGEX MATCH "wave ([0-9-]+) ([0-9-]+) ([0-9-]+) ([0-9-]+)$" _ "${wl}")
math(EXPR wy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
math(EXPR wx "${CMAKE_MATCH_1} + 30")
run(card 60 "20 move ${wx} ${wy}
21 down 0
22 up 0
34 key escape
50 quit discard
")
check("a click on a region opens its card" "" card_out MATCHES "seltmann: height full [^\n]*card 1 [0-9]")
string(REGEX MATCHALL "seltmann: height full [^\n]*" hl "${card_out}")
list(GET hl -1 hlast)
check("... Esc closes it" "" hlast MATCHES " card 0 ")
run(stepcard 60 "20 move ${s0x} ${s0y}
21 down 0
22 up 0
50 quit discard
")
check("a click on a block opens the step card" "" stepcard_out MATCHES "seltmann: height full [^\n]*card 2 [0-9]")
foreach(c 3 4 5 6 7)
  set(ENV{GRIDLINE_SELTMANN_CARD} "${c}")
  run(cardn${c} 40 "38 quit discard\n")
  check("card ${c} opens" "" cardn${c}_out MATCHES "card ${c} [0-9]")
endforeach()
unset(ENV{GRIDLINE_SELTMANN_CARD})

# ---- 6. the four layouts, 100 % and 200 %
foreach(lay studio live logic tracker)
  foreach(sc 1 2)
    if(sc EQUAL 2)
      set(size 2880x1800)
    else()
      set(size 1440x960)
    endif()
    file(WRITE "${OUT}/lay_${lay}_${sc}.txt" "5 waitfor seltmann-ready 160\n50 shot \"${OUT}/lay_${lay}_${sc}.ppm\"\n60 quit discard\n")
    execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.seltmann --layout ${lay} --size ${size} --ui-scale ${sc}
                            --frames 70 --screenshot "${OUT}/lay_${lay}_${sc}.ppm" --dev-input "${OUT}/lay_${lay}_${sc}.txt"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(WRITE "${OUT}/lay_${lay}_${sc}.log" "${out}")
    check("${lay} at x${sc}: the app runs" "" rc EQUAL 0)
    if(lay STREQUAL "tracker")
      # the Tracker: the instrument's own screens in the lower band (the editor opens from "Open plugin window")
      continue()
    endif()
    check("${lay} at x${sc}: the editor is drawn" "" out MATCHES "seltmann: height ")
    rowsFit("${out}" "${lay} x${sc}")
    if(lay STREQUAL "logic")
      check("Smooth: hosted in the lower pane" "" out MATCHES "seltmann: height [a-z]+ host [1-9]")
    endif()
  endforeach()
endforeach()

if(failures GREATER 0)
  message(FATAL_ERROR "k078 seltmann ribbon: ${failures} checks failed")
endif()
message(STATUS "k078 seltmann ribbon: all checks passed")
