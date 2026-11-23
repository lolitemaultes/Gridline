# Gridline: hidden, narrow and collapsed mixer strips in the real app (hidden, isolated config and
# HOME, a fixed 16 ms frame time).
#   1. Studio: a hidden track has no strip and the toolbar counts it; a narrow track draws a slim
#      strip; a group collapsed shows once as a slim strip; each change is one undo step; the
#      toolbar's list shows everything again (Show all); the track menu offers Hide.
#   2. The state is saved with the project and read back.
#   3. Smooth: the same tracks are left out or slim in its mixer, and its bar counts them.
#   4. Clips: a hidden track keeps its column with a stand-in; a click on it shows the strip.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k078_mixer_hide.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
if(NOT DEFINED SIZE)
  set(SIZE 1920x1080)
endif()

function(settings layout)
  file(WRITE "${OUT}/cfg/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"${layout}\"}}\n")
endfunction()

function(run name script)
  file(WRITE "${OUT}/${name}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --size ${SIZE} ${ARGN} --dev-input "${OUT}/${name}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 240)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${o}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

function(expect re what)
  if(NOT out MATCHES "${re}")
    message(FATAL_ERROR "FAIL: ${what}\n(looked for: ${re})\n${out}")
  endif()
  message(STATUS "ok: ${what}")
endfunction()

function(expect_not re what)
  if(out MATCHES "${re}")
    message(FATAL_ERROR "FAIL: ${what}\n(found: ${re})\n${out}")
  endif()
  message(STATUS "ok: ${what}")
endfunction()

# The Studio strips drawn at a frame ("mixer frame N: ..." then "  strips 0,1,2").
# One dev dump: from "<what> frame N:" up to the next dump's first line.
function(block_of what frame var)
  string(FIND "${out}" "${what} frame ${frame}:" i)
  if(i LESS 0)
    message(FATAL_ERROR "FAIL: no \"${what}\" output at frame ${frame}\n${out}")
  endif()
  string(SUBSTRING "${out}" ${i} -1 rest)
  string(SUBSTRING "${rest}" 10 -1 tail)
  string(REGEX MATCH "\n[^\n]* frame [0-9]+:" nx "${tail}")
  if(nx)
    string(FIND "${tail}" "${nx}" j)
    math(EXPR j "${j} + 10")
    string(SUBSTRING "${rest}" 0 ${j} rest)
  endif()
  set(${var} "${rest}" PARENT_SCOPE)
endfunction()

