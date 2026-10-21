# Gridline — J-84 / Q-15: the channel rack with long patterns and narrow racks.
# Runs the app hidden (off-screen, isolated config) with the default layout's
# floating rack resized by --rack and scripts input over it:
#   layout: pan / volume knobs drawn on every row at 16 / 32 / 64 steps,
#     narrow and wide, floating and docked, UI scale 1 / 1.5 / 2; long
#     patterns get a horizontal step scroll range;
#   narrow rack, 64 steps (the steps scroll): the plain wheel over an active
#     step scrolls the steps and leaves its velocity; Ctrl+wheel changes it;
#     a click on a step (it turns on) gives it the wheel; a sideways swipe
#     scrolls the steps;
#   wide rack, 16 steps (nothing scrolls): the wheel changes the velocity as before;
#   short rack (the channel list scrolls): the wheel over a volume knob scrolls
#     the list, Ctrl+wheel changes the volume;
#   keyboard lane in a scrolling rack: the plain wheel leaves the step's note,
#     Ctrl+wheel moves it.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j84_rack_scroll.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag spec script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames 34 --rack ${spec}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# Field <key> of the n-th (0-based) "rack" dump of a run -> <tag>_<key>.
function(field tag n key)
  string(REGEX MATCHALL "rack frame [^\n]*\n  geom [^\n]*\n  channel 0 [^\n]*" dumps "${${tag}_out}")
  list(LENGTH dumps count)
  if(count LESS_EQUAL n)
    message(FATAL_ERROR "FAIL (${tag}): no rack dump ${n}. Output:\n${${tag}_out}")
  endif()
  list(GET dumps ${n} d)
  string(REGEX MATCH " ${key} ([-0-9.]+)" _ "${d}")
  set(${tag}_${key} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# ---- layout: knobs on every row, at every length / width / scale ----
foreach(cfg "steps16" "steps32" "steps64" "steps16,w420" "steps32,w420" "steps64,w420" "steps64,w1300" "steps16,dock" "steps64,dock")
  string(REPLACE "," "_" tag "L_${cfg}")
  run(${tag} ${cfg} "30 rack\n")
  field(${tag} 0 rows)
  field(${tag} 0 knobRows)
  field(${tag} 0 nameW)
  field(${tag} 0 scrollMax)
  field(${tag} 0 truncated)
  check("${cfg}: pan / volume knobs on all ${${tag}_rows} rows (${${tag}_knobRows}), name ${${tag}_nameW} px" ${tag}_rows GREATER 0 AND ${tag}_knobRows EQUAL ${tag}_rows AND ${tag}_nameW GREATER_EQUAL 36)
endforeach()
check("default rack: 16 and 32 steps fit, 64 steps scroll (${L_steps64_scrollMax})" L_steps16_scrollMax EQUAL 0 AND L_steps32_scrollMax EQUAL 0 AND L_steps64_scrollMax GREATER 0)
check("narrow rack, 64 steps: a horizontal scroll range (${L_steps64_w420_scrollMax})" L_steps64_w420_scrollMax GREATER L_steps64_scrollMax)
check("wide rack, 64 steps: all or nearly all steps fit (${L_steps64_w1300_scrollMax})" L_steps64_w1300_scrollMax LESS L_steps64_scrollMax)
check("docked narrow: names cut short (tooltips: ${L_steps64_dock_truncated})" L_steps64_dock_truncated GREATER 0)
foreach(sc 1.5 2.0)
  string(REPLACE "." "" tag "S${sc}")
  run(${tag} steps64 "30 rack\n" --ui-scale ${sc})
  field(${tag} 0 rows)
  field(${tag} 0 knobRows)
  check("UI scale ${sc}, 64 steps: knobs on all rows" ${tag}_rows GREATER 0 AND ${tag}_knobRows EQUAL ${tag}_rows)
endforeach()

# ---- geometry for scripted input (narrow 64 steps, wide 16, short, keyboard lane) ----
run(gN steps64,w420 "8 rack\n")
field(gN 0 originX)
field(gN 0 row0Y)
field(gN 0 viewX0)
string(REGEX REPLACE "\\..*" "" ry "${gN_row0Y}")
string(REGEX REPLACE "\\..*" "" vx "${gN_viewX0}")
math(EXPR sx0 "${vx} + 6")       # Kick step 1 (on)
math(EXPR sx1 "${vx} + 6 + 15")  # Kick step 2 (off)

# Narrow: the plain wheel scrolls the steps, the velocity stays.
run(nPlain steps64,w420 "9 rack\n10 move ${sx0} ${ry}\n12 wheel -1\n14 wheel -1\n16 wheel -1\n30 rack\n")
field(nPlain 0 velsum)
set(v0 "${nPlain_velsum}")
field(nPlain 1 velsum)
field(nPlain 1 scroll)
field(nPlain 1 wheelScrolls)
check("narrow rack: the wheel over an active step leaves its velocity (${v0} -> ${nPlain_velsum})" nPlain_velsum EQUAL v0 AND nPlain_wheelScrolls EQUAL 1)
check("narrow rack: ... and scrolls the steps instead (${nPlain_scroll})" nPlain_scroll GREATER 0)

run(nCtrl steps64,w420 "9 rack\n10 move ${sx0} ${ry}\n11 mod ctrl 1\n13 wheel 1\n24 rack\n25 mod ctrl 0\n")
field(nCtrl 0 velsum)
set(v0 "${nCtrl_velsum}")
field(nCtrl 1 velsum)
field(nCtrl 1 scroll)
check("narrow rack: Ctrl+wheel changes the step's velocity (${v0} -> ${nCtrl_velsum}), the steps stay" NOT nCtrl_velsum EQUAL v0 AND nCtrl_scroll EQUAL 0)

run(nClick steps64,w420 "9 rack\n10 move ${sx1} ${ry}\n12 down 0\n13 up 0\n16 wheel 1\n26 rack\n")
field(nClick 0 velsum)
set(v0 "${nClick_velsum}")
field(nClick 1 velsum)
math(EXPR want "${v0} + 108")
check("narrow rack: a click turns the step on and gives it the wheel (${v0} -> ${nClick_velsum})" nClick_velsum EQUAL want)

run(nSwipe steps64,w420 "9 rack\n10 move ${sx0} ${ry}\n12 scroll -2 0\n26 rack\n")
field(nSwipe 1 scroll)
field(nSwipe 1 velsum)
check("narrow rack: a sideways swipe scrolls the steps (${nSwipe_scroll})" nSwipe_scroll GREATER 0)

# Wide, 16 steps, nothing scrolls: the wheel adjusts as before.
run(gW steps16,w1300 "8 rack\n")
field(gW 0 viewX0)
field(gW 0 row0Y)
string(REGEX REPLACE "\\..*" "" wx "${gW_viewX0}")
string(REGEX REPLACE "\\..*" "" wy "${gW_row0Y}")
math(EXPR wx "${wx} + 8")
run(wPlain steps16,w1300 "9 rack\n10 move ${wx} ${wy}\n12 wheel -1\n24 rack\n")
field(wPlain 0 velsum)
set(v0 "${wPlain_velsum}")
field(wPlain 1 velsum)
field(wPlain 1 wheelScrolls)
math(EXPR want "${v0} - 8")
check("wide rack (nothing scrolls): the wheel changes the velocity as before (${v0} -> ${wPlain_velsum})" wPlain_velsum EQUAL want AND wPlain_wheelScrolls EQUAL 0)

# Short rack: the channel list scrolls vertically; the wheel over a volume knob scrolls it.
run(gS steps16,w1300,h200 "8 rack\n")
field(gS 0 originX)
field(gS 0 row0Y)
string(REGEX REPLACE "\\..*" "" kx "${gS_originX}")
string(REGEX REPLACE "\\..*" "" ky "${gS_row0Y}")
math(EXPR kx "${kx} + 49")  # volume knob centre
run(sPlain steps16,w1300,h200 "9 rack\n10 move ${kx} ${ky}\n12 wheel -1\n14 wheel -1\n26 rack\n")
field(sPlain 1 rowsScroll)
field(sPlain 1 wheelScrolls)
string(REGEX MATCHALL "channel 0 volume [0-9.]+" vols "${sPlain_out}")
list(GET vols 0 vol0)
list(GET vols 1 vol1)
check("short rack: the wheel over a volume knob leaves it (${vol0} / ${vol1}) and scrolls the list (${sPlain_rowsScroll})"
      vol1 STREQUAL vol0 AND sPlain_rowsScroll GREATER 0 AND sPlain_wheelScrolls EQUAL 1)
run(sCtrl steps16,w1300,h200 "9 rack\n10 move ${kx} ${ky}\n11 mod ctrl 1\n13 wheel 1\n24 rack\n25 mod ctrl 0\n")
string(REGEX MATCHALL "channel 0 volume [0-9.]+" vols "${sCtrl_out}")
list(GET vols 0 c0)
list(GET vols 1 c1)
check("short rack: Ctrl+wheel changes the volume knob (${c0} -> ${c1})" NOT c0 STREQUAL c1)

# Keyboard lane (graph editor) in a scrolling rack.
run(gK steps64,w420,h720 "8 rack\n" --show rackkeys)
field(gK 0 viewX0)
field(gK 0 graphY)
string(REGEX REPLACE "\\..*" "" gx "${gK_viewX0}")
string(REGEX REPLACE "\\..*" "" gy "${gK_graphY}")
math(EXPR gx "${gx} + 6")
math(EXPR gy "${gy} + 55")
run(kPlain steps64,w420,h720 "9 rack\n10 move ${gx} ${gy}\n12 wheel 1\n14 wheel 1\n26 rack\n" --show rackkeys)
field(kPlain 0 pitchsum)
set(p0 "${kPlain_pitchsum}")
field(kPlain 1 pitchsum)
check("keyboard lane, scrolling rack: the plain wheel leaves the step's note (${p0} -> ${kPlain_pitchsum})" kPlain_pitchsum EQUAL p0)
run(kCtrl steps64,w420,h720 "9 rack\n10 move ${gx} ${gy}\n11 mod ctrl 1\n13 wheel 1\n24 rack\n25 mod ctrl 0\n" --show rackkeys)
field(kCtrl 0 pitchsum)
set(p0 "${kCtrl_pitchsum}")
field(kCtrl 1 pitchsum)
check("keyboard lane: Ctrl+wheel moves the step's note (${p0} -> ${kCtrl_pitchsum})" NOT kCtrl_pitchsum EQUAL p0)
