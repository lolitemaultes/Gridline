# Gridline - quiet windows in the real app (src/host/ui/QuietWindows.h): a --hidden run's real plugin
# windows (the in-tree CLAP and VST3 test synths, and a plugin moved to the sandbox, whose process
# shows the interface in a window of its own on macOS) are made, shown, given the keys inside the
# app and closed as always, while none of them can be seen on a display and neither the app nor the
# sandbox ever becomes the active app. Fails if any window of a test run lands on a display visibly
# or the app activates. The first run opens no window: it proves the guards are in place before
# any window is made (the windows runs only follow when it passed).
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DCLAP=<bundle> -DVST3=<bundle> -DOUT=<dir> -P quiet_windows_app.cmake
cmake_minimum_required(VERSION 3.16)
if("$ENV{GRIDLINE_QUIET_WINDOWS}" STREQUAL "0")
  message(STATUS "SKIP: quiet windows turned off for this test run (GRIDLINE_TEST_QUIET_WINDOWS=OFF)")
  return()
endif()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_DEV_PLUGIN_WINDOWS} "1")
set(ENV{GRIDLINE_PLUGIN_WINDOWS} "own")  # installed plugins in windows of their own (the real OS windows this checks)
# --hidden alone turns quiet windows on (ctest sets the variable as well; not here).
unset(ENV{GRIDLINE_QUIET_WINDOWS})
unset(ENV{SDL_MAC_BACKGROUND_APP})
set(ENV{GRIDLINE_QUIET_WINDOWS_LOG} "${OUT}/quiet.log")
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1280x800
                          --dev-input "${OUT}/${tag}.txt" --plugin gl.patcher ${ARGN}
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
# quietcheck <frame>: quiet, guarded, never active, nothing visible.
set(calm "quiet 1 guards 1 policy [a-z-]+ active 0 became-active 0 own-activations 0 blocked-activations [0-9]+")
if(APPLE)
  set(calm "quiet 1 guards 1 policy prohibited active 0 became-active 0 own-activations 0 blocked-activations [0-9]+")
endif()

# 1. The probe: no window yet.
run(probe "3 quietcheck
10 quietcheck
12 quit discard
")
must(probe "quietcheck frame 3: ${calm} quieted 0 on-screen 0 visible 0" "before any window: guards in place, never the active app")
must(probe "quietcheck frame 10: ${calm} quieted 0 on-screen 0 visible 0" "the hidden main window stays off the screen")

# 2. Plugin windows of both formats: two instances, one closed by its channel button.
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
32 quietcheck
40 addvia rack ${type}
60 plugwin info
62 quietcheck
160 plugwin toggle ${type}
170 plugwin info
172 quietcheck
200 quit discard
")
  if(out MATCHES "plugwin frame 30: 0 windows")
    message(STATUS "SKIP: no plugin window on this machine\n${out}")
    return()
  endif()
  must(${fmt} "plugwin frame 30: 1 windows" "${fmt}: the window opens")
  must(${fmt} "\\[plugin-window\\] shown \\|" "${fmt}: it is really shown")
  if(APPLE)
    must(${fmt} "quietcheck frame 32: ${calm} quieted [1-9][0-9]* on-screen [1-9][0-9]* visible 0 key \"[^\"]+\""
         "${fmt}: on the window server's list, not visible, and it has the keys inside the app")
  else()
    must(${fmt} "quietcheck frame 32: ${calm} quieted [1-9][0-9]* on-screen [0-9]+ visible 0" "${fmt}: shown fully transparent")
  endif()
  must(${fmt} "plugwin frame 60: 2 windows" "${fmt}: a second instance's window opens beside it")
  must(${fmt} "quietcheck frame 62: ${calm} quieted [2-9][0-9]* on-screen [0-9]+ visible 0" "${fmt}: two windows, neither visible")
  must(${fmt} "plugwin frame 170: 1 windows" "${fmt}: one closes on its own")
  must(${fmt} "quietcheck frame 172: ${calm} quieted [0-9]+ on-screen [0-9]+ visible 0" "${fmt}: after closing, still never active")
endforeach()

# 3. macOS: the plugin moved to the sandbox (its bar's button), whose own process shows the
# interface in a window of its own (a process Gridline started: quiet too).
if(APPLE)
  run(sandbox "10 junction scan clap ${CLAP}
14 addvia rack clap:org.gridline.test.synth
30 plugwin info
120 plugwin info
122 quietcheck
200 plugwin info
202 quietcheck
220 quit discard
" --plugin-window-action sandbox)
  must(sandbox "plugwin frame 30: 1 windows" "sandbox: the window opens")
  must(sandbox "plugwin \"[^\"]*\" format [^\n]* external 1" "sandbox: the interface is in the sandbox's own window")
  must(sandbox "quietcheck frame 122: ${calm} quieted [0-9]+ on-screen [2-9][0-9]* visible 0" "sandbox: its window and Gridline's bar are ordered in, not visible")
  must(sandbox "quietcheck frame 202: ${calm} quieted [0-9]+ on-screen [0-9]+ visible 0" "sandbox: neither process ever became the active app")
  file(STRINGS "${OUT}/quiet.log" lines REGEX "quieted window")
  set(pids "")
  foreach(l IN LISTS lines)
    string(REGEX MATCH "^[0-9]+" p "${l}")
    list(APPEND pids "${p}")
  endforeach()
  list(REMOVE_DUPLICATES pids)
  list(LENGTH pids n)
  if(n LESS 2)
    message(FATAL_ERROR "FAIL (sandbox): the sandbox process should keep its own window quiet too (processes with windows: ${n})\n${lines}")
  endif()
  message(STATUS "ok - sandbox: the sandbox process kept its own window quiet too")
endif()
file(STRINGS "${OUT}/quiet.log" lines REGEX "became active|the system activated")
if(lines)
  message(FATAL_ERROR "FAIL: a test process became the active app\n${lines}")
endif()
message(STATUS "ok - no process of these runs ever became the active app")
