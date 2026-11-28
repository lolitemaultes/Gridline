# Gridline — K-408 app integration test: Import from URL targeting audio-loading plugins
# directly (Sampler, Seltmann, Kerf, Platter) with fake_media_tool (ZERO network).
# Verifies: WAV lands in <project>/Imported with .source.json sidecar, plugin state loads the
# WAV directly (no stray playlist clip added), undo reverts it, and collab shares the WAV.
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_NO_UPDATE_CHECK} "1")
set(ENV{GRIDLINE_QUIET_WINDOWS} "1")
unset(ENV{XDG_DATA_HOME})
unset(ENV{XDG_CONFIG_HOME})
file(READ "${DEMO}" demo_json)
string(REPLACE "\"gl.kick\"" "\"gl.sampler\"" demo_json "${demo_json}")
string(REPLACE "\"gl.clap\"" "\"gl.seltmann\"" demo_json "${demo_json}")
file(WRITE "${OUT}/fixture.gridline.json" "${demo_json}")

set(ENV{GRIDLINE_YTDLP} "${TOOL}")
set(ENV{GRIDLINE_FFMPEG} "${TOOL}")
unset(ENV{GRIDLINE_URLIMPORT_FAKE_NET})
set(ENV{GL_FAKE_SCENARIO} "ok")
set(link "https://example.com/watch?v=k408plug")

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  set(ENV{USERPROFILE} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1280x800 --show playlist
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.out" "${out}")
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

# ---- Sampler channel (channel 0 in test_song) targeted via urlimport forplugin 0 ----
run(sampler 80 "4 dump
5 channels
6 urlimport forplugin 0 0
8 urlimport url ${link}
9 urlimport fetch
10 waitfor urlimport-preview 60
14 urlimport import
15 waitfor urlimport-done 60
20 dump
21 channels
22 undo
25 channels
")
string(REGEX MATCH "dump frame 4: ([0-9]+) clips, [0-9]+ selected, undo ([0-9]+)" _ "${out}")
set(clips_before "${CMAKE_MATCH_1}")
set(undo_before "${CMAKE_MATCH_2}")
string(REGEX MATCH "dump frame 20: ([0-9]+) clips, [0-9]+ selected, undo ([0-9]+)" _ "${out}")
set(clips_after "${CMAKE_MATCH_1}")
set(undo_after "${CMAKE_MATCH_2}")
check("no stray playlist clip added when loading into Sampler (${clips_before} -> ${clips_after})" clips_after EQUAL clips_before)
math(EXPR want_undo "${undo_before} + 1")
check("one undo step recorded (${undo_before} -> ${undo_after})" undo_after EQUAL want_undo)
check("imported WAV saved to project Imported folder" EXISTS "${OUT}/Imported/Fake Song Take One.wav")
check("source sidecar JSON written beside WAV" EXISTS "${OUT}/Imported/Fake Song Take One.wav.source.json")
check("Sampler channel data updated to imported WAV" "${out}" MATCHES "Fake Song Take One\\.wav")
file(REMOVE_RECURSE "${OUT}/Imported")

# ---- Collab: importing into plugin shares WAV into CollabSong/Audio/ ----
run(collab_plug 85 "5 collab profile Alex
8 collab new \"${OUT}/CollabPlug\"
12 urlimport forplugin 0 0
13 urlimport url ${link}
14 urlimport fetch
15 waitfor urlimport-preview 60
19 urlimport import
20 waitfor urlimport-done 60
25 channels
28 quit
")
string(REGEX MATCH "channel [0-9]+ [^\n]* data ([0-9a-f]+\\.wav)" _ "${out}")
set(sharedWav "${CMAKE_MATCH_1}")
check("collab plugin import uses shared copy (${sharedWav})" sharedWav MATCHES "\\.wav$")
check("shared WAV copied into CollabPlug/Audio" EXISTS "${OUT}/CollabPlug/Audio/${sharedWav}")
file(REMOVE_RECURSE "${OUT}/CollabPlug" "${OUT}/Imported")
