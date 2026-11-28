# Gridline: K-445, "Show in Finder / Explorer / Files" in the file panes of the Tracker and Smooth
# layouts, as the browser has it in Studio and Clips. The app runs hidden (isolated config and HOME,
# SDL's dummy audio; a hidden run prints "reveal: <folder>" instead of opening a file manager):
#   1. Tracker, Files page: right-click a folder of the project's folder, Show in: that folder;
#   2. ... a song file: the folder holding it;
#   3. Smooth, Library: right-click a patch of an SF2 bank (a row from a file), Show in: the bank's folder.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DBANK=<gridline_test_patchplayer> -DDEMO=<song> -DOUT=<dir> -P k445_reveal_panes_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home" "${OUT}/proj/Loop folder" "${OUT}/banks")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/proj/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/proj/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
set(SF "${OUT}/banks/Test Bank.sf2")
execute_process(COMMAND "${BANK}" --write "${SF}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${SF}")
  message(FATAL_ERROR "FAIL: the test bank was not written (${rc})")
endif()
configure_file("${DEMO}" "${OUT}/proj/song.gridline.json" COPYONLY)
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_CACHE_DIR} "${OUT}/cache")
if(CMAKE_HOST_APPLE)
  set(reveal "Show in Finder")
elseif(CMAKE_HOST_WIN32)
  set(reveal "Show in Explorer")
else()
  set(reveal "Show in Files")
endif()

function(run tag layout script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"patchplayer.files\": \"${SF}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/proj/song.gridline.json" --hidden --no-ui-sounds --size 1440x960 --layout ${layout}
                          --frames 70 --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.out" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# ---- 1-2. Tracker: the Files page lists the project's folder.
run(tracker tracker "6 tracker swap files
14 tracker editors menu at file Loop folder
16 sdldown 1
17 sdlup 1
20 tracker editors menu at ${reveal}
22 sdldown 0
23 sdlup 0
28 tracker editors menu at file song.gridline.json
30 sdldown 1
31 sdlup 1
33 shot \"${OUT}/tracker_menu.ppm\"
34 tracker editors menu at ${reveal}
36 sdldown 0
37 sdlup 0
66 quit discard
")
check("the Files page lists the folder and the song" NOT out MATCHES "menu row \"file [^\"]*\" none")
check("Show in on a folder opens that folder" out MATCHES "reveal: [^\n]*[/\\]proj[/\\]Loop folder\n")
check("Show in on a file opens the folder holding it" out MATCHES "reveal: [^\n]*[/\\]proj\n")

# ---- 3. Smooth: the Library's patch rows come from the bank (read in the background).
run(smoothrow logic "6 parts lib nav 0
8 parts lib filter Test Bank
10 waitfor k445-never 3
30 sdlat smlibpatch
34 quit discard
")
check("the Library lists the bank's patches" out MATCHES "sdlat frame 30: smlibpatch at [0-9]+,[0-9]+")
string(REGEX MATCH "sdlat frame 30: smlibpatch at ([0-9]+),([0-9]+)" _ "${out}")
set(px "${CMAKE_MATCH_1}")
set(py "${CMAKE_MATCH_2}")
run(smooth logic "6 parts lib nav 0
8 parts lib filter Test Bank
10 waitfor k445-never 3
30 sdlmove ${px} ${py}
32 sdldown 1
33 sdlup 1
36 shot \"${OUT}/smooth_menu.ppm\"
38 atitem \"${reveal}\"
39 down 0
40 up 0
66 quit discard
")
check("a patch row's menu has Show in" NOT out MATCHES "atitem frame [0-9]+: [^\n]* not shown")
check("Show in on a patch opens the bank's folder" out MATCHES "reveal: [^\n]*[/\\]banks\n")
message(STATUS "done")
