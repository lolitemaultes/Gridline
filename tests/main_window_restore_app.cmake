# Gridline - GitHub issue #50: the main window opens at the size and state it was left at, in the
# real app (hidden, an isolated config folder and HOME, SDL's dummy audio driver):
#   1. a window resized to 1300 x 820 is saved in layout.ini [GridlineViewports][Main] (MainSize,
#      MainMax, MainFull beside MainOs and Scale) and the next start opens at that size;
#   2. a window left maximised is saved maximised and comes back maximised (a hidden run never
#      maximises a window it does not show: it plans it and keeps it saved so);
#   3. a size larger than every display (a display that has gone, a lower resolution) opens at
#      the largest size that fits the display it opens on;
#   4. an older layout.ini without the new lines opens the main window as before.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P main_window_restore_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_SAVE_LAYOUT} "1")  # these runs save their layout (their own config folder)
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# run(<tag> <cfg> <script>): config ${OUT}/cfg_<cfg> (kept between runs of the same cfg).
function(run tag cfg script)
  file(MAKE_DIRECTORY "${OUT}/cfg_${cfg}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${cfg}")
  set(ENV{HOME} "${OUT}/cfg_${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.out" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (${tag}): gridline-app ended with ${rc}\n${out}\n${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()
function(must tag regex what)
  if(NOT out MATCHES "${regex}")
    message(FATAL_ERROR "FAIL (${tag}): ${what}\n${out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()
# The size the "sdlwin" line of <frame> printed: <prefix>_w, <prefix>_h, <prefix>_max, and the
# default size the start planned for this run: <prefix>_defw, <prefix>_defh. The default is
# 1600 x 960 at most, 40 x 60 inside the display's usable area as it was when the run started. That
# area is the system's and can change between runs (on macOS it moved between 964 and 966 px high
# from one start to the next), so each run is checked against its own.
function(win prefix frame)
  string(REGEX MATCH "sdlwin frame ${frame}: [^\n]* window ([0-9]+)x([0-9]+) [^\n]* maximized ([01]) [^\n]* planned-in ([0-9]+)x([0-9]+)" line "${out}")
  if(line STREQUAL "")
    message(FATAL_ERROR "FAIL: no sdlwin line at frame ${frame}\n${out}")
  endif()
  set(${prefix}_w ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${prefix}_h ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${prefix}_max ${CMAKE_MATCH_3} PARENT_SCOPE)
  math(EXPR dw "${CMAKE_MATCH_4} - 40")
  math(EXPR dh "${CMAKE_MATCH_5} - 60")
  if(dw GREATER 1600)
    set(dw 1600)
  endif()
  if(dh GREATER 960)
    set(dh 960)
  endif()
  set(${prefix}_defw ${dw} PARENT_SCOPE)
  set(${prefix}_defh ${dh} PARENT_SCOPE)
endfunction()
set(ini "${OUT}/cfg_size/gridline/layout.ini")

# ---- 4 first: a first start (no layout.ini) is today's default.
run(first size "10 sdlwin show\n20 sdlwin size 1300 820\n30 sdlwin show\n40 quit discard\n")
win(a 10)
if(NOT a_w EQUAL a_defw OR NOT a_h EQUAL a_defh)
  message(FATAL_ERROR "FAIL: a first start should open at the default ${a_defw}x${a_defh}, not ${a_w}x${a_h}\n${out}")
endif()
message(STATUS "ok - a first start opens at the default size")
if(out MATCHES "mainwin plan")
  message(FATAL_ERROR "FAIL: a first start should not restore anything\n${out}")
endif()
win(b 30)
if(NOT b_w EQUAL 1300 OR NOT b_h EQUAL 820)
  message(STATUS "SKIP: this window system does not resize a hidden window (${b_w}x${b_h})")
  return()
endif()
if(NOT EXISTS "${ini}")
  message(FATAL_ERROR "FAIL: no layout.ini was saved")
endif()
file(READ "${ini}" text)
string(REGEX MATCH "\\[GridlineViewports\\]\\[Main\\]\n[^[]*" mainSec "${text}")
if(NOT mainSec MATCHES "MainOs=" OR NOT mainSec MATCHES "Scale=" OR NOT mainSec MATCHES "MainSize=1300,820\n" OR NOT mainSec MATCHES "MainMax=0\n" OR
   NOT mainSec MATCHES "MainFull=0\n")
  message(FATAL_ERROR "FAIL: [Main] should hold the size and state:\n${mainSec}")
endif()
message(STATUS "ok - the size and state are saved in layout.ini [Main]")

# ---- 1. the next start opens at that size.
run(second size "10 sdlwin show\n20 quit discard\n")
must(second "mainwin plan: 1300x820 " "the saved size is planned")
win(c 10)
if(NOT c_w EQUAL 1300 OR NOT c_h EQUAL 820)
  message(FATAL_ERROR "FAIL: the next start should open at 1300x820, not ${c_w}x${c_h}\n${out}")
endif()
message(STATUS "ok - the main window opens at the size it was left at")

# ---- 2. maximised.
file(READ "${ini}" text)
string(REPLACE "MainMax=0" "MainMax=1" text "${text}")
file(WRITE "${ini}" "${text}")
run(max size "10 sdlwin show\n20 quit discard\n")
must(max "mainwin plan: 1300x820 [^\n]* maximize 1 " "a window left maximised is planned maximised (its old size under it)")
file(READ "${ini}" text)
if(NOT text MATCHES "MainMax=1\n" OR NOT text MATCHES "MainSize=1300,820\n")
  message(FATAL_ERROR "FAIL: it should stay saved maximised, with its old size\n${text}")
endif()
message(STATUS "ok - and stays saved maximised")

# ---- 3. too large for the display: the largest size that fits.
file(READ "${ini}" text)
string(REPLACE "MainMax=1" "MainMax=0" text "${text}")
string(REPLACE "MainSize=1300,820" "MainSize=30000,20000" text "${text}")
string(REGEX REPLACE "MainOs=[-0-9]+,[-0-9]+" "MainOs=-50000,-50000" text "${text}")
file(WRITE "${ini}" "${text}")
run(huge size "10 sdlwin show\n20 quit discard\n")
string(REGEX MATCH "mainwin plan: ([0-9]+)x([0-9]+) at ([-0-9]+),([-0-9]+)" _ "${out}")
set(pw ${CMAKE_MATCH_1})
set(ph ${CMAKE_MATCH_2})
if(pw STREQUAL "" OR pw GREATER_EQUAL 30000 OR ph GREATER_EQUAL 20000 OR pw LESS 960 OR CMAKE_MATCH_3 LESS -20000)
  message(FATAL_ERROR "FAIL: a size larger than the display should be fitted onto it\n${out}")
endif()
message(STATUS "ok - a size saved on a display that is gone opens fitted (${pw}x${ph})")

# ---- 4. an older layout.ini: no size lines, today's default.
file(READ "${ini}" text)
string(REGEX REPLACE "MainSize=[^\n]*\nMainMax=[^\n]*\nMainFull=[^\n]*\n" "" text "${text}")
file(WRITE "${ini}" "${text}")
run(old size "10 sdlwin show\n20 quit discard\n")
if(out MATCHES "mainwin plan")
  message(FATAL_ERROR "FAIL: an older layout.ini should restore nothing\n${out}")
endif()
win(d 10)
if(NOT d_w EQUAL d_defw OR NOT d_h EQUAL d_defh)
  message(FATAL_ERROR "FAIL: an older layout.ini should open at the default ${d_defw}x${d_defh}, not ${d_w}x${d_h}\n${out}")
endif()
message(STATUS "ok - an older layout.ini opens the main window as before")
