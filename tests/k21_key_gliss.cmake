# Gridline: K-21, a glissando on the piano roll's keyboard: with the mouse held, only the key under the
# pointer sounds and is lit, one at a time (the key left behind stops), FL-familiar. Runs the app hidden
# (isolated config and HOME, fixed frame time, dummy audio) editing pattern 0, channel 6 of a copy of
# the test song: press a key, drag up twelve keys one per frame, release; every frame the sounding key
# (the piano roll's preview) is the one under the pointer and at most one live note is held; after the
# release nothing sounds. Also click both white edges beside black keys, then move between a white
# edge and the black key on the same row while holding the mouse.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k21_key_gliss.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)  # a copy: never the fixture itself

function(run tag script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --edit 0 6 --hide rack
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

run(geo "6 lane\n8 quit discard\n")
if(NOT geo_out MATCHES "lane frame [0-9]+: x0 ([-0-9.]+) [^\n]* yTop ([-0-9.]+) [^\n]* pitchY0 ([-0-9.]+) rowH ([0-9.]+)")
  message(FATAL_ERROR "FAIL: no piano roll geometry\n${geo_out}")
endif()
set(raw "${CMAKE_MATCH_1};${CMAKE_MATCH_2};${CMAKE_MATCH_3};${CMAKE_MATCH_4}")  # string(REGEX) resets CMAKE_MATCH_n
set(i 0)
foreach(m IN LISTS raw)
  math(EXPR i "${i} + 1")
  string(REGEX REPLACE "\\..*" "" v${i} "${m}")
endforeach()
set(x0 ${v1})
set(laneTop ${v2})
set(py0 ${v3})
set(rowH ${v4})
math(EXPR kx "${x0} - 50")  # on the short black keys, left of their right edge
math(EXPR ymid "(${laneTop} + 200) / 2")
math(EXPR base "127 - (${ymid} - ${py0}) / ${rowH} - 6")  # twelve keys up from here stay on screen
# Press the base key, then one key up per frame (two frames per key: the move, then the check).
set(script "6 move ${kx} 0\n")
math(EXPR y "${py0} + (127 - ${base}) * ${rowH} + ${rowH} / 2")
string(APPEND script "8 move ${kx} ${y}\n10 down 0\n12 lane\n")
set(frame 12)
set(expected ${base})
foreach(k RANGE 1 12)
  math(EXPR p "${base} + ${k}")
  math(EXPR y "${py0} + (127 - ${p}) * ${rowH} + ${rowH} / 2")
  math(EXPR f1 "${frame} + 2")
  math(EXPR f2 "${frame} + 4")
  string(APPEND script "${f1} move ${kx} ${y}\n${f2} lane\n")
  list(APPEND expected ${p})
  set(frame ${f2})
endforeach()
math(EXPR fu "${frame} + 2")
math(EXPR fl "${frame} + 8")
string(APPEND script "${fu} up 0\n${fl} lane\n")
list(APPEND expected -1)

# The D between the black keys C# and D#. Its right edge spans half of each black row.
math(EXPR d "(${base} + 6) / 12 * 12 + 2")
math(EXPR dTop "${py0} + (127 - ${d}) * ${rowH}")
math(EXPR upper "${dTop} - ${rowH} / 4")
math(EXPR lower "${dTop} + ${rowH} * 5 / 4")
math(EXPR right "${x0} - 12")
math(EXPR cSharp "${d} - 1")
string(APPEND script "72 move ${right} ${upper}\n74 down 0\n76 lane\n78 up 0\n"
                     "82 move ${right} ${lower}\n84 down 0\n86 lane\n"
                     "88 move ${kx} ${lower}\n90 lane\n92 move ${right} ${lower}\n94 lane\n"
                     "96 up 0\n102 lane\n104 quit discard\n")
list(APPEND expected ${d} ${d} ${cSharp} ${d} -1)
run(gliss "${script}")

string(REGEX MATCHALL "lane frame [0-9]+: [^\n]* preview -?[0-9]+ live [0-9]+" lines "${gliss_out}")
list(LENGTH lines n)
list(LENGTH expected count)
if(NOT n EQUAL count)
  message(FATAL_ERROR "FAIL: want ${count} checks, got ${n}\n${gliss_out}")
endif()
set(k 0)
foreach(l IN LISTS lines)
  string(REGEX MATCH "preview (-?[0-9]+) live ([0-9]+)" _ "${l}")
  set(pv ${CMAKE_MATCH_1})
  set(live ${CMAKE_MATCH_2})
  list(GET expected ${k} want)
  if(NOT pv EQUAL want)
    message(FATAL_ERROR "FAIL: check ${k}: sounding ${pv}, expected ${want}\n${gliss_out}")
  endif()
  if(live GREATER 1 OR (want EQUAL -1 AND NOT live EQUAL 0))
    message(FATAL_ERROR "FAIL: check ${k}: unexpected number of live notes (${live})\n${gliss_out}")
  endif()
  math(EXPR k "${k} + 1")
endforeach()
message(STATUS "ok - white edges, black keys and glissando play the key under the pointer")
