# Gridline - K-75: the real app in a Chromebook's Linux environment (hidden, isolated configs,
# SDL's dummy audio driver). GRIDLINE_CHROMEOS=chromeos stands in for the files ChromeOS creates
# (the detection itself is covered by gridline_tests_k75):
#   1. the start says so ("[system] ChromeOS Linux environment");
#   2. a new installation saves 48000 Hz and 1024 frames (the container's sound server);
#   3. a second start with the user's own 256 frames keeps them;
#   4. off ChromeOS a new installation keeps 44100 Hz / 256 frames;
#   5. Linux with Mesa: OpenGL capped at 2.1 (a virtual GPU without 3.0) still starts and draws,
#      on a 2.1 context with GLSL 1.20, and capped at 3.0 with GLSL 1.30.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k75_chromeos_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
endfunction()
function(ok msg)
  message(STATUS "ok - ${msg}")
endfunction()

# run(<tag> <cfg>): gridline-app with config ${OUT}/<cfg> (HOME inside it); stderr in `out`.
function(run tag cfg)
  file(MAKE_DIRECTORY "${OUT}/${cfg}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/${cfg}")
  set(ENV{HOME} "${OUT}/${cfg}/home")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --size 1024x700 --frames 8 --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e TIMEOUT 120)
  set(out "${o}\n${e}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}):\n${o}\n${e}")
  endif()
endfunction()

# frame(<tag> <what>): the screenshot is a full 1024x700 P6 frame with a drawn interface (the
# middle rows differ from the top-left pixel; a blank clear colour would not).
function(frame tag what)
  set(f "${OUT}/${tag}.ppm")
  if(NOT EXISTS "${f}")
    fail("no frame drawn on ${what}: ${tag}.ppm was not written")
  endif()
  file(SIZE "${f}" sz)
  file(READ "${f}" head LIMIT 16)
  string(REGEX MATCH "\\[graphics\\][^\n]*" g "${out}")
  if(NOT head MATCHES "^P6\n1024 700\n255\n" OR NOT sz EQUAL 2150416)
    string(REPLACE "\n" " " head "${head}")
    fail("no frame drawn on ${what} (${sz} bytes, header '${head}', ${g})")
  endif()
  file(READ "${f}" first OFFSET 16 LIMIT 3 HEX)
  file(READ "${f}" mid OFFSET 1075216 LIMIT 3072 HEX)
  string(REPEAT "${first}" 1024 flat)
  if(mid STREQUAL flat)
    fail("the frame on ${what} is blank (${g})")
  endif()
endfunction()

function(setting cfg key var)
  file(READ "${OUT}/${cfg}/gridline/settings.json" j)
  string(JSON v GET "${j}" values "${key}")
  set(${var} "${v}" PARENT_SCOPE)
endfunction()

# 1-2: a new installation on a Chromebook.
set(ENV{GRIDLINE_CHROMEOS} "chromeos")
run(cros_new cfg_cros)
if(NOT out MATCHES "\\[system\\] ChromeOS Linux environment")
  fail("the start does not report the ChromeOS Linux environment")
endif()
ok("the start reports the ChromeOS Linux environment")
setting(cfg_cros audio.sampleRate rate)
setting(cfg_cros audio.bufferFrames frames)
if(NOT rate EQUAL 48000 OR NOT frames EQUAL 1024)
  fail("a new installation on ChromeOS saved ${rate} Hz / ${frames} frames, not 48000 / 1024")
endif()
ok("a new installation on ChromeOS starts at 48000 Hz / 1024 frames")

# 3: the user's own buffer stays.
file(READ "${OUT}/cfg_cros/gridline/settings.json" j)
string(JSON j SET "${j}" values audio.bufferFrames 256)
file(WRITE "${OUT}/cfg_cros/gridline/settings.json" "${j}")
run(cros_again cfg_cros)
setting(cfg_cros audio.bufferFrames frames)
if(NOT frames EQUAL 256)
  fail("the saved 256 frames became ${frames}")
endif()
ok("a saved buffer size wins over the ChromeOS default")

# 4: elsewhere, as before.
set(ENV{GRIDLINE_CHROMEOS} "off")
run(plain_new cfg_plain)
if(out MATCHES "\\[system\\]")
  fail("ChromeOS reported with GRIDLINE_CHROMEOS=off")
endif()
setting(cfg_plain audio.sampleRate rate)
setting(cfg_plain audio.bufferFrames frames)
if(NOT rate EQUAL 44100 OR NOT frames EQUAL 256)
  fail("a new installation elsewhere saved ${rate} Hz / ${frames} frames, not 44100 / 256")
endif()
ok("off ChromeOS a new installation keeps 44100 Hz / 256 frames")

# 5: OpenGL capped (Mesa only: the override is Mesa's).
if(APPLE OR WIN32 OR CMAKE_HOST_APPLE OR CMAKE_HOST_WIN32)
  message(STATUS "note: the OpenGL levels are checked on Linux only")
  return()
endif()
if(NOT out MATCHES "\\[graphics\\] OpenGL [^\n]*Mesa")
  string(REGEX MATCH "\\[graphics\\][^\n]*" g "${out}")
  message(STATUS "note: not a Mesa driver, OpenGL levels not checked (${g})")
  return()
endif()
set(ENV{MESA_GL_VERSION_OVERRIDE} "2.1")
set(ENV{MESA_GLSL_VERSION_OVERRIDE} "120")
run(gl21 cfg_gl21)
if(NOT out MATCHES "\\[graphics\\] OpenGL 2\\.1[^\n]*shaders #version 120")
  fail("with OpenGL capped at 2.1 the app is not on a 2.1 context with GLSL 1.20")
endif()
frame(gl21 "OpenGL 2.1")
ok("OpenGL 2.1: starts and draws with GLSL 1.20")
set(ENV{MESA_GL_VERSION_OVERRIDE} "3.0")
set(ENV{MESA_GLSL_VERSION_OVERRIDE} "130")
run(gl30 cfg_gl30)
if(NOT out MATCHES "\\[graphics\\] OpenGL 3\\.0[^\n]*shaders #version 130")
  fail("with OpenGL capped at 3.0 the app is not on a 3.0 context with GLSL 1.30")
endif()
frame(gl30 "OpenGL 3.0")
ok("OpenGL 3.0: starts and draws with GLSL 1.30")
