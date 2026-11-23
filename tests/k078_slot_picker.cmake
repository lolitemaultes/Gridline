# Gridline: the effect picker of an empty mixer slot, in the real app (hidden, isolated config and
# HOME, a fixed 16 ms frame time). User report: on a thin mixer the list took the slot's width, so
# its right side (format labels, stars) and its scrollbar were cut off. Two installed effects with
# long names are put in the plugin cache (no plugin is loaded or scanned), so the list is wider
# than any slot. In each case an empty slot is clicked and the picker popup must:
#   * lie inside the main window;
#   * be at least as wide as its widest row needs (and never narrower than the slot);
#   * have its scrollbar inside it, and scroll with the wheel;
#   * put the effect in the slot when a row is clicked (searching by name first).
# Cases: Studio's thin mixer column (the default workspace, a narrow track beside it), Studio's
# wide mixer, Smooth's mixer with the slot at the window's right edge, and Smooth's mixer at the
# bottom of the window (it opens above the slot).
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k078_slot_picker.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/plugs/Wide.vst3" "${OUT}/plugs/Tape.vst3")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

function(klass uid name sub var)
  set(${var} "{\"uid\":\"${uid}\",\"name\":\"${name}\",\"vendor\":\"Test Audio\",\"version\":\"1.0\",\"subCategories\":\"${sub}\",\"instrument\":false,\"eventInput\":false,\"inputs\":2,\"outputs\":2,\"sidechain\":0,\"latency\":0,\"params\":[]}" PARENT_SCOPE)
endfunction()
klass(0123456789ABCDEF0123456789ABCDEF "Ultra Wide Stereo Imaging Mastering Suite Deluxe" "Fx|Spatial" k1)
klass(1123456789ABCDEF0123456789ABCDEF "Vintage Tape Echo Chamber With Spring Reverb" "Fx|Delay" k2)
set(cache "{\"version\": 3, \"bundles\": [{\"format\":\"vst3\",\"bundle\":\"${OUT}/plugs/Wide.vst3\",\"ok\":true,\"classes\":[${k1}]},{\"format\":\"vst3\",\"bundle\":\"${OUT}/plugs/Tape.vst3\",\"ok\":true,\"classes\":[${k2}]}]}")

# run(name layout size script [args...])
function(run name layout size script)
  set(dir "${OUT}/${name}")
  file(MAKE_DIRECTORY "${dir}/cfg/gridline" "${dir}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${dir}/cfg")
  set(ENV{HOME} "${dir}/home")
  file(WRITE "${dir}/cfg/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"${layout}\"}}\n")
  file(WRITE "${dir}/cfg/gridline/plugin-cache.json" "${cache}")
  file(WRITE "${dir}/script.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --size ${size} ${ARGN} --dev-input "${dir}/script.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 240)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: gridline-app failed (${rc}): ${err}\n${o}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n${out}")
endfunction()

# The n-th (0-based) "slotpick" line, its numbers in variables prefixed by `p`.
function(slotpick n p)
  string(REGEX MATCHALL "slotpick frame [^\n]*" lines "${out}")
  list(LENGTH lines cnt)
  if(cnt LESS_EQUAL ${n})
    fail("no slotpick report ${n}")
  endif()
  list(GET lines ${n} l)
  if(NOT l MATCHES "anchor [-0-9 ]+ popup [-0-9 ]+ need [0-9]+ bounds [-0-9 ]+ bar [01][-0-9 ]+ scroll [0-9]+ of [0-9]+ up [01] left [01] open [01] effects [0-9]+$")
    fail("slotpick report not understood: ${l}")
  endif()
  # Its numbers in order: frame, anchor (4), popup (4), need, bounds (4), bar, scrollbar (4),
  # scroll, max, up, left, open, effects.
  string(REGEX MATCHALL "-?[0-9]+" nums "${l}")
  set(i 1)
  foreach(v a0x a0y a1x a1y x0 y0 x1 y1 need b0x b0y b1x b1y bar bx0 by0 bx1 by1 scroll smax up left open effects)
    list(GET nums ${i} x)
    set(${p}${v} "${x}" PARENT_SCOPE)
    math(EXPR i "${i} + 1")
  endforeach()
endfunction()

# The picker of report n: open, inside the W x H window, wide enough, its scrollbar inside it.
function(check_popup name n W H)
  slotpick(${n} q)
  if(NOT qopen EQUAL 1)
    fail("${name}: the picker did not open")
  endif()
  if(qx0 LESS 0 OR qy0 LESS 0 OR qx1 GREATER W OR qy1 GREATER H)
    fail("${name}: the picker (${qx0} ${qy0} ${qx1} ${qy1}) runs out of the ${W} x ${H} window")
  endif()
  math(EXPR pw "${qx1} - ${qx0}")
  math(EXPR sw "${qa1x} - ${qa0x}")
  if(pw LESS qneed OR pw LESS sw)
    fail("${name}: the picker is ${pw} px wide; its rows need ${qneed} px (the slot is ${sw} px)")
  endif()
  if(NOT qbar EQUAL 1)
    fail("${name}: the long list should scroll (no scrollbar)")
  endif()
  if(qbx0 LESS qx0 OR qbx1 GREATER qx1 OR qby0 LESS qy0 OR qby1 GREATER qy1)
    fail("${name}: the scrollbar (${qbx0} ${qby0} ${qbx1} ${qby1}) is not inside the picker (${qx0} ${qy0} ${qx1} ${qy1})")
  endif()
  message(STATUS "ok: ${name}: picker ${pw} px wide (needs ${qneed}, slot ${sw}) at ${qx0},${qy0}-${qx1},${qy1}, scrollbar inside, up ${qup} left ${qleft}")
  foreach(v x0 y0 x1 y1 up left effects scroll)
    set(${name}_${v} "${q${v}}" PARENT_SCOPE)
  endforeach()
