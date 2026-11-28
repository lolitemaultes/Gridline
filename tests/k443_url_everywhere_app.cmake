# Gridline: Import from URL in every plugin that loads audio, with a stand-in yt-dlp / ffmpeg
# (fake_media_tool: no network). For each plugin: the popup opened for it, a link read and
# imported, the WAV in the project's Imported folder, the plugin's data holds it (through the
# load its Load button uses), no playlist clip, one undo step, and Undo takes it back out.
# Also: a playlist imported into a multi-file plugin is one undo step; the Add menu's
# "Audio from URL as sampler..." adds a sampler channel per file; a Sampler takes the title as
# its name, as a dropped file does.
#   cmake -DAPP=<gridline-app> -DTOOL=<fake_media_tool> -DDEMO=<test_song> -DOUT=<dir> -P k443_url_everywhere_app.cmake
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
file(COPY_FILE "${DEMO}" "${OUT}/fixture.gridline.json")
set(ENV{GRIDLINE_YTDLP} "${TOOL}")
set(ENV{GRIDLINE_FFMPEG} "${TOOL}")
unset(ENV{GRIDLINE_URLIMPORT_FAKE_NET})
set(link "https://example.com/watch?v=k443")
set(wav "Fake Song Take One")

function(run tag script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}" "${OUT}/Imported")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  set(ENV{USERPROFILE} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1280x800 --show playlist
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.out" "${out}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    if(KEEP_GOING)  # -DKEEP_GOING=1: report every failing check (fail-first runs)
      message(SEND_ERROR "FAIL: ${msg}")
      return()
    endif()
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# The fixture's channel count: a new instrument is added after them.
run(count "4 channels
6 quit discard
")
string(REGEX MATCH "channels frame 4: ([0-9]+) channels" _ "${out}")
set(n "${CMAKE_MATCH_1}")
check("fixture has channels (${n})" n GREATER 0)

# One plugin: add it (rack or mixer track 1), import into it, check, undo, check.
#   plugin(<type> <add: rack|mixer> <sub> <text the data holds after> [<steps>])
# <steps>: undo steps the import makes (1: Seltmann's analysis of the new file joins the step).
function(plugin type add sub text)
  set(steps 1)
  if(ARGC GREATER 4)
    set(steps "${ARGV4}")
  endif()
  set(undos "26 undo")
  if(steps EQUAL 2)
    set(undos "26 undo\n27 undo")
  endif()
  if(add STREQUAL "rack")
    set(addcmd "addvia rack ${type}")
    set(tgt "sel")
  else()
    set(addcmd "addvia dropmixer ${type}")
    set(tgt "m1.0")
  endif()
  set(ENV{GL_FAKE_SCENARIO} "ok")
  string(REPLACE "." "_" tag "${type}_${sub}")
  run(${tag} "5 ${addcmd}
8 dump
9 urlimport has ${tgt} \"${text}\"
10 urlimport forplugin ${tgt} ${sub}
12 urlimport url ${link}
13 urlimport fetch
14 waitfor urlimport-preview 60
18 urlimport import
19 waitfor urlimport-done 60
24 dump
25 urlimport has ${tgt} \"${text}\"
${undos}
28 urlimport has ${tgt} \"${text}\"
30 quit discard
")
  string(REGEX MATCH "urlimport has [^:]+: ([0-9]+) type ([^ ]+) undo ([0-9]+)" _ "${out}")
  set(before "${CMAKE_MATCH_1}")
  set(ty "${CMAKE_MATCH_2}")
  set(u0 "${CMAKE_MATCH_3}")
  check("${type}: the target is the plugin (${ty})" ty STREQUAL "${type}")
  check("${type}: the data does not hold the file before" before EQUAL 0)
  string(REGEX MATCHALL "urlimport has [^:]+: [0-9]+ type [^ ]+ undo [0-9]+ \"[^\"]*\"" lines "${out}")
  list(LENGTH lines nl)
  check("${type}: three data reports (${nl})" nl EQUAL 3)
  list(GET lines 1 after)
  list(GET lines 2 undone)
  string(REGEX MATCH ": ([0-9]+) type [^ ]+ undo ([0-9]+) \"([^\"]*)\"" _ "${after}")
  set(c1 "${CMAKE_MATCH_1}")
  set(u1 "${CMAKE_MATCH_2}")
  set(l1 "${CMAKE_MATCH_3}")
  string(REGEX MATCH ": ([0-9]+) type [^ ]+ undo ([0-9]+)" _ "${undone}")
  set(c2 "${CMAKE_MATCH_1}")
  set(u2 "${CMAKE_MATCH_2}")
  check("${type}: the data holds the imported file (${c1})" c1 GREATER 0)
  math(EXPR want "${u0} + ${steps}")
  check("${type}: ${steps} undo step(s) (${u0} -> ${u1}, \"${l1}\")" u1 EQUAL want)
  if(steps EQUAL 1)
    check("${type}: the step is named for the import (\"${l1}\")" l1 MATCHES "^Import from URL")
  endif()
  check("${type}: undo takes the file back out (${c2}, undo ${u2})" c2 EQUAL 0 AND u2 EQUAL u0)
  string(REGEX MATCH "dump frame 8: ([0-9]+) clips" _ "${out}")
  set(k0 "${CMAKE_MATCH_1}")
  string(REGEX MATCH "dump frame 24: ([0-9]+) clips" _ "${out}")
  set(k1 "${CMAKE_MATCH_1}")
  check("${type}: no playlist clip added (${k0} -> ${k1})" k1 EQUAL k0)
  check("${type}: the WAV is in the project's Imported folder" EXISTS "${OUT}/Imported/${wav}.wav")
  check("${type}: its source sidecar too" EXISTS "${OUT}/Imported/${wav}.wav.source.json")
  set(out "${out}" PARENT_SCOPE)
endfunction()

# Instruments in the rack (the file as the data, or inside structured data).
plugin(gl.sampler rack 0 "${wav}")
plugin(gl.slicer rack 0 "${wav}")
plugin(gl.granular rack 0 "${wav}")
plugin(gl.synth rack 0 "${wav}")
plugin(gl.pads rack 0 "${wav}")
plugin(gl.seltmann rack 0 "${wav}")
plugin(gl.platter rack 0 "${wav}")
plugin(gl.platter rack 1 "${wav}")
plugin(gl.grainscan rack 0 "${wav}")
plugin(gl.splinescratch rack 0 "${wav}")
plugin(gl.multisampler rack 0 "${wav}")
plugin(gl.scratch rack 0 "${wav}")
plugin(gl.tracker rack 0 "${wav}")
plugin(gl.plasma rack 0 "${wav}")
plugin(gl.plasma rack 1 "${wav}")
plugin(gl.harmor rack 0 "${wav}")
plugin(gl.turntable rack 0 "${wav}")
# Effects on mixer track 1.
plugin(gl.convolver mixer 0 "${wav}")
plugin(gl.turntablefx mixer 0 "${wav}")

# Facet keeps no path (the first cycle is analysed into a table): the step and its undo.
set(ENV{GL_FAKE_SCENARIO} "ok")
run(facet "5 addvia rack gl.additive
9 urlimport has sel none
10 urlimport forplugin sel 0
12 urlimport url ${link}
13 urlimport fetch
14 waitfor urlimport-preview 60
18 urlimport import
19 waitfor urlimport-done 60
25 urlimport has sel none
30 quit discard
")
string(REGEX MATCHALL "urlimport has [^:]+: [0-9]+ type gl.additive undo [0-9]+ \"[^\"]*\"" lines "${out}")
list(LENGTH lines nl)
check("Facet: two data reports (${nl})" nl EQUAL 2)
list(GET lines 0 l0)
list(GET lines 1 l1)
string(REGEX MATCH "undo ([0-9]+)" _ "${l0}")
set(u0 "${CMAKE_MATCH_1}")
string(REGEX MATCH "undo ([0-9]+) \"([^\"]*)\"" _ "${l1}")
math(EXPR want "${u0} + 1")
check("Facet: one undo step named for the import (${l1})" CMAKE_MATCH_1 EQUAL want AND CMAKE_MATCH_2 MATCHES "^Import from URL")

# A Sampler takes the link's title as its name, as a dropped file names it.
set(ENV{GL_FAKE_SCENARIO} "ok")
run(sampler_name "5 addvia rack gl.sampler
10 urlimport forplugin sel 0
12 urlimport url ${link}
13 urlimport fetch
14 waitfor urlimport-preview 60
18 urlimport import
19 waitfor urlimport-done 60
25 urlimport has sel \"${wav}\"
30 quit discard
")
string(REGEX MATCH "urlimport has sel: [0-9]+ type gl.sampler [^\n]* name \"([^\"]*)\"" _ "${out}")
check("Sampler: named after the imported file (\"${CMAKE_MATCH_1}\")" CMAKE_MATCH_1 STREQUAL "${wav}")

# A playlist into a multi-file plugin: every item a sample, all in one undo step.
foreach(type gl.tracker gl.multisampler)
  set(ENV{GL_FAKE_SCENARIO} "playlist")
  run(pl_${type} "5 addvia rack ${type}
9 urlimport has sel \".wav\"
10 urlimport forplugin sel 0
12 urlimport url https://example.com/playlist?list=PL1
13 urlimport fetch
14 waitfor urlimport-preview 60
16 urlimport playlist all
18 urlimport import
19 waitfor urlimport-done 60
25 urlimport has sel \".wav\"
30 quit discard
")
  string(REGEX MATCHALL "urlimport has [^:]+: [0-9]+ type [^ ]+ undo [0-9]+" lines "${out}")
  list(GET lines 0 l0)
  list(GET lines 1 l1)
  string(REGEX MATCH ": ([0-9]+) type [^ ]+ undo ([0-9]+)" _ "${l0}")
  set(c0 "${CMAKE_MATCH_1}")
  set(u0 "${CMAKE_MATCH_2}")
  string(REGEX MATCH ": ([0-9]+) type [^ ]+ undo ([0-9]+)" _ "${l1}")
  set(c1 "${CMAKE_MATCH_1}")
  set(u1 "${CMAKE_MATCH_2}")
  math(EXPR want "${u0} + 1")
  math(EXPR files "${c1} - ${c0}")
  check("${type}: the playlist's three items are all in it (${files} more files)" files GREATER_EQUAL 3)
  check("${type}: one undo step for the playlist (${u0} -> ${u1})" u1 EQUAL want)
endforeach()

# The Add menu's "Audio from URL as sampler...": a new sampler channel playing the file.
set(ENV{GL_FAKE_SCENARIO} "ok")
run(newsampler "9 urlimport has 0 none
10 urlimport newsampler
12 urlimport url ${link}
13 urlimport fetch
14 waitfor urlimport-preview 60
18 urlimport import
19 waitfor urlimport-done 60
25 urlimport has sel \"${wav}\"
30 quit discard
")
string(REGEX MATCH "urlimport has 0: [0-9]+ type [^ ]+ undo ([0-9]+) [^\n]* channels ([0-9]+)" _ "${out}")
set(u0 "${CMAKE_MATCH_1}")
set(ch0 "${CMAKE_MATCH_2}")
string(REGEX MATCH "urlimport has sel: ([0-9]+) type gl.sampler undo ([0-9]+) [^\n]* channels ([0-9]+)" _ "${out}")
check("New sampler: a sampler channel playing the file (${CMAKE_MATCH_1})" CMAKE_MATCH_1 GREATER 0)
math(EXPR want "${ch0} + 1")
check("New sampler: one more channel (${ch0} -> ${CMAKE_MATCH_3})" CMAKE_MATCH_3 EQUAL want)
math(EXPR want "${u0} + 1")
check("New sampler: one undo step (${u0} -> ${CMAKE_MATCH_2})" CMAKE_MATCH_2 EQUAL want)

file(REMOVE_RECURSE "${OUT}/Imported")
message(STATUS "k443 Import from URL in every audio plugin: all checks passed")
