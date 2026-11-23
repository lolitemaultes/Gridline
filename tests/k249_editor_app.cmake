# Gridline - K-249: the audio clip editor opens on an audio clip's double-click in every layout.
# Runs the app hidden (isolated config, fixed frame time) on a copy of the test song with an audio clip
# added, and checks:
#   A. Studio: a double-click opens the editor's window on the clip;
#   B. Clips: the clip band draws the editor (the clip selected);
#   C. Smooth: the lower pane draws it;
#   D. Tracker: the Audio page draws it;
#   E. the setting "Double-click an audio clip" = Playback tab: the same window, on its Playback tab;
#   F. Edit audio: Reverse writes a new file the clip's channel plays, one undo step brings the old one back.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k249_editor_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)
set(chanJson "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clipJson "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chanJson}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clipJson}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

function(run tag layout extra frames script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.clipWindowView\": \"advanced\", \"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\", \"tracker.sampleView\": \"advanced\"${extra}}}\n")  # the Tracker starts on its classic Sample editor since K-391; this test checks the Advanced editor
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${last_out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# The state line at a frame: host, shown, clip.
function(state out frame host shown clip)
  string(REGEX MATCH "clipeditor state frame [0-9]+ host ([a-z]+) shown ([01]) clip ([0-9]+)" m "${out}")
  if(NOT m)
    message(FATAL_ERROR "FAIL: no clipeditor state line at frame ${frame}\n${out}")
  endif()
  set(${host} ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${shown} ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${clip} ${CMAKE_MATCH_3} PARENT_SCOPE)
endfunction()

set(script "6 clipeditor dbl 70\n30 clipeditor state\n32 clipeditor geom\n40 quit discard\n")

# ---- A. Studio: the window
run(studio studio "" 50 "${script}")
set(last_out "${studio_out}")
state("${studio_out}" 30 H S C)
check("Studio: the editor's window shows the clip (${H} ${S} ${C})" H STREQUAL "window" AND S EQUAL 1 AND C EQUAL 70)

# ---- B. Clips: the clip band
run(clips live "" 50 "${script}")
set(last_out "${clips_out}")
state("${clips_out}" 30 H S C)
check("Clips: the clip band draws the editor (${H} ${S} ${C})" H STREQUAL "clips" AND S EQUAL 1 AND C EQUAL 70)

# ---- C. Smooth: the lower pane
run(smooth logic "" 50 "${script}")
set(last_out "${smooth_out}")
state("${smooth_out}" 30 H S C)
check("Smooth: the lower pane draws the editor (${H} ${S} ${C})" H STREQUAL "pane" AND S EQUAL 1 AND C EQUAL 70)

# ---- D. Tracker: the Audio page
run(tracker tracker "" 50 "${script}")
set(last_out "${tracker_out}")
state("${tracker_out}" 30 H S C)
check("Tracker: the Audio page draws the editor (${H} ${S} ${C})" H STREQUAL "pane" AND S EQUAL 1 AND C EQUAL 70)

# ---- E. The double-click on the Playback tab (the setting once said "Channel settings")
run(oldway studio ", \"ui.clipDoubleClick\": \"channel\"" 50 "${script}")
set(last_out "${oldway_out}")
state("${oldway_out}" 30 H S C)
check("Setting Playback tab: the window opens on the clip (${S} ${C})" S EQUAL 1 AND C EQUAL 70)
check("Setting Playback tab: on the Playback tab" "${oldway_out}" MATCHES "clipeditor state frame [0-9]+ host window shown 1 clip 70 tab 3 ")

# ---- F. Edit audio: Reverse makes a new file; undo brings the old one back
run(edit studio "" 80
    "6 clipeditor dbl 70\n20 clipeditor op Reverse\n24 clipeditor state\n26 save \"${OUT}/after.gridline.json\"\n30 undo\n34 save \"${OUT}/undone.gridline.json\"\n40 quit discard\n")
set(last_out "${edit_out}")
check("Reverse wrote a new file" "${edit_out}" MATCHES "last \"Reverse [^\"]+\\.wav\"")
file(READ "${OUT}/after.gridline.json" after)
file(READ "${OUT}/undone.gridline.json" undone)
check("The clip's channel plays the new file" NOT after MATCHES "\"data\": \"${OUT}/Take.wav\"")
check("Undo: the channel plays the old file again" undone MATCHES "\"data\": \"${OUT}/Take.wav\"")

# ---- G. The clip window against what plays: the waveform is drawn exactly where the clip sounds
# (the engine's spans, pass by pass), for a plain clip longer than its audio and a looping clip slipped,
# trimmed and started inside its loop, at another tempo. Within 2 px.
function(runsOf out kind frame var)
  string(REGEX MATCH "clipeditor ${kind} frame [0-9]+ px([^\n]*)" m "${out}")
  string(STRIP "${CMAKE_MATCH_1}" r)
  set(${var} "${r}" PARENT_SCOPE)
endfunction()
# Runs "a-b c-d" equal within 2 px (compared in tenths of a pixel).
function(sameRuns a b var)
  string(REPLACE " " ";" la "${a}")
  string(REPLACE " " ";" lb "${b}")
  list(LENGTH la na)
  list(LENGTH lb nb)
  set(ok 1)
  if(NOT na EQUAL nb OR na EQUAL 0 OR a STREQUAL "none" OR b STREQUAL "none")
    set(ok 0)
  else()
    math(EXPR last "${na} - 1")
    foreach(i RANGE ${last})
      list(GET la ${i} ra)
      list(GET lb ${i} rb)
      foreach(e a0 a1 b0 b1)
        set(${e} "")
      endforeach()
      if(ra MATCHES "^(-?[0-9]+)\\.([0-9])-(-?[0-9]+)\\.([0-9])$")
        set(a0 "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
        set(a1 "${CMAKE_MATCH_3}${CMAKE_MATCH_4}")
      endif()
      if(rb MATCHES "^(-?[0-9]+)\\.([0-9])-(-?[0-9]+)\\.([0-9])$")
        set(b0 "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
        set(b1 "${CMAKE_MATCH_3}${CMAKE_MATCH_4}")
      endif()
      if(a0 STREQUAL "" OR b0 STREQUAL "")
        set(ok 0)
      else()
        math(EXPR d0 "${a0} - ${b0}")
        math(EXPR d1 "${a1} - ${b1}")
        if(d0 GREATER 20 OR d0 LESS -20 OR d1 GREATER 20 OR d1 LESS -20)
          set(ok 0)
        endif()
      endif()
    endforeach()
  endif()
  set(${var} ${ok} PARENT_SCOPE)
endfunction()

foreach(variant plain loop)
  file(READ "${OUT}/song.gridline.json" song)
  if(variant STREQUAL "loop")
    string(REPLACE "\"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}"
                   "\"start\": 192, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0, \"audioStart\": 0.3, \"audioEnd\": 1.1, \"loop\": true, \"offset\": 40}" song "${song}")
    string(REGEX REPLACE "\"tempo\": *[0-9.]+" "\"tempo\": 97" song "${song}")
  endif()
  file(WRITE "${OUT}/song_${variant}.gridline.json" "${song}")
  set(cfg "${OUT}/cfg_win_${variant}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.clipWindowView\": \"advanced\", \"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"studio\", \"keys.profile\": \"studio\", \"ui.menuStyle\": \"studio\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  file(WRITE "${OUT}/win_${variant}.txt" "6 clipeditor dbl 70\n30 clipeditor geom\n34 quit discard\n")
  execute_process(COMMAND "${APP}" "${OUT}/song_${variant}.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames 50
                          --screenshot "${OUT}/win_${variant}.ppm" --dev-input "${OUT}/win_${variant}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/win_${variant}.log" "${out}\n${err}")
  set(last_out "${out}")
  check("${variant}: the app ran (${rc})" rc EQUAL 0)
  runsOf("${out}" audible 30 aud)
  runsOf("${out}" drawn 30 drw)
  sameRuns("${aud}" "${drw}" ok)
  check("${variant}: the waveform is drawn where the clip sounds (audible ${aud}; drawn ${drw})" ok EQUAL 1)
endforeach()
