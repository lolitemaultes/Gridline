# Gridline: K-445, Seltmann's loops reach the playlist and their folder can be found.
# The app runs hidden (isolated config and HOME, SDL's dummy audio) with a synthetic break loaded
# into Seltmann, driven with real SDL mouse events:
#   1. the LOOP chip dragged onto a playlist track makes an audio clip there;
#   2. right-click LOOP > Add to the playlist places it without a drag (a notice says where);
#   3. right-click LOOP > Show in Finder / Files opens the loops folder (a hidden run prints it);
#   4. Make loops says where the loops are: the notice holds the folder's whole path.
# Screenshots: <OUT>/menu.ppm (the chip's menu).
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P k445_seltmann_out_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home" "${OUT}/cfg/gridline")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_CACHE_DIR} "${OUT}/cache")
set(ENV{GRIDLINE_SELTMANN_TRACE} "1")
file(WRITE "${OUT}/cfg/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true}}\n")

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --plugin gl.seltmann --size 1440x960 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.out" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# The break dropped on the editor's drop zone, analysed; the chips' places and the playlist's rows.
run(where 30 "")
string(REGEX MATCH "seltmann: zone ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+)" _ "${out}")
math(EXPR zx "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
math(EXPR zy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
set(load "10 dragenter ${zx} ${zy} \"${OUT}/Break 96.wav\"\n12 dragmove ${zx} ${zy}\n14 dragmove ${zx} ${zy}\n16 drop\n17 waitfor seltmann-ready 120\n")
run(ready 40 "${load}24 sdlat selloop\n26 plrows\n27 windows\n38 quit discard\n")
check("the break is analysed" out MATCHES "seltmann: state ready kind break")
string(REGEX MATCH "sdlat frame 24: selloop at ([0-9]+),([0-9]+)" _ "${out}")
set(lx "${CMAKE_MATCH_1}")
set(ly "${CMAKE_MATCH_2}")
check("the LOOP chip is shown (${lx} ${ly})" lx MATCHES "[0-9]")
# A playlist row below the Seltmann window (channelsettings), its lane a little right of the grid's start.
string(REGEX MATCH "window \"channelsettings\" pos [0-9.]+,([0-9.]+) size [0-9.]+x([0-9.]+)" _ "${out}")
set(wy "${CMAKE_MATCH_1}")
set(wh "${CMAKE_MATCH_2}")
string(REGEX REPLACE "\\..*" "" wy "${wy}")
string(REGEX REPLACE "\\..*" "" wh "${wh}")
math(EXPR below "${wy} + ${wh} + 30")
string(REGEX MATCHALL "plrow [0-9]+ \"[^\"]*\" [0-9]+ [0-9]+ top [0-9]+ h [0-9]+ lane [0-9]+ bus [0-9]+ grid [0-9]+ [0-9]+ [0-9]+" rows "${out}")
set(track -1)
foreach(r ${rows})
  string(REGEX MATCH "plrow ([0-9]+) \"[^\"]*\" [0-9]+ ([0-9]+) top [0-9]+ h [0-9]+ lane [0-9]+ bus [0-9]+ grid ([0-9]+) [0-9]+ ([0-9]+)" _ "${r}")
  if(track LESS 0 AND CMAKE_MATCH_2 GREATER below AND CMAKE_MATCH_2 LESS CMAKE_MATCH_4)
    set(track "${CMAKE_MATCH_1}")
    set(ty "${CMAKE_MATCH_2}")
    math(EXPR tx "${CMAKE_MATCH_3} + 250")
  endif()
endforeach()
check("a playlist track shows below the editor (track ${track} at ${tx} ${ty})" track GREATER -1)

# ---- 1. the drag
math(EXPR mx "(${lx} + ${tx}) / 2")
math(EXPR my "(${ly} + ${ty}) / 2")
run(drag 60 "${load}24 sdlmove ${lx} ${ly}\n26 sdldown 0\n28 sdlmove ${lx} ${my}\n30 sdlmove ${mx} ${my}\n32 sdlmove ${tx} ${ty}\n34 sdlmove ${tx} ${ty}\n36 sdlmove ${tx} ${ty}\n38 sdlup 0\n44 dump\n58 quit discard\n")
check("the LOOP chip dragged onto track ${track} makes an audio clip there" out MATCHES "dump frame 44: 1 clips[^\n]*\n  clip [0-9]+ kind 1 track ${track} ")

# ---- 2. and 3. the chip's menu (a right-click with the real mouse; its items picked by their labels)
if(CMAKE_HOST_APPLE)
  set(reveal "Show in Finder")
elseif(CMAKE_HOST_WIN32)
  set(reveal "Show in Explorer")
else()
  set(reveal "Show in Files")
endif()
run(menu 64 "${load}24 sdlmove ${lx} ${ly}\n26 sdldown 1\n27 sdlup 1\n29 shot \"${OUT}/menu.ppm\"\n30 atitem \"Add to the playlist\"\n31 down 0\n32 up 0\n34 toastlist\n36 dump\n38 sdlmove ${lx} ${ly}\n39 sdldown 1\n40 sdlup 1\n42 atitem \"${reveal}\"\n43 down 0\n44 up 0\n62 quit discard\n")
check("the chip's menu lists its items" NOT out MATCHES "atitem frame [0-9]+: [^\n]* not shown")
check("Add to the playlist places the loop as an audio clip" out MATCHES "dump frame 36: 1 clips[^\n]*\n  clip [0-9]+ kind 1 ")
check("... and says where" out MATCHES "toast \"Added to the playlist on track [0-9]+\"")
check("Show in Finder / Files opens the loops folder" out MATCHES "reveal: [^\n]*Seltmann loops\n")

# ---- 4. Make loops: the notice says where they went
math(EXPR bx "${lx} + 106")  # Make loops: right of the LOOP and MIDI chips
run(make 140 "${load}24 sdlmove ${bx} ${ly}\n26 sdldown 0\n27 sdlup 0\n40 waitfor k445-never 4\n120 toastlist\n138 quit discard\n")
check("Make loops names the folder the loops are in" out MATCHES "toast \"Made [0-9]+ loops in [^\"\n]*[/\\]Seltmann loops\" \\[")
message(STATUS "done")
