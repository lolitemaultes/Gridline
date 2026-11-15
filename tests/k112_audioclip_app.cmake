# Gridline - K-112: the Live clip band shows a selected audio clip as an audio clip view.
# Runs the app hidden (isolated config, fixed frame time) on a copy of the test song with an audio clip
# added, in the Live and Studio layouts, and checks:
#   A. Live: with the audio clip selected (dev "clipsel"), the band draws the audio clip view;
#   B. Live: with no clip selected, the band goes back to the notes editor;
#   C. Studio: the piano roll never shows the audio clip view.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k112_audioclip_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)
set(chan "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clips "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chan}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clips}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

function(run tag layout frames script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames ${frames}
                          --show piano --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${last_out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

function(view out frame var)
  string(REGEX MATCH "clipview frame ${frame}: audio ([01])" m "${out}")
  if(NOT m)
    message(FATAL_ERROR "FAIL: no clipview line at frame ${frame}\n${out}")
  endif()
  set(${var} ${CMAKE_MATCH_1} PARENT_SCOPE)
endfunction()

# ---- A, B. Live: the audio clip selected, then none
run(live live 40 "6 clipsel 70\n20 clipview\n22 clipsel\n30 clipview\n")
set(last_out "${live_out}")
check("Live: the selection names the audio clip" "${live_out}" MATCHES "clipsel frame 6: audio clip 70")
view("${live_out}" 20 A)
check("Live: the band draws the audio clip view (${A})" A EQUAL 1)
view("${live_out}" 30 B)
check("Live: with no clip selected the band shows the notes editor again (${B})" B EQUAL 0)

# ---- C. Studio: never the audio clip view
run(studio studio 40 "6 clipsel 70\n20 clipview\n")
set(last_out "${studio_out}")
view("${studio_out}" 20 C)
check("Studio: the piano roll stays the piano roll (${C})" C EQUAL 0)
