# Gridline - K-112c: showing the mixer in the Clips layout lands in its home.
#   Three ways in, each from a clean config in the Clips layout (the region shell, as a development
#   layout): the dev option --show mixer, the Clips mixer shortcut (Ctrl+Alt+M) and --pane mixer. Each time the
#   Mixer window is drawn docked in the mixer band (live.mixer), the arrangement stays docked in the
#   main view (live.main), and the dock tree is not left empty (it holds the arrangement).
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112_clips_mixer_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(failures 0)

# run(<tag> <dev input> args...): one run in its own config folder; checks the windows at frame 40.
function(run tag input)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"clips.arrangementSets\": \"${OUT}/${tag}.gridline.json\"}}\n")
  # (N45-10: the song was last saved in the arrangement, so Clips opens it there, as these runs expect.)
  file(WRITE "${cfg}/input.txt" "5 toasts clear\n${input}40 layout where Mixer\n40 layout where Playlist\n40 layout docks\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${OUT}/${tag}.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${OUT}/${tag}.gridline.json" --hidden --no-ui-sounds --size 1440x900 --layout live --layout-dev
                          --dev-input "${cfg}/input.txt" --frames 50 ${ARGN} --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  set(bad 0)
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "FAIL: ${tag}: gridline-app failed (${rc})")
    set(bad 1)
  endif()
  if(NOT out MATCHES "where Mixer layout live drawn 1 docked 1 region live\\.mixer ")
    message(SEND_ERROR "FAIL: ${tag}: the mixer is not docked in the mixer band")
    set(bad 1)
  endif()
  if(NOT out MATCHES "where Playlist layout live drawn 1 docked 1 region live\\.main ")
    message(SEND_ERROR "FAIL: ${tag}: the arrangement is not docked in the main view")
    set(bad 1)
  endif()
  if(NOT out MATCHES "dock r[.0-9]* split - central [01] rect [-0-9. ]+ windows \\[[A-Za-z ,]*Playlist")
    message(SEND_ERROR "FAIL: ${tag}: no dock node holds the arrangement")
    set(bad 1)
  endif()
  if(bad)
    message(STATUS "${tag} output:\n${out}")
  else()
    message(STATUS "ok - ${tag}: Mixer in live.mixer, Playlist in live.main")
  endif()
endfunction()

run(show "" --show mixer)
run(key "10 sdlkey m down ctrl+alt\n11 sdlkey m up ctrl+alt\n")  # the Clips keymap's mixer shortcut
run(pane "" --pane mixer)
