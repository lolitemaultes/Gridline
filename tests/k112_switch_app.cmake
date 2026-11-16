# Gridline - K-112: switching layouts keeps everything and never shows a blank frame, fast on a big project.
#   1. state: during playback, with a selection (channel, clips, a note, a mixer track), the layouts switch
#      Studio -> Live -> Logic -> Tracker -> Studio -> Logic -> Studio (the region shells, as development
#      layouts). On every frame from each switch on: the project is the same (hash), the selection is the
#      same, playback goes on and moves, and the frame is either fully laid out or covered by the
#      snapshot of the frame before the switch (shown at full strength on the two frames the new
#      layout settles in). Back in Studio, the windows open before are open again. The song saved at
#      exit equals the one saved by a run that never switched.
#   2. speed: a generated big project (400 channels more, 100 mixer tracks, 2000 clips) switches
#      through every layout in under 50 ms each, the whole frame included (first visits too); a
#      software renderer has its own budget (see below).
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112_switch_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(failures 0)
macro(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR failures "${failures} + 1")
endmacro()

function(run tag script)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true}}\n")
  file(WRITE "${cfg}/input.txt" "${script}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${OUT}/${tag}.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${OUT}/${tag}.gridline.json" --hidden --no-ui-sounds --size 1440x900 --dev-input "${cfg}/input.txt"
                          ${ARGN} --screenshot "${OUT}/${tag}.ppm" --save-at-exit "${OUT}/${tag}.saved.gridline.json"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "FAIL: ${tag}: gridline-app failed (${rc})")
  endif()
endfunction()

# ---- 1. state ----
# K-306: the Tracker's pattern editor follows the song while it plays (Follow, on by default: it
# shows, and so selects, the pattern under the playhead). The playhead moves in real time, so on a
# slow runner it reached the song's second pattern (tick 1152) by the switch into the Tracker, and the
# selection changed by design. Follow is off here: this test is about what a switch itself keeps
# (k106_tracker covers Follow).
set(sel "5 toasts clear\n6 tracker follow off\n8 layout select\n15 layout state\n")
run(still "${sel}" --play --frames 150)
run(switched "${sel}" --play --frames 150 --switch-check --switch-layout live@20 --switch-layout logic@40
    --switch-layout tracker@60 --switch-layout studio@80 --switch-layout logic@100 --switch-layout studio@120)
file(SHA256 "${OUT}/still.saved.gridline.json" a)
file(SHA256 "${OUT}/switched.saved.gridline.json" b)
if(NOT a STREQUAL b)
  fail("switching layouts during playback changed the song")
endif()
file(READ "${OUT}/switched.log" log)
string(REGEX MATCH "frame 15 layoutstate layout studio channel ([0-9]+) pattern ([0-9]+) mixer ([0-9]+) clips \\[([^]]*)\\] notes \\[([^]]*)\\] playing 1 tick [0-9]+ [^\n]* project ([0-9a-f]+) snapshot [^\n]* open \\[([^]]*)\\]" base "${log}")
if(NOT base)
  fail("no state line before the switches")
endif()
set(selKey "channel ${CMAKE_MATCH_1} pattern ${CMAKE_MATCH_2} mixer ${CMAKE_MATCH_3} clips [${CMAKE_MATCH_4}] notes [${CMAKE_MATCH_5}]")
set(project "${CMAKE_MATCH_6}")
set(studioOpen "${CMAKE_MATCH_7}")
if(CMAKE_MATCH_4 STREQUAL "" OR CMAKE_MATCH_1 STREQUAL "0")
  fail("the selection was not made (${selKey})")
endif()
string(REGEX MATCHALL "frame [0-9]+ layoutstate [^\n]*" lines "${log}")
list(LENGTH lines n)
if(n LESS 60)
  fail("too few switch frames logged (${n})")
