# Gridline: Platter's platter under the mouse, driven like a user through the dev input hooks.
# Runs the app hidden (off-screen, isolated config, SDL's dummy audio driver: nothing shows,
# nothing sounds) with a Platter channel open and a track on each deck (GRIDLINE_PLATTER_DEMO /
# _DEMO_B), answers the controller question with "No controller", starts deck A, then:
#   1. drags the record round anti-clockwise: deck A plays backwards while the hand turns it;
#   2. drags the rim clockwise: the deck speeds up for a moment (a nudge), and still plays;
#   3. scrolls over the platter: a nudge too, and the editor does not scroll away;
#   4. screenshots with the pointer on the rim (its nudge ring lit): Dark and Light, 100% and 200%.
# The trace lines come from the editor's GRIDLINE_PLATTER_TRACE hook ("platter: state ... speed x",
# "platter: rect platterA x0 y0 x1 y1").
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_platter> -DOUT=<dir> -P platter_jog_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
file(MAKE_DIRECTORY "${OUT}/home")
execute_process(COMMAND "${TESTBIN}" writetrack "${OUT}/Deck A 120.wav" 120 RESULT_VARIABLE rc1)
execute_process(COMMAND "${TESTBIN}" writetrack "${OUT}/Deck B 100.wav" 100 RESULT_VARIABLE rc2)
if(NOT rc1 EQUAL 0 OR NOT rc2 EQUAL 0)
  message(FATAL_ERROR "FAIL: could not write the test tracks (${rc1} ${rc2})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_PLATTER_TRACE} "1")
set(ENV{GRIDLINE_PLATTER_DEMO} "${OUT}/Deck A 120.wav")
set(ENV{GRIDLINE_PLATTER_DEMO_B} "${OUT}/Deck B 100.wav")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(run tag frames script)
  # (a fresh config each run: a window one run saves would move the next one's controls)
  file(REMOVE_RECURSE "${OUT}/cfg" "${OUT}/home")
  file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}quit discard\n")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --plugin gl.platter --size 1700x1150 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${last_out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

