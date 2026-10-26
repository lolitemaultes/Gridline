# Gridline - the channel strip of a plugin window (volume, pan, pitch and the preview keyboard at
# the top of the channel settings window, app/ChannelStrip.h), in the real app (hidden, isolated
# config, off-screen at 1440x900, SDL's dummy audio driver, a fixed 16 ms frame time so the slide
# is the same on every machine). The demo copy opens with channel 1's window (short enough to
# grow by the whole strip):
#   1. a fresh config: the strip is hidden (nothing of it drawn);
#   2. the header's eye shows it: it slides down over several frames (eased, never back, its full
#      height fixed from the first frame), lands fully shown, and the window then keeps its size
#      frame after frame, taller by the strip's height, the content under it lower by as much;
#   3. ended, started again with the same config: hidden at once (the eye's choice is never
#      saved, settings.json has no key for it);
#   4. shown with the eye, the preview keyboard plays a note while held and the volume knob turns;
#   5. the eye hides it again: it slides back up and the window returns to its hidden size;
#   6. shown, the window closed and opened again in the same session: hidden at once.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P plugin_strip.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x (CMP0054: a quoted "keep" is that string, never a variable)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_REDUCED_MOTION} "0")  # the slide is on whatever the machine's accessibility setting
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# run(<tag> <frames> <script>): the same config every run (nothing of the strip may survive a restart).
function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --chan 1
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
function(ok msg)
  message(STATUS "ok - ${msg}")
endfunction()

