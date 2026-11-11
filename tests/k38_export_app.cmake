# Gridline: an audio export of a project with an installed plugin, in the real app (hidden, isolated
# config and HOME, dummy audio driver, the in-tree test plugin). The export used to run on the
# interface thread for such a project: the window stopped answering until the end (no progress, no
# Cancel, "Not responding" on Windows). Now:
#   1. the frame right after the start finds the render still running: frames go on during it;
#   2. it finishes by itself with the file written;
#   3. Cancel stops it, nothing stays behind;
#   4. quitting during a render ends the app (no wait on a call only the interface could make);
#   5. the freeze watchdog stays quiet while a slow plugin load of the render holds a frame up, also
#      under Cancel and when leaving the app, and it still sees an interface frozen during a render.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DCLAP=<bundle> -DOUT=<dir> -P k38_export_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/home")
set(ENV{HOME} "${OUT}/home")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
set(type "clap:org.gridline.test.synth")

function(run tag script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames 600 --screenshot "${OUT}/${tag}.ppm"
                          --dev-input "${OUT}/${tag}.txt" --plugin gl.patcher
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 200)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (${tag}): gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# The test plugin registers through the Junction's editor; then it replaces that instrument.
set(prep "10 junction scan clap ${CLAP}\n15 replace ${type}\n17 channels\n")

# 1 and 2: a render from start to end, the frames going on.
run(whole "${prep}20 exportstart 1 \"${OUT}/whole\"\n21 export\n22 export\n400 waitfor render-finished 120\n402 export\n410 quit discard\n")
if(NOT whole_out MATCHES "junction scan ok 1")
  message(FATAL_ERROR "FAIL: the test plugin was not registered\n${whole_out}")
endif()
if(NOT whole_out MATCHES "${type}")
  message(FATAL_ERROR "FAIL: the plugin is not in the project\n${whole_out}")
endif()
if(NOT whole_out MATCHES "export frame 21: running 1")
  message(FATAL_ERROR "FAIL: the frame after the start found no running render (it ran on the interface thread)\n${whole_out}")
endif()
if(NOT whole_out MATCHES "export frame 402: running 0 finished 0 progress [0-9.]+ files 1 errors 0 cancelled 0")
  message(FATAL_ERROR "FAIL: the render did not finish with its file\n${whole_out}")
endif()
file(GLOB files "${OUT}/whole/*.wav")
list(LENGTH files n)
if(NOT n EQUAL 1)
  message(FATAL_ERROR "FAIL: ${n} files written\n${whole_out}")
endif()
file(SIZE "${files}" size)
if(size LESS 1000)
  message(FATAL_ERROR "FAIL: the file is empty (${size} bytes)")
endif()
message(STATUS "ok - the frames go on during a render with an installed plugin; the file is written (${size} bytes)")

# 3: Cancel. The plugin's own creation waits for frames, so the first frames always find it running.
run(cancel "${prep}20 exportstart 1 \"${OUT}/cancel\"\n21 exportcancel\n300 waitfor render-finished 120\n302 export\n310 quit discard\n")
if(NOT cancel_out MATCHES "export frame 302: running 0 finished 0 progress [0-9.]+ files 0 errors 0 cancelled 1")
  message(FATAL_ERROR "FAIL: Cancel did not stop the render\n${cancel_out}")
endif()
file(GLOB left "${OUT}/cancel/*.wav")
if(left)
  message(FATAL_ERROR "FAIL: a cancelled render left ${left}")
endif()
message(STATUS "ok - Cancel stops it")

# 4: quitting in the middle.
run(quit "${prep}20 exportstart 1 \"${OUT}/quit\"\n21 quit discard\n")
message(STATUS "ok - quitting during a render ends the app")

# 5: the freeze watchdog (its report goes away when the interface comes back; its log line stays).
# The render's own instance of the plugin takes longer to load than the watchdog allows a silent frame.
# (10 s, not less: on a shared CI runner with software OpenGL an ordinary frame now and then takes
# 3 to 4 s, which a shorter limit would report as a freeze of its own.)
set(ENV{GRIDLINE_CRASH_TEST_FREEZE_SECONDS} "10")
# The user's "log to file" switch, so the test can read what the watchdog said.
set(appHome "${OUT}/home")
file(READ "${appHome}/gridline/settings.json" settings)
string(REPLACE "\"debug.logToFile\": false" "\"debug.logToFile\": true" settings "${settings}")
file(WRITE "${appHome}/gridline/settings.json" "${settings}")
set(ENV{GRIDLINE_TEST_SLOW_NEXT_INSTANCE_MS} "14000")
function(app_log outVar)
  set(text "")
  if(EXISTS "${appHome}/gridline/gridline.log")
    file(READ "${appHome}/gridline/gridline.log" text)
    file(REMOVE "${appHome}/gridline/gridline.log")
  endif()
  set(${outVar} "${text}" PARENT_SCOPE)
endfunction()
function(quiet tag)
  app_log(text)
  if(text MATCHES "has not answered")
    message(FATAL_ERROR "FAIL (${tag}): the watchdog took a slow plugin call of a render for a freeze\n${text}")
  endif()
endfunction()

run(slowwhole "${prep}20 exportstart 1 \"${OUT}/slowwhole\"\n21 export\n400 waitfor render-finished 120\n402 export\n410 quit discard\n")
if(NOT slowwhole_out MATCHES "export frame 402: running 0 finished 0 progress [0-9.]+ files 1 errors 0 cancelled 0")
  message(FATAL_ERROR "FAIL: the slow render did not finish with its file\n${slowwhole_out}")
endif()
quiet(slowwhole)
message(STATUS "ok - the watchdog stays quiet during a slow plugin call of a render")

run(slowcancel "${prep}20 exportstart 1 \"${OUT}/slowcancel\"\n21 exportcancel\n300 waitfor render-finished 120\n302 export\n310 quit discard\n")
if(NOT slowcancel_out MATCHES "export frame 302: running 0 finished 0 progress [0-9.]+ files 0 errors 0 cancelled 1")
  message(FATAL_ERROR "FAIL: Cancel did not stop the slow render\n${slowcancel_out}")
endif()
quiet(slowcancel)
message(STATUS "ok - and under Cancel")

run(slowquit "${prep}20 exportstart 1 \"${OUT}/slowquit\"\n21 quit discard\n")
quiet(slowquit)
message(STATUS "ok - and when leaving the app during the render")

# The control, which proves the checks above can fail: the interface itself stalls while a render
# is under way, and the watchdog says so.
unset(ENV{GRIDLINE_TEST_SLOW_NEXT_INSTANCE_MS})
set(ENV{GRIDLINE_CRASH_TEST_FORCE} "stall:14@24")
run(stall "${prep}20 exportstart 1 \"${OUT}/stall\"\n21 export\n400 waitfor render-finished 120\n402 export\n410 quit discard\n")
unset(ENV{GRIDLINE_CRASH_TEST_FORCE})
unset(ENV{GRIDLINE_CRASH_TEST_FREEZE_SECONDS})
app_log(text)
if(NOT text MATCHES "has not answered")
  message(FATAL_ERROR "FAIL: an interface stalled during a render went unnoticed\n${text}\n${stall_out}")
endif()
message(STATUS "ok - an interface stalled during a render is seen")