function(strips_at frame var)
  block_of(mixer ${frame} b)
  string(REGEX MATCH "  strips ([0-9,]*)" m "${b}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(undo_at frame var)
  string(REGEX MATCH "mixhide frame ${frame}: hidden ([0-9]+) undo ([0-9]+)" m "${out}")
  set(${var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# 1. Studio.
settings(studio)
run(studio "6 mixhide state
6 mixer
8 mixhide hide 2
10 mixhide state
10 mixer
12 mixhide narrow 3
14 mixhide state
14 mixer
16 mixhide group Drums 4 5 6
18 mixhide fold Drums
20 mixhide state
20 mixer
22 shot \"${OUT}/studio-hidden.ppm\"
24 undo
26 mixhide state
26 mixer
28 redo
30 mixhide state
32 sdlat mixhidden
34 sdldown 0
35 sdlup 0
38 sdlat mixshowall
40 shot \"${OUT}/studio-list.ppm\"
42 sdldown 0
43 sdlup 0
46 mixhide state
46 mixer
48 save \"${OUT}/hide.gridline\"
50 quit discard
" --mixer-view rows1)
strips_at(6 s0)
if(NOT s0 MATCHES "(^|,)2(,|$)")
  message(FATAL_ERROR "FAIL: track 2 should have a strip at first (strips ${s0})\n${out}")
endif()
expect("mixhide frame 6: hidden 0 " "nothing hidden at first")
expect("mixhide frame 10: hidden 1 [^\n]*\n(  mhtrack[^\n]*\n)*  mhtrack 2 hidden 1 narrow 0 folded 0 shown 0" "Hide marks the track hidden")
strips_at(10 s1)
if(s1 MATCHES "(^|,)2(,|$)")
  message(FATAL_ERROR "FAIL: a hidden track has no strip (strips ${s1})\n${out}")
endif()
message(STATUS "ok: a hidden track has no strip")
block_of(mixer 14 b14)
if(NOT b14 MATCHES "  srect 3 ([0-9]+) [0-9]+ ([0-9]+) ")
  message(FATAL_ERROR "FAIL: a narrow track keeps a strip\n${out}")
endif()
message(STATUS "ok: a narrow track keeps a strip")
math(EXPR w3 "${CMAKE_MATCH_2} - ${CMAKE_MATCH_1}")
if(w3 GREATER 40)
  message(FATAL_ERROR "FAIL: the narrow strip should be slim (width ${w3})\n${out}")
endif()
message(STATUS "ok: the narrow strip is slim (${w3} px)")
expect("mixhide frame 20: hidden 4 " "a collapsed group counts its three tracks, beside the hidden one")
strips_at(20 s2)
if(s2 MATCHES "(^|,)(5|6)(,|$)" OR NOT s2 MATCHES "(^|,)4(,|$)")
  message(FATAL_ERROR "FAIL: a collapsed group shows once, at its first member (strips ${s2})\n${out}")
endif()
message(STATUS "ok: a collapsed group shows once")
undo_at(20 u20)
undo_at(26 u26)
undo_at(30 u30)
math(EXPR back "${u20} - 1")
if(NOT u26 EQUAL back OR NOT u30 EQUAL u20)
  message(FATAL_ERROR "FAIL: collapsing is one undo step (undo ${u20} -> ${u26} -> ${u30})\n${out}")
endif()
expect("mixhide frame 26: hidden 1 " "undo expands the group again")
expect("mixhide frame 30: hidden 4 " "redo collapses it")
expect("sdlat frame 32: mixhidden at " "the toolbar counts the hidden tracks")
expect("sdlat frame 38: mixshowall at " "its list offers Show all")
expect("mixhide frame 46: hidden 0 " "Show all brings every strip back")
strips_at(46 s3)
foreach(k 2 3 4 5 6)
  if(NOT s3 MATCHES "(^|,)${k}(,|$)")
    message(FATAL_ERROR "FAIL: Show all: strip ${k} is back (strips ${s3})\n${out}")
  endif()
endforeach()
expect("mixhide frame 46:[^\n]*\n(  mhtrack[^\n]*\n)*  mhtrack 4 hidden 0 narrow 0 folded 0 shown 1 group \\[Drums\\]" "the group stays, expanded")

# 2. Saved with the project: hide and narrow, then saved and read back.
run(save "6 mixhide hide 2
8 mixhide narrow 3
10 mixhide group Drums 4 5
12 mixhide fold Drums
14 save \"${OUT}/hide2.gridline\"
16 quit discard
")
run(load "6 mixhide state
8 quit discard
" "${OUT}/hide2.gridline")
expect("mixhide frame 6: hidden 3 " "the project reads its hidden tracks back")
expect("mhtrack 2 hidden 1 narrow 0 folded 0 shown 0" "a hidden track stays hidden")
expect("mhtrack 3 hidden 0 narrow 1 folded 0 shown 1" "a narrow track stays narrow")
expect("mhtrack 5 hidden 0 narrow 0 folded 1 shown 0 group \\[Drums\\]" "a collapsed group stays collapsed")

# 3. Smooth's mixer.
settings(logic)
run(smooth "4 layout open Mixer
6 mixhide smoothview 2
8 mixhide state
10 mixhide hide 2
12 mixhide narrow 3
14 mixhide group Drums 4 5 6
16 mixhide fold Drums
20 mixhide state
22 shot \"${OUT}/smooth-hidden.ppm\"
24 sdlat mixhidden
26 sdldown 0
27 sdlup 0
30 shot \"${OUT}/smooth-list.ppm\"
32 sdlat mixshowall
34 sdldown 0
35 sdlup 0
38 mixhide state
40 quit discard
")
block_of(mixhide 8 b8)
if(NOT b8 MATCHES "  cols [^\n]* 2:[0-9]+")
  message(FATAL_ERROR "FAIL: Smooth: track 2 has a column at first\n${out}")
endif()
message(STATUS "ok: Smooth: track 2 has a column at first")
block_of(mixhide 20 b20)
string(REGEX MATCH "  cols ([^\n]*)" m "${b20}")
set(c20 " ${CMAKE_MATCH_1} ")
if(c20 MATCHES " 2:" OR c20 MATCHES " 5:" OR c20 MATCHES " 6:")
  message(FATAL_ERROR "FAIL: Smooth: hidden and folded tracks have no column (${c20})\n${out}")
endif()
if(NOT c20 MATCHES " 3:3[0-9] " OR NOT c20 MATCHES " 4:3[0-9] ")
  message(FATAL_ERROR "FAIL: Smooth: the narrow track and the collapsed group are slim columns (${c20})\n${out}")
endif()
message(STATUS "ok: Smooth leaves hidden tracks out and draws slim columns")
expect("sdlat frame 24: mixhidden at " "Smooth: its bar counts the hidden tracks")
expect("mixhide frame 38: hidden 0 " "Smooth: Show all brings them back")

# 4. Clips (user 2026-11-21): a hidden track keeps its column in the session mixer, with a quiet
# stand-in (the clip grid keeps its columns); a click on the stand-in shows the strip again.
settings(live)
run(clips "6 sdlat clipshiddenstrip
8 mixhide hide 2
12 sdlat clipshiddenstrip
14 shot \"${OUT}/clips-hidden.ppm\"
16 sdldown 0
17 sdlup 0
20 mixhide state
22 sdlat clipshiddenstrip
24 quit discard
")
expect("sdlat frame 6: clipshiddenstrip not shown" "Clips: no stand-in while nothing is hidden")
expect("sdlat frame 12: clipshiddenstrip at " "Clips: a hidden track keeps its column, with a stand-in")
expect("mixhide frame 20: hidden 0 " "Clips: a click on the stand-in shows the strip again")
expect("sdlat frame 22: clipshiddenstrip not shown" "Clips: the stand-in goes with it")
