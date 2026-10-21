# Gridline — J-83: the mouse wheel over a mixer gain fader.
# Runs the app hidden (off-screen, isolated config) with the mixer filling the
# window and scripts wheel input over the Kick strip's fader (insert 1):
#   thin (400 px wide, J-38 stacked layout): plain wheel scrolls the strips,
#     the gain stays; Ctrl+wheel adjusts it; after a click the wheel adjusts it;
#     a flick (momentum) over the faders never changes a gain, even with Ctrl
#     pressed mid-flick;
#   wide (1280 px): the wheel adjusts the fader as before (control run);
#   resize: wide adjusts, then the window shrinks to the thin layout and the
#     same wheel scrolls instead (no restart).
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j83_mixer_wheel.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# Kick fader: (133, 330) in the thin layout at 400x900, (155, 400) in the wide one at 1280x800.
file(WRITE "${OUT}/thin.txt" "8 move 133 330\n10 wheel -1\n12 wheel -1\n14 wheel -1\n30 mixer\n")
file(WRITE "${OUT}/wide.txt" "8 move 155 400\n10 wheel -1\n20 mixer\n")
file(WRITE "${OUT}/ctrl.txt" "8 move 133 330\n9 mod ctrl 1\n10 wheel 1\n12 wheel 1\n20 mixer\n21 mod ctrl 0\n")
file(WRITE "${OUT}/click.txt" "8 move 133 330\n10 down 0\n11 up 0\n13 wheel 1\n20 mixer\n")
file(WRITE "${OUT}/momentum.txt" "8 move 133 330\n10 momentum 0 -1.5 40\n13 mod ctrl 1\n60 mixer\n")
file(WRITE "${OUT}/resize.txt" "8 move 155 400\n10 wheel -1\n20 mixer\n36 move 133 330\n40 wheel -1\n55 mixer\n")

function(run tag size frames)
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --show mixer --size ${size} --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# The n-th (0-based) "mixer" dump of a run: kick volume and strips scroll.
function(dump tag n)
  string(REGEX MATCHALL "mixer frame [0-9]+: scroll [-0-9.]+ wheelScrolls [01][^\n]*\n  track 0 [^\n]*\n  track 1 volume [0-9.]+" dumps "${${tag}_out}")
  list(LENGTH dumps count)
  if(count LESS_EQUAL n)
    message(FATAL_ERROR "FAIL (${tag}): no mixer dump ${n}. Output:\n${${tag}_out}")
  endif()
  list(GET dumps ${n} d)
  string(REGEX MATCH "scroll ([-0-9.]+)" _ "${d}")
  set(scroll "${CMAKE_MATCH_1}")
  string(REGEX MATCH "wheelScrolls ([01])" _ "${d}")
  set(policy "${CMAKE_MATCH_1}")
  string(REGEX MATCH "track 1 volume ([0-9.]+)" _ "${d}")
  set(${tag}_vol "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${tag}_scroll "${scroll}" PARENT_SCOPE)
  set(${tag}_policy "${policy}" PARENT_SCOPE)
endfunction()

# check("<message>" <condition...>)
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

run(thin 400x900 32)
dump(thin 0)
check("thin: wheel over the gain fader leaves the gain (${thin_vol})" thin_vol STREQUAL 0.8000)
check("thin: the wheel scrolls the strips instead (scroll ${thin_scroll})" thin_scroll GREATER 0 AND thin_policy STREQUAL 1)

run(wide 1280x800 22)
dump(wide 0)
check("wide: wheel over the gain fader adjusts it as before (${wide_vol})" NOT wide_vol STREQUAL 0.8000 AND wide_policy STREQUAL 0)

run(ctrl 400x900 22)
dump(ctrl 0)
check("thin: Ctrl+wheel adjusts the fader (${ctrl_vol}), the strips stay" ctrl_vol GREATER 0.8000 AND ctrl_scroll EQUAL 0)

run(click 400x900 22)
dump(click 0)
check("thin: after a click on the fader the wheel adjusts it (${click_vol})" click_vol GREATER 0.8000)

run(momentum 400x900 62)
dump(momentum 0)
check("thin: a flick over the faders scrolls (${momentum_scroll}) and changes no gain, Ctrl pressed mid-flick or not" momentum_vol STREQUAL 0.8000 AND momentum_scroll GREATER 0)

run(resize 1280x800 60 --resize 400x900)
dump(resize 0)
set(before "${resize_vol}")
check("resize: the wide mixer adjusts (${before})" NOT before STREQUAL 0.8000 AND resize_policy STREQUAL 0)
dump(resize 1)
check("resize: shrunk to the thin layout, the same wheel scrolls (scroll ${resize_scroll}) and the gain stays" resize_vol STREQUAL before AND resize_scroll GREATER 0 AND resize_policy STREQUAL 1)
