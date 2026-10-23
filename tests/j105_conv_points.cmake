# Gridline - J-105 user report: "convolver eq drawing: i don't like how i can click drag and it just
# paints, i like having to click and add points". Opens the Convolver (hidden, isolated config) at
# 100 % and 200 % interface scale and edits its IR EQ and IR envelope with real SDL mouse events:
#  * a click-drag on empty space adds exactly one point and moves it (no painting);
#  * a click without moving adds one point;
#  * a drag that starts on a point moves it (grab offset kept, clamped between its neighbours and
#    to the graph) and never adds points; a release outside the graph ends the drag;
#  * a double-click on a point deletes it; every gesture is one undo step;
#  * the points survive save + load.
# J-106: curves are Smooth by default; one click on the curve-mode button switches to Straight (one
# undo step), Alt-drag bends only Straight segments, switching back keeps the tensions, and the mode
# survives save + load.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j105_conv_points.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")

# "pts" prints: pts frame F: <name> n N drag D hot H undo U, then "  pt i x X v V k K" (x as a fraction of the span).
function(pt out frame name i x v)
  if(NOT out MATCHES "pts frame ${frame}: ${name} [^\n]*\n(  pt [^\n]*\n)*  pt ${i} x ([0-9.]+) v ([0-9.]+)")
    message(FATAL_ERROR "FAIL (${tag}): frame ${frame}: no point ${i}. Output:\n${out}")
  endif()
  set(px "${CMAKE_MATCH_2}")
  set(pv "${CMAKE_MATCH_3}")
  # Compared in thousandths, within 15 (1.5 % of the span / value range: pixel rounding at 100 %).
  string(REGEX MATCH "^([0-9]+)\\.([0-9][0-9][0-9])" _m "${px}")
  math(EXPR ix "${CMAKE_MATCH_1} * 1000 + 1${CMAKE_MATCH_2} - 1000")
  string(REGEX MATCH "^([0-9]+)\\.([0-9][0-9][0-9])" _m "${pv}")
  math(EXPR iv "${CMAKE_MATCH_1} * 1000 + 1${CMAKE_MATCH_2} - 1000")
  math(EXPR dx "${ix} - ${x}")
  math(EXPR dv "${iv} - ${v}")
  if(dx LESS -15 OR dx GREATER 15 OR dv LESS -15 OR dv GREATER 15)
    message(FATAL_ERROR "FAIL (${tag}): frame ${frame}: ${name} point ${i} at x ${px} v ${pv}, expected x ${x} v ${v} (thousandths). Output:\n${out}")
  endif()
endfunction()
function(count out frame name n drag undo)
  if(NOT out MATCHES "pts frame ${frame}: ${name} n ${n} drag ${drag} hot -?[0-9]+ undo ${undo}\n")
    message(FATAL_ERROR "FAIL (${tag}): frame ${frame}: expected ${name} n ${n} drag ${drag} undo ${undo}. Output:\n${out}")
  endif()
endfunction()

