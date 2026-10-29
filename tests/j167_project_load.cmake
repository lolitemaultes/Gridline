# Gridline — J-167: opening a project never freezes the interface (hidden off-screen app runs,
# isolated config). A slow project (the demo plus sampler channels on Gridline's own sounds, a
# sample that is gone; GRIDLINE_DEV_SLOW_LOAD_MS makes every plugin start take a while) is opened
# through File > Open while the demo is open:
#   1. the file dialog closes the moment OK is pressed and the loading window takes over;
#   2. frames keep coming while the plugins start one by one (the counts grow frame by frame);
#   3. the window shows the progress and the log, the missing sample as a warning;
#   4. input does not reach the project behind (Space, a click on Play: the transport stays stopped);
#   5. Esc cancels: the demo is back exactly as it was (channels, undo position);
# a second run lets the load finish (the new channels, the warning kept on a card), a third opens a
# corrupt file: the reason shows in the window and Close (Esc) returns to the demo.
# Nothing waits on wall-clock time: the slow plugins take one frame each, whatever the machine.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DSOUNDS=<assets/sounds> -DOUT=<dir> -P j167_project_load.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x (CMP0054: a quoted "keep" is that string, never a variable)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

# The slow project: the demo with one sampler channel per sound of two themes and one whose file is gone.
file(READ "${DEMO}" demo)
file(GLOB wavs "${SOUNDS}/classic/*.wav" "${SOUNDS}/prism/*.wav")
list(SORT wavs)
list(SUBLIST wavs 0 12 wavs)
set(chans "")
set(id 9001)
set(n 0)
foreach(w IN LISTS wavs)
  math(EXPR n "${n} + 1")
  string(APPEND chans "{\"id\": ${id}, \"name\": \"Load ${n}\", \"mixerTrack\": 3, \"gain\": 0.7, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${w}\"}}, ")
  math(EXPR id "${id} + 1")