macro(locate out name)
  string(REGEX MATCHALL "platter: rect ${name} [0-9-]+ [0-9-]+ [0-9-]+ [0-9-]+" all_ "${out}")
  set(last_ "")
  if(all_)
    list(GET all_ -1 last_)
  endif()
  string(REGEX MATCH "platter: rect ${name} ([0-9-]+) ([0-9-]+) ([0-9-]+) ([0-9-]+)" _ "${last_}")
  set(${name}_x0 "${CMAKE_MATCH_1}")
  set(${name}_x1 "${CMAKE_MATCH_3}")
  check("the ${name} control is shown" CMAKE_MATCH_1 MATCHES "[0-9]")
  math(EXPR ${name}_x "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
  math(EXPR ${name}_y "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
endmacro()

# A drag round the platter's centre (cx, cy) at radius r: `steps` points 30 degrees apart from the
# top, clockwise (dir 1) or anti-clockwise (dir -1), two frames each from frame f0.
set(cos_ 0 500 866 1000 866 500 0 -500 -866 -1000 -866 -500)
set(sin_ -1000 -866 -500 0 500 866 1000 866 500 0 -500 -866)
function(circle var f0 cx cy r dir steps)
  set(s "")
  set(f ${f0})
  math(EXPR x "${cx}")
  math(EXPR y "${cy} - ${r}")
  string(APPEND s "${f} move ${x} ${y}\n")
  math(EXPR f "${f} + 1")
  string(APPEND s "${f} down 0\n")
  foreach(k RANGE 1 ${steps})
    math(EXPR i "((${k} * ${dir}) % 12 + 12) % 12")
    list(GET cos_ ${i} c)
    list(GET sin_ ${i} sn)
    math(EXPR x "${cx} + ${r} * ${c} / 1000")
    math(EXPR y "${cy} + ${r} * ${sn} / 1000")
    math(EXPR f "${f} + 2")
    string(APPEND s "${f} move ${x} ${y}\n")
  endforeach()
  math(EXPR f "${f} + 2")
  string(APPEND s "${f} up 0\n")
  set(${var} "${s}" PARENT_SCOPE)
  set(${var}_end ${f} PARENT_SCOPE)
endfunction()

# ---- the decks, "No controller" (the song at deck A's 120 BPM: a synced deck A plays at 1x)
run(setup 30 "1 waitfor platter-steady 60\n")
set(last_out "${setup_out}")
locate("${setup_out}" none)
set(answer "2 waitfor platter-steady 60
3 tempo 120
8 move ${none_x} ${none_y}
10 down 0
11 up 0
12 waitfor platter-ready 300
")
run(decks 20 "${answer}")
set(last_out "${decks_out}")
check("\"No controller\" answers the question" decks_out MATCHES "platter: state ready layout 2 asked 1 mapped 0")
locate("${decks_out}" playA)
locate("${decks_out}" platterA)
math(EXPR R "(${platterA_x1} - ${platterA_x0}) / 2")
check("deck A's platter is drawn" R GREATER 40)
set(play "14 move ${playA_x} ${playA_y}
16 down 0
17 up 0
19 waitfor platter-playing 120
")

# ---- 1. the record turned back by hand plays backwards
math(EXPR rTop "${R} / 2")
circle(back 24 ${platterA_x} ${platterA_y} ${rTop} -1 24)
run(scratch 120 "${answer}${play}${back}")
set(last_out "${scratch_out}")
check("deck A plays" scratch_out MATCHES "A\\[bpm [0-9.]+ playing 1 speed 1\\.0")
check("the record turned anti-clockwise by the mouse plays backwards" scratch_out MATCHES "A\\[bpm [0-9.]+ playing 1 speed -[0-9]")

# ---- 2. the rim dragged clockwise nudges the speed up for a moment
math(EXPR rRim "${R} - 5")
circle(rim 24 ${platterA_x} ${platterA_y} ${rRim} 1 6)
run(rim 120 "${answer}${play}${rim}")
set(last_out "${rim_out}")
check("the rim dragged clockwise speeds the deck up for a moment" rim_out MATCHES "playing 1 speed 1\\.(0[2-9]|[1-4])")
check("... without scratching it (never backwards)" NOT rim_out MATCHES "playing 1 speed -")

# ---- 3. the scroll wheel over the platter nudges too
run(scroll 90 "${answer}${play}24 move ${platterA_x} ${platterA_y}
26 wheel 3
27 wheel 3
28 wheel 3
29 wheel 3
")
set(last_out "${scroll_out}")
check("scrolling over the platter speeds the deck up for a moment" scroll_out MATCHES "playing 1 speed 1\\.(0[1-9]|[1-4])")

# ---- 4. the nudge ring under the pointer: Dark, Light, 100% and 200%
math(EXPR rimY "${platterA_y} - ${R} + 5")
run(rim_dark 40 "${answer}24 move ${platterA_x} ${rimY}\n")
run(rim_light 40 "${answer}24 move ${platterA_x} ${rimY}\n" --light)
# (at 200% in a window twice the size, as a 200% screen has: the question and the platter are found again)
run(setup2 30 "1 waitfor platter-steady 60\n" --ui-scale 2 --size 3400x2300)
set(last_out "${setup2_out}")
locate("${setup2_out}" none)
set(answer2 "2 waitfor platter-steady 60
3 tempo 120
8 move ${none_x} ${none_y}
10 down 0
11 up 0
12 waitfor platter-ready 300
")
run(decks2 20 "${answer2}" --ui-scale 2 --size 3400x2300)
set(last_out "${decks2_out}")
locate("${decks2_out}" platterA)
math(EXPR R2 "(${platterA_x1} - ${platterA_x0}) / 2")
math(EXPR rimY2 "${platterA_y} - ${R2} + 8")
run(rim_dark2x 40 "${answer2}24 move ${platterA_x} ${rimY2}\n" --ui-scale 2 --size 3400x2300)
run(rim_light2x 40 "${answer2}24 move ${platterA_x} ${rimY2}\n" --ui-scale 2 --size 3400x2300 --light)

# ---- 5. every window layout: the platter scratches and nudges there too, and looks right
foreach(lay live logic tracker)
  run(setup_${lay} 30 "1 waitfor platter-steady 60\n" --layout ${lay})
  set(last_out "${setup_${lay}_out}")
  locate("${setup_${lay}_out}" none)
  set(answerL "2 waitfor platter-steady 60
3 tempo 120
8 move ${none_x} ${none_y}
10 down 0
11 up 0
12 waitfor platter-ready 300
")
  run(decks_${lay} 20 "${answerL}" --layout ${lay})
  set(last_out "${decks_${lay}_out}")
  locate("${decks_${lay}_out}" playA)
  locate("${decks_${lay}_out}" platterA)
  math(EXPR RL "(${platterA_x1} - ${platterA_x0}) / 2")
  set(playL "14 move ${playA_x} ${playA_y}
16 down 0
17 up 0
19 waitfor platter-playing 120
")
  math(EXPR rTopL "${RL} / 2")
  circle(backL 24 ${platterA_x} ${platterA_y} ${rTopL} -1 24)
  run(scratch_${lay} 120 "${answerL}${playL}${backL}" --layout ${lay})
  set(last_out "${scratch_${lay}_out}")
  check("${lay}: the record turned anti-clockwise plays backwards" scratch_${lay}_out MATCHES "A\\[bpm [0-9.]+ playing 1 speed -[0-9]")
  math(EXPR rRimL "${RL} - 5")
  circle(rimL 24 ${platterA_x} ${platterA_y} ${rRimL} 1 6)
  run(rim_${lay} 120 "${answerL}${playL}${rimL}" --layout ${lay})
  set(last_out "${rim_${lay}_out}")
  check("${lay}: the rim nudges the speed up" rim_${lay}_out MATCHES "playing 1 speed 1\\.(0[2-9]|[1-4])")
  math(EXPR rimYL "${platterA_y} - ${RL} + 5")
  run(rim_${lay}_dark 40 "${answerL}24 move ${platterA_x} ${rimYL}\n" --layout ${lay})
  run(rim_${lay}_light 40 "${answerL}24 move ${platterA_x} ${rimYL}\n" --layout ${lay} --light)
endforeach()

# ---- 6. the mapping drawer's encoding button (a controller profile with jog wheels): it shows
# the wheel's encoding, a click picks the next one; Dark and Light, 100% and 200%
set(ENV{GRIDLINE_PLATTER_PROFILE} "ddj200")
set(ENV{GRIDLINE_PLATTER_DRAWER} "1")
# The jog wheel's row sits low in the drawer's list: the editor and then the list are scrolled
# (about 70 units a wheel step, eased: the script waits for it to settle) until the row is near
# y 930, finding the button again after each scroll, then it is clicked.
macro(find_enc tag)
  string(REGEX MATCHALL "platter: rect encjogA([0-9]) [0-9-]+ [0-9-]+ [0-9-]+ [0-9-]+" encs "${${tag}_out}")
  set(last_out "${${tag}_out}")
  check("${tag}: the jog wheel's encoding button is shown" encs MATCHES "encjogA")
  list(GET encs -1 enc)
  string(REGEX MATCH "encjogA([0-9]) ([0-9-]+) ([0-9-]+) ([0-9-]+) ([0-9-]+)" _ "${enc}")
  set(enc0 "${CMAKE_MATCH_1}")
  math(EXPR ex "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
  math(EXPR ey "(${CMAKE_MATCH_3} + ${CMAKE_MATCH_5}) / 2")
endmacro()
foreach(sc 1 2)
  if(sc EQUAL 2)
    set(scl --ui-scale 2 --size 3400x2300)
    set(sfx 2x)
  else()
    set(scl "")
    set(sfx "")
  endif()
  run(map_find${sfx} 20 "1 waitfor platter-steady 60\n" ${scl})
  find_enc(map_find${sfx})
  set(scroll "2 waitfor platter-steady 60\n")
  set(f 3)
  foreach(round 1 2 3 4)
    if(ey GREATER 880 AND ey LESS 985)
      break()
    endif()
    math(EXPR steps "(${ey} - 930 + 35) / 70")
    math(EXPR px "${ex} - 300")
    math(EXPR f1 "${f} + 2")
    math(EXPR f2 "${f} + 3")
    math(EXPR f3 "${f} + 4")
    math(EXPR f4 "${f} + 5")
    string(APPEND scroll "${f} move ${px} 960\n${f1} wheel -${steps}\n${f2} waitfor never 2\n${f3} forget platter-steady\n${f4} waitfor platter-steady 60\n")
    math(EXPR f "${f} + 6")
    run(map_at${sfx} 20 "${scroll}" ${scl})
    find_enc(map_at${sfx})
  endforeach()
  check("${sfx} the scrolled list shows the button (y ${ey})" ey GREATER 880 AND ey LESS 985)
  math(EXPR f1 "${f} + 2")
  math(EXPR f2 "${f} + 3")
  run(map_click${sfx} 30 "${scroll}${f} move ${ex} ${ey}\n${f1} down 0\n${f2} up 0\n" ${scl})
  set(last_out "${map_click${sfx}_out}")
  math(EXPR enc1 "${enc0} % 3 + 1")
  check("${sfx} a click on it picks the next encoding (${enc0} to ${enc1})" map_click${sfx}_out MATCHES "platter: rect encjogA${enc1} ")
  run(map_dark${sfx} 20 "${scroll}${f} move ${ex} ${ey}\n" ${scl})
  run(map_light${sfx} 20 "${scroll}${f} move ${ex} ${ey}\n" ${scl} --light)
endforeach()
unset(ENV{GRIDLINE_PLATTER_PROFILE})
unset(ENV{GRIDLINE_PLATTER_DRAWER})

foreach(f decks scratch rim scroll rim_dark rim_light rim_dark2x rim_light2x rim_live_dark rim_live_light rim_logic_dark rim_logic_light
          rim_tracker_dark rim_tracker_light map_dark map_light map_dark2x map_light2x)
  check("screenshot ${f}.ppm written" EXISTS "${OUT}/${f}.ppm")
endforeach()
