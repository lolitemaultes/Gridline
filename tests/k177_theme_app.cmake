# Gridline: K-177 (GitHub issue 42), theme brightness to pitch black and white text in the real
# app (hidden, isolated configs, 1440x900, a fixed 16 ms frame time). A copy of the test song gets
# a sample channel and audio clips. GL_THEME_DUMP prints the palette and the colours the top bar,
# the browser's folder glyphs and the playlist's waveforms were drawn with (theme::devNote), and
# the look given to the desktop title bar.
#   1. default dark: the top bar is the stock chrome, folders the lit folder role, title bar dark;
#   2. started at brightness -100% with white text: the top bar is pitch black, folders white,
#      waveforms lighter than in 1, the desktop title bar dark with a black caption;
#   3. the same switched live (as Settings > Theme does) from a default start: the same colours
#      as the fresh start in 2;
#   4. default light: the stock light chrome, a light title bar.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k177_theme_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GL_THEME_DUMP} "90")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)
set(chan "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clips "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 0, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(APPEND clips "{\"id\": 71, \"kind\": \"audio\", \"source\": 60, \"track\": 1, \"start\": 384, \"length\": 1536, \"pattern\": 0, \"gain\": 1.5}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chan}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clips}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# run(<tag> <settings values json> <dev-input script>) -> dump_<tag> (the THEME_DUMP line)
function(run tag values script)
  file(MAKE_DIRECTORY "${OUT}/${tag}/cfg/gridline")
  file(WRITE "${OUT}/${tag}/cfg/gridline/settings.json" "{\"version\": 1, \"values\": {${values}}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/${tag}/cfg")
  set(ENV{HOME} "${OUT}/${tag}")
  file(WRITE "${OUT}/${tag}/input.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames 110
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}/input.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL: ${tag}: gridline-app failed (${rc}):\n${err}\n${out}")
  endif()
  if(NOT err MATCHES "THEME_DUMP frame [0-9]+ ([^\n]*)")
    message(FATAL_ERROR "FAIL: ${tag}: no theme dump:\n${err}")
  endif()
  set(d "${CMAKE_MATCH_1}")
  file(WRITE "${OUT}/${tag}.dump" "${d}\n")
  message(STATUS "${tag}: ${d}")
  set(dump_${tag} "${d}" PARENT_SCOPE)
endfunction()

# key(<dump> <key> <var>): the #RRGGBB (or number) after "<key>=".
function(key dump k var)
  if(NOT dump MATCHES "(^| )${k}=([#0-9A-Fa-f]+)")
    message(FATAL_ERROR "FAIL: no ${k} in the dump: ${dump}")
  endif()
  set(${var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
# lum(<#RRGGBB> <var>): r + 2g + b (0 .. 1020)
function(lum hex var)
  string(SUBSTRING "${hex}" 1 2 r)
  string(SUBSTRING "${hex}" 3 2 g)
  string(SUBSTRING "${hex}" 5 2 b)
  math(EXPR v "0x${r} + 2 * 0x${g} + 0x${b}")
  set(${var} ${v} PARENT_SCOPE)
endfunction()

set(darkMode "\"theme.mode\": \"dark\", \"ui.darkTheme\": true")
run(dark "${darkMode}" "")
run(black "${darkMode}, \"theme.brightness\": -1, \"theme.whiteText\": true" "")
run(live "${darkMode}" "20 theme brightness -1\n30 theme whitetext on\n")
run(light "\"theme.mode\": \"light\", \"ui.darkTheme\": false" "")

# 1. default dark
key("${dump_dark}" chrome chrome)
key("${dump_dark}" used.topbar top)
key("${dump_dark}" used.folder folder)
key("${dump_dark}" used.wave wave)
key("${dump_dark}" text text)
key("${dump_dark}" frame.dark fdark)
check("dark: the top bar is drawn in the theme chrome (${top})" top STREQUAL chrome AND chrome STREQUAL "#262C30")
check("dark: folders keep their lit role colour (${folder})" NOT folder STREQUAL text)
check("dark: the desktop title bar is dark" fdark EQUAL 1)
set(waveDark "${wave}")

# 2. pitch black + white text
key("${dump_black}" chrome chrome)
key("${dump_black}" used.topbar top)
key("${dump_black}" used.topbar.panel panel)
key("${dump_black}" used.folder folder)
key("${dump_black}" used.wave wave)
key("${dump_black}" text text)
key("${dump_black}" frame.caption cap)
key("${dump_black}" frame.dark fdark)
check("black: the top bar is pitch black from the theme (${top})" top STREQUAL "#000000" AND top STREQUAL chrome)
lum("${panel}" lp)
check("black: the top bar panels are near black (${panel})" lp LESS 40)
check("black: text is white" text STREQUAL "#FFFFFF")
check("black: browser folders are drawn white (${folder})" folder STREQUAL "#FFFFFF")
lum("${wave}" lw)
lum("${waveDark}" lwd)
check("black: waveforms come from the theme, lighter with white text (${wave} vs ${waveDark})" lw GREATER lwd)
check("black: the desktop title bar is dark with a black caption (${cap})" fdark EQUAL 1 AND cap STREQUAL "#000000")

# 3. live switch = fresh start (frame.count differs: windows updated twice)
string(REGEX REPLACE " frame\\.count=[0-9]+" "" a "${dump_live}")
string(REGEX REPLACE " frame\\.count=[0-9]+" "" b "${dump_black}")
check("live switch gives the colours of a fresh pitch-black white-text start" a STREQUAL b)

# 4. default light
key("${dump_light}" chrome chrome)
key("${dump_light}" used.topbar top)
key("${dump_light}" frame.dark fdark)
check("light: the stock light chrome (${top})" top STREQUAL chrome AND chrome STREQUAL "#C9D1D6")
check("light: the desktop title bar is light" fdark EQUAL 0)
