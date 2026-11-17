# Gridline: K-327 (hidden runs, a copy of the test song, HOME and GRIDLINE_CONFIG_HOME both in OUT):
#   1. a --frames run whose script changes the song ends after its frames with no "save changes?"
#      prompt, says the changes were discarded and leaves the project file as it was; also when
#      the script asked to quit (the prompt up) before the last frame;
#   2. a --frames run with nothing changed ends quietly; a screenshot run on a changed song too;
#   3. opening the command palette (it walks every menu and settings page) and the menus reading
#      the collaboration settings with a TURN server set touch no secret: no AI assistant token is
#      made or stored by the walk.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k327_frames_palette_app.cmake
cmake_minimum_required(VERSION 3.19)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_NO_UPDATE_CHECK} "1")
set(ENV{GRIDLINE_SECRET_STORE} "file")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
file(SHA256 "${OUT}/song.gridline.json" songHash)

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\n(output in ${OUT})")
endfunction()

# run(<tag> <script text or "">  <extra args...>): output in ${out} and ${rc}
function(run tag script)
  file(MAKE_DIRECTORY "${OUT}/${tag}/home" "${OUT}/${tag}/cfg/gridline")
  set(ENV{HOME} "${OUT}/${tag}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/${tag}/cfg")
  set(args ${ARGN})
  if(NOT script STREQUAL "")
    file(WRITE "${OUT}/${tag}.txt" "${script}")
    list(APPEND args --dev-input "${OUT}/${tag}.txt")
  endif()
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1280x800 ${args}
                  RESULT_VARIABLE r OUTPUT_VARIABLE o ERROR_VARIABLE e TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.out" "${o}\n${e}")
  if(NOT r EQUAL 0)
    fail("${tag}: gridline-app failed or did not end (${r}): ${e}\n${o}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

set(note "--frames run ended: unsaved changes discarded")

# 1. Changed, no quit: ends after its frames.
run(changed "3 tempo 133\n5 docstate\n" --frames 30)
if(NOT out MATCHES "docstate modified 1")
  fail("changed: the script did not change the song")
endif()
string(FIND "${out}" "${note}" at)
if(at LESS 0)
  fail("changed: no note that the changes were discarded")
endif()
# 1b. Changed, then a quit without "discard" (the prompt comes up): still ends at the frame limit.
run(prompt "3 tempo 134\n6 quit\n" --frames 40)
string(FIND "${out}" "${note}" at)
if(at LESS 0)
  fail("prompt: no note that the changes were discarded")
endif()
file(SHA256 "${OUT}/song.gridline.json" after)
if(NOT after STREQUAL songHash)
  fail("the project file changed: a --frames run saved")
endif()

# 2. Nothing changed: ends, no note. A screenshot run on a changed song: ends with the note.
run(plain "" --frames 20)
string(FIND "${out}" "${note}" at)
if(at GREATER_EQUAL 0)
  fail("plain: a note though nothing changed")
endif()
run(shot "3 tempo 135\n" --frames 12 --screenshot "${OUT}/shot.ppm")
string(FIND "${out}" "${note}" at)
if(at LESS 0 OR NOT EXISTS "${OUT}/shot.ppm")
  fail("shot: no picture or no note")
endif()

# 3. The palette walk with a TURN server set and the assistant server off: no secret made or read.
file(MAKE_DIRECTORY "${OUT}/walk/cfg/gridline")
file(WRITE "${OUT}/walk/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"collab.turnHost\": \"turn.example.org\", \"collab.turnUser\": \"me\", \"mcp.enabled\": false}}\n")
run(walk "4 palette open\n10 palette dump\n14 palette close\n16 quit\n" --frames 30)
if(NOT out MATCHES "Token")
  fail("walk: the palette did not list the assistant settings (the settings pages were not read)")
endif()
if(EXISTS "${OUT}/walk/cfg/gridline/secrets/mcp-token")
  fail("walk: the palette's walk made and stored an assistant token (a secret store access)")
endif()
if(EXISTS "$ENV{HOME}/.config/gridline")
  fail("walk: a config folder under HOME although GRIDLINE_CONFIG_HOME is set")
endif()
message(STATUS "k327: all checks passed")
