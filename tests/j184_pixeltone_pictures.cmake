# Gridline — J-184: Pixel Tone opens pictures, in the real app (hidden, isolated config).
#   1. The editor shows the built-in picture, its picture tools and the picture well.
#   2. "Load picture..." opens the file dialog; OK on a synthetic PNG loads it as one undo
#      step ("load picture"); undo brings the built-in picture back.
#   3. A picture dragged from the desktop onto the picture lights it up and loads on drop.
#   4. Dropped on another channel's row (Kick) nothing loads; on the Pixel Tone row it loads.
#   5. A corrupt file dropped on the picture is refused with a message; the picture stays.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_pixeltone> -DOUT=<dir> -P j184_pixeltone_pictures.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
file(MAKE_DIRECTORY "${OUT}/home")
execute_process(COMMAND "${TESTBIN}" writepng "${OUT}/Sunset.png" RESULT_VARIABLE rc1)
execute_process(COMMAND "${TESTBIN}" writebad "${OUT}/Broken.png" RESULT_VARIABLE rc2)
if(NOT rc1 EQUAL 0 OR NOT rc2 EQUAL 0 OR NOT EXISTS "${OUT}/Sunset.png")
  message(FATAL_ERROR "FAIL: could not write the synthetic pictures (${rc1} ${rc2})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_PIXEL_TRACE} "1")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.pixeltone --size 1920x1080 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 280)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.log" "${out}")
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(centre text name ox oy)
  if(NOT text MATCHES " ${name}=([0-9]+),([0-9]+),([0-9]+),([0-9]+)")
    message(FATAL_ERROR "FAIL: no ${name} rectangle in the trace: ${text}")
  endif()
  math(EXPR x "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
  math(EXPR y "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
  set(${ox} ${x} PARENT_SCOPE)
  set(${oy} ${y} PARENT_SCOPE)
endfunction()

# The data hash of the plug line at a frame.
function(hashAt text frame out label)
  if(NOT text MATCHES "plug frame ${frame}: [^\n]*undoLabel \"([^\"]*)\"[^\n]* data ([0-9a-f]+)")
    message(FATAL_ERROR "FAIL: no plug line at frame ${frame}:\n${text}")
  endif()
  set(${label} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${out} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# ---- 1. layout: the editor's rectangles, the rack rows and the channel order
run(layout 20 "10 rack\n12 channels\n")
string(REGEX MATCHALL "pixel-ui:[^\n]*" lines "${layout_out}")
list(LENGTH lines n)
if(n EQUAL 0)
  message(FATAL_ERROR "FAIL: the editor printed no trace line:\n${layout_out}")
endif()
list(GET lines -1 l)
if(NOT l MATCHES " pic=48x16 " OR NOT l MATCHES " name=-")
  message(FATAL_ERROR "FAIL: the editor does not start with the built-in picture: ${l}")
endif()
centre("${l}" load lx ly)
centre("${l}" well wx wy)
if(NOT layout_out MATCHES "geom originX ([0-9.]+) row0Y ([0-9.]+) [^\n]*rowPitch ([0-9.]+)")
  message(FATAL_ERROR "FAIL: no rack geometry:\n${layout_out}")
endif()
set(gx "${CMAKE_MATCH_1}")
set(gy "${CMAKE_MATCH_2}")
set(gp "${CMAKE_MATCH_3}")
string(REGEX REPLACE "\\..*" "" ox "${gx}")
string(REGEX REPLACE "\\..*" "" r0 "${gy}")
string(REGEX REPLACE "\\..*" "" pitch "${gp}")
string(REGEX MATCHALL "  channel [0-9]+ [^\n]* type [^ ]+" chans "${layout_out}")
set(idx 0)
set(ptRow -1)
set(otherRow -1)
foreach(c IN LISTS chans)
  if(c MATCHES " type gl\\.pixeltone")
    set(ptRow ${idx})
  elseif(otherRow EQUAL -1)
    set(otherRow ${idx})
  endif()
  math(EXPR idx "${idx} + 1")
endforeach()
if(ptRow EQUAL -1 OR otherRow EQUAL -1)
  message(FATAL_ERROR "FAIL: no Pixel Tone / other channel in the rack:\n${layout_out}")
endif()
math(EXPR rx "${ox} + 166")
math(EXPR ptY "${r0} + ${ptRow} * ${pitch}")
math(EXPR otY "${r0} + ${otherRow} * ${pitch}")

# ---- 2..5. dialog, undo, drop on the picture, rack rows, corrupt file
set(P "${OUT}/Sunset.png")
set(B "${OUT}/Broken.png")
run(flow 400 "6 plug
8 move ${lx} ${ly}
10 down 0
12 up 0
16 forget pixeltone-picture
18 dialogok \"${P}\"
20 waitfor pixeltone-picture 30
24 plug
26 undo
28 plug
30 move ${wx} ${wy}
32 forget pixeltone-picture
34 dragenter ${wx} ${wy} \"${P}\"
36 dragmove ${wx} ${wy}
38 dragmove ${wx} ${wy}
40 shot \"${OUT}/hover.ppm\"
42 drop
44 waitfor pixeltone-picture 30
48 plug
50 shot \"${OUT}/loaded.ppm\"
52 undo
54 plug
56 move ${rx} ${otY}
58 dragenter ${rx} ${otY} \"${P}\"
60 dragmove ${rx} ${otY}
62 dragmove ${rx} ${otY}
64 drop
80 plug
82 move ${rx} ${ptY}
84 forget pixeltone-picture
86 dragenter ${rx} ${ptY} \"${P}\"
88 dragmove ${rx} ${ptY}
90 dragmove ${rx} ${ptY}
92 drop
94 waitfor pixeltone-picture 30
98 plug
100 move ${wx} ${wy}
102 forget pixeltone-picture
104 dragenter ${wx} ${wy} \"${B}\"
106 dragmove ${wx} ${wy}
108 dragmove ${wx} ${wy}
110 drop
112 waitfor pixeltone-picture 30
116 plug
")
hashAt("${flow_out}" 6 h0 l0)
hashAt("${flow_out}" 24 h1 l1)
hashAt("${flow_out}" 28 h2 l2)
hashAt("${flow_out}" 48 h3 l3)
hashAt("${flow_out}" 54 h4 l4)
hashAt("${flow_out}" 80 h5 l5)
hashAt("${flow_out}" 98 h6 l6)
hashAt("${flow_out}" 116 h7 l7)
if(h1 STREQUAL h0 OR NOT l1 STREQUAL "load picture")
  message(FATAL_ERROR "FAIL: OK in the file dialog did not load the picture as one undo step (${l1}):\n${flow_out}")
endif()
if(NOT flow_out MATCHES "pixel-ui: loaded Sunset\\.png 41x23 from 160x90")
  message(FATAL_ERROR "FAIL: the 160 x 90 picture was not reduced to 41 x 23 (23 rows, aspect kept):\n${flow_out}")
endif()
if(NOT h2 STREQUAL h0 OR l2 STREQUAL "load picture")
  message(FATAL_ERROR "FAIL: undo did not bring the built-in picture back:\n${flow_out}")
endif()
if(NOT flow_out MATCHES "pixel-ui:[^\n]* hover=1 ")
  message(FATAL_ERROR "FAIL: the picture did not light up while a picture hovered it:\n${flow_out}")
endif()
if(NOT h3 STREQUAL h1 OR NOT l3 STREQUAL "load picture")
  message(FATAL_ERROR "FAIL: the picture dropped on the editor did not load:\n${flow_out}")
endif()
if(NOT h4 STREQUAL h0)
  message(FATAL_ERROR "FAIL: undo after the drop did not bring the built-in picture back:\n${flow_out}")
endif()
if(NOT h5 STREQUAL h0)
  message(FATAL_ERROR "FAIL: a picture dropped on another channel's row changed Pixel Tone:\n${flow_out}")
endif()
if(NOT h6 STREQUAL h1 OR NOT l6 STREQUAL "load picture")
  message(FATAL_ERROR "FAIL: the picture dropped on the Pixel Tone row did not load:\n${flow_out}")
endif()
if(NOT h7 STREQUAL h1 OR NOT flow_out MATCHES "pixel-ui: load-failed Broken\\.png")
  message(FATAL_ERROR "FAIL: the corrupt file was not refused, or it changed the picture:\n${flow_out}")
endif()
string(REGEX MATCHALL "pixel-ui:[^\n]*" lines "${flow_out}")
list(GET lines -1 l)
if(NOT l MATCHES " err=1 " OR NOT l MATCHES " name=Sunset\\.png" OR NOT l MATCHES " src=160x90 ")
  message(FATAL_ERROR "FAIL: the editor does not show the refused file's message over the kept picture: ${l}")
endif()
message(STATUS "j184 Pixel Tone pictures: dialog load + undo, drop highlight + load, rack rows, corrupt file refused")
