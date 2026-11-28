# Gridline - 0.7.8: the Audio clip window's two views, Main and Advanced. Runs the app hidden (isolated
# config, fixed frame time) in Studio on a copy of the test song with a sampler channel "Take" playing an
# audio clip (clip 70) and a sampler channel "Kick" no clip plays, and checks:
#   1. a double-click opens the window on its Main view (the default); the page fits the window at
#      1440x900 without scrolling: nothing outside its card, over another, cut or wider than the page;
#   2. the Main view draws every control of the sampler channel's old window (the checklist of its
#      page: nothing missing), with the clip and with a channel alone; its controls change the song
#      in one undo step each and undo brings the song back;
#   3. the header's Advanced button switches to the tabs (the same clip, its time selection kept), and
#      the Advanced view still draws every control of both old windows (the full checklist);
#   4. the choice is remembered: the next run opens on Advanced, and Main switches back;
#   5. at 1024x700 the Main view still has every control, none cut or over another;
#   6. the Main view's boxes are evenly set at both sizes (within 1 px): the gaps between boxes and
#      between their lines, a line's box tops, bottoms, headings and knob names level, the chips'
#      middles level with their knobs', the lines' outer edges on the page's (the sample strip's);
#   7. the Advanced view is the window as it was before the Main view: on each tab the same
#      modules (ids, places and sizes) as the window drew them before it.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k078_clipview_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Kick.wav" COPYONLY)
file(READ "${DEMO}" demo)
set(chanJson "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, {\"color\": 14251600, \"gain\": 0.8, \"id\": 61, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Kick\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Kick.wav\", \"params\": []}}, ")
set(clipJson "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chanJson}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clipJson}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

set(cfg "${OUT}/cfg")
file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
file(WRITE "${cfg}/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"studio\", \"keys.profile\": \"studio\", \"ui.menuStyle\": \"studio\"}}\n")

function(run tag size script)
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size ${size} --frames 400
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# The n-th (0-based) "clipeditor state" line: its clip, time selection and view.
function(stateAt n clip sel view)
  string(REGEX MATCHALL "clipeditor state frame [0-9]+ [^\n]*" lines "${out}")
  list(GET lines ${n} l)
  string(REGEX MATCH "clip ([0-9]+) .* sel ([0-9.]+ [0-9.]+) .* view ([a-z]+)" _ "${l}")
  set(${clip} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${sel} "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${view} "${CMAKE_MATCH_3}" PARENT_SCOPE)
endfunction()

set(pagePattern "outside 0 overlaps 0 clipped 0 cut 0 wide 0 apart 0")
set(spacePattern "gaps [01] tops [01] bottoms [01] headings [01] names [01] linegaps [01] centres [01] edges [01]")
set(mainKeys vol pan pitch colour gain reverse.clip semis engine startend removedc smpfades oneshot reverse.notes loop.notes root volenv filter lfo fx.shape
             fx.tone fx.space)

# ---- 1, 2, 3: the clip on the Main view, then Advanced
set(s "4 clipeditor reach on\n6 clipeditor dbl 70\n12 clipeditor state\n14 clipeditor menu\n18 clipeditor checklist main\n20 clipeditor mainpage\n")
string(APPEND s "22 save \"${OUT}/before.gridline.json\"\n")
set(f 24)
foreach(k ${mainKeys})
  math(EXPR u "${f} + 2")
  string(APPEND s "${f} clipeditor poke ${k}\n${u} undo\n")
  math(EXPR f "${f} + 4")
endforeach()
string(APPEND s "${f} save \"${OUT}/after.gridline.json\"\n")
math(EXPR f "${f} + 2")
string(APPEND s "${f} clipeditor select 120 480\n")
math(EXPR f "${f} + 2")
string(APPEND s "${f} sdlat ceviewadvanced\n")
math(EXPR f "${f} + 2")
string(APPEND s "${f} sdldown 0\n")
math(EXPR f "${f} + 1")
string(APPEND s "${f} sdlup 0\n")
math(EXPR f "${f} + 3")
# (again: the first click closes the header menu the checklist opened; Advanced twice stays Advanced)
string(APPEND s "${f} sdldown 0\n")
math(EXPR f "${f} + 1")
string(APPEND s "${f} sdlup 0\n")
math(EXPR f "${f} + 6")
string(APPEND s "${f} clipeditor state\n")
math(EXPR f "${f} + 2")
string(APPEND s "${f} clipeditor reach on\n")
math(EXPR f "${f} + 2")
foreach(t RANGE 0 11)
  string(APPEND s "${f} clipeditor tab ${t}\n")
  math(EXPR f "${f} + 4")
endforeach()
string(APPEND s "${f} clipeditor menu\n")
math(EXPR f "${f} + 4")
string(APPEND s "${f} clipeditor checklist\n")
math(EXPR f "${f} + 2")
string(APPEND s "${f} quit discard\n")
run(clip 1440x900 "${s}")
stateAt(0 C SEL V)
check("a double-click opens the clip (${C}) on the Main view (${V})" C EQUAL 70 AND V STREQUAL "main")
check("the Main view has every control of the old sampler window, nothing missing" "${out}" MATCHES "clipeditor checklist main reached [0-9]+ missing 0\n")
check("the Main view fits the window at 1440x900: no scrolling, nothing outside, over another or cut"
      "${out}" MATCHES "clipeditor mainpage frame [0-9]+ shown 1 [^\n]*${pagePattern} scroll 0")
foreach(k ${mainKeys})
  check("main: ${k} is one undo step" "${out}" MATCHES "clipeditor poke ${k} done undo 1\n")
endforeach()
file(READ "${OUT}/before.gridline.json" before)
file(READ "${OUT}/after.gridline.json" after)
check("undo brought every change back" before STREQUAL after)
stateAt(1 C SEL V)
check("the header's Advanced button shows the tabs (${V}), the same clip (${C}) and its selection (${SEL})"
      V STREQUAL "advanced" AND C EQUAL 70 AND SEL STREQUAL "120.0 480.0")
check("the Advanced view still has every control of both old windows" "${out}" MATCHES "clipeditor checklist reached [0-9]+ missing 0\n")
check("the Main view's boxes are evenly set at 1440x900 (${spacePattern})" "${out}" MATCHES "clipeditor mainpage frame [0-9]+ shown 1 [^\n]*scroll 0 ${spacePattern}")


# ---- 4: remembered; Main switches back
run(again 1440x900 "6 clipeditor dbl 70
12 clipeditor state
14 sdlat ceviewmain
16 sdldown 0
17 sdlup 0
24 clipeditor state
26 quit discard
")
stateAt(0 C SEL V)
check("the next run opens on Advanced, as last chosen (${V})" V STREQUAL "advanced")
stateAt(1 C SEL V)
check("the header's Main button goes back to the one page (${V})" V STREQUAL "main")

# ---- 2 (a channel alone) and 5: the Kick sampler, then the clip at 1024x700
run(channel 1440x900 "4 clipeditor reach on
6 clipeditor channel 61
12 clipeditor menu
16 clipeditor checklist main
18 clipeditor mainpage
20 quit discard
")
check("a sampler channel alone: the Main view has every control of the old window" "${out}" MATCHES "clipeditor checklist main reached [0-9]+ missing 0\n")
check("a sampler channel alone: the page fits, nothing outside, over another or cut" "${out}" MATCHES "clipeditor mainpage frame [0-9]+ shown 1 [^\n]*${pagePattern} scroll 0")
run(small 1024x700 "4 clipeditor reach on
6 clipeditor dbl 70
12 clipeditor menu
16 clipeditor checklist main
18 clipeditor mainpage
20 quit discard
")
check("1024x700: the Main view has every control of the old window" "${out}" MATCHES "clipeditor checklist main reached [0-9]+ missing 0\n")
check("1024x700: nothing outside its card, over another or cut" "${out}" MATCHES "clipeditor mainpage frame [0-9]+ shown 1 [^\n]*${pagePattern}")
check("1024x700: the boxes are evenly set (${spacePattern})" "${out}" MATCHES "clipeditor mainpage frame [0-9]+ shown 1 [^\n]* ${spacePattern}")

# ---- 7: the Advanced view is the window as it stood before the Main view. On each tab, its modules
# (id, left, top, width, height at 1440x900) as that window drew them (taken from it, this song).
# (0.7.9: the Clip module holds Pan beside Gain (K-414) and the Channel module Transpose (K-411): both are wider)
set(advancedModules "clip 62 560 532 262|fades 602 560 532 262|output 1146 560 232 262|warp 62 560 753 262|smpwarp 823 560 311 262|output 1146 560 232 262|sample 62 560 640 262|output 1146 560 232 262|playback 62 560 428 262|voice 498 560 636 262|output 1146 560 232 262|env 62 560 738 262|lfo 808 560 326 262|output 1146 560 232 262|filter 62 560 664 262|output 1146 560 232 262|fxshape 62 560 436 262|fxtone 506 560 436 262|fxspace 950 560 184 262|output 1146 560 232 262|channel 62 560 635 262|poly 705 560 429 262|output 1146 560 232 262|echo 62 560 618 262|notefx 688 560 446 262|output 1146 560 232 262|layer 62 560 391 262|macros 461 560 673 262|output 1146 560 232 262|clip 62 560 532 262|fades 602 560 532 262|output 1146 560 232 262|clip 62 560 532 262|fades 602 560 532 262|output 1146 560 232 262")
file(WRITE "${cfg}/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.clipWindowView\": \"advanced\", \"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"studio\", \"keys.profile\": \"studio\", \"ui.menuStyle\": \"studio\"}}\n")
set(s "6 clipeditor dbl 70\n")
set(f 20)
foreach(t RANGE 0 11)
  math(EXPR u "${f} + 6")
  string(APPEND s "${f} clipeditor tab ${t}\n${u} clipeditor strip\n")
  math(EXPR f "${f} + 10")
endforeach()
string(APPEND s "${f} quit discard\n")
run(advanced 1440x900 "${s}")
string(REGEX MATCHALL "clipeditor module [^\n]* compact [01]" mods "${out}")
string(REGEX REPLACE "clipeditor module ([^ ]+) (-?[0-9]+) (-?[0-9]+) ([0-9]+) ([0-9]+) compact [01]" "\\1 \\2 \\3 \\4 \\5" mods "${mods}")
string(REPLACE ";" "|" mods "${mods}")
check("the Advanced view's tabs hold the modules they held before the Main view, where they were" mods STREQUAL advancedModules)