endif()
set(lastTick -1)
set(layoutsSeen "")
foreach(l IN LISTS lines)
  string(REGEX MATCH "^frame ([0-9]+) layoutstate layout ([a-z]+) " _ "${l}")
  set(f "${CMAKE_MATCH_1}")
  set(lay "${CMAKE_MATCH_2}")
  list(APPEND layoutsSeen ${lay})
  string(FIND "${l}" "${selKey}" at)
  if(at LESS 0)
    fail("frame ${f} (${lay}): the selection changed: ${l}")
  endif()
  string(FIND "${l}" "project ${project}" at)
  if(at LESS 0)
    fail("frame ${f} (${lay}): the project changed")
  endif()
  if(NOT l MATCHES "playing 1 tick ([0-9]+)")
    fail("frame ${f} (${lay}): playback stopped")
  endif()
  string(REGEX MATCH "open \\[([^]]*)\\]" _ "${l}")
  set(open "${CMAKE_MATCH_1}")
  # The frames from each switch on carry the layout check; the line before the switches has none.
  string(REGEX MATCH "snapshot (-?[0-9.]+) open \\[[^]]*\\] check (.*)$" checked "${l}")
  set(alpha "${CMAKE_MATCH_1}")
  set(check "${CMAKE_MATCH_2}")
  if(checked AND NOT check STREQUAL "ok" AND NOT alpha STREQUAL "1.00")
    fail("frame ${f} (${lay}): a window was not laid out (${check}) and no snapshot covered it (${alpha})")
  endif()
  string(FIND "${open}" "Playlist" at)
  # N45-10: Clips opens a set in the view it was saved in (the clip grid unless saved in the
  # arrangement): its main view there is the clip grid (Live pads), in place of the playlist.
  if(at LESS 0 AND lay STREQUAL "live")
    string(FIND "${open}" "Live pads" at)
  endif()
  if(at LESS 0)
    fail("frame ${f} (${lay}): the playlist is not open")
  endif()
endforeach()
foreach(want live logic tracker studio)
  list(FIND layoutsSeen ${want} at)
  if(at LESS 0)
    fail("the ${want} layout was never on screen")
  endif()
endforeach()
# Each switch frame and the next are covered by the snapshot.
string(REGEX MATCHALL "\\[layout-switch\\] frame [0-9]+ to [a-z]+ took" sw "${log}")
list(LENGTH sw nSwitch)
if(NOT nSwitch EQUAL 6)
  fail("expected 6 switches, saw ${nSwitch}")
endif()
foreach(s IN LISTS sw)
  string(REGEX MATCH "frame ([0-9]+)" _ "${s}")
  set(f "${CMAKE_MATCH_1}")
  math(EXPR f1 "${f} + 1")
  foreach(ff ${f} ${f1})
    if(NOT log MATCHES "frame ${ff} layoutstate [^\n]* snapshot 1.00 ")
      fail("frame ${ff}: the switch snapshot was not drawn at full strength")
    endif()
  endforeach()
endforeach()
# Back in Studio (the last switch, at 120): the windows open before the first switch are open.
string(REGEX MATCH "frame 13[0-2] layoutstate layout studio [^\n]* open \\[([^]]*)\\]" back "${log}")
if(NOT back)
  fail("no Studio state line after the last switch")
elseif(NOT CMAKE_MATCH_1 STREQUAL studioOpen)
  fail("back in Studio the open windows are [${CMAKE_MATCH_1}], before they were [${studioOpen}]")
endif()

# ---- 2. speed ----
# The whole frame of every switch (first visits included) costs the interface thread under 50 ms
# of CPU time: its own work, whatever else runs on the machine (ctest -j4 next to it). The wall
# time is held too, so a stall that spends no CPU (a wait, a lock, a disk read) cannot hide: the
# median under 50 ms and every frame under 100 ms. The app leaves its own state check (the line
# this test reads, with the big project's hash) out of the figures (K-291).
#
# A software renderer (the Linux CI runners: Mesa's llvmpipe under Xvfb and Weston) draws on the
# CPU: the app reports the CPU time spent in the GL calls ("gl") and the interface is held to its
# own CPU time without it. There the wall time also carries the rasteriser (its threads share the
# runner with ctest -j4; a texture changed while the last frame is still being rasterised waits for
# it in whatever part of the frame changes it), and the runners' interface CPU time measured 1.25 to
# 1.6 times a developer Mac's for the same frames (first CI run, d179fb8: Smooth's first visit 64 ms
# against 41 ms on the Intel Mac these budgets were set on). So with a software renderer:
# the interface's own CPU time under 100 ms (a doubling still fails), every frame under 400 ms and
# the median under 200 ms (the CI runners measured 58 to 199 ms, median 73 to 113 ms).
#
# K-291: a machine busy with other work (parallel builds) slows single frames, CPU time included
# (shared caches and cores). The run is made up to three times, each in a fresh process, and each
# switch is held to its best run (least CPU time, least wall time): a load spike passes on a later
# run, while a real regression is slow in every run and still fails.
#
# K-297: the machine's load around each timed run, for every speed failure (a run next to a full
# build or ctest -j fails the wall-time checks without anything being slower).
function(machine_load var)
  set(l "unknown")
  if(EXISTS "/proc/loadavg")
    file(READ "/proc/loadavg" l)
  elseif(APPLE)
    execute_process(COMMAND sysctl -n vm.loadavg OUTPUT_VARIABLE l ERROR_QUIET)
  endif()
  string(REGEX REPLACE "[{}\n]" "" l "${l}")
  string(STRIP "${l}" l)
  set(${var} "${l}" PARENT_SCOPE)
