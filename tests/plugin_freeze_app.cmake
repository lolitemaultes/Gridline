# Gridline - plugin windows under stress, in the real app (hidden, quiet windows, an isolated config
# folder and HOME, SDL's dummy audio driver), with the in-tree CLAP test effect's interface made to
# misbehave (tests/clap/TestPlugin.cpp, GRIDLINE_TEST_GUI_*):
#   1. an interface that asks again for each size it is given (request_resize from inside set_size):
#      a user resize ends after one set-size, not one every frame (the window flickered, the UI thread
#      stayed busy), in a window of its own;
#   2. an interface that asks for a new size twenty times a second: Gridline follows, stays responsive,
#      closes it;
#   3. in the sandbox, an interface whose thread is busy for 4 s: no frame waits for it, it shows as not
#      responding (bar, panel, notice), its window still closes, and it answers again afterwards;
#   4. in the sandbox, an interface that takes 4 s to open: no frame waits for it, it opens then;
#   5. in the sandbox, an interface that never comes back: stopped after the give-up time (crashed, said
#      why), Gridline goes on;
#   6. open and close quickly, many times, sandbox on and off, inside Gridline and in its own window;
#   7. 3 and 4 in the Clips, Smooth and Tracker layouts and the Light theme.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DCLAP=<bundle> -DOUT=<dir> -P plugin_freeze_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_DEV_PLUGIN_WINDOWS} "1")
set(ENV{GRIDLINE_TEST_GAIN_GUI} "1")  # the CLAP test gain gets an interface
if(NOT APPLE)
  set(ENV{GRIDLINE_TEST_PLAIN_GUI} "1")  # (elsewhere it takes any window and draws nothing)