endforeach()
string(APPEND chans "{\"id\": ${id}, \"name\": \"Load gone\", \"mixerTrack\": 3, \"gain\": 0.7, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/gone/missing-kick.wav\"}}, ")
math(EXPR id "${id} + 1")
string(REPLACE "\"channels\": [" "\"channels\": [${chans}" slow "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": ${id}" slow "${slow}")
file(WRITE "${OUT}/slow project.gridline.json" "${slow}")
file(WRITE "${OUT}/broken.gridline.json" "{\"version\": 9, \"channels\": [ {\"id\": 1, ")
configure_file("${DEMO}" "${OUT}/demo.gridline.json" COPYONLY)

function(run tag script slowMs)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  set(ENV{GRIDLINE_DEV_SLOW_LOAD_MS} "${slowMs}")
  execute_process(COMMAND "${APP}" "${OUT}/demo.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1280x800 ${ARGN}
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.out" "${out}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# The value of `field` on the "loadstate frame <frame>:" line.
function(state out tag frame field)
  string(REGEX MATCH "loadstate frame ${frame}: [^\n]*" line "${${tag}_out}")
  if(line STREQUAL "")
    message(FATAL_ERROR "FAIL (${tag}): no loadstate at frame ${frame}\n${${tag}_out}")
  endif()
  if(field STREQUAL "line")
    set(${out} "${line}" PARENT_SCOPE)
    return()
  endif()
  string(REGEX MATCH " ${field} ([^ ]+)" m "${line}")
  set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(fail tag what)
  message(FATAL_ERROR "FAIL (${tag}): ${what}\n${${tag}_out}")
endfunction()

# The channel list of the "channels frame <frame>:" report.
function(channels out tag frame)
  string(REGEX MATCH "channels frame ${frame}: [^\n]*\n(  channel [^\n]*\n)*" block "${${tag}_out}")
  string(REGEX REPLACE "^channels frame ${frame}: " "" block "${block}")
  set(${out} "${block}" PARENT_SCOPE)
endfunction()

# ---- 1-5: open through the file dialog, watch, try to play, cancel ----
set(script "2 loadhold off
2 toasts clear
3 channels
3 transport
5 dialogopen \"${OUT}/slow project.gridline.json\"
6 loadstate
7 loadstate
8 waitfor project-load-preparing 120
9 loadstate
10 key space
11 sdlat play
12 sdldown 0
13 sdlup 0
14 loadstate
15 transport
17 loadstate
19 shot \"${OUT}/loading.ppm\"
20 loadstate
21 key escape
23 loadstate
24 channels
24 transport
26 shot \"${OUT}/after-cancel.ppm\"
28 quit discard
")
run(cancel "${script}" 150)

state(s6 cancel 6 popups)
state(s7 cancel 7 popups)
state(p7 cancel 7 phase)
if(NOT s6 STREQUAL "[##filedialog]")
  fail(cancel "the file dialog was not up before OK (popups ${s6})")
endif()
if(NOT s7 STREQUAL "[##projectload]" OR p7 STREQUAL "idle")
  fail(cancel "the file dialog did not give way to the loading window at once (popups ${s7}, phase ${p7})")
endif()
message(STATUS "ok - the file dialog closed at once; the loading window took over")

state(a9 cancel 9 plugins)
state(a14 cancel 14 plugins)
state(a17 cancel 17 plugins)
state(a20 cancel 20 plugins)
state(ph20 cancel 20 phase)
string(REGEX REPLACE "/.*" "" d9 "${a9}")
string(REGEX REPLACE "/.*" "" d14 "${a14}")
string(REGEX REPLACE "/.*" "" d17 "${a17}")
string(REGEX REPLACE "/.*" "" d20 "${a20}")
string(REGEX REPLACE ".*/" "" total "${a20}")
if(NOT ph20 STREQUAL "preparing" OR NOT d14 GREATER d9 OR NOT d17 GREATER d14 OR NOT d20 GREATER d17 OR NOT total GREATER d20)
  fail(cancel "the plugins did not start one frame at a time while frames kept coming (${a9} ${a14} ${a17} ${a20}, phase ${ph20})")
endif()
message(STATUS "ok - frames kept coming during the load: plugins ${a9} -> ${a14} -> ${a17} -> ${a20}")

state(line20 cancel 20 line)
state(shown20 cancel 20 shown)
state(warn20 cancel 20 warnings)
if(NOT shown20 STREQUAL "1" OR warn20 LESS 1 OR NOT line20 MATCHES "now \"Starting plugin ")
  fail(cancel "the window did not show the progress and the current plugin (${line20})")
endif()
if(NOT cancel_out MATCHES "log [0-9.]+ 2 Sample not found: [^\n]*missing-kick.wav" OR NOT cancel_out MATCHES "log [0-9.]+ 0 Started [^\n]*ms\\)")
  fail(cancel "the log lacks the missing sample warning or the timed plugin lines")
endif()
message(STATUS "ok - the window shows the progress, the current plugin and the log (missing sample as a warning)")

string(REGEX MATCH "transport frame 15: [^\n]*" t15 "${cancel_out}")
if(NOT t15 MATCHES "playing 0")
  fail(cancel "Space or a click on Play reached the project behind the loading window (${t15})")
endif()
message(STATUS "ok - Space and a click on Play did not reach the project behind")

state(p23 cancel 23 phase)
state(pop23 cancel 23 popups)
channels(before cancel 3)
channels(after cancel 24)
string(REGEX MATCH "transport frame 3: [^\n]*" t3 "${cancel_out}")
string(REGEX MATCH "transport frame 24: [^\n]*" t24 "${cancel_out}")
string(REPLACE "frame 3:" "" t3 "${t3}")
string(REPLACE "frame 24:" "" t24 "${t24}")
if(NOT p23 STREQUAL "idle" OR NOT pop23 STREQUAL "[]")
  fail(cancel "Esc did not cancel the load and close the window (phase ${p23}, popups ${pop23})")
endif()
if(before STREQUAL "" OR NOT before STREQUAL after OR NOT t3 STREQUAL t24)
  fail(cancel "the project after the cancel differs from the one before:\n${before}\n---\n${after}\n${t3} / ${t24}")
endif()
message(STATUS "ok - Esc cancelled; the open project is exactly as it was")

# ---- the load finishes: the new project, the warning kept on a card ----
set(script "2 toasts clear
3 open \"${OUT}/slow project.gridline.json\"
4 channels
4 loadstate
6 quit discard
")
run(finish "${script}" 20)
channels(loaded finish 4)
state(pf finish 4 phase)
if(NOT loaded MATCHES "Load 12 type gl.sampler" OR NOT pf STREQUAL "idle")
  fail(finish "the slow project did not open (phase ${pf})")
endif()
if(NOT finish_out MATCHES "toast \"Opened slow project.gridline.json: Sample not found: [^\n]*missing-kick.wav \\(\\+?[0-9]*\\)?")
  if(NOT finish_out MATCHES "toast \"Opened slow project.gridline.json: Sample not found")
    fail(finish "the warning did not stay on a notification card")
  endif()
endif()
message(STATUS "ok - the load finished: the new project is open, its warning stays on a card")

# ---- a corrupt file: the reason in the window, Close returns to the project ----
set(script "2 loadhold off
3 channels
4 open \"${OUT}/broken.gridline.json\"
6 loadstate
8 shot \"${OUT}/error.ppm\"
9 loadstate
10 key escape
12 loadstate
12 channels
14 quit discard
")
run(error "${script}" 0)
state(e9 error 9 phase)
state(es9 error 9 shown)
state(ee9 error 9 errors)
state(e12 error 12 phase)
channels(eb error 3)
channels(ea error 12)
if(NOT e9 STREQUAL "failed" OR NOT es9 STREQUAL "1" OR NOT ee9 STREQUAL "1" OR NOT e12 STREQUAL "idle" OR NOT eb STREQUAL ea)
  fail(error "a corrupt file did not show its reason, or Close did not return to the project (${e9} ${es9} ${ee9} ${e12})")
endif()
message(STATUS "ok - a corrupt file shows why in the window; Close returns to the project as it was")
