# Gridline - 0.7.8: the audio clip's and sampler's settings in each layout's own places. Runs the app
# hidden (isolated config, fixed frame time) on a copy of the test song with a sampler channel playing
# an audio clip (Take, clip 70) and a sampler channel no clip plays (Kick), at 1024 x 700 and 1440 x 900,
# and checks:
#   Clips, the clip band (the clip selected) and the device band (the Kick sampler's card): the band
#     shows its own view (the head card, the sample display, a card per module side by side), every
#     control of the Audio clip window is reached there (the checklist: nothing missing), nothing it
#     draws shows outside the band (nothing over the clip grid or the arrangement), no popup is open,
#     every control sits inside its card and none overlaps another; scrolled to its last cards, the same.
#   Smooth, an audio region: the inspector's Region card (the clip's settings as rows) and the lower
#     pane's audio editor (the local inspector with the sample's and the channel's sections, the
#     tools, the waveform) reach every control together, and neither draws outside its own place; the
#     Kick sampler alone (Channel settings): the audio editor reaches everything it needs.
#   The tools row (Smooth's audio editor, Studio's Audio clip window), also at the smallest window and
#     the window at its minimum size: nothing outside the row, nothing over anything else (Edit
#     audio... included); narrow, the view's options in the view menu.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k078_nativeclip_app.cmake
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

