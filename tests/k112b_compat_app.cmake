# Gridline - K-112B (checkpoint 52, ahead of the cross-layout audit K-314): the project data only the
# Smooth arrange writes survives the other layouts, and Smooth's hidden spare rows stay a view matter.
#   1. The demo with every Smooth arrange field set (cycle, projectEnd, a track's lane, autoMode and
#      sound, a region's name and playback settings) is saved once from Smooth (the baseline).
#   2. The baseline opened and saved in Studio and in Tracker is byte-identical to it; in Clips the
#      only difference is Clips' own "live" view key. Every field is still there.
#   3. Each of those saves opened in Smooth again saves as the baseline.
#   4. (R12-05) A new project in Smooth shows one track row; saved, it keeps the song's default tracks,
#      and Studio shows them all as rows.
#   cmake -DAPP=<gridline-app> -DDEMO=<Gridline.gridline.json> -DOUT=<dir> -P k112b_compat_app.cmake
cmake_minimum_required(VERSION 3.19)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(run tag layout song input)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  if(layout STREQUAL "logic")
    set(extra ", \"keys.profile\": \"logic\", \"ui.menuStyle\": \"logic\"")
  else()
    set(extra "")
  endif()
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\"${extra}}}\n")
  file(WRITE "${cfg}/input.txt" "5 toasts clear\n${input}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  execute_process(COMMAND "${APP}" "${song}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x1400 --layout ${layout} --layout-dev
                          --dev-input "${cfg}/input.txt" --frames 60
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

# A save's text without Clips' own view key (Clips writes "live": {"view": ...} on every save).
function(withoutLive file var)
  file(READ "${file}" j)
  string(JSON j ERROR_VARIABLE e REMOVE "${j}" live)
  set(${var} "${j}" PARENT_SCOPE)
endfunction()

function(hasFields file)
  file(READ "${file}" j)
  string(JSON c ERROR_VARIABLE e1 GET "${j}" cycle on)
  string(JSON pe ERROR_VARIABLE e2 GET "${j}" projectEnd)
  string(JSON lo ERROR_VARIABLE e3 GET "${j}" tracks 0 lane owner)
  string(JSON am ERROR_VARIABLE e4 GET "${j}" tracks 0 autoMode)
  string(JSON so ERROR_VARIABLE e5 GET "${j}" tracks 1 sound)
  string(JSON nm ERROR_VARIABLE e6 GET "${j}" playlist 0 name)
  string(JSON tp ERROR_VARIABLE e7 GET "${j}" playlist 0 playTranspose)
  check("${file}: every Smooth arrange field is there (${c} ${pe} ${lo} ${am} ${so} ${nm} ${tp})"
        c STREQUAL "ON" AND pe EQUAL 30720 AND lo EQUAL ${owner} AND am EQUAL 3 AND so EQUAL ${owner} AND nm STREQUAL "Hook" AND tp EQUAL -5)
endfunction()

# ---- 1. the demo with every field set, saved once from Smooth
file(READ "${DEMO}" d)
string(JSON owner GET "${d}" channels 0 id)
string(JSON d SET "${d}" cycle "{\"start\": 1536, \"end\": 3072, \"on\": true}")
string(JSON d SET "${d}" projectEnd 30720)
string(JSON d SET "${d}" tracks 0 lane "{\"owner\": ${owner}, \"index\": 0, \"shown\": true}")
string(JSON d SET "${d}" tracks 0 autoMode 3)
string(JSON d SET "${d}" tracks 1 sound ${owner})
string(JSON d SET "${d}" playlist 0 name "\"Hook\"")
string(JSON d SET "${d}" playlist 0 playTranspose -5)
string(JSON d SET "${d}" playlist 0 playVelocity 9)
file(WRITE "${OUT}/s.gridline.json" "${d}")
run(base logic "${OUT}/s.gridline.json" "20 save ${OUT}/base.gridline.json\n24 quit discard\n")
hasFields("${OUT}/base.gridline.json")
file(READ "${OUT}/base.gridline.json" base)
withoutLive("${OUT}/base.gridline.json" baseNoLive)

# ---- 2 and 3. through Studio, Tracker and Clips, then back in Smooth
foreach(L studio tracker live)
  run(${L} ${L} "${OUT}/base.gridline.json" "20 save ${OUT}/${L}.gridline.json\n24 quit discard\n")
  hasFields("${OUT}/${L}.gridline.json")
  run(${L}_back logic "${OUT}/${L}.gridline.json" "20 save ${OUT}/${L}_back.gridline.json\n24 quit discard\n")
  if(L STREQUAL "live")
    withoutLive("${OUT}/${L}.gridline.json" t)
    withoutLive("${OUT}/${L}_back.gridline.json" tb)
    check("Clips' save is the baseline but for its own view key" t STREQUAL baseNoLive)
    check("Clips' save back in Smooth is the baseline but for Clips' view key" tb STREQUAL baseNoLive)
  else()
    file(READ "${OUT}/${L}.gridline.json" t)
    file(READ "${OUT}/${L}_back.gridline.json" tb)
    check("${L}'s save is byte-identical to Smooth's" t STREQUAL base)
    check("${L}'s save back in Smooth is byte-identical to the baseline" tb STREQUAL base)
  endif()
endforeach()

# ---- 4. R12-05: a new project in Smooth, then in Studio
run(blank logic "${DEMO}" "6 template -\n16 plrows\n20 save ${OUT}/new.gridline.json\n24 quit discard\n")
string(REGEX MATCHALL "plrow [0-9]+ \"[^\"]*\" [-0-9]+ [-0-9]+ top [-0-9]+ h [1-9][0-9]*" shown "${blank_log}")
list(LENGTH shown nShown)
check("a new project in Smooth shows one track row (${nShown})" nShown EQUAL 1)
file(READ "${OUT}/new.gridline.json" nj)
string(JSON nTracks LENGTH "${nj}" tracks)
check("the new project keeps the song's default tracks (${nTracks})" nTracks GREATER 1)
run(newstudio studio "${OUT}/new.gridline.json" "16 plrows\n20 save ${OUT}/new_studio.gridline.json\n24 quit discard\n")
string(REGEX MATCHALL "plrow [0-9]+ \"[^\"]*\" [-0-9]+ [-0-9]+ top [-0-9]+ h [1-9][0-9]*" shownS "${newstudio_log}")
list(LENGTH shownS nShownS)
check("Studio shows the new project's default rows (${nShownS} of ${nTracks})" nShownS GREATER 1)
file(READ "${OUT}/new_studio.gridline.json" ns)
check("Studio's save of the new project is byte-identical to Smooth's" ns STREQUAL nj)
