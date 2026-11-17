# Gridline - K-112B (R14-01): Smooth's control-bar Record button works as the R key and the other
# layouts' buttons. Real app, hidden, isolated config, fixed frame time.
#   1. Stopped: a click arms Record, starts playback and opens the Recording card; a second click
#      stops recording while playback goes on.
#   2. Quick punch while playing: a click punches in (the transport keeps playing), the next punches out.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k112b_record_button_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

function(run tag input)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"logic\", \"keys.profile\": \"logic\", \"ui.menuStyle\": \"logic\"}}\n")
  file(WRITE "${cfg}/input.txt" "5 toasts clear\n10 playmode song\n${input}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  execute_process(COMMAND "${APP}" "${DEMO}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --layout logic --layout-dev
                          --dev-input "${cfg}/input.txt" --frames 200
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_log "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# 1. Stopped.
run(stopped "20 recmore cardstate\n24 sdlat rec\n26 sdldown 0\n28 sdlup 0\n40 recmore cardstate\n44 sdlat rec\n46 sdldown 0\n48 sdlup 0\n60 recmore cardstate\n64 key space\n70 quit discard\n")
check("before the click: no card, not armed, stopped" stopped_log MATCHES "recmore frame 20: card 0 armed 0 playing 0")
check("a click while stopped records and plays, with the Recording card" stopped_log MATCHES "recmore frame 40: card 1 armed 1 playing 1")
check("a second click stops recording, playback goes on" stopped_log MATCHES "recmore frame 60: card 0 armed 0 playing 1")

# 2. Quick punch while playing.
run(punch "14 recmore quick 1\n20 key space\n30 recmore cardstate\n34 sdlat rec\n36 sdldown 0\n38 sdlup 0\n50 recmore cardstate\n54 sdlat rec\n56 sdldown 0\n58 sdlup 0\n70 recmore cardstate\n74 key space\n80 quit discard\n")
check("playing before the punch" punch_log MATCHES "recmore frame 30: card [01] armed 0 playing 1")
check("quick punch: a click punches in, playback goes on" punch_log MATCHES "recmore frame 50: card [01] armed 1 playing 1")
check("quick punch: the next click punches out, playback goes on" punch_log MATCHES "recmore frame 70: card [01] armed 0 playing 1")
