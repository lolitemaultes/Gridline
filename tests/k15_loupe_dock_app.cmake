# Gridline - K-15: Loupe pinned (docked) into the main layout, in the real app (hidden,
# isolated config, off-screen, SDL's dummy audio driver). A Loupe effect is opened (its
# window), then pinned:
#   1. as a tab next to the playlist: docked with the playlist, the effect window gone, the
#      full form at its design size filling the panel (nothing outside it);
#   2. on the left with the browser and on the right with the mixer: the compact form,
#      within the column;
#   3. below the playlist: a node of its own, under the playlist;
#   4. unpinned: the panel closes and the effect window is back;
#   5. pinned, the app quits: the next run opens the panel where it was (the pin in the
#      settings, its place in layout.ini).
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k15_loupe_dock_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_EDISON_DEMO} "1")
set(ENV{GRIDLINE_SAVE_LAYOUT} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg/home")
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1920x1080 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" --plugin gl.edison
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
endfunction()

# dump(<what> <n>): the n-th "loupe:" line (0-based); sets <what>_pinned _docked _with _x _y _w _h _rw _rh _compact _form _head _wave _fx.
function(dump what n)
  string(REGEX MATCHALL "loupe: [^\n]*" lines "${out}")
  list(LENGTH lines count)
  if(NOT n LESS count)
    fail("${what}: no loupe dump #${n}")
  endif()
  list(GET lines ${n} l)
  string(REGEX MATCH "pinned ([01]) shown ([01]) docked ([01]) with \"([^\"]*)\" pos ([-0-9]+),([-0-9]+) size ([0-9]+)x([0-9]+) room ([0-9]+)x([0-9]+) compact ([01]) scale [0-9.]+ form ([0-9]+) head ([0-9]+) wave ([0-9]+) fx ([01])" m "${l}")
  if(m STREQUAL "")
    fail("${what}: unreadable dump: ${l}")
  endif()
  set(${what}_pinned "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${what}_docked "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(${what}_with "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(${what}_x "${CMAKE_MATCH_5}" PARENT_SCOPE)
  set(${what}_y "${CMAKE_MATCH_6}" PARENT_SCOPE)
  set(${what}_w "${CMAKE_MATCH_7}" PARENT_SCOPE)
  set(${what}_h "${CMAKE_MATCH_8}" PARENT_SCOPE)
  set(${what}_rw "${CMAKE_MATCH_9}" PARENT_SCOPE)
  # (CMake keeps nine groups per match: the rest from a second one)
  string(REGEX MATCH "room [0-9]+x([0-9]+) compact ([01]) scale [0-9.]+ form ([0-9]+) head ([0-9]+) wave ([0-9]+) fx ([01])" m "${l}")
  set(${what}_rh "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${what}_compact "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${what}_form "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(${what}_head "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(${what}_wave "${CMAKE_MATCH_5}" PARENT_SCOPE)
  set(${what}_fx "${CMAKE_MATCH_6}" PARENT_SCOPE)
  message(STATUS "${what}: ${l}")
endfunction()

# fits(<what>): the form lies inside the panel's room (width and height; its bottom margin is
# 8 px at 100 %, 6 px at the compact form's 70 %).
function(fits what)
  math(EXPR used "${${what}_head} + ${${what}_wave} + 6")
  if(${what}_form GREATER ${what}_rw OR used GREATER ${what}_rh)
    fail("${what}: form ${${what}_form}x${used} larger than the room ${${what}_rw}x${${what}_rh}")
  endif()
endfunction()

run(spots 60 "4 loupe\n6 loupe pin tab\n14 loupe\n16 loupe pin left\n24 loupe\n26 loupe pin right\n34 loupe\n36 loupe pin below\n44 loupe\n46 loupe unpin\n54 loupe\n")
dump(before 0)
if(NOT before_pinned EQUAL 0 OR NOT before_fx EQUAL 1)
  fail("before pinning: Loupe should be in its effect window, not pinned")
endif()

# ---- 1. a tab next to the playlist
dump(tab 1)
if(NOT tab_pinned EQUAL 1 OR NOT tab_docked EQUAL 1 OR NOT tab_with STREQUAL "Playlist" OR NOT tab_fx EQUAL 0)
  fail("pinned as a tab: not docked with the playlist (or the effect window still open)")
endif()
if(NOT tab_compact EQUAL 0 OR tab_rw LESS 900 OR tab_rh LESS 600 OR tab_wave LESS 500)
  fail("pinned as a tab: the full form should fill a large panel (room ${tab_rw}x${tab_rh}, wave ${tab_wave})")
endif()
fits(tab)
math(EXPR fill "${tab_head} + ${tab_wave} + 8")
if(NOT fill EQUAL tab_rh OR NOT tab_form EQUAL tab_rw)
  fail("pinned as a tab: the form ${tab_form}x${fill} should fill the room ${tab_rw}x${tab_rh}")
endif()
message(STATUS "ok - a tab next to the playlist: ${tab_w}x${tab_h}, the form fills it")

# ---- 2. the side columns
dump(left 2)
if(NOT left_docked EQUAL 1 OR NOT left_with STREQUAL "Browser" OR NOT left_compact EQUAL 1 OR left_x GREATER 100)
  fail("pinned left: should be the compact form docked with the browser on the left")
endif()
fits(left)
dump(right 3)
if(NOT right_docked EQUAL 1 OR NOT right_with STREQUAL "Mixer" OR NOT right_compact EQUAL 1 OR right_x LESS 1200)
  fail("pinned right: should be the compact form docked with the mixer on the right")
endif()
fits(right)
message(STATUS "ok - side columns: compact forms ${left_rw} and ${right_rw} px wide, inside")

# ---- 3. below the playlist
dump(below 4)
if(NOT below_docked EQUAL 1 OR NOT below_with STREQUAL "" OR below_y LESS 300 OR below_compact EQUAL 1)
  fail("pinned below: should be a node of its own under the playlist (y ${below_y})")
endif()
message(STATUS "ok - below the playlist: ${below_w}x${below_h} at y ${below_y}")

# ---- 4. unpinned
dump(unpinned 5)
if(NOT unpinned_pinned EQUAL 0 OR NOT unpinned_fx EQUAL 1)
  fail("unpinned: the panel should close and the effect window come back")
endif()
message(STATUS "ok - unpinned: back in the effect window")

# ---- 5. kept with the layout
run(save 30 "6 loupe pin tab\n20 loupe\n")
run(again 30 "20 loupe\n")
dump(again 0)
if(NOT again_pinned EQUAL 1 OR NOT again_docked EQUAL 1 OR NOT again_with STREQUAL "Playlist")
  fail("the next run: the pinned panel should open next to the playlist again")
endif()
message(STATUS "ok - the next run opens the panel where it was pinned")
