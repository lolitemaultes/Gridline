# Gridline - drag-1119: a clip dragged in one go across several tracks of a new set, in each layout, driven by
# scripted pointer input in the real app (hidden, isolated config and HOME, the layout chosen as a person
# chooses it: the real settings keys, no --layout), over many frames:
#   1. a new set of the layout is saved, and one pattern clip put on its first track (nothing selected);
#   2. the clip is pressed (Clips: on its title bar) and dragged a bar on and three rows down over 30 frames:
#      Studio: on track 3; Smooth: on track 3 (the rows below the new set's one track are spare rows that take
#      no room until used: the region stopped on the last row and could not go down at all); Clips: the new
#      set's tracks are two MIDI tracks over two audio tracks: the clip stays on the last MIDI track (1) and
#      still follows the pointer in time (the whole move stopped as the pointer reached an audio track);
#   3. Smooth: the same drag with a region shown part-way (the dock tree rebuilt under the pressed button):
#      the drag goes on to track 3 (it ended on the first track it reached).
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -DLAYOUT=studio|logic|live -P drag1119_new_set_drag_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

function(fail msg)
  message(FATAL_ERROR "FAIL (${LAYOUT}): ${msg}\n${out}")
endfunction()

# run <tag> <song or ""> <script>
function(run tag song script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline")
  file(MAKE_DIRECTORY "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"${LAYOUT}\", \"keys.profile\": \"${LAYOUT}\", \"ui.menuStyle\": \"${LAYOUT}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  set(songArg "")
  if(song)
    set(songArg "${song}")
  endif()
  set(showArg "")
  if(LAYOUT STREQUAL "studio")  # (Studio's channel rack floats over the playlist)
    set(showArg --show playlist)
  endif()
  execute_process(COMMAND "${APP}" ${songArg} --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900 ${showArg}
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.log" "${o}\n${err}")
  set(out "${o}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (${LAYOUT}) ${tag}: gridline-app failed (${rc}): ${err}")
  endif()
endfunction()

# 1. the layout's new set, with one clip of its first pattern on the first track
run(fresh "" "4 typing off\n20 save ${OUT}/fresh.gridline.json\n24 quit discard\n")
file(READ "${OUT}/fresh.gridline.json" song)
if(NOT song MATCHES "\"patterns\": \\[[^{]*{[^}]*\"id\": ([0-9]+)")
  fail("the new set has no pattern")
endif()
set(pat ${CMAKE_MATCH_1})
set(clip "{\"id\": 91, \"length\": 768, \"pattern\": ${pat}, \"start\": 0, \"track\": 0}")
set(live "")
if(LAYOUT STREQUAL "live")
  set(live "\"live\": {\"view\": \"arrangement\"}, ")
endif()
string(REGEX REPLACE "\"playlist\": \\[[ \n]*\\]" "${live}\"playlist\": [${clip}]" song "${song}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 200" song "${song}")
file(WRITE "${OUT}/song.gridline.json" "${song}")

run(geo "${OUT}/song.gridline.json" "4 typing off\n12 plgeom\n14 dump\n16 quit discard\n")
if(NOT out MATCHES "plgeom frame [0-9]+: valid 1 x0 [-0-9.]+ pxPerTick ([0-9]+)\\.([0-9][0-9][0-9])")
  fail("no playlist geometry")
endif()
set(whole ${CMAKE_MATCH_1})
string(REGEX REPLACE "^0+(.)" "\\1" f "${CMAKE_MATCH_2}")
math(EXPR ppt "${whole} * 1000 + ${f}")
math(EXPR bar "384 * ${ppt} / 1000")
if(NOT out MATCHES "\n  clip 91 x ([-0-9]+)[.0-9]* ([-0-9]+)[.0-9]* y ([-0-9]+)[.0-9]* ([-0-9]+)")
  fail("clip 91 is not drawn")
endif()
set(x0 ${CMAKE_MATCH_1})
set(x1 ${CMAKE_MATCH_2})
set(y0 ${CMAKE_MATCH_3})
set(y1 ${CMAKE_MATCH_4})
if(NOT out MATCHES "\n  track 0 top ([-0-9]+)[.0-9]* h ([0-9]+)")
  fail("no geometry for track 0")
endif()
set(rowH ${CMAKE_MATCH_2})
math(EXPR down "${rowH} * 3")  # three rows of the first track's height (Smooth's spare rows have none yet)
math(EXPR px "(${x0} + ${x1}) / 2")
math(EXPR py "(${y0} + ${y1}) / 2")
if(LAYOUT STREQUAL "live")  # Clips: a clip moves by its title bar (the body selects time)
  math(EXPR py "${y0} + 5")
endif()

# drag <tag> <extra line or "">: pressed, dragged a bar on and three rows down over 30 frames, let go.
function(drag tag extra)
  set(sc "4 typing off\n30 move ${px} ${py}\n32 down 0\n")
  foreach(i RANGE 1 30)
    math(EXPR fi "32 + ${i}")
    math(EXPR xi "${px} + ${bar} * ${i} / 30")
    math(EXPR yi "${py} + ${down} * ${i} / 30")
    string(APPEND sc "${fi} move ${xi} ${yi}\n")
  endforeach()
  string(APPEND sc "${extra}66 up 0\n72 dump\n76 quit discard\n")
  run(${tag} "${OUT}/song.gridline.json" "${sc}")
  string(REGEX MATCH "\n  clip 91 kind [0-9]+ track ([0-9]+) start ([0-9]+)" _ "${out}")
  set(c_track "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(c_start "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(want what got expect)
  if(NOT "${got}" STREQUAL "${expect}")
    fail("${what}: want ${expect}, got ${got}")
  endif()
  message(STATUS "ok - ${LAYOUT}: ${what}")
endfunction()

drag(plain "")
if(LAYOUT STREQUAL "live")
  want("the MIDI clip stays on the last MIDI track over the audio tracks" "${c_track}" "1")
  want("and follows the pointer a bar on" "${c_start}" "384")
else()
  want("the clip goes three tracks down in one drag" "${c_track}" "3")
  want("and a bar on" "${c_start}" "384")
endif()

if(LAYOUT STREQUAL "logic")
  drag(rebuild "44 layout show logic.right\n")
  want("a region shown part-way through the drag: it goes on to track 3" "${c_track}" "3")
endif()