function(run tag layout size frames script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.clipWindowView\": \"advanced\", \"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size ${size} --frames ${frames}
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(run_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${run_out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# Every "clipeditor native" line of `slot` clean: nothing showing past its place, no popup, every
# control inside its card, none over another, every label with its width; at least `cards` cards.
function(checkNative what slot cards)
  string(REGEX MATCHALL "clipeditor native frame [0-9]+ slot ${slot} [^\n]*" lines "${run_out}")
  list(LENGTH lines n)
  check("${what}: drawn and measured (${n} checks)" n GREATER 0)
  foreach(l ${lines})
    check("${what}: nothing outside its place, no popup, controls inside their cards and apart (${l})"
          l MATCHES "outside 0 popups 0 cards [0-9]+ modules [0-9]+ ctloutside 0 overlaps 0 clipped 0$")
    string(REGEX MATCH "cards ([0-9]+)" m "${l}")
    check("${what}: ${CMAKE_MATCH_1} cards (at least ${cards})" CMAKE_MATCH_1 GREATER_EQUAL ${cards})
  endforeach()
endfunction()

foreach(size 1024x700 1440x900)
  # ---- Clips: the clip band (the clip selected), then scrolled to its last cards
  set(s "4 clipeditor reach on\n5 clipeditor bounds on\n6 clipeditor open 70\n14 clipeditor host clips\n16 clipeditor state\n18 clipeditor native\n")
  string(APPEND s "22 clipeditor tab 9\n28 clipeditor native\n30 clipeditor tab 0\n36 clipeditor native\n38 clipeditor geom\n40 clipeditor menu\n46 clipeditor checklist\n48 quit discard\n")
  run(clips_${size} live ${size} 58 "${s}")
  check("Clips ${size}: the clip band shows the clip" "${run_out}" MATCHES "clipeditor state frame [0-9]+ host clips shown 1 clip 70 ")
  check("Clips ${size}: no chip row" "${run_out}" MATCHES "host clips shown 1 [^\n]* chips 0\n")
  check("Clips ${size}, clip band: every control of the Audio clip window reached" "${run_out}" MATCHES "clipeditor checklist reached [0-9]+ missing 0\n")
  checkNative("Clips ${size}, clip band" 1 20)
  check("Clips ${size}, clip band: the head card, then the sample display" "${run_out}" MATCHES "clipeditor card \"head\"[^\n]*\nclipeditor card \"wave\"")

  # ---- Clips: the device view (the shared spot's view with no clip selected; Devices lit), the Kick
  # sampler selected: its card (no clip)
  set(s "3 typing select 1\n4 clipeditor reach on\n5 clipeditor bounds on\n12 livedetail\n16 clipeditor host device\n18 clipeditor state\n20 clipeditor native\n")
  string(APPEND s "24 clipeditor tab 8\n30 clipeditor native\n32 clipeditor menu\n38 clipeditor checklist\n40 quit discard\n")
  run(device_${size} live ${size} 50 "${s}")
  check("Clips ${size}: the shared spot shows the device view, Devices lit" "${run_out}" MATCHES "livedetail frame 12: clip 0 device 1 [^\n]* lit clip 0 device 1 ")
  check("Clips ${size}: the device band shows the Kick sampler" "${run_out}" MATCHES "clipeditor state frame [0-9]+ host device shown 1 clip 0 [^\n]* channel 61 ")
  check("Clips ${size}, device band: every control it needs reached" "${run_out}" MATCHES "clipeditor checklist reached [0-9]+ missing 0\n")
  checkNative("Clips ${size}, device band" 3 15)

  # ---- Smooth: an audio region (the inspector's Region card and the audio editor)
  set(s "4 clipeditor reach on\n5 clipeditor bounds on\n6 clipeditor dbl 70\n10 clipeditor unfold\n18 clipeditor host pane\n20 clipeditor state\n22 clipeditor native\n")
  string(APPEND s "24 clipeditor menu\n30 clipeditor checklist\n32 quit discard\n")
  run(smooth_${size} logic ${size} 42 "${s}")
  check("Smooth ${size}: the audio editor shows the region" "${run_out}" MATCHES "clipeditor state frame [0-9]+ host pane shown 1 clip 70 ")
  check("Smooth ${size}: the inspector and the audio editor reach every control" "${run_out}" MATCHES "clipeditor checklist reached [0-9]+ missing 0\n")
  checkNative("Smooth ${size}, the audio editor" 2 10)
  checkNative("Smooth ${size}, the inspector's Region card" 4 3)

  # ---- Smooth: the Kick sampler alone (Channel settings)
  set(s "4 clipeditor reach on\n5 clipeditor bounds on\n6 plugwin chan 1\n10 clipeditor unfold\n18 clipeditor host pane\n20 clipeditor state\n22 clipeditor native\n")
  string(APPEND s "24 clipeditor menu\n30 clipeditor checklist\n32 quit discard\n")
  run(smoothk_${size} logic ${size} 42 "${s}")
  check("Smooth ${size}: Channel settings of the Kick sampler open the audio editor on it" "${run_out}" MATCHES "clipeditor state frame [0-9]+ host pane shown 1 clip 0 [^\n]* channel 61 ")
  check("Smooth ${size}, the Kick sampler: every control it needs reached" "${run_out}" MATCHES "clipeditor checklist reached [0-9]+ missing 0\n")
  checkNative("Smooth ${size}, the Kick sampler's audio editor" 2 10)
endforeach()

# ---- The tools row: nothing over anything else, nothing outside the row, at every size (the smallest
# window too); Studio's Audio clip window at its minimum size puts the view's options in its menu.
foreach(size 480x400 1024x700 1440x900)
  run(tools_smooth_${size} logic ${size} 34 "6 clipeditor dbl 70\n18 clipeditor host pane\n24 clipeditor toolrow\n28 quit discard\n")
  check("Smooth ${size}: the audio editor's tools row fits" "${run_out}" MATCHES "clipeditor toolrow frame [0-9]+ width [0-9]+ items [1-9][0-9]* menu [01] outside 0 overlaps 0\n")
  run(tools_studio_${size} studio ${size} 34 "6 clipeditor dbl 70\n14 clipeditor toolrow\n16 clipeditor winsize 720 420\n24 clipeditor toolrow\n28 quit discard\n")
  string(REGEX MATCHALL "clipeditor toolrow [^\n]*" rows "${run_out}")
  list(LENGTH rows n)
  check("Studio ${size}: the window's tools row, twice" n EQUAL 2)
  list(GET rows 0 r0)
  list(GET rows 1 r1)
  check("Studio ${size}: the Audio clip window's tools row fits" "${r0}" MATCHES "items [1-9][0-9]* menu [01] outside 0 overlaps 0$")
  check("Studio ${size}: at its minimum size, the view's options in the menu, nothing over Edit audio..." "${r1}" MATCHES "width 720 items [1-9][0-9]* menu 1 outside 0 overlaps 0$")
endforeach()