# strip(<prefix> <frame>): the "strip" dump of <frame>: <prefix>_on _frac _moving _shown _full
# _drawn (frac a decimal, heights rounded down), the eye's centre _ex _ey, the window's height _wh;
# when drawn, the keyboard's top-left _kx _ky, its key width _kw and the volume knob's centre _vx _vy.
function(strip prefix frame)
  set(n "[-0-9.]+")
  # (CMake keeps at most 9 groups per match: two matches on the same line.)
  if(NOT out MATCHES "strip frame ${frame}: on ([01]) frac (${n}) moving ([01]) shown ([0-9]+)[.0-9]* full ([0-9]+)[.0-9]* drawn ([01]) eye ([^\n]*)")
    fail("no strip dump at frame ${frame}")
  endif()
  set(${prefix}_on ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${prefix}_frac ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${prefix}_moving ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(${prefix}_shown ${CMAKE_MATCH_4} PARENT_SCOPE)
  set(${prefix}_full ${CMAKE_MATCH_5} PARENT_SCOPE)
  set(${prefix}_drawn ${CMAKE_MATCH_6} PARENT_SCOPE)
  set(rest "${CMAKE_MATCH_7}")
  if(NOT rest MATCHES "^(${n}) (${n}) (${n}) (${n}) window ${n} ${n} ${n} ([0-9]+)[.0-9]* below ([-0-9]+)")
    fail("strip dump at frame ${frame}: no eye / window rect")
  endif()
  set(${prefix}_below ${CMAKE_MATCH_6} PARENT_SCOPE)
  set(ex0 ${CMAKE_MATCH_1})
  set(ey0 ${CMAKE_MATCH_2})
  set(ex1 ${CMAKE_MATCH_3})
  set(ey1 ${CMAKE_MATCH_4})
  set(${prefix}_wh ${CMAKE_MATCH_5} PARENT_SCOPE)
  string(REGEX REPLACE "\\..*" "" ex0 "${ex0}")
  string(REGEX REPLACE "\\..*" "" ey0 "${ey0}")
  string(REGEX REPLACE "\\..*" "" ex1 "${ex1}")
  string(REGEX REPLACE "\\..*" "" ey1 "${ey1}")
  math(EXPR ex "(${ex0} + ${ex1}) / 2")
  math(EXPR ey "(${ey0} + ${ey1}) / 2")
  set(${prefix}_ex ${ex} PARENT_SCOPE)
  set(${prefix}_ey ${ey} PARENT_SCOPE)
  if(out MATCHES "strip frame ${frame}: [^\n]*\n  clip [^\n]* keys ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* ${n} ${n} keyw ([0-9]+)[.0-9]* vol ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* ([0-9]+)")
    set(${prefix}_kx ${CMAKE_MATCH_1} PARENT_SCOPE)
    set(${prefix}_ky ${CMAKE_MATCH_2} PARENT_SCOPE)
    set(${prefix}_kw ${CMAKE_MATCH_3} PARENT_SCOPE)
    math(EXPR vx "(${CMAKE_MATCH_4} + ${CMAKE_MATCH_6}) / 2")
    math(EXPR vy "(${CMAKE_MATCH_5} + ${CMAKE_MATCH_7}) / 2")
    set(${prefix}_vx ${vx} PARENT_SCOPE)
    set(${prefix}_vy ${vy} PARENT_SCOPE)
  endif()
endfunction()

# slide(<from> <to> <dir>): frames from..to of a slide (dir up: showing, down: hiding). Sets
# slide_mid (frames strictly between hidden and shown), slide_end (first frame at rest) and checks
# that the shown fraction only ever moves one way and the strip's full height never changes (no
# jump when its content is first measured).
function(slide from to dir)
  set(prev "")
  set(mid 0)
  set(endf "")
  foreach(f RANGE ${from} ${to})
    strip(s ${f})
    if(NOT prev STREQUAL "")
      if(dir STREQUAL "up" AND s_frac LESS prev)
        fail("frame ${f}: the strip went back up while showing (${s_frac} after ${prev})")
      elseif(dir STREQUAL "down" AND s_frac GREATER prev)
        fail("frame ${f}: the strip came back down while hiding (${s_frac} after ${prev})")
      endif()
    endif()
    set(prev ${s_frac})
    if(NOT DEFINED full0)
      set(full0 ${s_full})
    elseif(NOT s_full EQUAL full0)
      fail("frame ${f}: the strip's full height changed mid-slide (${full0} -> ${s_full})")
    endif()
    if(s_frac GREATER 0 AND s_frac LESS 1)
      math(EXPR mid "${mid} + 1")
      if(NOT s_moving EQUAL 1)
        fail("frame ${f}: part-way (${s_frac}) but not moving")
      endif()
    elseif(endf STREQUAL "" AND s_moving EQUAL 0 AND ((dir STREQUAL "up" AND s_frac EQUAL 1) OR (dir STREQUAL "down" AND s_frac EQUAL 0)))
      set(endf ${f})
    endif()
  endforeach()
  set(slide_mid ${mid} PARENT_SCOPE)
  set(slide_end "${endf}" PARENT_SCOPE)
endfunction()

# ---- 1. hidden by default; where the eye, keyboard and knob are ----
run(geo 14 "8 strip\n")
strip(h 8)
if(NOT h_on EQUAL 0 OR NOT h_frac EQUAL 0 OR NOT h_drawn EQUAL 0 OR NOT h_shown EQUAL 0)
  fail("a fresh config should start with the strip hidden (on ${h_on} frac ${h_frac} drawn ${h_drawn} shown ${h_shown})")
endif()
ok("hidden by default (nothing of the strip drawn), window ${h_wh} high")

# ---- 2. the eye shows it with a slide; the layout is stable after it ----
math(EXPR clickDown "20")
math(EXPR clickUp "21")
run(show 110 "8 strip
18 move ${h_ex} ${h_ey}
${clickDown} down 0
${clickUp} up 0
${clickUp} strip 30
60 strip
")
slide(${clickUp} 51 up)
if(slide_mid LESS 4)
  fail("the strip should slide in over several frames (${slide_mid} part-way frames at 16 ms)")
endif()
if(slide_end STREQUAL "")
  fail("the strip never came to rest fully shown")
endif()
math(EXPR took "(${slide_end} - ${clickUp}) * 16")
if(took LESS 96 OR took GREATER 224)
  fail("the slide took ${took} ms (about 140 ms expected)")
endif()
ok("the eye shows the strip: ${slide_mid} part-way frames, at rest after ${took} ms")
strip(s0 ${slide_end})
if(NOT s0_on EQUAL 1 OR NOT s0_drawn EQUAL 1 OR NOT s0_shown EQUAL s0_full)
  fail("shown: on ${s0_on}, drawn ${s0_drawn}, shown ${s0_shown} of ${s0_full}")
endif()
# The window follows the strip a frame later (auto-sized): from 2 frames after rest it holds still.
math(EXPR settle "${slide_end} + 2")
strip(s1 ${settle})
foreach(f RANGE ${settle} 51)
  strip(s ${f})
  if(NOT s_wh EQUAL s1_wh OR NOT s_shown EQUAL s1_shown)
    fail("frame ${f}: the window is still changing after the slide (height ${s_wh} vs ${s1_wh}, strip ${s_shown} vs ${s1_shown})")
  endif()
endforeach()
strip(s2 60)
if(NOT s2_wh EQUAL s1_wh)
  fail("frame 60: window height ${s2_wh}, ${s1_wh} after the slide")
endif()
# What is under the strip moved down by exactly its height, and the window grew by as much.
math(EXPR moved "${s1_below} - ${h_below}")
if(NOT moved EQUAL s0_full)
  fail("the content under the strip moved by ${moved}, the strip is ${s0_full} high")
endif()
math(EXPR grew "${s1_wh} - ${h_wh}")
math(EXPR diff "${grew} - ${s0_full}")
if(diff LESS -2 OR diff GREATER 2)
  fail("the window grew by ${grew}, the strip is ${s0_full} high")
endif()
ok("after the slide the window holds its size (${s1_wh}, ${grew} taller: the strip's height)")

# ---- 3. not remembered: after a restart it is hidden again (no slide either way) ----
run(again 40 "2 strip 10\n30 strip\n")
foreach(f RANGE 2 12)
  strip(r ${f})
  if(NOT r_on EQUAL 0 OR NOT r_frac EQUAL 0 OR NOT r_moving EQUAL 0 OR NOT r_drawn EQUAL 0)
    fail("frame ${f} after a restart: the strip should be hidden (shown ${r_on} frac ${r_frac} moving ${r_moving} drawn ${r_drawn})")
  endif()
endforeach()
strip(r 30)
if(NOT r_drawn EQUAL 0 OR NOT r_wh EQUAL h_wh)
  fail("after a restart: drawn ${r_drawn}, window ${r_wh} (${h_wh} hidden)")
endif()
if(EXISTS "${OUT}/cfg/gridline/settings.json")
  file(READ "${OUT}/cfg/gridline/settings.json" st)
  if(st MATCHES "ui.channelStrip")
    fail("the strip's state should not be saved with the settings:\n${st}")
  endif()
endif()
ok("after a restart the strip is hidden again (shown last time, not saved)")

# ---- 4. shown with the eye, the keyboard and the volume knob work as before ----
math(EXPR keyX "${s1_kx} + ${s1_kw} / 2")  # the first white key (C3), below the black keys
math(EXPR keyY "${s1_ky} + 40")
math(EXPR knobUp "${s1_vy} - 40")
run(play 120 "18 move ${h_ex} ${h_ey}
20 down 0
21 up 0
60 strip
62 move ${keyX} ${keyY}
64 down 0
72 sound
74 up 0
82 sound
84 rack
86 move ${s1_vx} ${s1_vy}
88 down 0
90 move ${s1_vx} ${s1_vy}
92 move ${s1_vx} ${knobUp}
94 move ${s1_vx} ${knobUp}
96 up 0
100 rack
110 quit discard
")
strip(p 60)
if(NOT p_on EQUAL 1 OR NOT p_drawn EQUAL 1 OR NOT p_wh EQUAL s1_wh)
  fail("shown with the eye: shown ${p_on}, drawn ${p_drawn}, window ${p_wh} (${s1_wh} before)")
endif()
if(NOT out MATCHES "sound frame 72: [^\n]* live 1")
  fail("holding a key of the preview keyboard should play a note")
endif()
if(NOT out MATCHES "sound frame 82: [^\n]* live 0")
  fail("letting go of the key should end the note")
endif()
ok("the preview keyboard plays while a key is held")
string(REGEX MATCH "rack frame 84:[^\n]*\n(  [^\n]*\n)*  channel 1 volume ([-0-9.]+)" _ "${out}")
set(vol0 "${CMAKE_MATCH_2}")
string(REGEX MATCH "rack frame 100:[^\n]*\n(  [^\n]*\n)*  channel 1 volume ([-0-9.]+)" _ "${out}")
set(vol1 "${CMAKE_MATCH_2}")
if(vol0 STREQUAL "" OR vol1 STREQUAL "" OR vol0 STREQUAL vol1)
  fail("dragging the strip's volume knob should change the channel's volume (${vol0} -> ${vol1})")
endif()
ok("the volume knob turns (${vol0} -> ${vol1})")

# ---- 5. the eye hides it: slides up, the window returns to its hidden size ----
run(hide 110 "18 move ${h_ex} ${h_ey}
20 down 0
21 up 0
60 strip
62 move ${s1_ex} ${s1_ey}
64 down 0
65 up 0
65 strip 30
100 strip
")
strip(sh 60)
if(NOT sh_on EQUAL 1 OR NOT sh_drawn EQUAL 1)
  fail("shown with the eye before hiding it (shown ${sh_on}, drawn ${sh_drawn})")
endif()
slide(65 95 down)
if(slide_mid LESS 4 OR slide_end STREQUAL "")
  fail("hiding should slide up over several frames and come to rest (${slide_mid} part-way frames, rest at ${slide_end})")
endif()
strip(e 100)
if(NOT e_on EQUAL 0 OR NOT e_drawn EQUAL 0 OR NOT e_wh EQUAL h_wh)
  fail("hidden again: shown ${e_on}, drawn ${e_drawn}, window ${e_wh} (${h_wh} hidden at first)")
endif()
ok("the eye hides it again: slides up, the window is back to ${e_wh}")

# ---- 6. closed and opened again in the same session: hidden again ----
run(reopen 110 "18 move ${h_ex} ${h_ey}
20 down 0
21 up 0
60 strip
62 plugwin close
64 strip
70 plugwin chan 1
72 strip 10
")
strip(c 60)
if(NOT c_on EQUAL 1 OR NOT c_drawn EQUAL 1)
  fail("shown with the eye before the window is closed (shown ${c_on}, drawn ${c_drawn})")
endif()
if(NOT out MATCHES "strip frame 64: window closed")
  fail("plugwin close should close the window")
endif()
foreach(f RANGE 72 82)
  strip(o ${f})
  if(NOT o_on EQUAL 0 OR NOT o_frac EQUAL 0 OR NOT o_moving EQUAL 0 OR NOT o_drawn EQUAL 0)
    fail("frame ${f}, the window opened again: the strip should be hidden at once (shown ${o_on} frac ${o_frac} moving ${o_moving} drawn ${o_drawn})")
  endif()
endforeach()
ok("closed and opened again: the strip is hidden (no slide)")
