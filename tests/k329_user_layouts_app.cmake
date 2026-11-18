# Gridline - K-329: what people see never depends on the developer switch.
#   Runs the app as a person does: no --layout-dev, no ui.layoutDev in the settings (a release
#   build ignores both anyway). Clips, Smooth and the Tracker are built from their region shells
#   ("layout dump" reports "shell <id> built"), and a run with --layout-dev reports the same
#   shell. In Smooth the palette's MIDI settings row carries no shortcut (Cmd+, is Settings there).
#   In the Tracker the palette's About Gridline opens the header band's About, not the About window.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k329_user_layouts_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(failures 0)

# run(<tag> <layout> args...): one run in its own config folder; sets <tag>_log.
function(run tag lay)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"${lay}\", \"keys.profile\": \"${lay}\", \"ui.menuStyle\": \"${lay}\"}}\n")
  file(WRITE "${cfg}/input.txt"
       "5 toasts clear\n20 layout dump\n24 palette open\n28 palette dump\n30 palette close\n34 palette run win:About Gridline\n40 tracker swapstate\n44 quit discard\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${cfg}/song.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${cfg}/song.gridline.json" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900
                          --dev-input "${cfg}/input.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "FAIL: ${tag}: gridline-app failed (${rc})")
  endif()
  set(${tag}_log "${out}" PARENT_SCOPE)
endfunction()

foreach(lay live logic tracker)
  run(${lay} ${lay})
  run(${lay}_dev ${lay} --layout-dev)
  if(NOT ${lay}_log MATCHES "shell ${lay} built")
    message(SEND_ERROR "FAIL: ${lay}: not built from its region shell without --layout-dev (the people's build would show the old arrangement)")
  else()
    message(STATUS "${lay}: shell built without --layout-dev")
  endif()
  string(REGEX MATCH "shell ${lay} [a-z0-9]+" plain "${${lay}_log}")
  string(REGEX MATCH "shell ${lay} [a-z0-9]+" dev "${${lay}_dev_log}")
  if(NOT plain STREQUAL dev)
    message(SEND_ERROR "FAIL: ${lay}: '${plain}' without --layout-dev, '${dev}' with it")
  endif()
endforeach()

# Smooth: Cmd+, (Ctrl+,) is Settings..., so the MIDI settings row shows no shortcut.
string(REGEX MATCH "ROW\t[a-z]+\t[^\t]*\t[^\t]*\t[01]\t[^\t]*\tMIDI settings...\t[^\t]*\t([^\n]*)" m "${logic_log}")
if(NOT m)
  message(SEND_ERROR "FAIL: logic: no MIDI settings row in the palette")
elseif(NOT "${CMAKE_MATCH_1}" STREQUAL "")
  message(SEND_ERROR "FAIL: logic: the palette's MIDI settings row shows '${CMAKE_MATCH_1}' (the Settings key)")
else()
  message(STATUS "logic: MIDI settings row has no shortcut")
endif()

# Tracker: About from the palette (as from Help) is the header band's About.
if(NOT tracker_log MATCHES "tracker swapstate frame [0-9]+: 4 about-window 0")
  string(REGEX MATCH "tracker swapstate[^\n]*" got "${tracker_log}")
  message(SEND_ERROR "FAIL: tracker: About Gridline did not open the header's About (${got})")
else()
  message(STATUS "tracker: About Gridline opens the header's About")
endif()
if(NOT live_log MATCHES "tracker swapstate frame [0-9]+: 0 about-window 1")
  message(SEND_ERROR "FAIL: live: About Gridline did not open the About window")
endif()
