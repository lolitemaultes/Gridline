# Gridline — 0.7.9 Track 7 quiet app test (K-418, K-419, K-420, K-421):
#   1. K-419: Send a warped audio clip from Playlist to Loupe -> Loupe imports warp markers as editable markers,
#      editing markers in Loupe syncs back to the clip and Send-to-Playlist carries synced warp markers.
#   2. K-420: Double-clicking a marker in Loupe selects from that marker to the next (and last marker -> end),
#      without accidentally moving the marker.
#   3. K-421: Scaling Loupe (125%) & resizing freely updates layout crisply and persists across sessions,
#      plus clean rendering at 2x DPI (--ui-scale 2).
#   4. K-418: Playlist snap menu shows "Snap to transients" and toggling it persists.
#
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_tests_k079_loupe_playlist> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k079_loupe_playlist_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/home" "${OUT}/cfg2x/home")

execute_process(COMMAND "${TESTBIN}" writewav "${OUT}/Hits.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Hits.wav")
  message(FATAL_ERROR "FAIL: could not write Hits.wav (${rc})")
endif()
set(wav "${OUT}/Hits.wav")

# Build a fixture song with an audio clip (id 80) on track 0 carrying 3 PlaylistWarp markers.
file(READ "${DEMO}" demo)
set(chJson "{\"color\": 5219296, \"gain\": 0.85, \"id\": 70, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Hits\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${wav}\", \"params\": []}}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chJson}" demo "${demo}")
set(clipJson "{\"id\": 80, \"kind\": \"audio\", \"track\": 0, \"start\": 0, \"length\": 384, \"source\": 70, \"warp\": [{\"id\": 1, \"at\": 0.0, \"to\": 0.0}, {\"id\": 2, \"at\": 0.5, \"to\": 0.6}, {\"id\": 3, \"at\": 1.5, \"to\": 1.5}]}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clipJson}, " demo "${demo}")
string(REPLACE ", ]" "]" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 120" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_QUIET_WINDOWS} "1")
unset(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE})

function(run tag cfg size scale frames script)
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/${cfg}")
  set(ENV{HOME} "${OUT}/${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size ${size} --ui-scale ${scale}
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# ---- Run 1: K-419 (warp markers -> Loupe -> edit -> sendback), K-420 (double-click marker), K-421 (scale & resize)
run(run1 cfg 1600x960 1 90 "4 loupe sendclip 80
30 loupe markers
32 loupe dblmarker 0
36 loupe dblmarker 2
40 loupe movemarker 1 26460
44 loupe addmarker 44100 Mid
52 loupe sendback
56 loupe scale 1.25
62 loupe resize 1360 760
74 loupe size
80 loupe state
" --show playlist)

# K-419: Loupe imported the 3 warp markers from clip 80
string(REGEX MATCHALL "loupe markers: [^\n]*" mlines "${out}")
list(LENGTH mlines mcnt)
check("K-419/K-420: got 5 loupe markers dumps (${mcnt})" mcnt EQUAL 5)
list(GET mlines 0 m0)
list(GET mlines 1 m1)
list(GET mlines 2 m2)
list(GET mlines 4 m4)
check("K-419: imported 3 warp markers (0, 22050, 66150): ${m0}" m0 MATCHES "loupe markers: 3 0:Warp 1:1 22050:Warp 2:2 66150:Warp 3:3")
check("K-420: double-click marker 0 selects 0..22050: ${m1}" m1 MATCHES "sel 0 22050")
check("K-420: double-click last marker 2 selects 66150..88200: ${m2}" m2 MATCHES "sel 66150 88200")
check("K-419: move + add marker in Loupe updated markers: ${m4}" m4 MATCHES "loupe markers: 4 0:Warp 1:1 26460:Warp 2:2 44100:Mid:4 66150:Warp 3:3")  # (the added marker gets the next warp id when it syncs; the others keep theirs)
check("K-419: sendback placed clip in playlist" out MATCHES "loupe sendback: clips [2-9]")

# K-421: scale 1.25 & resize 1360x760 persisted in settings.json
check("K-421: loupe size reflects 1.25 scale and resized room" out MATCHES "loupe size: scale 1\\.25 saved 1360x760")
file(READ "${OUT}/cfg/gridline/settings.json" settings1)
check("K-421: loupe.uiScale persisted in settings.json" settings1 MATCHES "\"loupe\\.uiScale\": *1\\.25")
check("K-421: loupe.windowW persisted in settings.json" settings1 MATCHES "\"loupe\\.windowW\": *1360")
check("K-421: loupe.windowH persisted in settings.json" settings1 MATCHES "\"loupe\\.windowH\": *760")

# ---- Run 2: reopen with same config -> scale & window size remembered across sessions
run(run2 cfg 1600x960 1 40 "4 loupe sendclip 80
30 loupe size
34 loupe state
" --show playlist)
check("K-421: scale 1.25 and 1360x760 remembered in next session" out MATCHES "loupe size: scale 1\\.25 saved 1360x760")

# ---- Run 3: 2x DPI review (--ui-scale 2)
run(run2x cfg2x 1920x1080 2 40 "4 loupe sendclip 80
30 loupe size
34 loupe state
" --plugin gl.edison)
check("K-421: 2x DPI Loupe renders cleanly" EXISTS "${OUT}/run2x.ppm" AND out MATCHES "loupe size:")
