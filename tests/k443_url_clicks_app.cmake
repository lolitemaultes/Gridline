# Gridline: Import from URL reached by real clicks on the editors' own controls (the pointer on the
# button or menu row, a press and a release), with a stand-in yt-dlp / ffmpeg (fake_media_tool: no
# network). Each: the dialog opens for the right target, the import lands in the plugin's data as
# one undo step, and Undo takes it back out.
#   - Seltmann's empty state "From URL..."
#   - the Tracker layout's Sample editor "From URL..." (an empty entry)
#   - the Tracker layout's entry menu "Import from URL into entry..." (right-click on an entry)
#   - the Multisampler zone menu "Replace from URL..." (replaces the zone, adds none)
#   - the Plasma wavetable editor's "URL"
# Also: a sampler's file dialog names the channel after the file, in one undo step, as a drop and
# Import from URL do.
#   cmake -DAPP=<gridline-app> -DTOOL=<fake_media_tool> -DOUT=<dir> -P k443_url_clicks_app.cmake
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
set(ENV{GRIDLINE_YTDLP} "${TOOL}")
set(ENV{GRIDLINE_FFMPEG} "${TOOL}")
set(ENV{GL_FAKE_SCENARIO} "ok")
unset(ENV{GRIDLINE_URLIMPORT_FAKE_NET})
unset(ENV{GL_PLASMA_VIEW})
set(link "https://example.com/watch?v=k443c")
set(wav "Fake Song Take One")