endfunction()
set(cpuMax 50)
set(stallMax 100)
set(medianMax 50)
set(attempts 0)
set(loadNote "")
foreach(attempt 1 2 3)
  set(attempts ${attempt})
  machine_load(loadBefore)
  run(big${attempt} "5 toasts clear\n8 layout big\n" --frames 90 --switch-check --switch-layout live@30 --switch-layout logic@40
      --switch-layout tracker@50 --switch-layout studio@60 --switch-layout live@70 --switch-layout tracker@80)
  machine_load(loadAfter)
  string(APPEND loadNote "run ${attempt}: load average before ${loadBefore}, after ${loadAfter}; ")
  message(STATUS "big project run ${attempt}: load average before ${loadBefore}, after ${loadAfter}")
  file(READ "${OUT}/big${attempt}.log" blog)
  # K-297: each switch by its number and the layout it went to, for the failure messages.
  string(REGEX MATCHALL "\\[layout-switch\\] frame [0-9]+ to [a-z]+ took" swb "${blog}")
  set(swNo 0)
  foreach(s IN LISTS swb)
    math(EXPR swNo "${swNo} + 1")
    string(REGEX MATCH "frame ([0-9]+) to ([a-z]+)" _ "${s}")
    set(swName_${CMAKE_MATCH_1} "switch ${swNo} of 6 (frame ${CMAKE_MATCH_1}, to ${CMAKE_MATCH_2})")
    math(EXPR swIdx "${swNo} - 1")
    set(swName_i${swIdx} "switch ${swNo} of 6 (frame ${CMAKE_MATCH_1}, to ${CMAKE_MATCH_2})")
  endforeach()
  if(NOT blog MATCHES "big project: ([0-9]+) channels ([0-9]+) mixer ([0-9]+) clips")
    fail("the big project was not made")
    break()
  elseif(CMAKE_MATCH_1 LESS 400 OR CMAKE_MATCH_3 LESS 2000)
    fail("the big project is too small (${CMAKE_MATCH_1} channels, ${CMAKE_MATCH_3} clips)")
    break()
  endif()
  string(REGEX MATCHALL "\\[layout-switch\\] frame [0-9]+ to [a-z]+ took [0-9.]+ ms" took "${blog}")
  string(REGEX MATCHALL "\\[layout-switch\\] frame [0-9]+ whole frame [0-9.]+ ms cpu [0-9.]+ ms gl [0-9.]+ ms renderer [a-z]+" whole "${blog}")
  list(LENGTH took nb)
  list(LENGTH whole nw)
  if(NOT nb EQUAL 6 OR NOT nw EQUAL 6)
    fail("big project: expected 6 switches and 6 whole switch frames, saw ${nb} and ${nw}")
    break()
  endif()
  if(blog MATCHES "renderer software")
    set(cpuMax 100)
    set(stallMax 400)
    set(medianMax 200)
  endif()
  set(i 0)
  foreach(t IN LISTS took)
    string(REGEX MATCH "to ([a-z]+) took ([0-9]+)\\.[0-9]+ ms" _ "${t}")
    set(to_${i} "${CMAKE_MATCH_1}")
    if(attempt EQUAL 1 OR CMAKE_MATCH_2 LESS best_took_${i})
      set(best_took_${i} "${CMAKE_MATCH_2}")
      set(took_line_${i} "${t}")
    endif()
    math(EXPR i "${i} + 1")
  endforeach()
  set(i 0)
  foreach(t IN LISTS whole)
    string(REGEX MATCH "frame ([0-9]+) whole frame ([0-9]+)\\.[0-9]+ ms cpu ([0-9]+)\\.[0-9]+ ms gl ([0-9]+)\\.[0-9]+ ms renderer ([a-z]+)" _ "${t}")
    set(f "${CMAKE_MATCH_1}")
    set(ms "${CMAKE_MATCH_2}")
    set(cpu "${CMAKE_MATCH_3}")
    if(CMAKE_MATCH_5 STREQUAL "software")
      math(EXPR cpu "${cpu} - ${CMAKE_MATCH_4}")  # the interface's own CPU time
    endif()
    if(attempt EQUAL 1 OR cpu LESS best_cpu_${i})
      set(best_cpu_${i} "${cpu}")
      string(REGEX MATCHALL "\\[frame-trace\\] frame ${f} [^\n]*" trace "${blog}")
      string(REPLACE "[frame-trace] frame ${f} " "" trace "${trace}")
      set(cpu_line_${i} "${t}; parts: ${trace}")
    endif()
    if(attempt EQUAL 1 OR ms LESS best_wall_${i})
      set(best_wall_${i} "${ms}")
      set(wall_line_${i} "${t}")
    endif()
    math(EXPR i "${i} + 1")
  endforeach()
  # Every switch within its budgets on its best run so far: no further run.
  set(ok TRUE)
  set(walls "")
  foreach(i RANGE 5)
    list(APPEND walls ${best_wall_${i}})
    if(best_took_${i} GREATER_EQUAL 50 OR best_cpu_${i} GREATER_EQUAL cpuMax OR best_wall_${i} GREATER_EQUAL stallMax)
      set(ok FALSE)
    endif()
  endforeach()
  list(SORT walls COMPARE NATURAL)
  list(GET walls 3 median)  # the upper middle of six
  if(median GREATER_EQUAL medianMax)
    set(ok FALSE)
  endif()
  if(ok)
    break()
  endif()
  message(STATUS "big project run ${attempt}: a switch over budget, running again (the machine may be busy)")
