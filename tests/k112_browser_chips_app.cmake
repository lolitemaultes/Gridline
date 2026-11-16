# Gridline - K-112c: the Live browser's filter rows, in the real app (hidden, isolated config and
# HOME, a folder of samples added to the browser):
#   1. Live, a place that lists files (Folders): chips for the tags the files carry (kick, snare
#      among them), the most used first;
#   2. a click on the first chip searches for its tag; a second click clears the search;
#   3. Studio draws no filter rows on its first look (All tab); K-218: its Sounds tab shows them.
# Screenshot: <OUT>/chips.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k112_browser_chips_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/lib" "${OUT}/hm" "${OUT}/nofl")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{HOME} "${OUT}/hm")
set(ENV{GRIDLINE_FL_DATA} "${OUT}/nofl")  # no factory content: the index holds this folder and Gridline's own folders only
get_filename_component(src "${DEMO}" DIRECTORY)
set(wav "${src}/../../assets/sounds/classic/open.wav")
foreach(n "Kick 1" "Kick 2" "Snare 1")
  configure_file("${wav}" "${OUT}/lib/${n}.wav" COPYONLY)
endforeach()
configure_file("${DEMO}" "${OUT}/fixture.gridline.json" COPYONLY)

function(run tag layout script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline")
  file(WRITE "${cfg}/gridline/browser-folders.txt" "${OUT}/lib\n")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900
                          --frames 90 --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.out" "${out}\n---- stderr ----\n${err}")
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
function(chips_at var frame)
  string(REGEX MATCH "browserchips frame ${frame}: chips ([0-9]+) search \"([^\"]*)\"([^\n]*)" m "${out}")
  if(m STREQUAL "")
    message(FATAL_ERROR "FAIL: no browserchips line at frame ${frame}\nOutput:\n${out}")
  endif()
  set(${var}_n "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${var}_q "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${var}_tags "${CMAKE_MATCH_3}" PARENT_SCOPE)
endfunction()

# ---- Live: the chips, then a click on the first one (and again)
run(live live "6 browser cat 12\n40 browser chips\n42 sdlat brchip\n44 sdldown 0\n45 sdlup 0\n50 browser chips\n52 sdlat brchip\n54 sdldown 0\n55 sdlup 0\n60 browser chips\n")
chips_at(c0 40)
check("live: chips drawn (${c0_n})" c0_n GREATER 1)
# (The index holds only this folder and the empty Gridline folders: GRIDLINE_FL_DATA points at an empty one.)
check("... kick and snare among them (${c0_tags})" c0_tags MATCHES " kick=[0-9]+" AND c0_tags MATCHES " snare=[0-9]+")
string(REGEX MATCHALL "=[0-9]+" counts "${c0_tags}")
set(prev 999999999)
set(sorted 1)
foreach(c IN LISTS counts)
  string(SUBSTRING "${c}" 1 -1 v)
  if(v GREATER prev)
    set(sorted 0)
  endif()
  set(prev ${v})
endforeach()
check("... the most used first" sorted EQUAL 1)
string(REGEX MATCH "^ ([^=]+)=" _ "${c0_tags}")
set(want "tag:${CMAKE_MATCH_1}")
chips_at(c1 50)
check("a click on it searches for the tag (${c1_q})" c1_q STREQUAL want)
chips_at(c2 60)
check("a second click clears the search" NOT c2_q)

# ---- Studio: no filter rows on its first look (All tab); K-218: on the Sounds tab they show
run(studio studio "6 browser tab 0\n40 browser chips\n42 browser tab 3\n80 browser chips\n")
chips_at(s0 40)
check("studio: no chips on the All tab (${s0_n})" s0_n EQUAL 0)
chips_at(s1 80)
check("studio: chips on the Sounds tab (${s1_n})" s1_n GREATER 1)