endif()
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1280x800
                          --dev-input "${OUT}/${tag}.txt" --plugin gl.patcher ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 150)
  file(WRITE "${OUT}/${tag}.out" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL ${tag}: gridline-app ended with ${rc}\n${out}\n${err}")
  endif()
  set(out "${out}\n${err}" PARENT_SCOPE)
endfunction()
function(must tag regex what)
  if(NOT out MATCHES "${regex}")
    message(FATAL_ERROR "FAIL (${tag}): ${what}\n${out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()
function(mustnot tag regex what)
  if(out MATCHES "${regex}")
    message(FATAL_ERROR "FAIL (${tag}): ${what}\n${out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()
# The longest plugin window idle reported at `frame` ("stalls idle N ms") is below `ms`, and so is the
# longest gap between frames (or below the optional fifth argument: a window that also holds the
# sandbox process starting, which is not waited on here and is slower on a loaded machine).
function(idle_below tag frame ms what)
  set(gap ${ms})
  if(ARGC GREATER 4)
    set(gap ${ARGV4})
  endif()
  if(NOT out MATCHES "plugwin frame ${frame}: stalls idle ([0-9]+) ms apart ([0-9]+) ms")
    message(FATAL_ERROR "FAIL (${tag}): no stalls report at frame ${frame}\n${out}")
  endif()
  if(NOT CMAKE_MATCH_1 LESS ms OR NOT CMAKE_MATCH_2 LESS gap)
    message(FATAL_ERROR "FAIL (${tag}): ${what}: idle ${CMAKE_MATCH_1} ms, frames ${CMAKE_MATCH_2} ms apart (limits ${ms}, ${gap})\n${out}")
  endif()
  message(STATUS "ok - ${what} (idle ${CMAKE_MATCH_1} ms, frames ${CMAKE_MATCH_2} ms apart)")
endfunction()
function(count tag regex)
  string(REGEX MATCHALL "${regex}" hits "${out}")
  list(LENGTH hits n)
  set(n ${n} PARENT_SCOPE)
endfunction()

# The sandbox embeds interfaces on Linux; elsewhere (macOS: a window of its process; Windows, the BSDs:
# Gridline's panel) it shows them in other ways, macOS's tested here.
set(SANDBOX_UI ON)
if(WIN32 OR NOT (APPLE OR CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"))
  set(SANDBOX_UI OFF)
endif()

# 1. Asked again for each size: one user resize, one set-size (each further one a loop).
set(ENV{GRIDLINE_PLUGIN_WINDOWS} "own")
set(ENV{GRIDLINE_TEST_GUI_RESIZE} "echo")
set(ENV{GRIDLINE_TEST_LOG_GUI} "1")
run(echo "10 plugwin scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
30 plugwin stalls
140 plugwin stalls
144 plugwin info
150 quit discard
" --plugin-window-resize 500x400)
must(echo "plugwin frame 144: 1 windows\n  plugwin [^\n]* external 0 window 0x[1-9a-f][0-9a-f]* [^\n]* resizable 1" "the resizable interface opens in a window of its own")
must(echo "testgui set_size [0-9]+x[0-9]+" "the user's resize reaches the plugin")
count(echo "testgui set_size")
if(n GREATER 3)
  message(FATAL_ERROR "FAIL (echo): ${n} set-size calls for one user resize (the plugin's echo made a loop)\n${out}")
endif()
message(STATUS "ok - one user resize: ${n} set-size call(s), no loop")
idle_below(echo 140 250 "no plugin window idle held a frame up")

# 2. A new size twenty times a second, for two seconds: followed, the window still closes.
set(ENV{GRIDLINE_TEST_GUI_RESIZE} "loop")
run(loop "10 plugwin scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
30 plugwin stalls
32 waitfor never 2
40 plugwin stalls
44 plugwin close
60 plugwin info
64 quit discard
")
must(loop "plugwin frame 40: stalls [^\n]*\n  plugwin [^\n]* resizes ([1-9][0-9]+) responding 1" "every size it asks for is taken")
idle_below(loop 40 250 "resizes twenty times a second hold no frame up")
must(loop "plugwin frame 60: 0 windows" "it closes")
unset(ENV{GRIDLINE_TEST_GUI_RESIZE})
unset(ENV{GRIDLINE_TEST_LOG_GUI})
unset(ENV{GRIDLINE_PLUGIN_WINDOWS})

if(NOT SANDBOX_UI)
  message(STATUS "SKIP: sandboxed interfaces are Gridline's panel on this system (1, 2 checked)")
  return()
endif()

# 3. Sandboxed, its interface thread busy for 4 s half a second after it shows.
set(ENV{GRIDLINE_SANDBOX_ALL} "1")
set(ENV{GRIDLINE_TEST_GUI_BUSY_MS} "4000")
function(busy tag)
  run(${tag} "10 plugwin scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
16 waitfor never 1
40 plugwin stalls
42 waitfor never 2
50 plugwin stalls
54 plugwin drawn
56 waitfor never 3
70 plugwin stalls
74 plugwin drawn
80 quit discard
" ${ARGN})
  must(${tag} "testgui busy 4000 ms" "${tag}: the interface's thread is busy")
  must(${tag} "plugwin frame 50: stalls [^\n]*\n  plugwin [^\n]* responding 0 crashed 0" "${tag}: shown as not responding meanwhile, not crashed")
  must(${tag} "\\[plugin\\] Gridline Test Gain \\(CLAP\\): not responding" "${tag}: the notice is given")
  idle_below(${tag} 50 400 "${tag}: no frame waits for the busy plugin")
  must(${tag} "plugwin frame 70: stalls [^\n]*\n  plugwin [^\n]* responding 1 crashed 0" "${tag}: it answers again, still open, not crashed")
  must(${tag} "responding again" "${tag}: a notice says it answers again")
endfunction()
busy(busy)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  must(busy "plugwin frame 54: drawn [^\n]*not responding" "the plugin's panel says it is not responding")
endif()

# 3b. Closed while busy: at once, and it opens again afterwards.
run(busyclose "10 plugwin scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
16 waitfor never 2
38 plugwin stalls
40 plugwin close
44 plugwin stalls
48 plugwin info
50 waitfor never 4
60 plugwin fx 1 0
62 waitfor never 1
80 plugwin stalls
84 plugwin info
90 quit discard
")
must(busyclose "plugwin frame 48: 0 windows" "a busy plugin's window closes")
idle_below(busyclose 44 400 "closing it waits for nothing")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")  # (the sandbox embeds its window in Gridline's container)
  must(busyclose "container kept until its process answers[^\n]*\n(.*\n)*.*container destroyed after its process answered" "the embed container outlives the busy interface, then goes")
endif()
must(busyclose "plugwin frame 80: stalls [^\n]* windows 1 [^\n]*\n  plugwin [^\n]* responding 1 crashed 0" "it opens again once it answers")
unset(ENV{GRIDLINE_TEST_GUI_BUSY_MS})

# 4. Sandboxed, opening its interface takes 4 s.
set(ENV{GRIDLINE_TEST_GUI_OPEN_MS} "4000")
run(slowopen "10 plugwin scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
16 waitfor never 2
40 plugwin stalls
42 waitfor never 4
60 plugwin stalls
64 plugwin info
70 quit discard
")
must(slowopen "testgui opening for 4000 ms" "the interface opens slowly")
idle_below(slowopen 40 600 "no frame waits for it to open (4 s)" 2000)
must(slowopen "plugwin frame 60: stalls [^\n]* windows 1 [^\n]*\n  plugwin [^\n]* responding 1 crashed 0" "it opens, then answers")
mustnot(slowopen "its interface could not be opened" "no refusal reported")
unset(ENV{GRIDLINE_TEST_GUI_OPEN_MS})

# 5. Sandboxed, an interface thread that never comes back: stopped after the give-up time.
set(ENV{GRIDLINE_TEST_GUI_BUSY_MS} "60000")
set(ENV{GRIDLINE_DEV_SANDBOX_GIVE_UP_MS} "3000")
run(stuck "10 plugwin scan clap ${CLAP}
14 plugwin pick 1 0 clap:org.gridline.test.gain
16 waitfor never 0.3
18 plugwin stalls
20 waitfor never 6
40 plugwin stalls
44 hostedparam fx 1 0
50 quit discard
")
must(stuck "crashed \\(the plugin stopped responding\\)" "a plugin that never answers is stopped, and the notice says why")
mustnot(stuck "responding again" "a plugin that was stopped is never said to answer again")
must(stuck "hostedparam frame 44: instance 1 sandboxed 1 crashed 1" "it counts as crashed (silenced, reload offered)")
idle_below(stuck 40 600 "Gridline went on meanwhile")
unset(ENV{GRIDLINE_TEST_GUI_BUSY_MS})
unset(ENV{GRIDLINE_DEV_SANDBOX_GIVE_UP_MS})

# 6. Opened and closed quickly, again and again: sandbox on and off, inside Gridline and in its own window.
foreach(mode sandbox plain)
  if(mode STREQUAL "sandbox")
    set(ENV{GRIDLINE_SANDBOX_ALL} "1")
  else()
    unset(ENV{GRIDLINE_SANDBOX_ALL})
  endif()
  foreach(place inside own)
    set(ENV{GRIDLINE_PLUGIN_WINDOWS} "${place}")
    set(script "10 plugwin scan clap ${CLAP}\n14 plugwin pick 1 0 clap:org.gridline.test.gain\n")
    set(f 30)
    foreach(i RANGE 1 12)
      math(EXPR c "${f} + 2")
      math(EXPR n "${f} + 4")
      string(APPEND script "${f} plugwin close\n${c} plugwin fx 1 0\n")
      set(f ${n})
    endforeach()
    math(EXPR s1 "${f} + 30")
    math(EXPR s2 "${f} + 34")
    math(EXPR q "${f} + 40")
    string(APPEND script "${s1} plugwin stalls\n${s2} plugwin info\n${q} quit discard\n")
    run(quick-${mode}-${place} "${script}")
    must(quick-${mode}-${place} "plugwin frame ${s2}: 1 windows" "${mode}, ${place}: one window after twelve quick closes and opens")
    mustnot(quick-${mode}-${place} "crashed \\(|could not be opened|stopped responding" "${mode}, ${place}: nothing crashed or refused")
    idle_below(quick-${mode}-${place} ${s1} 1000 "${mode}, ${place}: no long stall")
  endforeach()
endforeach()
unset(ENV{GRIDLINE_PLUGIN_WINDOWS})

# 7. The busy and the slow interface in the other layouts and the Light theme.
set(ENV{GRIDLINE_SANDBOX_ALL} "1")
set(ENV{GRIDLINE_TEST_GUI_BUSY_MS} "4000")
foreach(lay live logic tracker)
  busy(busy-${lay} --layout ${lay})
endforeach()
busy(busy-light --light)
unset(ENV{GRIDLINE_TEST_GUI_BUSY_MS})
unset(ENV{GRIDLINE_SANDBOX_ALL})