endforeach()
if(blog MATCHES "renderer software")
  message(STATUS "software renderer: interface CPU < ${cpuMax} ms without the GL calls, frames < ${stallMax} ms, median < ${medianMax} ms")
endif()
set(table "")
foreach(i RANGE 5)
  if(NOT DEFINED best_cpu_${i})
    break()
  endif()
  set(name "${swName_i${i}}")
  string(APPEND table "\n    ${name}: best whole frame ${best_wall_${i}} ms, cpu ${best_cpu_${i}} ms")
  if(best_took_${i} GREATER_EQUAL 50)
    fail("big project: [switch call] ${name}: the switch itself took 50 ms or more in every run (${attempts}): ${took_line_${i}} (${loadNote})")
  else()
    message(STATUS "ok - ${took_line_${i}}")
  endif()
  if(best_cpu_${i} GREATER_EQUAL cpuMax)
    fail("big project: [cpu check] ${name}: the interface took ${cpuMax} ms of CPU or more in every run (${attempts}), best ${best_cpu_${i}} ms: ${cpu_line_${i}}; ${loadNote}")
  elseif(best_wall_${i} GREATER_EQUAL stallMax)
    fail("big project: [stall check] ${name}: the whole frame took ${stallMax} ms or more in every run (${attempts}), best ${best_wall_${i}} ms: ${wall_line_${i}}; ${loadNote}")
  else()
    message(STATUS "ok - switch to ${to_${i}}: best of ${attempts} run(s) ${best_cpu_${i}} ms CPU, ${best_wall_${i}} ms")
  endif()
endforeach()
if(DEFINED median)
  if(median GREATER_EQUAL medianMax)
    fail("big project: [median check] the median switch frame took ${median} ms (${medianMax} ms or more); ${loadNote}each switch:${table}")
  else()
    message(STATUS "ok - median switch frame ${median} ms")
  endif()
endif()

# Icons the new layout shows at sizes not made yet are drawn from a size already made on the
# switch frame and made exactly over the next frames: none is still waiting 8 frames on.
string(REGEX MATCHALL "\\[layout-switch\\] frame [0-9]+ icons pending [0-9]+" pend "${blog}")
list(LENGTH pend np)
if(NOT np EQUAL 6)
  fail("big project: expected 6 icon checks, saw ${np}")
endif()
foreach(t IN LISTS pend)
  if(NOT t MATCHES "pending 0$")
    string(REGEX MATCH "frame ([0-9]+) icons" _ "${t}")
    math(EXPR fs "${CMAKE_MATCH_1} - 8")
    fail("big project: [icons check] ${swName_${fs}}: icon sizes still waiting 8 frames on: ${t}")
  endif()
endforeach()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - switching keeps the project, selection, playback and windows, with no blank frame, fast")
