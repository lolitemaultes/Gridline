# Gridline — J-182: Kerf's time and marker settings in the real app (hidden, isolated config).
#   1. A synthetic break loads; the Snap menu sets 1/3 beat (triplets) and a click on the ruler
#      switches it to bars:beats (the editor's trace line reports both).
#   2. A marker dragged in the wave view lands on the triplet grid; a double-click adds one on it.
#   3. The region panel names the selected region (typed, Enter) and moves its marker to a typed
#      bars:beats:ticks position; the Right arrow over the wave nudges it one snap step; one undo
#      takes the nudge back.
#   4. The same at 1280 x 800 (the editor scaled to fit): the settings respond there too.
#   Frames (shot) are kept in OUT for review.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P j182_kerf_time.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_KERF_TRACE} "1")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(run tag size frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.slicer --size ${size} --frames ${frames} --screenshot "${OUT}/${tag}.ppm"
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 280)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.log" "${out}")
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# The centre of a traced rectangle "name=x0,y0,x1,y1" (the last one printed): <out>_x / <out>_y.
function(centre text name out)
  string(REGEX MATCHALL " ${name}=[0-9]+,[0-9]+,[0-9]+,[0-9]+" all "${text}")
  list(LENGTH all n)
  if(n EQUAL 0)
    message(FATAL_ERROR "FAIL: the editor never traced '${name}'. Output:\n${text}")
  endif()
  math(EXPR i "${n} - 1")
  list(GET all ${i} r)
  string(REGEX MATCH "=([0-9]+),([0-9]+),([0-9]+),([0-9]+)" _ "${r}")
  math(EXPR cx "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
  math(EXPR cy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
  set(${out}_x ${cx} PARENT_SCOPE)
  set(${out}_y ${cy} PARENT_SCOPE)
  set(${out}_x0 ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${out}_x1 ${CMAKE_MATCH_3} PARENT_SCOPE)
endfunction()

# A field of the last trace line that matches `cond` (a regex), else of the last line.
function(field text key cond out)
  string(REGEX MATCHALL "kerf-ui:[^\n]*" lines "${text}")
  set(v "")
  foreach(l IN LISTS lines)
    if(l MATCHES "${cond}" AND l MATCHES " ${key}=([^ ]*)")
      set(v "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  set(${out} "${v}" PARENT_SCOPE)
endfunction()

# |frame - round(frame / step) * step| <= 1 for a step of fpb / div (integer maths on milli-frames).
function(ongrid frame fpb div out)
  string(REGEX REPLACE "\\..*" "" fpbInt "${fpb}")
  string(REGEX MATCH "\\.([0-9]+)" _ "${fpb}")
  set(frac "${CMAKE_MATCH_1}000")
  string(SUBSTRING "${frac}" 0 3 frac)
  math(EXPR stepM "(${fpbInt} * 1000 + ${frac}) / ${div}")
  math(EXPR fM "${frame} * 1000")
  math(EXPR k "(${fM} + ${stepM} / 2) / ${stepM}")
  math(EXPR d "${fM} - ${k} * ${stepM}")
  if(d LESS 0)
    math(EXPR d "0 - ${d}")
  endif()
  if(d LESS_EQUAL 1500)
    set(${out} 1 PARENT_SCOPE)
  else()
    set(${out} 0 PARENT_SCOPE)
  endif()
endfunction()

function(flow tag size)
  # Probe: where the drop zone is, then (loaded) the snap button, the menu's 1/3 beat item, the ruler,
  # the REGION tab, the marker of region 2, the wave view, the region panel's fields.
  run(${tag}_probe0 ${size} 20 "# nothing\n")
  centre("${${tag}_probe0_out}" dropzone dz)
  set(load "10 dragenter ${dz_x} ${dz_y} \"${OUT}/Break 96.wav\"
12 dragmove ${dz_x} ${dz_y}
14 dragmove ${dz_x} ${dz_y}
16 drop
")
  centre("${${tag}_probe0_out}" snapbtn sb)
  centre("${${tag}_probe0_out}" tab_region tr)
  run(${tag}_probe1 ${size} 60 "${load}30 move ${sb_x} ${sb_y}
32 down 0
34 up 0
40 move ${sb_x} ${sb_y}
")
  centre("${${tag}_probe1_out}" snapitem6 s6)
  centre("${${tag}_probe1_out}" ruler ru)
  centre("${${tag}_probe1_out}" mk1 m1)
  centre("${${tag}_probe1_out}" wave wv)
  run(${tag}_probe2 ${size} 40 "${load}26 move ${tr_x} ${tr_y}
27 down 0
28 up 0
")
  centre("${${tag}_probe2_out}" namefld nf)
  centre("${${tag}_probe2_out}" posfld pf)
  # Drag region 2's marker 30 px right; double-click at 5 2/3 beats of the 8-beat loop (a triplet line).
  math(EXPR dragTo "${m1_x} + 30")
  math(EXPR addX "${wv_x0} + (${wv_x1} - ${wv_x0}) * 7083 / 10000")
  run(${tag} ${size} 200 "${load}30 move ${sb_x} ${sb_y}
32 down 0
34 up 0
40 move ${s6_x} ${s6_y}
42 down 0
44 up 0
46 shot \"${OUT}/${tag}_snap.ppm\"
50 move ${ru_x} ${ru_y}
52 down 0
54 up 0
60 move ${tr_x} ${tr_y}
62 down 0
64 up 0
70 move ${m1_x} ${wv_y}
72 down 0
74 move ${m1_x} ${wv_y}
76 move ${dragTo} ${wv_y}
78 move ${dragTo} ${wv_y}
80 up 0
90 move ${addX} ${wv_y}
92 down 0
93 up 0
94 down 0
95 up 0
100 move ${nf_x} ${nf_y}
102 down 0
104 up 0
110 text Kick one
112 shot \"${OUT}/${tag}_rename.ppm\"
114 key enter
120 move ${pf_x} ${pf_y}
122 down 0
124 up 0
130 text 2:1:000
132 shot \"${OUT}/${tag}_position.ppm\"
134 key enter
140 move ${wv_x} ${wv_y}
142 key right
150 move 5 5
160 undo
170 move 5 5
")
  set(o "${${tag}_out}")
  field("${o}" snap " regions=[1-9]" snap)
  field("${o}" rulerunit " regions=[1-9]" ruler)
  field("${o}" fpb " regions=[1-9]" fpb)
  if(NOT snap STREQUAL "6" OR NOT ruler STREQUAL "1")
    message(FATAL_ERROR "FAIL (${tag}): the Snap menu / ruler click did not take (snap ${snap}, ruler ${ruler}). Output:\n${o}")
  endif()
  # The drag: the first trace line on the REGION tab after the drag with a new selected frame.
  string(REGEX MATCHALL "kerf-ui:[^\n]*" lines "${o}")
  set(dragged "")
  set(added "")
  set(named 0)
  set(typed "")
  set(nudged "")
  set(undone "")
  foreach(l IN LISTS lines)
    string(REGEX MATCH " selframe=([-0-9]+)" _ "${l}")
    set(sf "${CMAKE_MATCH_1}")
    string(REGEX MATCH " lastadd=([-0-9]+)" _ "${l}")
    set(la "${CMAKE_MATCH_1}")
    string(REGEX MATCH " startsum=([-0-9]+)" _ "${l}")
    set(ss "${CMAKE_MATCH_1}")
    if(dragged STREQUAL "" AND l MATCHES " tab=1 " AND NOT sf STREQUAL "0" AND NOT sf STREQUAL "-1")
      set(dragged "${sf}")
    endif()
    if(added STREQUAL "" AND NOT la STREQUAL "-1")
      set(added "${la}")
    endif()
    if(l MATCHES "name=\\[Kick one\\]")
      set(named 1)
    endif()
    if(named AND sf STREQUAL "110250" AND typed STREQUAL "")
      set(typed "${sf}")
      set(typedSum "${ss}")
    endif()
    if(NOT typed STREQUAL "" AND NOT sf STREQUAL "110250" AND nudged STREQUAL "")
      set(nudged "${sf}")
    elseif(NOT nudged STREQUAL "" AND ss STREQUAL typedSum)
      set(undone 1)  # the markers are back where they were before the nudge
    endif()
  endforeach()
  if(dragged STREQUAL "")
    message(FATAL_ERROR "FAIL (${tag}): the dragged marker did not move. Output:\n${o}")
  endif()
  ongrid(${dragged} ${fpb} 3 g)
  if(NOT g)
    message(FATAL_ERROR "FAIL (${tag}): the dragged marker landed at ${dragged}, off the 1/3 beat grid (beat ${fpb} frames)")
  endif()
  if(added STREQUAL "")
    message(FATAL_ERROR "FAIL (${tag}): the double-click added no marker. Output:\n${o}")
  endif()
  ongrid(${added} ${fpb} 3 g)
  if(NOT g)
    message(FATAL_ERROR "FAIL (${tag}): the added marker is at ${added}, off the 1/3 beat grid")
  endif()
  if(NOT named)
    message(FATAL_ERROR "FAIL (${tag}): the region was not renamed. Output:\n${o}")
  endif()
  if(typed STREQUAL "")
    message(FATAL_ERROR "FAIL (${tag}): the typed position 2:1:000 (110250) did not move the marker. Output:\n${o}")
  endif()
  # One snap step right: 2:1:000 + 1/3 beat = 110250 + 9187.5.
  if(NOT nudged STREQUAL "119438")
    message(FATAL_ERROR "FAIL (${tag}): the Right arrow nudged to '${nudged}', not 119438. Output:\n${o}")
  endif()
  if(NOT undone)
    message(FATAL_ERROR "FAIL (${tag}): one undo did not take the nudge back. Output:\n${o}")
  endif()
endfunction()

flow(full 1440x960)
flow(small 1280x800)
message(STATUS "j182 Kerf time: snap menu, bars:beats ruler, drag and double-click on the triplet grid, rename, typed position, arrow nudge, undo (1440x960, 1280x800)")
