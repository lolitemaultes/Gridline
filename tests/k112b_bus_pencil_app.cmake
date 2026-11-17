# Gridline - K-112B (R09-01): in Smooth, with automation shown, a Pencil click near the top of a bus
# track's row adds an automation point and never a region. Runs the demo hidden in Smooth (dummy
# audio, isolated config), shows the automation (A), finds the first bus track on screen ("plrows"),
# clicks with the Pencil 4 px under the row's top, saves, and checks:
#   1. no pattern or audio region lies on the bus track (before: Kick's pattern landed there);
#   2. the song has one more automation point than before;
#   3. the bus track keeps only its automation, and no other track gained a region;
#   4. (R09-10) with the typing keyboard on, R still arms recording (R plays no note on the A row).
#   cmake -DAPP=<gridline-app> -DDEMO=<Gridline.gridline.json> -DOUT=<dir> -P k112b_bus_pencil_app.cmake
cmake_minimum_required(VERSION 3.19)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(run tag input)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"logic\", \"keys.profile\": \"logic\", \"ui.menuStyle\": \"logic\"}}\n")
  file(WRITE "${cfg}/input.txt" "5 toasts clear\n${input}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  configure_file("${DEMO}" "${OUT}/${tag}.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${OUT}/${tag}.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x1400 --layout logic --layout-dev
                          --dev-input "${cfg}/input.txt" --frames 70
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_log "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# The song's automation points, and the regions (non-automation clips) per track, from a save.
function(tally file pointsVar regionsVar)
  file(READ "${file}" j)
  set(points 0)
  string(JSON na LENGTH "${j}" automations)
  if(na GREATER 0)
    math(EXPR last "${na} - 1")
    foreach(i RANGE ${last})
      string(JSON np LENGTH "${j}" automations ${i} points)
      math(EXPR points "${points} + ${np}")
    endforeach()
  endif()
  set(regions "")
  string(JSON nc LENGTH "${j}" playlist)
  math(EXPR last "${nc} - 1")
  foreach(i RANGE ${last})
    string(JSON kind ERROR_VARIABLE noKind GET "${j}" playlist ${i} kind)
    if(noKind)
      set(kind "pattern")
    endif()
    if(NOT kind STREQUAL "automation")
      string(JSON tr GET "${j}" playlist ${i} track)
      list(APPEND regions "${tr}")
    endif()
  endforeach()
  set(${pointsVar} ${points} PARENT_SCOPE)
  set(${regionsVar} "${regions}" PARENT_SCOPE)
endfunction()

# ---- the rows with automation shown
run(probe "10 key a\n24 plrows\n30 save ${OUT}/before.json\n34 quit discard\n")
string(REGEX MATCHALL "plrow [0-9]+ \"[^\"]*\" [-0-9]+ [-0-9]+ top [-0-9]+ h [-0-9]+ lane [-0-9]+ bus 1 grid [-0-9]+ [-0-9]+ [-0-9]+" busRows "${probe_log}")
set(bus -1)
foreach(l ${busRows})
  string(REGEX MATCH "plrow ([0-9]+) \"([^\"]*)\" [-0-9]+ [-0-9]+ top ([-0-9]+) h ([-0-9]+) lane ([-0-9]+) bus 1 grid ([-0-9]+) ([-0-9]+) ([-0-9]+)" m "${l}")
  if(bus LESS 0 AND CMAKE_MATCH_5 GREATER 0 AND CMAKE_MATCH_3 GREATER_EQUAL CMAKE_MATCH_7)
    math(EXPR bottom "${CMAKE_MATCH_3} + ${CMAKE_MATCH_4}")
    if(bottom LESS_EQUAL CMAKE_MATCH_8)
      set(bus ${CMAKE_MATCH_1})
      set(busName "${CMAKE_MATCH_2}")
      math(EXPR cy "${CMAKE_MATCH_3} + 4")
      math(EXPR cx "${CMAKE_MATCH_6} + 260")
    endif()
  endif()
endforeach()
check("a bus track with its automation lane is on screen (track ${bus} \"${busName}\")" bus GREATER_EQUAL 0)
tally("${OUT}/before.json" p0 r0)
list(FIND r0 ${bus} busHad)
check("the demo's bus track holds no region to begin with" busHad EQUAL -1)
list(LENGTH r0 n0)

# ---- a Pencil click 4 px under the bus row's top (above its curve)
run(pencil "10 key a\n20 pltool draw\n26 move ${cx} ${cy}\n30 down 0\n32 up 0\n40 save ${OUT}/after.json\n44 quit discard\n")
tally("${OUT}/after.json" p1 r1)
list(FIND r1 ${bus} busHas)
check("no region on the bus track after the Pencil click" busHas EQUAL -1)
list(LENGTH r1 n1)
check("no region added anywhere (${n0} -> ${n1})" n1 EQUAL n0)
math(EXPR want "${p0} + 1")
check("the click added one automation point (${p0} -> ${p1})" p1 EQUAL want)

# ---- R09-10: with the typing keyboard on, R (no note on the A row) still starts recording
run(typing "6 typing on\n12 typing state\n16 key r\n24 recording\n30 quit discard\n")
check("R arms recording while the typing keyboard is on" "${typing_log}" MATCHES "recording frame 24: armed 1")
