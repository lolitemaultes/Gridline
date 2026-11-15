# Gridline - K-289: which adds open a window by themselves, in the real app (hidden, isolated config
# and HOME, SDL's dummy audio driver), in the Studio, Clips and Smooth layouts:
#   1. an instrument from the channel rack's Add menu opens its window (an empty sampler too);
#   2. an audio file added to the rack never opens one: dropped from the browser, opened with a
#      double-click in the browser, dropped from the desktop;
#   3. a drop onto a mixer track opens nothing: an audio file (an impulse response), a built-in
#      effect, an installed effect (Clips opens an installed plugin's window when it is put on a
#      track by hand: not for a mixer drop);
#   4. an AI tool's add_channel opens nothing.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DCLAP=<GridlineTest.clap> -DOUT=<dir> -P k289_auto_open_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/open.wav" "${OUT}/hit.wav" COPYONLY)
set(W "${OUT}/hit.wav")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_DEV_PLUGIN_WINDOWS} "1")

function(run tag frames script)
  set(d "${OUT}/${tag}")
  file(MAKE_DIRECTORY "${d}/cfg/gridline" "${d}/home")
  file(WRITE "${d}/cfg/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${d}/cfg")
  set(ENV{HOME} "${d}/home")
  file(WRITE "${d}/input.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1440x900 --frames ${frames} --screenshot "${d}/end.ppm"
                          --dev-input "${d}/input.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${d}/run.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (${tag}): gridline-app exited with ${rc}:\n${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
  set(${tag}_err "${err}" PARENT_SCOPE)
endfunction()

function(fail tag what)
  message(FATAL_ERROR "FAIL (${tag}): ${what}. Output:\n${${tag}_out}")
endfunction()

# "instview frame <f>: open "<name>" type <type> focus "<window>" channels <n> undo <u> popup <0|1> text <0|1> fx <slot>"
function(view tag f)
  if(NOT ${tag}_out MATCHES "instview frame ${f}: open \"([^\"]*)\" type ([^ ]+) focus \"([^\"]*)\" channels ([0-9]+) undo [0-9]+ popup [01] text [01] fx (-?[0-9]+)")
    fail(${tag} "no instview report at frame ${f}")
  endif()
  set(v_open "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(v_type "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(v_channels "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(v_fx "${CMAKE_MATCH_5}" PARENT_SCOPE)
endfunction()

foreach(layout studio live logic)
  set(lay "")
  if(NOT layout STREQUAL "studio")
    set(lay --layout ${layout})
  endif()
  run(${layout} 200 "5 toasts clear
6 closeplugins
10 instview
14 addvia rack gl.fm
20 instview
22 closeplugins
26 addvia rack gl.sampler
32 instview
34 closeplugins
38 addvia filerack \"${W}\"
44 instview
46 closeplugins
50 addvia fileopen \"${W}\"
56 instview
58 closeplugins
62 addvia filedesk \"${W}\"
68 instview
70 closeplugins
74 addvia filemixer \"${W}\"
80 instview
82 closeplugins
86 addvia dropmixer gl.reverb
92 instview
94 closeplugins
98 mcp call add_channel {}
104 instview
106 channels
" ${lay})
  set(tag ${layout})
  view(${tag} 10)
  if(NOT v_open STREQUAL "-" OR NOT v_fx EQUAL -1)
    fail(${tag} "a window was open at the start ('${v_open}', fx ${v_fx})")
  endif()
  set(n ${v_channels})
  foreach(pair "20;gl.fm;an instrument from the Add menu" "32;gl.sampler;an empty sampler from the Add menu")
    list(GET pair 0 f)
    list(GET pair 1 t)
    list(GET pair 2 how)
    view(${tag} ${f})
    if(NOT v_type STREQUAL t)
      fail(${tag} "${how}: the open window is ${v_type} '${v_open}', wanted ${t}")
    endif()
    message(STATUS "ok - ${layout}: ${how} opens its window")
  endforeach()
  foreach(pair "44;an audio file dropped on the rack" "56;an audio file double-clicked in the browser" "68;an audio file dropped from the desktop"
               "80;an audio file dropped on a mixer track" "92;an effect dropped on a mixer track" "104;an AI tool's add_channel")
    list(GET pair 0 f)
    list(GET pair 1 how)
    view(${tag} ${f})
    if(NOT v_open STREQUAL "-" OR NOT v_fx EQUAL -1)
      fail(${tag} "${how} opened a window ('${v_open}', fx slot ${v_fx})")
    endif()
    message(STATUS "ok - ${layout}: ${how} opens nothing")
  endforeach()
  # Each add happened: 2 instruments, 3 audio channels and the AI tool's channel.
  math(EXPR want "${n} + 6")
  view(${tag} 104)
  if(NOT v_channels EQUAL want)
    fail(${tag} "${v_channels} channels after the adds, wanted ${want}")
  endif()
endforeach()

# An installed effect dropped on a mixer track in Clips (its windows open for a plugin put on a track
# by hand): it stays closed. An installed instrument from the Add menu still opens.
if(EXISTS "${CLAP}")
  foreach(layout live)
    run(hosted_${layout} 120 "5 toasts clear
10 junction scan clap \"${CLAP}\"
14 addvia dropmixer clap:org.gridline.test.gain
30 instview
40 addvia rack clap:org.gridline.test.synth
60 instview
" --layout ${layout} --plugin gl.patcher)  # (a scan runs from a Junction editor)
    set(tag hosted_${layout})
    if(NOT ${tag}_err MATCHES "\\[plugin-window\\] placed \\| clap:org.gridline.test.gain: stays closed")
      fail(${tag} "the installed effect dropped on a mixer track was set to open its window: ${${tag}_err}")
    endif()
    if(${tag}_err MATCHES "placed \\| clap:org.gridline.test.gain: opens")
      fail(${tag} "the installed effect dropped on a mixer track opens its window")
    endif()
    view(${tag} 30)
    if(NOT v_fx EQUAL -1)
      fail(${tag} "the installed effect's drop opened mixer slot ${v_fx}")
    endif()
    if(NOT ${tag}_err MATCHES "placed \\| clap:org.gridline.test.synth: opens")
      fail(${tag} "the installed instrument from the Add menu did not open its window: ${${tag}_err}")
    endif()
    message(STATUS "ok - ${layout}: an installed effect dropped on a mixer track stays closed; an instrument from the Add menu opens")
  endforeach()
endif()
message(STATUS "k289: auto-open rules hold in every layout")
