# Gridline - K-71, in the real app (hidden, isolated config, HOME and cache, dummy audio): a plugin scan
# never holds up the interface. A large plugin cache (synthetic) and a plugin folder that takes a while
# to search (symlink loops, a wide tree) with a few fake plugins in it; the Plugins settings page is
# open (it reads the scan status and the cache every frame). A Rescan of that folder, as its button
# does: the frame that asks for it and every frame while the scan runs stay short, the scan ends by
# itself and every fake plugin is listed as failed (the scanner children are this app).
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k71_scan_app.cmake
cmake_minimum_required(VERSION 3.18)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/plugins")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{XDG_CACHE_HOME} "${OUT}/xdgcache")
set(ENV{GRIDLINE_CACHE_DIR} "${OUT}/cache")
set(ENV{TMPDIR} "${OUT}/tmp")
file(MAKE_DIRECTORY "${OUT}/tmp")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")  # no scan at startup: the script starts the one it watches

# The cache: 4000 plugins with 40 parameters each (about 25 MB, like a big plugin collection).
set(param "")
foreach(j RANGE 39)
  string(APPEND param "{\"id\": ${j}, \"title\": \"Parameter number ${j} of a plugin with many of them\", \"units\": \"dB\", \"def\": 0.5},")
endforeach()
string(APPEND param "{\"id\": 99, \"title\": \"Last\", \"def\": 0}")
set(cache "{\"version\": 3, \"bundles\": [")
foreach(i RANGE 3999)
  if(i GREATER 0)
    string(APPEND cache ",")
  endif()
  string(APPEND cache "{\"format\": \"clap\", \"bundle\": \"/nowhere/cached${i}.clap\", \"mtime\": 1, \"ok\": true, \"classes\": [{\"uid\": \"k71.cached${i}\", "
                      "\"name\": \"Cached ${i}\", \"vendor\": \"Test\", \"params\": [${param}]}]}")
endforeach()
string(APPEND cache "]}")
file(WRITE "${OUT}/cfg/gridline/plugin-cache.json" "${cache}")
file(SIZE "${OUT}/cfg/gridline/plugin-cache.json" size)
message(STATUS "cache: ${size} bytes")

# The folder: three links back to itself, 400 empty folders beside them, six fake CLAP plugins.
set(root "${OUT}/plugins")
foreach(i 0 1 2)
  file(CREATE_LINK "${root}" "${root}/loop${i}" SYMBOLIC)
endforeach()
foreach(i RANGE 399)
  file(MAKE_DIRECTORY "${root}/wide/w${i}")
endforeach()
foreach(i RANGE 5)
  if(APPLE)
    file(MAKE_DIRECTORY "${root}/fake${i}.clap/Contents")
  else()
    file(WRITE "${root}/fake${i}.clap" "not a plugin")
  endif()
endforeach()

file(WRITE "${OUT}/script.txt" "20 scan state\n30 forget scan-idle\n30 scan folder clap \"${root}\"\n31 scan state\n32 waitfor scan-idle 120\n34 scan state\n36 quit discard\n")
execute_process(COMMAND "${APP}" "${DEMO}" --hidden --no-ui-sounds --no-update-check --size 1280x800 --show plugin-folders --dev-input "${OUT}/script.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 200)
file(WRITE "${OUT}/run.out" "${out}\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${out}")
endif()

string(REGEX MATCHALL "scan running [0-9]+ done [0-9]+ total [0-9]+ active [0-9]+ searching [0-9]+ failed [0-9]+ classes [0-9]+ longest-frame [0-9.]+ ms average-frame [0-9.]+ ms frames [0-9]+ second-frame [0-9.]+ ms lock-wait [0-9.]+ ms" lines "${out}")
list(LENGTH lines n)
# Frames 20, 30 (the Rescan, printed before that frame ends), 31 (covers frame 30) and 34 (covers the scan).
if(NOT n EQUAL 4)
  message(FATAL_ERROR "FAIL: four scan states expected, got ${n}\n${out}\n${err}")
endif()
list(GET lines 1 quiet)
list(GET lines 2 asked)
list(GET lines 3 after)
message(STATUS "quiet: ${quiet}")
message(STATUS "asked: ${asked}")
message(STATUS "after: ${after}")
string(REGEX REPLACE ".*average-frame ([0-9.]+) ms.*" "\\1" baseline "${quiet}")
string(REGEX REPLACE ".*longest-frame ([0-9.]+) ms.*" "\\1" askedMs "${asked}")
string(REGEX REPLACE ".*longest-frame ([0-9.]+) ms.*" "\\1" afterMs "${after}")
string(REGEX REPLACE ".*lock-wait ([0-9.]+) ms.*" "\\1" askedWait "${asked}")
string(REGEX REPLACE ".*lock-wait ([0-9.]+) ms.*" "\\1" afterWait "${after}")
if(NOT asked MATCHES "^scan running 1 ")
  message(FATAL_ERROR "FAIL: the Rescan did not start a scan: ${asked}")
endif()
# The interface never waits for the scan's lock (measured by the app, whatever the machine's speed).
if(askedWait GREATER 150 OR afterWait GREATER 150)
  message(FATAL_ERROR "FAIL: the interface waited ${askedWait} and ${afterWait} ms for the scan")
endif()
message(STATUS "ok - the interface never waited for the scan (${askedWait} and ${afterWait} ms)")
# Frame times are judged where the machine draws its quiet frames (frames 21 to 29, before the
# Rescan) quickly. Software OpenGL on a shared CI runner with other app tests beside it (quiet frames
# of 30 ms and more on average) holds up frames for seconds at times on its own: reported only.
if(baseline GREATER 30)
  message(STATUS "note - the frame of the Rescan took ${askedMs} ms, the longest during the scan ${afterMs} ms; "
                 "not judged: quiet frames here average ${baseline} ms")
else()
  if(askedMs GREATER 150)
    message(FATAL_ERROR "FAIL: the frame of the Rescan took ${askedMs} ms (the folder was searched on the interface thread)")
  endif()
  message(STATUS "ok - the frame of the Rescan stays short (${askedMs} ms)")
  if(afterMs GREATER 150)
    message(FATAL_ERROR "FAIL: a frame during the scan took ${afterMs} ms (the interface waited for the scan)")
  endif()
  message(STATUS "ok - every frame during the scan stays short (${afterMs} ms)")
endif()
if(NOT after MATCHES "^scan running 0 done 6 total 6 ")
  message(FATAL_ERROR "FAIL: the scan should end with the six fake plugins: ${after}")
endif()
message(STATUS "ok - the scan ends by itself with each fake plugin once (no copies through the loops)")
if(NOT after MATCHES " failed 6 ")
  message(FATAL_ERROR "FAIL: the six fake plugins should be listed as failed: ${after}")
endif()
message(STATUS "ok - the fake plugins are blacklisted")