# The EQ: the view tab, then the gestures. U = undo position before the first edit.
set(eq "5 toasts clear
8 sdlpt conv.tabeq 0.5 0.5
10 sdldown 0
12 sdlup 0
16 pts conv.eq
17 pts conv.curvemode
20 sdlpt conv.eq 0.30 0.50
22 sdldown 0
24 sdlpt conv.eq 0.35 0.60
26 sdlpt conv.eq 0.40 0.70
28 sdlpt conv.eq 0.45 0.75
30 pts conv.eq
32 sdlup 0
34 pts conv.eq
40 sdlpt conv.eq 0.70 0.30
42 sdldown 0
44 sdlup 0
48 pts conv.eq
60 sdlpt conv.eq pt 1 3 -2
62 sdldown 0
64 sdlpt conv.eq 0.55 0.60 3 -2
66 pts conv.eq
68 sdlpt conv.eq 0.95 0.60 3 -2
70 pts conv.eq
72 sdlpt conv.eq 0.50 1.60 3 -2
74 pts conv.eq
76 sdlpt conv.eq 0.50 0.60 3 -2
78 sdlup 0
80 pts conv.eq
84 undo
88 pts conv.eq
100 sdlpt conv.eq pt 2
102 sdldown 0
104 sdlup 0
106 sdldown 0
108 sdlup 0
112 pts conv.eq
120 sdlpt conv.eq pt 1
122 sdldown 0
124 sdlpt conv.eq 0.50 -0.9
126 sdlup 0
128 sdlpt conv.eq 0.20 0.20
132 pts conv.eq
133 pts conv.curvemode
136 mod alt 1
138 sdlpt conv.eq 0.75 0.25
140 sdldown 0
142 sdlpt conv.eq 0.75 0.25 0 -30
144 sdlup 0
146 mod alt 0
150 pts conv.eq
152 sdlpt conv.curvemode 0.5 0.5
154 sdldown 0
156 sdlup 0
160 pts conv.curvemode
164 mod alt 1
166 sdlpt conv.eq 0.75 0.25
168 sdldown 0
170 sdlpt conv.eq 0.75 0.25 0 -30
172 sdlup 0
174 mod alt 0
178 pts conv.eq
180 sdlpt conv.curvemode 0.5 0.5
182 sdldown 0
184 sdlup 0
188 pts conv.curvemode
189 pts conv.eq
")
# The IR envelope (ENVELOPE on): add by click-drag, drag a point, double-click delete, undo.
set(env "5 toasts clear
16 pts conv.env
17 pts conv.curvemode
20 sdlpt conv.env 0.30 0.50
22 sdldown 0
24 sdlpt conv.env 0.32 0.60
26 sdlpt conv.env 0.35 0.80
28 pts conv.env
30 sdlup 0
32 pts conv.env
40 sdlpt conv.env 0.60 0.30
42 sdldown 0
44 sdlup 0
48 pts conv.env
60 sdlpt conv.env pt 2 -2 3
62 sdldown 0
64 sdlpt conv.env 0.50 0.40 -2 3
66 sdlpt conv.env 0.20 0.40 -2 3
68 pts conv.env
70 sdlup 0
74 pts conv.env
78 undo
82 pts conv.env
100 sdlpt conv.env pt 1
102 sdldown 0
104 sdlup 0
106 sdldown 0
108 sdlup 0
112 pts conv.env
")
file(WRITE "${OUT}/eq.txt" "${eq}")
file(WRITE "${OUT}/env.txt" "${env}")

foreach(scale 1 2)
  if(scale STREQUAL "1")
    set(size 1600x1000)
  else()
    set(size 3200x2000)
  endif()
  # ---- EQ
  set(tag "eq x${scale}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.convolver --size ${size} --ui-scale ${scale} --frames 194
                          --screenshot "${OUT}/eq${scale}.ppm" --dev-input "${OUT}/eq.txt" --save-at-exit "${OUT}/eq${scale}.gridline.json"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  if(out MATCHES "not shown")
    message(FATAL_ERROR "FAIL (${tag}): the Convolver EQ is not on screen. Output:\n${out}")
  endif()
  if(NOT out MATCHES "pts frame 16: conv.eq n 0 drag -1 hot -?[0-9]+ undo ([0-9]+)")
    message(FATAL_ERROR "FAIL (${tag}): the EQ does not start flat. Output:\n${out}")
  endif()
  set(u0 ${CMAKE_MATCH_1})
  math(EXPR u1 "${u0} + 1")
  math(EXPR u2 "${u0} + 2")
  math(EXPR u3 "${u0} + 3")
  # Click-drag on a flat curve: its two ends plus exactly one new point, which follows the pointer.
  count("${out}" 30 conv.eq 3 1 ${u1})
  pt("${out}" 30 conv.eq 1 450 750)
  count("${out}" 34 conv.eq 3 -1 ${u1})
  # A click adds one point (one undo step).
  count("${out}" 48 conv.eq 4 -1 ${u2})
  pt("${out}" 48 conv.eq 2 700 300)
  # A drag that starts on a point moves it (offset kept), adds nothing, stops at its neighbour and the top.
  count("${out}" 66 conv.eq 4 1 ${u3})
  pt("${out}" 66 conv.eq 1 550 600)
  pt("${out}" 70 conv.eq 1 700 600)
  pt("${out}" 74 conv.eq 1 500 1000)
  count("${out}" 80 conv.eq 4 -1 ${u3})
  pt("${out}" 80 conv.eq 1 500 600)
  # Undo takes the whole drag back in one step.
  count("${out}" 88 conv.eq 4 -1 ${u2})
  pt("${out}" 88 conv.eq 1 450 750)
  # Double-click deletes the point; the release outside the graph ends the drag (nothing follows the pointer after).
  count("${out}" 112 conv.eq 3 -1 ${u3})
  pt("${out}" 112 conv.eq 2 1000 500)
  if(NOT out MATCHES "pts frame 132: conv.eq n 3 drag -1")
    message(FATAL_ERROR "FAIL (${tag}): the drag released outside the graph is still running. Output:\n${out}")
  endif()
  pt("${out}" 132 conv.eq 1 500 0)
  # J-106: the curve is Smooth from the start; Alt-drag on a Smooth curve neither bends nor adds.
  if(NOT out MATCHES "pts frame 17: conv.curvemode n 1 [^\n]*\n  pt 0 x 1.0000" OR NOT out MATCHES "pts frame 133: conv.curvemode n 1 [^\n]*\n  pt 0 x 1.0000")
    message(FATAL_ERROR "FAIL (${tag}): a new EQ curve is not Smooth. Output:\n${out}")
  endif()
  if(NOT out MATCHES "pts frame 132: conv.eq n 3 drag -1 hot -?[0-9]+ undo ([0-9]+)")
    message(FATAL_ERROR "FAIL (${tag}): no EQ state at frame 132. Output:\n${out}")
  endif()
  set(ua ${CMAKE_MATCH_1})
  math(EXPR ub "${ua} + 1")
  math(EXPR uc "${ua} + 2")
  math(EXPR ud "${ua} + 3")
  if(NOT out MATCHES "pts frame 150: conv.eq n 3 drag -1 hot -?[0-9]+ undo ${ua}\n  pt 0 [^\n]*\n  pt 1 [^\n]*\n  pt 2 x 1.0000 v 0.5000 k 0.0000")
    message(FATAL_ERROR "FAIL (${tag}): Alt-drag changed a Smooth curve. Output:\n${out}")
  endif()
  # One click on the button: Straight, one undo step.
  if(NOT out MATCHES "pts frame 160: conv.curvemode n 1 drag -1 hot -?[0-9]+ undo ${ub}\n  pt 0 x 0.0000")
    message(FATAL_ERROR "FAIL (${tag}): the curve-mode button did not switch to Straight in one undo step. Output:\n${out}")
  endif()
  # Alt-drag on a Straight segment bends it (the end point's tension) and adds no point.
  if(NOT out MATCHES "pts frame 178: conv.eq n 3 drag -1 hot -?[0-9]+ undo ${uc}\n  pt 0 [^\n]*\n  pt 1 [^\n]*\n  pt 2 x 1.0000 v 0.5000 k (-?0\\.[0-9]*[1-9][0-9]*)")
    message(FATAL_ERROR "FAIL (${tag}): Alt-drag did not bend the Straight segment. Output:\n${out}")
  endif()
  set(bent "${CMAKE_MATCH_1}")
  # Back to Smooth (one more step); the tension is kept for Straight.
  if(NOT out MATCHES "pts frame 188: conv.curvemode n 1 drag -1 hot -?[0-9]+ undo ${ud}\n  pt 0 x 1.0000" OR
     NOT out MATCHES "pts frame 189: conv.eq n 3 [^\n]*\n  pt 0 [^\n]*\n  pt 1 [^\n]*\n  pt 2 x 1.0000 v 0.5000 k ${bent}\n")
    message(FATAL_ERROR "FAIL (${tag}): switching back to Smooth failed or lost the tension. Output:\n${out}")
  endif()
  message(STATUS "ok - ${tag}: click-drag adds one point, click adds one, drags on points add none, clamps, undo per gesture, double-click deletes; Smooth by default, the button toggles in one undo step, Alt-drag bends Straight only")

  # ---- Save + load: the saved project shows the same points.
  set(tag "eq reload x${scale}")
  file(WRITE "${OUT}/reload.txt" "5 sdlpt conv.tabeq 0.5 0.5\n7 sdldown 0\n9 sdlup 0\n14 pts conv.eq\n15 pts conv.curvemode\n")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 "${OUT}/eq${scale}.gridline.json" --fx 1 9 --size ${size} --ui-scale ${scale}
                          --frames 18 --screenshot "${OUT}/reload${scale}.ppm" --dev-input "${OUT}/reload.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out2 ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  string(REGEX MATCH "pts frame 189: conv.eq n 3[^\n]*\n  pt 0 ([^\n]*)\n  pt 1 ([^\n]*)\n  pt 2 ([^\n]*)\n" before "${out}")
  set(b0 "${CMAKE_MATCH_1}")
  set(b1 "${CMAKE_MATCH_2}")
  set(b2 "${CMAKE_MATCH_3}")
  if(NOT out2 MATCHES "pts frame 14: conv.eq n 3[^\n]*\n  pt 0 ([^\n]*)\n  pt 1 ([^\n]*)\n  pt 2 ([^\n]*)\n" OR NOT CMAKE_MATCH_1 STREQUAL b0
     OR NOT CMAKE_MATCH_2 STREQUAL b1 OR NOT CMAKE_MATCH_3 STREQUAL b2)
    message(FATAL_ERROR "FAIL (${tag}): the EQ points differ after save + load.\nBefore:\n${out}\nAfter:\n${out2}")
  endif()
  # A curve with points and no stored mode would show Straight: Smooth here is the saved mode.
  if(NOT out2 MATCHES "pts frame 15: conv.curvemode n 1 [^\n]*\n  pt 0 x 1.0000")
    message(FATAL_ERROR "FAIL (${tag}): the Smooth mode did not survive save + load. Output:\n${out2}")
  endif()
  message(STATUS "ok - ${tag}: the EQ points and the curve mode round-trip through the project file")

  # ---- IR envelope
  set(tag "env x${scale}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.convolver --param 23 1 --size ${size} --ui-scale ${scale}
                          --frames 116 --screenshot "${OUT}/env${scale}.ppm" --dev-input "${OUT}/env.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  if(NOT out MATCHES "pts frame 16: conv.env n 0 drag -1 hot -?[0-9]+ undo ([0-9]+)")
    message(FATAL_ERROR "FAIL (${tag}): the IR envelope is not shown flat. Output:\n${out}")
  endif()
  set(u0 ${CMAKE_MATCH_1})
  if(NOT out MATCHES "pts frame 17: conv.curvemode n 1 [^\n]*\n  pt 0 x 1.0000")
    message(FATAL_ERROR "FAIL (${tag}): a new IR envelope is not Smooth. Output:\n${out}")
  endif()
  math(EXPR u1 "${u0} + 1")
  math(EXPR u2 "${u0} + 2")
  math(EXPR u3 "${u0} + 3")
  count("${out}" 28 conv.env 3 1 ${u1})
  pt("${out}" 28 conv.env 1 350 800)
  count("${out}" 32 conv.env 3 -1 ${u1})
  count("${out}" 48 conv.env 4 -1 ${u2})
  pt("${out}" 48 conv.env 2 600 300)
  # Point 2 dragged left stops at point 1 (0.35); nothing is added.
  count("${out}" 68 conv.env 4 2 ${u3})
  pt("${out}" 68 conv.env 2 350 400)
  count("${out}" 74 conv.env 4 -1 ${u3})
  count("${out}" 82 conv.env 4 -1 ${u2})
  pt("${out}" 82 conv.env 2 600 300)
  count("${out}" 112 conv.env 3 -1 ${u3})
  pt("${out}" 112 conv.env 1 600 300)
  message(STATUS "ok - ${tag}: envelope points: click-drag adds one, drag moves without adding, clamps, undo per gesture, double-click deletes")
endforeach()
