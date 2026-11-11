# Gridline: selecting mixer tracks, resetting a control on the selection and scrolling the strips
# with the wheel, in the real app (hidden, isolated config, scripted input, 1280x800, mixer filling
# the window: Kick's fader at x 155, the next ones 68 px further; names at y 145):
#   1. Ctrl+A (Cmd+A) selects every insert only with the mixer focused; with the playlist focused,
#      or a text prompt open over the mixer, the selection stays; Ctrl+D goes back to one;
#   2. a double-click reset on a selected track's fader resets all selected tracks in one undo
#      step, undo restores them all; on a track outside the selection it resets only that track;
#   3. the wheel over the names or a strip's empty part scrolls sideways at the playlist's speed
#      (Shift and a horizontal wheel too); over a fader it changes the fader and does not scroll;
#      a scroll under way never changes a fader sliding under the pointer;
#   4. all of it in a detached mixer.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k176_mixer_select_app.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# The primary shortcut modifier: "super" is the Cmd key on macOS.
if(CMAKE_HOST_APPLE)
  set(P super)
else()
  set(P ctrl)
endif()

# run(<tag> <size> <frames> <script> [extra args]); the output in ${tag}_out.
function(run tag size frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size ${size} --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${o}")
  endif()
  set(${tag}_out "${o}" PARENT_SCOPE)
endfunction()

# dump(<tag> <n>): from the n-th (0-based) "mixer" report: d_undo, d_x, d_sel, d_focus, d_v<i>.
function(dump tag n)
  string(REGEX MATCHALL "mixer frame [0-9]+: [^\n]*\n(  track [^\n]*\n)+" all "${${tag}_out}")
  list(LENGTH all count)
  if(count LESS_EQUAL n)
    message(FATAL_ERROR "FAIL (${tag}): no mixer report ${n}. Output:\n${${tag}_out}")
  endif()
  list(GET all ${n} d)
  string(REGEX MATCH "undo ([0-9]+) scrollX ([-0-9.]+) sel ([-0-9,]+) focus ([-0-9]+)" _ "${d}")
  set(d_undo "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(d_x "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(d_sel "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(d_focus "${CMAKE_MATCH_4}" PARENT_SCOPE)
  foreach(i 1 2 3 5)
    string(REGEX MATCH "track ${i} volume ([0-9.]+)" _ "${d}")
    set(d_v${i} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  endforeach()
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

set(all_inserts "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16")
set(unity "0.8000")

# ---- 1. Ctrl+A follows the focus ----
run(keys 1440x900 44 "8 focus Playlist\n11 playstate\n12 mod ${P} 1\n13 key a\n15 mod ${P} 0\n18 mixer\n20 focus Mixer\n23 playstate\n24 mod ${P} 1\n25 key a\n27 mod ${P} 0\n30 mixer\n32 mod ${P} 1\n33 key d\n35 mod ${P} 0\n38 mixer\n")
check("the playlist has the focus" keys_out MATCHES "playstate frame 11:[^\n]*focus \"[^\"]*Playlist")
check("the mixer has the focus" keys_out MATCHES "playstate frame 23:[^\n]*focus \"[^\"]*Mixer")
dump(keys 0)
check("Ctrl+A with the playlist focused leaves the mixer selection (${d_sel})" d_sel STREQUAL "0")
dump(keys 1)
check("Ctrl+A with the mixer focused selects every insert, not the master (${d_sel})" d_sel STREQUAL all_inserts)
check("the focus moves from the master to the first insert (${d_focus})" d_focus EQUAL 1)
dump(keys 2)
check("Ctrl+D goes back to the focused track (${d_sel})" d_sel STREQUAL "1")

# A text prompt open over the mixer keeps Ctrl+A for itself (double-click on a name: rename).
run(prompt 1280x800 34 "6 focus Mixer\n10 move 140 145\n12 down 0\n13 up 0\n14 down 0\n15 up 0\n22 mod ${P} 1\n23 key a\n25 mod ${P} 0\n28 mixer\n" --show mixer)
dump(prompt 0)
check("Ctrl+A in the rename prompt does not select the tracks (${d_sel})" d_sel STREQUAL "1")

# ---- 2. a reset covers the selection ----
run(reset 1280x800 86 "8 move 155 400\n10 wheel -1\n14 move 223 400\n16 wheel -1\n20 move 291 400\n22 wheel -1\n26 move 427 400\n28 wheel -1\n32 move 140 145\n34 down 0\n35 up 0\n38 mod shift 1\n40 move 276 145\n42 down 0\n43 up 0\n45 mod shift 0\n48 mixer\n50 move 223 400\n52 down 0\n53 up 0\n54 down 0\n55 up 0\n60 mixer\n62 undo\n66 mixer\n68 move 427 400\n70 down 0\n71 up 0\n72 down 0\n73 up 0\n78 mixer\n" --show mixer)
dump(reset 0)
set(before_undo "${d_undo}")
set(before_v1 "${d_v1}")
set(before_v3 "${d_v3}")
set(before_v5 "${d_v5}")
check("Shift+click selects three tracks (${d_sel})" d_sel STREQUAL "1,2,3")
check("the wheel moved the faders first (${d_v1})" NOT d_v1 STREQUAL unity AND NOT d_v3 STREQUAL unity AND NOT d_v5 STREQUAL unity)
dump(reset 1)
check("double-click on a selected fader resets all three (${d_v1} ${d_v2} ${d_v3})" d_v1 STREQUAL unity AND d_v2 STREQUAL unity AND d_v3 STREQUAL unity)
check("a track outside the selection stays (${d_v5})" d_v5 STREQUAL before_v5)
math(EXPR one_more "${before_undo} + 1")
check("the reset of three tracks is one undo step (${before_undo} to ${d_undo})" d_undo EQUAL one_more)
dump(reset 2)
check("one undo restores all three (${d_v1} ${d_v3})" d_v1 STREQUAL before_v1 AND d_v3 STREQUAL before_v3 AND NOT d_v2 STREQUAL unity)
dump(reset 3)
check("double-click outside the selection resets only that track (${d_v5})" d_v5 STREQUAL unity AND d_v1 STREQUAL before_v1 AND d_v3 STREQUAL before_v3)

# ---- 3. the wheel ----
run(names 1280x800 26 "8 move 140 145\n10 wheel -1\n14 waitfor k176-none 0.8\n20 mixer\n" --show mixer)
dump(names 0)
check("the wheel over a track name scrolls sideways (${d_x})" d_x GREATER 70 AND d_x LESS 90)
check("... at the playlist's speed, changing nothing (undo ${d_undo})" d_undo EQUAL 0)

run(meter 1280x800 34 "8 move 131 400\n10 wheel -1\n12 wheel -1\n14 wheel -1\n20 waitfor k176-none 0.8\n28 mixer\n" --show mixer)
dump(meter 0)
check("the wheel over a strip's empty part scrolls (${d_x})" d_x GREATER 70)
check("a scroll under way leaves the faders sliding under the pointer alone (undo ${d_undo})" d_undo EQUAL 0)

run(fader 1280x800 34 "8 move 155 400\n10 wheel -1\n14 waitfor k176-none 0.8\n20 mixer\n" --show mixer)
dump(fader 0)
check("the wheel over a fader changes it (${d_v1})" NOT d_v1 STREQUAL unity)
check("... and does not scroll (${d_x})" d_x EQUAL 0)

run(shift 1280x800 34 "8 move 140 145\n9 mod shift 1\n10 wheel -1\n14 waitfor k176-none 0.8\n20 mixer\n21 mod shift 0\n" --show mixer)
dump(shift 0)
check("Shift+wheel scrolls sideways (${d_x})" d_x GREATER 70)

run(hwheel 1280x800 34 "8 move 155 400\n10 scroll -1 0\n14 waitfor k176-none 0.8\n20 mixer\n" --show mixer)
dump(hwheel 0)
check("a horizontal wheel scrolls sideways, also over a fader (${d_x})" d_x GREATER 70 AND d_v1 STREQUAL unity)

run(thin 400x900 34 "8 move 133 330\n10 wheel -1\n14 waitfor k176-none 0.8\n20 mixer\n" --show mixer)
dump(thin 0)
check("the thin mixer does not scroll sideways (${d_x}) or change the fader (${d_v1})" d_x EQUAL 0 AND d_v1 STREQUAL unity)

# ---- 4. detached ----
run(geo 1280x800 12 "8 viewports\n")
if(geo_out MATCHES "available 0")
  string(REGEX MATCH "unavailable: [^\n]*" why "${geo_out}")
  message(STATUS "SKIP: the detached part: windows cannot be detached on this platform (${why})")
  return()
endif()
run(detached 1440x900 110 "8 detach Mixer 1600 120 900 520\n14 viewports\n16 move 1755 420\n18 wheel -1\n22 move 1823 420\n24 wheel -1\n26 move 1731 420\n27 down 0\n28 up 0\n29 focusmove Mixer\n31 mixer\n32 playstate\n34 mod ${P} 1\n35 key a\n37 mod ${P} 0\n40 mixer\n44 move 1823 420\n46 down 0\n47 up 0\n48 down 0\n49 up 0\n54 mixer\n56 undo\n60 mixer\n64 move 1740 212\n66 wheel -1\n70 waitfor k176-none 0.8\n76 mixer\n78 move 1743 420\n80 waitfor k176-none 0.5\n84 wheel -1\n88 waitfor k176-none 0.8\n94 mixer\n")
check("the mixer is detached" detached_out MATCHES "window \"Mixer\"[^\n]*viewport [1-9]")
dump(detached 0)
set(moved1 "${d_v1}")
set(moved2 "${d_v2}")
set(undo0 "${d_undo}")
check("detached: the wheel over faders changes them (${d_v1} ${d_v2})" NOT d_v1 STREQUAL unity AND NOT d_v2 STREQUAL unity AND d_x EQUAL 0)
dump(detached 1)
string(REGEX MATCH "playstate frame 32:[^\n]*" focusline "${detached_out}")
check("detached: Ctrl+A selects every insert (${d_sel}; ${focusline})" d_sel STREQUAL all_inserts)
dump(detached 2)
math(EXPR one_more "${undo0} + 1")
check("detached: double-click resets the selection in one step (${d_v1} ${d_v2}, undo ${d_undo})" d_v1 STREQUAL unity AND d_v2 STREQUAL unity AND d_undo EQUAL one_more)
dump(detached 3)
check("detached: undo restores them (${d_v1} ${d_v2})" d_v1 STREQUAL moved1 AND d_v2 STREQUAL moved2)
dump(detached 4)
set(x0 "${d_x}")
check("detached: the wheel over a name scrolls sideways (${d_x})" d_x GREATER 70)
dump(detached 5)
check("detached: over a fader the wheel changes it (${d_v2}) and the strips stay (${d_x})" d_x EQUAL x0 AND NOT d_v2 STREQUAL moved2)