endfunction()

# open_case(name layout size setup W H [args]): opens the picker, wheels it, then a second run searches
# and clicks the first match.
function(open_case name layout size setup W H)
  run(${name} ${layout} ${size} "${setup}20 sdlat fxslotempty
22 sdldown 0
23 sdlup 0
30 slotpick
31 shot \"${OUT}/${name}.ppm\"
32 slotpick
34 quit discard
" ${ARGN})
  if(NOT out MATCHES "sdlat frame 20: fxslotempty at ")
    fail("${name}: no empty slot shown")
  endif()
  check_popup(${name} 0 ${W} ${H})
  # The wheel over the list scrolls it.
  math(EXPR mx "(${${name}_x0} + ${${name}_x1}) / 2")
  math(EXPR my "(${${name}_y0} + ${${name}_y1}) / 2")
  run(${name}_wheel ${layout} ${size} "${setup}20 sdlat fxslotempty
22 sdldown 0
23 sdlup 0
30 sdlmove ${mx} ${my}
32 wheel -4
40 slotpick
41 quit discard
" ${ARGN})
  check_popup(${name}_wheel 0 ${W} ${H})
  if(NOT ${name}_wheel_scroll GREATER 0)
    fail("${name}: the wheel over the picker did not scroll it")
  endif()
  message(STATUS "ok: ${name}: the wheel scrolls the list (${${name}_wheel_scroll} px)")
  # Typing searches; a click on the first match puts it in the slot.
  run(${name}_pick ${layout} ${size} "${setup}20 sdlat fxslotempty
22 sdldown 0
23 sdlup 0
30 slotpick
32 text Compressor
36 plugsearch
37 slotpick
60 slotpick
62 quit discard
" ${ARGN})
  slotpick(0 b)
  slotpick(1 s)
  if(NOT sup EQUAL bup)
    fail("${name}: the picker changed sides while searching (up ${bup} -> ${sup})")
  endif()
  if(NOT out MATCHES "plugsearch frame [0-9]+ x0 [-0-9]+ y0 [-0-9]+ x1 [-0-9]+ y1 [-0-9]+ active 1 rows ([-0-9]+)")
    fail("${name}: the search field does not hold the keyboard")
  endif()
  math(EXPR ry "${CMAKE_MATCH_1} + 13")
  math(EXPR rx "(${sx0} + ${sx1}) / 2")
  run(${name}_pick ${layout} ${size} "${setup}20 sdlat fxslotempty
22 sdldown 0
23 sdlup 0
30 slotpick
32 text Compressor
36 sdlmove ${rx} ${ry}
40 sdldown 0
41 sdlup 0
46 slotpick
48 quit discard
" ${ARGN})
  slotpick(0 b)
  slotpick(1 a)
  if(NOT aopen EQUAL 0 OR NOT aeffects GREATER beffects)
    fail("${name}: a click on a match should put it in the slot and close the picker (effects ${beffects} -> ${aeffects}, open ${aopen})")
  endif()
  message(STATUS "ok: ${name}: a click on a match puts the effect in the slot")
  set(${name}_left "${${name}_left}" PARENT_SCOPE)
  set(${name}_up "${${name}_up}" PARENT_SCOPE)
endfunction()

# 1. Studio: the default workspace docks the mixer as a thin column at the right; track 3 narrow.
open_case(studio_thin studio 1440x900 "6 mixhide narrow 3\n8 save \"${OUT}/song.gridline\"\n" 1440 900)
if(NOT studio_thin_left EQUAL 1)
  fail("studio_thin: the picker should line up with the slot's right end at the window's edge")
endif()

# 2. Studio's wide mixer (normal strips, the inspector beside them).
open_case(studio_wide studio 1920x1080 "6 mixhide narrow 3\n" 1920 1080 --mixer-view rows1)

# 3. Smooth's mixer, every track shown, the window narrow: the last whole free slot is near its
#    right edge.
open_case(smooth_edge logic 1000x800 "4 layout open Mixer\n6 mixhide smoothview 2\n8 mixhide narrow 3\n" 1000 800 "${OUT}/song.gridline")

# 4. Smooth's mixer at the bottom of the window: the list opens above the slot.
open_case(smooth_low logic 1440x900 "4 layout open Mixer\n" 1440 900)
if(NOT smooth_low_up EQUAL 1)
  fail("smooth_low: with no room below the slot the picker should open above it")
endif()
