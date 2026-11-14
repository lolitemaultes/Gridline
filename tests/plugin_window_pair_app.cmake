# Gridline - plugin windows of two instances of the same plugin, and the mouse in a plugin's own
# interface, in the real app (hidden, an isolated config folder and HOME, SDL's dummy audio driver):
#   1. GitHub issue #47: two channels with the same plugin (the in-tree CLAP and VST3 test synths):
#      both interface windows open, stay open, and each closes on its own; each is shown with
#      the main window as its owner (set before it is shown, except on macOS: a child window);
#   2. GitHub issue #7: a drag in a plugin's own interface (an on-screen keyboard played by
#      dragging across it) reaches the interface from the press to the release, every drag
#      included (macOS: the drag is posted to the window as the mouse does it).
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DCLAP=<bundle> -DVST3=<bundle> -DOUT=<dir> -P plugin_window_pair_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_TEST_LOG_MOUSE} "1")
set(ENV{GRIDLINE_DEV_PLUGIN_WINDOWS} "1")
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1280x800
                          --dev-input "${OUT}/${tag}.txt" --plugin gl.patcher
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
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

# Adding an instrument opens its window.
foreach(fmt clap vst3)
  if(fmt STREQUAL "clap")
    set(bundle "${CLAP}")
    set(type "clap:org.gridline.test.synth")
  else()
    set(bundle "${VST3}")
    set(type "vst3:475249444C494E455453593100000001")
  endif()
  run(${fmt} "10 junction scan ${fmt} ${bundle}
14 addvia rack ${type}
30 plugwin info
40 addvia rack ${type}
60 plugwin info
150 plugwin info
160 plugwin toggle ${type}
170 plugwin info
200 plugwin info
220 quit discard
")
  if(out MATCHES "plugwin frame 30: 0 windows")
    message(STATUS "SKIP: no plugin window on this machine\n${out}")
    return()
  endif()
  must(${fmt} "plugwin frame 30: 1 windows" "${fmt}: the first instance's window opens")
  must(${fmt} "plugwin frame 60: 2 windows" "${fmt}: the second instance of the same plugin opens its own window beside it")
  must(${fmt} "plugwin frame 150: 2 windows" "${fmt}: both stay open")
  if(out MATCHES "interface window closed \\((its|the)[^\n]*\n[^\n]*plugwin frame 150")
    message(FATAL_ERROR "FAIL (${fmt}): a window closed by itself before frame 150\n${out}")
  endif()
  must(${fmt} "plugwin frame 170: 1 windows" "${fmt}: one closes on its own (its channel button)")
  must(${fmt} "plugwin frame 200: 1 windows" "${fmt}: and the other stays")
  must(${fmt} "\\[plugin-window\\] shown \\| [^\n]*: owner (set before it was shown|set after it was shown \\(child window\\)|none \\(main window hidden\\))" "${fmt}: shown through the one path that keeps it above the main window")
endforeach()

if(APPLE)
  run(drag "10 junction scan clap ${CLAP}
14 addvia rack clap:org.gridline.test.synth
30 plugwin info
40 plugwin drag 60 60 0.6
140 plugwin info
150 quit discard
")
  must(drag "plugwin frame 40: drag started" "a drag is posted to the interface")
  must(drag "testview mouse down 1" "the interface gets the press")
  string(REGEX MATCH "testview mouse up 1 after ([0-9]+) drags" _ "${out}")
  if(CMAKE_MATCH_1 STREQUAL "" OR CMAKE_MATCH_1 LESS 10)
    message(FATAL_ERROR "FAIL (drag): the interface should get the release after every drag (got '${CMAKE_MATCH_1}')\n${out}")
  endif()
  message(STATUS "ok - the interface gets every drag (${CMAKE_MATCH_1}) and the release")
  must(drag "plugwin frame 140: 1 windows" "the window is still open after the drag")
endif()
