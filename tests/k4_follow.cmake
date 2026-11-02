# Gridline: K-4, auto scrolling can be turned off, for the playlist and for the pattern editors
# (piano roll, channel rack) on their own. Runs the app hidden (off-screen, isolated configs) and
# scripts input over it:
#   playlist (song mode): zoomed in so the song position leaves the view, playback started and
#     moved past the view's right edge: with "ui.autoScroll" on the view pages along, off it stays
#     put; the pattern editors' switch does not change the playlist;
#   piano roll (pattern mode, a 64-beat pattern): the same with "ui.autoScrollPattern";
#   manual scrolling while playing still moves the view with following off;
#   channel rack (64 steps, narrow): the step view pages along with the playing step only when on.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k4_follow.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
if(APPLE)
  set(ctrl super)  # ImGui swaps Cmd and Ctrl on macOS
else()
  set(ctrl ctrl)
endif()

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 ${ARGN}
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# <view> scroll x (playlist / piano) of trace frame <frame> -> <tag>_<view><frame>.
function(traced tag view frame)
  if(view STREQUAL "playlist")
    string(REGEX MATCH "trace frame ${frame}: wheel [-0-9.]+ [-0-9.]+ playlist ([-0-9.]+)" _ "${${tag}_out}")
  else()
    string(REGEX MATCH "trace frame ${frame}: wheel [-0-9.]+ [-0-9.]+ playlist [-0-9.]+ [-0-9.]+ piano ([-0-9.]+)" _ "${${tag}_out}")
  endif()
  if("${CMAKE_MATCH_1}" STREQUAL "")
    message(FATAL_ERROR "FAIL (${tag}): no trace of frame ${frame}. Output:\n${${tag}_out}")
  endif()
  string(REGEX REPLACE "\\..*" "" v "${CMAKE_MATCH_1}")
  set(${tag}_${view}${frame} "${v}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# ---- playlist: zoomed in at its left edge, the song plays from beat 14 (past the view) ----
foreach(state on off)
  run(pl_${state} 70 "2 typing off
4 setting ui.autoScroll ${state}
6 move 345 400
8 mod ${ctrl} 1
10 wheel 3
12 wheel 3
14 wheel 3
16 mod ${ctrl} 0
18 move 1300 860
34 trace on
36 playmode song
38 startsong
40 seekbeat 14
60 transport
62 trace off
" --show playlist)
  traced(pl_${state} playlist 35)
  traced(pl_${state} playlist 60)
endforeach()
check("playlist: zoomed in, the view starts at the song start (${pl_on_playlist35} / ${pl_off_playlist35})"
      pl_on_playlist35 LESS 40 AND pl_off_playlist35 LESS 40)
check("playlist, auto scrolling on: the view follows the song position (${pl_on_playlist35} -> ${pl_on_playlist60})"
      pl_on_playlist60 GREATER 300)
check("playlist, auto scrolling off: the view stays put while the song plays (${pl_off_playlist35} -> ${pl_off_playlist60})"
      pl_off_playlist60 EQUAL pl_off_playlist35)
string(REGEX MATCH "transport frame 60: [^\n]*" t "${pl_off_out}")
check("playlist, auto scrolling off: the song is playing meanwhile (${t})" t MATCHES "playing 1")

# The pattern editors' switch off leaves the playlist following.
run(pl_pat_off 70 "2 typing off
4 setting ui.autoScrollPattern off
6 move 345 400
8 mod ${ctrl} 1
10 wheel 3
12 wheel 3
14 wheel 3
16 mod ${ctrl} 0
18 move 1300 860
34 trace on
36 playmode song
38 startsong
40 seekbeat 14
62 trace off
" --show playlist)
traced(pl_pat_off playlist 60)
check("playlist: switching the pattern editors' following off leaves the playlist following (${pl_pat_off_playlist60})"
      pl_pat_off_playlist60 GREATER 300)

# ---- piano roll: a 64-beat pattern, played in pattern mode from beat 40 ----
foreach(state on off)
  run(pr_${state} 70 "2 typing off
4 setting ui.autoScrollPattern ${state}
6 mknotes 64 96 48
18 move 1300 860
34 trace on
36 playmode pattern
38 startsong
40 seekbeat 40
60 transport
62 trace off
" --show piano)
  traced(pr_${state} piano 35)
  traced(pr_${state} piano 60)
endforeach()
check("piano roll: the view starts at the pattern start (${pr_on_piano35} / ${pr_off_piano35})" pr_on_piano35 LESS 40 AND pr_off_piano35 LESS 40)
check("piano roll, auto scrolling on: the view follows the pattern position (${pr_on_piano35} -> ${pr_on_piano60})" pr_on_piano60 GREATER 1000)
check("piano roll, auto scrolling off: the view stays put while the pattern plays (${pr_off_piano35} -> ${pr_off_piano60})"
      pr_off_piano60 EQUAL pr_off_piano35)

# Manual scrolling while playing, following off: the view moves and stays where it was scrolled to.
run(pr_manual 80 "2 typing off
4 setting ui.autoScrollPattern off
6 mknotes 64 96 48
18 move 700 700
34 trace on
36 playmode pattern
38 startsong
40 seekbeat 40
44 scroll -6 0
70 trace off
" --show piano)
traced(pr_manual piano 43)
traced(pr_manual piano 68)
check("piano roll, following off, playing: a sideways scroll still moves the view (${pr_manual_piano43} -> ${pr_manual_piano68})"
      pr_manual_piano68 GREATER pr_manual_piano43)

# ---- channel rack: 64 steps in a narrow rack (the steps scroll), played from step 48 ----
foreach(state on off)
  run(rk_${state} 50 "2 typing off
4 setting ui.autoScrollPattern ${state}
10 rack
12 playmode pattern
14 startsong
16 seekbeat 12
40 rack
" --rack steps64,w420)
  string(REGEX MATCHALL "rack frame [^\n]*\n  geom [^\n]*" dumps "${rk_${state}_out}")
  list(LENGTH dumps count)
  if(count LESS 2)
    message(FATAL_ERROR "FAIL (rk_${state}): no rack dumps. Output:\n${rk_${state}_out}")
  endif()
  list(GET dumps 0 d0)
  list(GET dumps 1 d1)
  string(REGEX MATCH " scroll ([-0-9.]+)" _ "${d0}")
  set(rk_${state}_s0 "${CMAKE_MATCH_1}")
  string(REGEX MATCH " scroll ([-0-9.]+)" _ "${d1}")
  set(rk_${state}_s1 "${CMAKE_MATCH_1}")
endforeach()
check("channel rack, auto scrolling on: the steps page along with the playing step (${rk_on_s0} -> ${rk_on_s1})" rk_on_s1 GREATER 0)
check("channel rack, auto scrolling off: the steps stay put (${rk_off_s0} -> ${rk_off_s1})" rk_off_s1 EQUAL rk_off_s0)