# run(<tag> <script> <app args...>)
function(run tag script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  set(ENV{USERPROFILE} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 ${ARGN} --dev-input "${OUT}/${tag}.txt"
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

# The import, from the frame after the click: the dialog's target, then link, preview, import.
set(importSteps "40 urlimport state
42 urlimport url ${link}
43 urlimport fetch
44 waitfor urlimport-preview 60
48 urlimport import
49 waitfor urlimport-done 60
")

# Checks a run whose script reported "urlimport has <spec>" at frames 30 (before), 55 (after) and
# 58 (after one undo): the target plugin, the dialog's target, the file in the data, one step.
function(verify name spec type target)  # <target>: a regex for the dialog's target
  string(REGEX MATCH "atitem frame [0-9]+: \"[^\"]*\" not shown" missing "${out}")
  check("${name}: the control was on screen to click (${missing})" NOT missing)
  string(REGEX MATCH "urlimport state [a-z]+ [^\n]* target \"([^\"]*)\"" _ "${out}")
  check("${name}: the click opened the dialog for ${target} (\"${CMAKE_MATCH_1}\")" CMAKE_MATCH_1 MATCHES "${target}")
  string(REGEX MATCHALL "urlimport has ${spec}: [0-9]+ type [^ ]+ undo [0-9]+ \"[^\"]*\"[^\n]* text \"${wav}\"" lines "${out}")
  list(LENGTH lines nl)
  check("${name}: three data reports (${nl})" nl EQUAL 3)
  if(NOT nl EQUAL 3)
    return()
  endif()
  set(i 0)
  foreach(l IN LISTS lines)
    string(REGEX MATCH ": ([0-9]+) type ([^ ]+) undo ([0-9]+) \"([^\"]*)\"" _ "${l}")
    set(c${i} "${CMAKE_MATCH_1}")
    set(t${i} "${CMAKE_MATCH_2}")
    set(u${i} "${CMAKE_MATCH_3}")
    set(l${i} "${CMAKE_MATCH_4}")
    math(EXPR i "${i} + 1")
  endforeach()
  check("${name}: the target is ${type} (${t0})" t0 STREQUAL "${type}")
  math(EXPR want "${u0} + 1")
  check("${name}: one undo step (${u0} -> ${u1}) named for the import (\"${l1}\")" u1 EQUAL want AND l1 MATCHES "^Import from URL")
  set(c0 "${c0}" PARENT_SCOPE)
  set(c1 "${c1}" PARENT_SCOPE)
  set(c2 "${c2}" PARENT_SCOPE)
  check("${name}: undo goes back to before (undo ${u2})" u2 EQUAL u0)
endfunction()

# Seltmann's empty state: "From URL..."
run(seltmann "20 atitem \"From URL...\"
21 down 0
22 up 0
30 urlimport has open \"${wav}\"
${importSteps}55 urlimport has open \"${wav}\"
56 undo
58 urlimport has open \"${wav}\"
62 quit discard
" --plugin gl.seltmann)
verify("Seltmann button" open gl.seltmann "^Seltmann$")
check("Seltmann button: the file is in, then out (${c0} -> ${c1} -> ${c2})" c0 EQUAL 0 AND c1 GREATER 0 AND c2 EQUAL 0)

# The Tracker layout's Sample editor, an empty entry: "From URL..."
run(tracker_well "10 tracker editors open sample
20 atitem \"From URL...\"
21 down 0
22 up 0
30 urlimport has sel \"${wav}\"
${importSteps}55 urlimport has sel \"${wav}\"
56 undo
58 urlimport has sel \"${wav}\"
62 quit discard
" --plugin gl.tracker --layout tracker)
verify("Tracker Sample editor button" sel gl.tracker "^Sample editor$")
check("Tracker Sample editor button: the file is in, then out (${c0} -> ${c1} -> ${c2})" c0 EQUAL 0 AND c1 GREATER 0 AND c2 EQUAL 0)

# The Tracker layout's entry menu: right-click entry 03, "Import from URL into entry..."
run(tracker_entry "18 atitem \"Entry 03\"
19 down 1
20 up 1
24 atitem \"Import from URL into entry...\"
25 down 0
26 up 0
30 urlimport has sel \"${wav}\"
${importSteps}55 urlimport has sel \"${wav}\"
56 undo
58 urlimport has sel \"${wav}\"
62 quit discard
" --plugin gl.tracker --layout tracker)
verify("Tracker entry menu" sel gl.tracker "^Entry 03$")
check("Tracker entry menu: the file is in, then out (${c0} -> ${c1} -> ${c2})" c0 EQUAL 0 AND c1 GREATER 0 AND c2 EQUAL 0)

# The Multisampler: a zone first (a sample file), then its menu's "Replace from URL...": still one
# zone, now the imported file.
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/../assets/sounds/classic/save.wav" "${OUT}/First Zone.wav")
run(multisampler "4 atitem \"ADD\"
5 down 0
6 up 0
7 dialogok \"${OUT}/First Zone.wav\"
10 urlimport has open \"First Zone\"
12 atitem \"row\"
13 down 1
14 up 1
18 atitem \"Replace from URL...\"
19 down 0
20 up 0
30 urlimport has open \"${wav}\"
${importSteps}54 urlimport has open \"First Zone\"
55 urlimport has open \"${wav}\"
56 undo
58 urlimport has open \"${wav}\"
60 urlimport has open \"First Zone\"
62 quit discard
" --plugin gl.multisampler)
string(REGEX MATCHALL "urlimport has open: [0-9]+ type gl.multisampler[^\n]* text \"First Zone\"" zl "${out}")
list(LENGTH zl nz)
check("Multisampler: three zone reports (${nz})" nz EQUAL 3)
if(nz EQUAL 3)
  list(GET zl 0 z0)
  list(GET zl 1 z1)
  list(GET zl 2 z2)
  string(REGEX MATCH ": ([0-9]+) type" _ "${z0}")
  check("Multisampler: a first zone from ADD (${CMAKE_MATCH_1})" CMAKE_MATCH_1 EQUAL 1)
  string(REGEX MATCH ": ([0-9]+) type" _ "${z1}")
  check("Multisampler: the replace puts the import in that zone's place (${CMAKE_MATCH_1} left)" CMAKE_MATCH_1 EQUAL 0)
  string(REGEX MATCH ": ([0-9]+) type" _ "${z2}")
  check("Multisampler: undo brings the zone's file back (${CMAKE_MATCH_1})" CMAKE_MATCH_1 EQUAL 1)
endif()
verify("Multisampler zone menu" open gl.multisampler "^Multisampler \\(Zone [0-9]+\\)$")
check("Multisampler zone menu: the zone holds the file, then not (${c0} -> ${c1} -> ${c2})" c0 EQUAL 0 AND c1 GREATER 0 AND c2 EQUAL 0)

# The Plasma wavetable editor: "URL"
set(ENV{GL_PLASMA_VIEW} "open:layer1")
run(plasma "20 atitem \"URL\"
21 down 0
22 up 0
30 urlimport has open \"${wav}\"
${importSteps}55 urlimport has open \"${wav}\"
56 undo
58 urlimport has open \"${wav}\"
62 quit discard
" --plugin gl.plasma)
unset(ENV{GL_PLASMA_VIEW})
verify("Plasma wavetable editor" open gl.plasma "^Plasma \\(Wavetable\\)$")
check("Plasma wavetable editor: the table is the file, then not (${c0} -> ${c1} -> ${c2})" c0 EQUAL 0 AND c1 GREATER 0 AND c2 EQUAL 0)

# A sampler's file dialog (the Audio clip window's "Replace sample...", where a sampler's settings
# open): the sampler takes the file and the file's name, in one step, as a drop and Import from URL do.
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/../assets/sounds/classic/save.wav" "${OUT}/Bright Loop.wav")
run(loadsample "10 urlimport has open \"Bright Loop\"
20 atitem \"Replace sample...\"
21 down 0
22 up 0
24 dialogok \"${OUT}/Bright Loop.wav\"
30 urlimport has open \"Bright Loop\"
32 undo
34 urlimport has open \"Bright Loop\"
40 quit discard
" --plugin gl.sampler)
string(REGEX MATCH "atitem frame [0-9]+: \"[^\"]*\" not shown" missing "${out}")
check("Replace sample: the button was on screen (${missing})" NOT missing)
string(REGEX MATCHALL "urlimport has open: [0-9]+ type [^ ]+ undo [0-9]+ \"[^\"]*\" name \"[^\"]*\"" lines "${out}")
list(LENGTH lines nl)
check("Replace sample: three reports (${nl})" nl EQUAL 3)
list(GET lines 0 r0)
list(GET lines 1 r1)
list(GET lines 2 r2)
string(REGEX MATCH ": ([0-9]+) type [^ ]+ undo ([0-9]+) \"[^\"]*\" name \"([^\"]*)\"" _ "${r0}")
set(c0 "${CMAKE_MATCH_1}")
set(u0 "${CMAKE_MATCH_2}")
set(n0 "${CMAKE_MATCH_3}")
string(REGEX MATCH ": ([0-9]+) type [^ ]+ undo ([0-9]+) \"([^\"]*)\" name \"([^\"]*)\"" _ "${r1}")
set(c1 "${CMAKE_MATCH_1}")
set(u1 "${CMAKE_MATCH_2}")
set(l1 "${CMAKE_MATCH_3}")
set(n1 "${CMAKE_MATCH_4}")
string(REGEX MATCH ": ([0-9]+) type [^ ]+ undo ([0-9]+) \"[^\"]*\" name \"([^\"]*)\"" _ "${r2}")
set(c2 "${CMAKE_MATCH_1}")
set(u2 "${CMAKE_MATCH_2}")
set(n2 "${CMAKE_MATCH_3}")
check("Replace sample: the sampler plays the file (${c0} -> ${c1})" c0 EQUAL 0 AND c1 GREATER 0)
check("Replace sample: the channel is named after the file (\"${n0}\" -> \"${n1}\")" n1 STREQUAL "Bright Loop")
math(EXPR want "${u0} + 1")
check("Replace sample: one undo step (${u0} -> ${u1}, \"${l1}\")" u1 EQUAL want)
check("Replace sample: undo brings back the sample and the name (${c2}, \"${n2}\")" c2 EQUAL 0 AND n2 STREQUAL n0 AND u2 EQUAL u0)

message(STATUS "k443 Import from URL by clicks, and Load sample naming: all checks passed")
