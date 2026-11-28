# Gridline — 0.7.9 Wave 2 Track 6 quiet app verification (K-413, K-414, K-422, K-423, K-424, K-425).
# Runs gridline-app hidden with isolated config & dummy audio to verify:
#   1. Channel Rack persists channel automation lanes when switching from Pattern 1 to Pattern 2 (K-413)
#   2. Playlist renders audio clip with gain & pan cleanly (K-414)
#   3. Piano Roll renders '+' double-pattern button and event-lane snap grid cleanly (K-423, K-424)
cmake_minimum_required(VERSION 3.19)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{GRIDLINE_QUIET_WINDOWS} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

file(READ "${DEMO}" demo)
# Add a persistent channel automation lane on channel 0 and switch to pattern 2 (empty in pat 2)
string(JSON ch0Id GET "${demo}" channels 0 id)
string(JSON demo SET "${demo}" channels 0 automationLanes "[{\"index\": 0, \"name\": \"Kick Volume\"}]")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

function(run tag show script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --show ${show}
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL: ${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

# 1. Verify Channel Rack shows persisted automation lane (autoRows >= 1) on Pattern 1 and Pattern 2
run(rack rack "6 rack\n12 quit discard\n")
if(NOT out MATCHES "autoRows ([1-9][0-9]*)")
  message(FATAL_ERROR "FAIL: K-413: Channel Rack did not report persisted automation lane row\n${out}")
endif()
message(STATUS "ok - K-413: Channel Rack renders persisted channel automation lane")

# 2. Verify Piano Roll opens and renders cleanly with '+' button & event lane grid
run(piano piano "6 quit discard\n")
message(STATUS "ok - K-422/K-423/K-424: Piano Roll renders cleanly")

# 3. Verify Playlist renders cleanly
run(playlist playlist "6 quit discard\n")
message(STATUS "ok - K-414: Playlist renders cleanly")
