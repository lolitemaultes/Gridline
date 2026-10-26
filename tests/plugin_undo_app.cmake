# Gridline - user report: "Ctrl+Z must undo tweaks made in plugin editors". The real app (hidden,
# isolated configs, 1440x900, SDL's dummy audio driver) with real SDL key events for the
# shortcuts (Ctrl+Z, Ctrl+Shift+Z, Ctrl+Y, Cmd+Z) and the scripted pointer for the gestures:
#   1. Plasma: a knob drag is one undo step; Ctrl+Z restores the value, Ctrl+Shift+Z and Ctrl+Y
#      redo it;
#   2. Plasma: two strokes on an LFO shape (plugin data, not a parameter) are two undo steps, and
#      Ctrl+Z takes back only the second (before: the two strokes merged into one step);
#   3. another pack's plugin (Wavetable, an instrument) and a mixer insert (Delay): knob drag, undo,
#      redo; a wheel turn of several steps over the Delay's knob is one step (before: one per step), and
#      two turns with a pause between them are two;
#   4. Plasma popped out into a window of its own, the keyboard in that window: Ctrl+Z / Cmd+Z
#      undo and Ctrl+Shift+Z redoes (skipped where windows cannot be detached).
# "atparam <k>" puts the pointer on the last drawn control of parameter k of the open editor;
# "plug <k>" prints the undo position, the labels, a hash of the plugin's data and the value.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P plugin_undo_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# run(<tag> <frames> <script> [extra args...]): a fresh config ${OUT}/cfg_<tag>.
function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  # the channel strip shown (hidden whenever a window opens): the LFO strokes below use the layout with it
  set(script "1 strip on\n${script}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(REMOVE "${OUT}/${tag}.ppm")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
endfunction()
function(ok msg)
  message(STATUS "ok - ${msg}")
endfunction()

# plug(<prefix> <frame> <param>): <prefix>_pos (undo position), _size, _data (hash), _v (value).
function(plug prefix frame param)
  if(NOT out MATCHES "plug frame ${frame}: undo ([0-9]+) of ([0-9]+) undoLabel \"[^\"]*\" redoLabel \"[^\"]*\" data ([0-9a-f]+)\n  param ${param} ([-0-9.]+)")
    fail("no plug dump at frame ${frame}")
  endif()
  set(${prefix}_pos ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${prefix}_size ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${prefix}_data ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(${prefix}_v ${CMAKE_MATCH_4} PARENT_SCOPE)
endfunction()

# A drag of parameter <k>'s control from frame <f>: press, three moves up, release (frames f .. f+10).
function(drag var k f)
  math(EXPR f1 "${f} + 2")
  math(EXPR f2 "${f} + 4")
  math(EXPR f3 "${f} + 6")
  math(EXPR f4 "${f} + 8")
  math(EXPR f5 "${f} + 10")
  set(${var} "${f} atparam ${k}\n${f1} down 0\n${f2} atparam ${k} 0 -10\n${f3} atparam ${k} 0 -20\n${f4} atparam ${k} 0 -30\n${f5} up 0\n" PARENT_SCOPE)
endfunction()

# The knob drag / Ctrl+Z / Ctrl+Shift+Z / Ctrl+Z / Ctrl+Y script for parameter <k> (frames 10 .. 70).
function(knobscript var k)
  drag(d ${k} 12)
  set(${var} "10 plug ${k}
${d}26 plug ${k}
30 sdlkey z down ctrl
31 sdlkey z up
36 plug ${k}
40 sdlkey z down ctrl+shift
41 sdlkey z up
46 plug ${k}
50 sdlkey z down ctrl
51 sdlkey z up
56 plug ${k}
60 sdlkey y down ctrl
61 sdlkey y up
66 plug ${k}
" PARENT_SCOPE)
endfunction()

# Checks the knobscript output: one step, undo, redo (both chords).
function(checkknob what k)
  plug(b 10 ${k})
  plug(m 26 ${k})
  plug(u 36 ${k})
  plug(r 46 ${k})
  plug(u2 56 ${k})
  plug(y 66 ${k})
  math(EXPR want "${b_pos} + 1")
  if(m_v STREQUAL b_v OR NOT m_pos EQUAL want)
    fail("${what}: the knob drag should change the value as one undo step (${b_v} -> ${m_v}, position ${b_pos} -> ${m_pos})")
  endif()
  if(NOT u_v STREQUAL b_v OR NOT u_pos EQUAL b_pos)
    fail("${what}: Ctrl+Z should restore ${b_v} (got ${u_v}, position ${u_pos})")
  endif()
  if(NOT r_v STREQUAL m_v OR NOT r_pos EQUAL m_pos)
    fail("${what}: Ctrl+Shift+Z should redo to ${m_v} (got ${r_v})")
  endif()
  if(NOT u2_v STREQUAL b_v OR NOT y_v STREQUAL m_v)
    fail("${what}: Ctrl+Z then Ctrl+Y should undo and redo again (${u2_v}, ${y_v})")
  endif()
  ok("${what}: a knob drag is one undo step; Ctrl+Z undoes it, Ctrl+Shift+Z and Ctrl+Y redo it")
endfunction()

# ---- 1. Plasma: knob drag, the shortcuts ----
knobscript(ks 0)
run(plasma 72 "${ks}" --plugin gl.plasma)
checkknob("Plasma (main window)" 0)

# ---- 2. Plasma: two strokes on LFO 1's shape (the peak point of the default triangle) ----
run(lfo 80 "10 plug 0
12 move 923 736
14 down 0
16 move 923 745
18 move 923 752
20 move 923 760
22 up 0
26 plug 0
30 move 923 760
32 down 0
34 move 923 770
36 move 923 780
38 move 923 790
40 up 0
44 plug 0
48 sdlkey z down ctrl
49 sdlkey z up
54 plug 0
58 sdlkey z down ctrl+shift
59 sdlkey z up
64 plug 0
" --plugin gl.plasma)
plug(l0 10 0)
plug(l1 26 0)
plug(l2 44 0)
plug(l3 54 0)
plug(l4 64 0)
if(l1_data STREQUAL l0_data OR l2_data STREQUAL l1_data)
  fail("both strokes should change Plasma's LFO shape (data ${l0_data} -> ${l1_data} -> ${l2_data}): has the layout moved?")
endif()
math(EXPR want1 "${l0_pos} + 1")
math(EXPR want2 "${l0_pos} + 2")
if(NOT l1_pos EQUAL want1 OR NOT l2_pos EQUAL want2)
  fail("each stroke on the LFO shape should be one undo step (positions ${l0_pos} -> ${l1_pos} -> ${l2_pos})")
endif()
if(NOT l3_data STREQUAL l1_data)
  fail("Ctrl+Z should take back only the second stroke (data ${l3_data}, want ${l1_data})")
endif()
if(NOT l4_data STREQUAL l2_data)
  fail("Ctrl+Shift+Z should redo the second stroke (data ${l4_data}, want ${l2_data})")
endif()
ok("Plasma: each stroke on an LFO shape (plugin data) is one undo step; Ctrl+Z / Ctrl+Shift+Z")

# ---- 3. another pack's instrument and a mixer insert (and a wheel turn) ----
knobscript(ks 0)
run(wavetable 72 "${ks}" --plugin gl.wavetable)
checkknob("Wavetable (synths pack)" 0)
# The wheel run uses a fixed frame time (--fixed-dt 16, the clock the wheel pause is measured on),
# so it does not depend on how fast the machine draws: steps 6 frames (96 ms) apart are one turn;
# then, after Cmd+Z, two turns with a pause of 34 frames (544 ms) between them are two steps.
knobscript(ks 3)
run(delay 250 "${ks}70 atparam 3
72 wheel 1
78 wheel 1
84 wheel 1
90 plug 3
150 plug 3
152 sdlkey z down super
153 sdlkey z up
158 plug 3
160 wheel 1
166 wheel 1
200 wheel 1
240 plug 3
" --plugin gl.delay --fixed-dt 16)
checkknob("Delay (mixer insert)" 3)
plug(w0 66 3)
plug(w1 90 3)
plug(w2 150 3)
plug(w3 158 3)
math(EXPR want "${w0_pos} + 1")
if(w1_v STREQUAL w0_v OR NOT w2_pos EQUAL want)
  fail("a wheel turn of three steps on a knob should be one undo step (${w0_v} -> ${w1_v}, position ${w0_pos} -> ${w2_pos})")
endif()
if(NOT w3_v STREQUAL w0_v)
  fail("Cmd+Z should undo the whole wheel turn (${w2_v} -> ${w3_v}, want ${w0_v})")
endif()
plug(w4 240 3)
math(EXPR want "${w3_pos} + 2")
if(w4_v STREQUAL w3_v OR NOT w4_pos EQUAL want)
  fail("two wheel turns with a pause between them should be two undo steps (${w3_v} -> ${w4_v}, position ${w3_pos} -> ${w4_pos})")
endif()
ok("a wheel turn of several steps is one undo step (Cmd+Z takes it back); a pause starts another")

# ---- 4. Plasma in a window of its own, the keyboard there ----
drag(d 0 18)
run(detached 90 "6 popout channelsettings
14 viewports
16 plug 0
${d}32 plug 0
36 sdlkeyin channelsettings z down ctrl
37 sdlkeyin channelsettings z up
42 plug 0
46 sdlkeyin channelsettings z down ctrl+shift
47 sdlkeyin channelsettings z up
52 plug 0
56 sdlkeyin channelsettings z down super
57 sdlkeyin channelsettings z up
62 plug 0
" --plugin gl.plasma)
if(out MATCHES "available 0")
  string(REGEX MATCH "unavailable: [^\n]*" why "${out}")
  message(STATUS "SKIP: windows cannot be detached on this platform (${why})")
  return()
endif()
if(NOT out MATCHES "viewport [1-9][0-9]* window \"channelsettings\"")
  fail("Plasma's window should be detached into a window of its own")
endif()
plug(a 16 0)
plug(b 32 0)
plug(c 42 0)
plug(e 52 0)
plug(f 62 0)
math(EXPR want "${a_pos} + 1")
if(b_v STREQUAL a_v OR NOT b_pos EQUAL want)
  fail("detached: the knob drag should be one undo step (${a_v} -> ${b_v}, position ${a_pos} -> ${b_pos})")
endif()
if(NOT c_v STREQUAL a_v OR NOT e_v STREQUAL b_v OR NOT f_v STREQUAL a_v)
  fail("detached: Ctrl+Z / Ctrl+Shift+Z / Cmd+Z typed into Plasma's own window (${c_v} ${e_v} ${f_v}, want ${a_v} ${b_v} ${a_v})")
endif()
ok("Plasma in a window of its own: Ctrl+Z, Ctrl+Shift+Z and Cmd+Z typed there undo and redo its knob")
