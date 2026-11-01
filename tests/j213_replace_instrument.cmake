# Gridline: J-213, replacing the instrument of an open channel (the Replace instrument pick).
#  * the channel had its automatic name: it takes the new instrument's name (window title too);
#  * the open plugin window's preset selector shows the new plugin's Default, not the old preset;
#  * undo brings back the old instrument and name in one step, and the selector shows the preset it
#    had (redo: the new plugin's Default again); a name the user typed stays through a replace.
# Hidden app, isolated config.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j213_replace_instrument.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")

set(script "5 toasts clear
8 pluginview
10 pickpreset 1
14 pluginview
15 shot \"${OUT}/before.ppm\"
18 replace gl.fm
24 pluginview
25 shot \"${OUT}/after.ppm\"
28 undo
34 pluginview
36 redo
40 pluginview
42 renamechannel \"My lead\"
46 replace gl.sampler
52 pluginview
")
file(WRITE "${OUT}/input.txt" "${script}")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --size 1440x900 --plugin gl.kick --frames 58
                        --screenshot "${OUT}/s.ppm" --dev-input "${OUT}/input.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 200)
file(WRITE "${OUT}/run.log" "${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}")
endif()

# "pluginview frame <f>: window "<channel>" plugin <type> "<name>" header <type> preset "<label>" ab <A|B> undo ..."
function(view f)
  if(NOT out MATCHES "pluginview frame ${f}: window \"([^\"]*)\" plugin ([^ ]+) \"([^\"]*)\" header ([^ ]+) preset \"([^\"]*)\" ab ([AB])")
    message(FATAL_ERROR "FAIL: no plugin view report at frame ${f}. Output:\n${out}")
  endif()
  set(window "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(type "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(pname "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(header "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(preset "${CMAKE_MATCH_5}" PARENT_SCOPE)
endfunction()
function(fail what)
  message(FATAL_ERROR "FAIL (${what}). Output:\n${out}")
endfunction()

view(8)
set(kickName "${pname}")
if(NOT type STREQUAL "gl.kick" OR NOT window STREQUAL kickName OR NOT preset STREQUAL "Default")
  fail("start: a Kick channel named after it, Default preset (got ${window} ${type} ${preset})")
endif()
view(14)
set(picked "${preset}")
if(NOT header STREQUAL "gl.kick" OR picked STREQUAL "Default" OR picked STREQUAL "")
  fail("the header did not load a factory preset (got ${preset})")
endif()
view(24)
set(fmName "${pname}")
if(NOT type STREQUAL "gl.fm")
  fail("replace: the instrument is not FM (${type})")
endif()
if(NOT window STREQUAL fmName)
  fail("replace: the channel / window is still called '${window}', wanted '${fmName}'")
endif()
if(NOT header STREQUAL "gl.fm" OR NOT preset STREQUAL "Default")
  fail("replace: the plugin view shows preset '${preset}' of ${header}, wanted Default of gl.fm")
endif()
view(34)
if(NOT type STREQUAL "gl.kick" OR NOT window STREQUAL kickName)
  fail("undo: wanted the Kick back with its name (got ${window} ${type})")
endif()
if(NOT header STREQUAL "gl.kick")
  fail("undo: the plugin view still belongs to ${header}")
endif()
if(NOT preset STREQUAL picked)
  fail("undo: the preset selector shows '${preset}', wanted the Kick's '${picked}' back")
endif()
view(40)
if(NOT type STREQUAL "gl.fm" OR NOT window STREQUAL fmName OR NOT preset STREQUAL "Default")
  fail("redo: wanted FM, named after it, Default (got ${window} ${type} ${preset})")
endif()
view(52)
if(NOT type STREQUAL "gl.sampler" OR NOT window STREQUAL "My lead" OR NOT header STREQUAL "gl.sampler" OR NOT preset STREQUAL "Default")
  fail("typed name: wanted a Sampler still called My lead, Default (got ${window} ${type} ${header} ${preset})")
endif()
message(STATUS "j213: replace renames '${kickName}' (preset '${picked}') to '${fmName}' with the Default preset; undo brings '${picked}' back, redo / typed names right")
