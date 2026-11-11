# Gridline: drum kits (.h2drumkit, a kit folder) in the real app: hidden, isolated config, HOME and
# cache, SDL's dummy audio driver. The kit is made by the kit test itself (generated audio).
#   1. a packed kit loads as an Abacus channel named after it, unpacked into the cache, with the
#      import report (author, licence, what was not carried over), as one undo step;
#   2. a kit folder loads the same way; dropping a kit on the playlist does too;
#   3. the browser's summary of a kit: its instruments and a sample to play for each.
# Screenshots: <OUT>/report1440.ppm, <OUT>/report1080.ppm, <OUT>/light1440.ppm, <OUT>/browser1440.ppm.
#   cmake -DAPP=<gridline-app> -DMAKEKIT=<gridline_test_hydrogen> -DDEMO=<song.gridline.json> -DOUT=<dir> -P k162_drum_kits.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home" "${OUT}/cfg" "${OUT}/kits")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_CACHE_DIR} "${OUT}/cache")
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
execute_process(COMMAND "${MAKEKIT}" --make-kit "${OUT}/kits" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/kits/Test Kit.h2drumkit")
  message(FATAL_ERROR "the test kit could not be made (${rc})")
endif()
set(PACKED "${OUT}/kits/Test Kit.h2drumkit")
set(FOLDER "${OUT}/kits/Test Kit")

function(run tag size frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size ${size} ${ARGN}
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
function(has text)
  string(FIND "${out}" "${text}" pos)
  check("${ARGV1}: finds '${text}'" pos GREATER -1)
endfunction()

# ---- 1. a packed kit, its report, one undo step
run(report1440 1440x900 60 "5 drumkit load \"${PACKED}\"\n50 drumkit report\n")
has("drumkit report: open 1 name \"Test Kit & Co\" instruments 2 layers 4 channels 1" "packed kit loads")
has("credit Licence: CC BY-SA 4.0" "the licence is in the report")
has("credit Author: Gridline Tests" "the author is in the report")
has("note Their filter is not carried over: Snare" "what was not carried over")
has("channel \"Test Kit & Co\"" "an Abacus channel named after the kit")
string(REGEX MATCH "pad1 [^\n]*" pad1 "${out}")
string(FIND "${pad1}" "${OUT}/cache/unpacked/" incache)
check("the packed kit plays from its copy in the cache (${pad1})" incache GREATER -1)
file(GLOB beside "${OUT}/kits/*")
list(LENGTH beside nb)
check("nothing is written beside the kit (${nb} entries)" nb EQUAL 2)
check("screenshot report1440.ppm" EXISTS "${OUT}/report1440.ppm")
run(report1080 1920x1080 60 "5 drumkit load \"${PACKED}\"\n50 drumkit report\n")
check("screenshot report1080.ppm" EXISTS "${OUT}/report1080.ppm")
run(light1440 1440x900 60 "5 drumkit load \"${PACKED}\"\n50 drumkit report\n" --light)
check("screenshot light1440.ppm" EXISTS "${OUT}/light1440.ppm")
run(undo 1440x900 70 "5 drumkit load \"${FOLDER}\"\n40 drumkit close\n44 drumkit report\n50 undo\n56 drumkit report\n")
string(REGEX MATCHALL "channel \"Test Kit & Co\"" chans "${out}")
list(LENGTH chans nc)
check("one undo takes the whole kit away again (${nc} reports with the channel)" nc EQUAL 1)

# ---- 2. the folder, and a drop on the playlist
run(folder 1440x900 60 "5 drumkit load \"${FOLDER}\"\n50 drumkit report\n")
has("instruments 2 layers 4 channels 1" "a kit folder loads")
run(drop 1440x900 60 "5 audio \"${PACKED}\" 0 0\n50 drumkit report\n")  # (the playlist drop of any file)
has("channel \"Test Kit & Co\"" "a kit dropped on the playlist loads")

# ---- 3. the browser's summary
run(browser1440 1440x900 60 "5 browser select \"${PACKED}\"\n10 drumkit summary \"${PACKED}\"\n50 drumkit summary \"${PACKED}\"\n")
has("drumkit summary: ok name \"Test Kit & Co\" license \"CC BY-SA 4.0\" instruments 2 previews 2 layers 5" "the browser reads the kit and finds a sample per instrument")
check("screenshot browser1440.ppm" EXISTS "${OUT}/browser1440.ppm")
