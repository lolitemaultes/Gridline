# Gridline: J-192, File > Import > Import from URL (hidden, isolated configs and HOME, 1280x800,
# a fixed 16 ms frame time). The tools are a fake yt-dlp / ffmpeg (tests/fake_media_tool.cpp):
# nothing touches the network.
#   flow      a link on the clipboard fills the field; cleared, Paste puts it back; Fetch shows the
#             preview; Import adds one audio clip on a new track (one undo step) named by the title,
#             the WAV and its source file in <project>/Imported; undo removes the clip
#   playlist  a playlist: all three items imported, three clips, one undo step
#   progress  a slow download shows its progress, Cancel stops it (nothing added)
#   error     the tool's error and its explanation
#   missing   a tool that is not there
#   setup     no tools: they are set up from a fake GitHub inside the popup, then the fetch goes on
#   cmake -DAPP=<gridline-app> -DTOOL=<fake tool> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j192_import_url.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_NO_UPDATE_CHECK} "1")
unset(ENV{XDG_DATA_HOME})
unset(ENV{XDG_CONFIG_HOME})
configure_file("${DEMO}" "${OUT}/fixture.gridline.json" COPYONLY)  # a copy: the demo song is never saved over

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  set(ENV{USERPROFILE} "${OUT}/cfg_${tag}")
  set(ENV{APPDATA} "${OUT}/cfg_${tag}/AppData")
  set(ENV{LOCALAPPDATA} "${OUT}/cfg_${tag}/LocalAppData")
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

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    fail("${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
# dump <frame> -> clips_<frame> (count), audio_<frame> (audio clips), undo_<frame>
function(dump frame)
  string(REGEX MATCH "dump frame ${frame}: ([0-9]+) clips, [0-9]+ selected, undo ([0-9]+)\n(  clip [^\n]*\n)*" d "${out}")
  if(d STREQUAL "")
    fail("no clip dump at frame ${frame}")
  endif()
  set(clips_${frame} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(undo_${frame} "${CMAKE_MATCH_2}" PARENT_SCOPE)
  string(REGEX MATCHALL "  clip [0-9]+ kind 1 " a "${d}")
  list(LENGTH a n)
  set(audio_${frame} ${n} PARENT_SCOPE)
endfunction()
function(state var frame)  # the "urlimport state" line printed at <frame> -> var (first one after the frame's dump)
  string(REGEX MATCHALL "urlimport state [^\n]*" lines "${out}")
  set(${var} "${lines}" PARENT_SCOPE)
endfunction()

set(ENV{GRIDLINE_YTDLP} "${TOOL}")
set(ENV{GRIDLINE_FFMPEG} "${TOOL}")
unset(ENV{GRIDLINE_URLIMPORT_FAKE_NET})
set(link "https://example.com/watch?v=abc123")
set(linkRe "https://example\\.com/watch\\?v=abc123")  # the link as a regular expression

# ---- flow ----
set(ENV{GL_FAKE_SCENARIO} "ok")
run(flow 70 "4 dump
5 urlimport clipboard ${link}
6 urlimport open
9 urlimport state
10 urlimport url
11 urlimport paste
12 urlimport state
13 urlimport fetch
14 waitfor urlimport-preview 60
20 shot \"${OUT}/shot_preview.ppm\"
21 urlimport state
22 urlimport import
23 waitfor urlimport-done 60
26 dump
27 channels
28 urlimport state
30 undo
33 dump
")
string(REGEX MATCHALL "urlimport state [^\n]*" st "${out}")
list(GET st 0 s0)
list(GET st 1 s1)
list(GET st 2 s2)
list(GET st 3 s3)
check("the clipboard's link fills the field on open" "${s0}" MATCHES "state entry url \"${linkRe}\"")
check("Paste puts the link back" "${s1}" MATCHES "state entry url \"${linkRe}\"")
check("Fetch shows the preview with the title" "${s2}" MATCHES "state preview .* title \"Fake Song: Take/One\"")
check("the popup closes after the import" "${s3}" MATCHES "state closed")
check("tools came from the environment" "${s2}" MATCHES "yt-dlp override ffmpeg override")
dump(4)
dump(26)
dump(33)
math(EXPR want "${clips_4} + 1")
check("one clip added (${clips_4} -> ${clips_26})" clips_26 EQUAL want)
math(EXPR wantA "${audio_4} + 1")
check("it is an audio clip" audio_26 EQUAL wantA)
math(EXPR wantU "${undo_4} + 1")
check("one undo step (${undo_4} -> ${undo_26})" undo_26 EQUAL wantU)
check("undo removes it" clips_33 EQUAL clips_4)
check("the channel is named by the title" "${out}" MATCHES "Fake Song: Take/One")
check("the WAV is in the project's Imported folder" EXISTS "${OUT}/Imported/Fake Song Take One.wav")
check("its source file is beside it" EXISTS "${OUT}/Imported/Fake Song Take One.wav.source.json")
file(READ "${OUT}/Imported/Fake Song Take One.wav.source.json" src)
check("the source keeps the link and uploader" "${src}" MATCHES "abc123.*Test Uploader|Test Uploader.*abc123")
file(REMOVE_RECURSE "${OUT}/Imported")

# ---- playlist ----
set(ENV{GL_FAKE_SCENARIO} "playlist")
run(playlist 60 "4 dump
6 urlimport open
7 urlimport url https://example.com/playlist?list=PL1
8 urlimport fetch
9 waitfor urlimport-preview 60
12 urlimport playlist all
16 shot \"${OUT}/shot_playlist.ppm\"
17 urlimport state
18 urlimport import
19 waitfor urlimport-done 60
22 dump
")
check("playlist preview with three items" "${out}" MATCHES "state preview .* title \"Fake Playlist\" items 3")
dump(4)
dump(22)
math(EXPR want "${clips_4} + 3")
check("three clips added (${clips_4} -> ${clips_22})" clips_22 EQUAL want)
math(EXPR wantU "${undo_4} + 1")
check("in one undo step" undo_22 EQUAL wantU)
check("the third item's name is made safe" EXISTS "${OUT}/Imported/Third Song Live.wav")
file(REMOVE_RECURSE "${OUT}/Imported")

# ---- progress, cancel ----
set(ENV{GL_FAKE_SCENARIO} "slowdl")
run(progress 80 "4 dump
6 urlimport open
7 urlimport url ${link}
8 urlimport fetch
9 waitfor urlimport-preview 60
10 urlimport import
11 waitfor urlimport-importing 30
40 shot \"${OUT}/shot_progress.ppm\"
41 urlimport state
42 urlimport cancel
43 waitfor urlimport-cancelled 30
60 urlimport state
61 dump
")
string(REGEX MATCHALL "urlimport state [^\n]*" st "${out}")
list(GET st 0 s0)
list(GET st 1 s1)
check("the download shows progress" "${s0}" MATCHES "state importing")
check("cancel returns to the preview" "${s1}" MATCHES "state preview")
dump(4)
dump(61)
check("nothing added after cancel" clips_61 EQUAL clips_4)

# ---- error ----
set(ENV{GL_FAKE_SCENARIO} "error")
run(error 30 "6 urlimport open
7 urlimport url https://example.com/not-media
8 urlimport fetch
9 waitfor urlimport-error 60
14 shot \"${OUT}/shot_error.ppm\"
15 urlimport state
")
check("the tool's error is shown with its explanation" "${out}" MATCHES "state error .*Unsupported URL.*not a media page")

# ---- missing tool ----
set(ENV{GL_FAKE_SCENARIO} "ok")
set(ENV{GRIDLINE_YTDLP} "${OUT}/no-such-yt-dlp")
run(missing 30 "6 urlimport open
7 urlimport url ${link}
8 urlimport fetch
14 shot \"${OUT}/shot_missing.ppm\"
15 urlimport state
")
check("a missing tool is explained" "${out}" MATCHES "state error .*yt-dlp none")

# ---- set up on first use (fake GitHub) ----
unset(ENV{GRIDLINE_YTDLP})
unset(ENV{GRIDLINE_FFMPEG})
set(ENV{GRIDLINE_URLIMPORT_FAKE_NET} "${TOOL}")
set(ENV{GRIDLINE_URLIMPORT_FAKE_STEP_MS} "150")
run(setup 60 "6 urlimport open
20 shot \"${OUT}/shot_empty.ppm\"
21 urlimport url ${link}
22 urlimport fetch
23 waitfor urlimport-setup 30
26 shot \"${OUT}/shot_setup.ppm\"
27 urlimport state
28 waitfor urlimport-preview 90
32 urlimport state
")
string(REGEX MATCHALL "urlimport state [^\n]*" st "${out}")
list(GET st 0 s0)
list(GET st 1 s1)
check("the tools are set up inside the popup" "${s0}" MATCHES "state setup")
check("then the fetch goes on with the installed tools" "${s1}" MATCHES "state preview .*yt-dlp installed ffmpeg installed")
unset(ENV{GRIDLINE_URLIMPORT_FAKE_NET})
