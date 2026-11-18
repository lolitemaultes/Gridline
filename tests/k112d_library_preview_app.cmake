# Gridline - Smooth's Library: a click previews, a double-click loads (lib-1118).
#   A click on a patch (and Up / Down) leaves the song alone (same revision, no undo step, same sound on
#   the track) and plays one C5 note of it on the preview output; a click that follows quickly replaces
#   the note before (one preview for the latest click only). The speaker off: a click only selects.
#   A double-click (and Enter) replaces the selected track's instrument in one undo step, no new
#   channel, and the list marks it as the track's sound; one undo brings the old sound back. The same
#   holds while the transport plays.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112d_library_preview_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"logic\", \"keys.profile\": \"logic\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)

function(run_app name script frames)
  file(WRITE "${OUT}/${name}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --layout logic --size 1920x1080
                          --dev-input "${OUT}/${name}.txt" --fixed-dt 16 --frames ${frames}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${name}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL: gridline-app failed on ${name} (${rc})")
  endif()
  set(OUTPUT "${out}" PARENT_SCOPE)
endfunction()

# Pulls field `key` (a number or a quoted string) out of a line.
function(field line key var)
  if(line MATCHES " ${key} \"([^\"]*)\"")
    set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  elseif(line MATCHES " ${key} (-?[0-9.]+)")
    set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  else()
    message(FATAL_ERROR "FAIL: no ${key} in: ${line}")
  endif()
endfunction()

function(check_stopped_or_playing name extra)
  run_app(${name} "5 toasts clear
12 parts lib nav 1
14 ${extra}
20 parts lib rows
22 parts lib state
24 parts lib preview
26 parts lib select 0
28 parts lib wait
30 parts lib preview
32 parts lib state
34 parts lib select 1
35 parts lib select 2
37 parts lib wait
39 parts lib preview
41 parts lib move 1
43 parts lib wait
45 parts lib preview
47 parts lib state
49 parts lib enter
53 parts lib state
55 parts lib preview
57 undo
61 parts lib state
63 parts lib previewon 0
65 parts lib select 5
67 parts lib wait
69 parts lib preview
71 parts lib state
73 parts lib previewon 1
75 parts lib open 6
79 parts lib state
81 parts lib preview
83 transport
85 quit discard
" 90)
  set(out "${OUTPUT}")
  string(REGEX MATCHALL "smoothlib state: [^\n]*" st "${out}")
  string(REGEX MATCHALL "smoothlib preview: [^\n]*" pv "${out}")
  string(REGEX MATCHALL "\n  [0-9]+ [^ ]+ \"[^\"]*\" preset -?[0-9]+" rows "${out}")
  list(LENGTH st nst)
  list(LENGTH pv npv)
  if(NOT nst EQUAL 7 OR NOT npv EQUAL 7)
    message(FATAL_ERROR "FAIL (${name}): expected 7 state and 7 preview lines, got ${nst} / ${npv}")
  endif()
  # each row's name, and its key as the Library names it (in|type|preset|name)
  set(names "")
  set(keys "")
  foreach(r IN LISTS rows)
    string(REGEX REPLACE "^\n  [0-9]+ ([^ ]+) \"([^\"]*)\" preset (-?[0-9]+)$" "\\2" n "${r}")
    string(REGEX REPLACE "^\n  [0-9]+ ([^ ]+) \"([^\"]*)\" preset (-?[0-9]+)$" "in|\\1|\\3|\\2" k "${r}")
    list(APPEND names "${n}")
    list(APPEND keys "${k}")
  endforeach()
  list(GET names 3 row3)
  list(GET names 6 row6)
  list(GET st 0 s0)
  list(GET st 1 s1)
  list(GET st 2 s2)
  list(GET st 3 s3)
  list(GET st 4 s4)
  list(GET st 5 s5)
  list(GET st 6 s6)
  list(GET pv 0 p0)
  list(GET pv 1 p1)
  list(GET pv 2 p2)
  list(GET pv 3 p3)
  list(GET pv 4 p4)
  list(GET pv 5 p5)
  list(GET pv 6 p6)
  field("${s0}" sound sound0)
  field("${s0}" channels ch0)
  field("${p0}" revision rev0)
  field("${p0}" undo undo0)
  field("${p0}" played played0)
  # A click: one C5 note heard, nothing in the song changed.
  field("${p1}" played played1)
  field("${p1}" hz hz1)
  field("${p1}" playing playing1)
  field("${p1}" revision rev1)
  field("${p1}" undo undo1)
  field("${p1}" peak peak1)
  field("${p1}" seconds secs1)
  math(EXPR want "${played0} + 1")
  if(NOT played1 EQUAL want OR NOT playing1 EQUAL 1)
    message(FATAL_ERROR "FAIL (${name}): a click did not play one preview: ${p1}")
  endif()
  if(NOT rev1 EQUAL rev0 OR NOT undo1 EQUAL undo0)
    message(FATAL_ERROR "FAIL (${name}): a click changed the song (revision ${rev0} -> ${rev1}, undo ${undo0} -> ${undo1})")
  endif()
  field("${s1}" sound sound1)
  field("${s1}" channels ch1)
  if(NOT sound1 STREQUAL sound0 OR NOT ch1 EQUAL ch0)
    message(FATAL_ERROR "FAIL (${name}): a click loaded the patch (\"${sound0}\" -> \"${sound1}\", channels ${ch0} -> ${ch1})")
  endif()
  if(peak1 LESS 0.01 OR secs1 LESS 0.3 OR secs1 GREATER 2.1)
    message(FATAL_ERROR "FAIL (${name}): the preview note is silent or of the wrong length: ${p1}")
  endif()
  # The note sent is C5 (MIDI 60); what is heard is a C (a patch may sound an octave up or down, as a
  # bass patch does): 261.63 Hz times a power of two, 3% either way.
  if(NOT p1 MATCHES " pitch 60 ")
    message(FATAL_ERROR "FAIL (${name}): the preview note is not C5: ${p1}")
  endif()
  set(isC 0)
  foreach(range "63.4;67.4" "126.9;134.7" "253.8;269.5" "507.5;539.0" "1015;1078")
    list(GET range 0 lo)
    list(GET range 1 hi)
    if(NOT hz1 LESS lo AND NOT hz1 GREATER hi)
      set(isC 1)
    endif()
  endforeach()
  if(NOT isC)
    message(FATAL_ERROR "FAIL (${name}): the preview does not sound a C (${hz1} Hz): ${p1}")
  endif()
  # Two quick clicks: only the latest is heard.
  field("${p2}" played played2)
  field("${p2}" last last2)
  math(EXPR want "${played1} + 1")
  if(NOT played2 EQUAL want)
    message(FATAL_ERROR "FAIL (${name}): two quick clicks did not end in one preview (${played1} -> ${played2}): ${p2}")
  endif()
  list(GET keys 2 key2)
  if(NOT last2 STREQUAL key2)
    message(FATAL_ERROR "FAIL (${name}): the preview heard is not the latest click's (${key2}): ${p2}")
  endif()
  # Down: the next row is selected and heard, nothing loaded.
  field("${p3}" played played3)
  field("${p3}" last last3)
  field("${p3}" revision rev3)
  math(EXPR want "${played2} + 1")
  list(GET keys 3 key3)
  if(NOT played3 EQUAL want OR NOT last3 STREQUAL key3 OR NOT rev3 EQUAL rev0)
    message(FATAL_ERROR "FAIL (${name}): Down did not preview the next row (${row3}) alone: ${p3}")
  endif()
  field("${s2}" sound sound2)
  if(NOT sound2 STREQUAL sound0)
    message(FATAL_ERROR "FAIL (${name}): Down changed the track's sound: ${s2}")
  endif()
  # Enter: the selected row replaces the track's instrument, one undo step, no new channel, marked.
  field("${s3}" sound sound3)
  field("${s3}" channels ch3)
  field("${s3}" marked marked3)
  field("${p4}" undo undo4)
  math(EXPR want "${undo0} + 1")
  if(NOT sound3 STREQUAL row3 OR NOT ch3 EQUAL ch0 OR NOT marked3 STREQUAL row3)
    message(FATAL_ERROR "FAIL (${name}): Enter did not replace the track's sound with ${row3}: ${s3}")
  endif()
  if(NOT undo4 EQUAL want)
    message(FATAL_ERROR "FAIL (${name}): the replace is not one undo step (${undo0} -> ${undo4})")
  endif()
  field("${s4}" sound sound4)
  field("${s4}" channels ch4)
  if(NOT sound4 STREQUAL sound0 OR NOT ch4 EQUAL ch0)
    message(FATAL_ERROR "FAIL (${name}): one undo did not bring \"${sound0}\" back: ${s4}")
  endif()
  # The speaker off: a click selects and is not heard.
  field("${p5}" played played5)
  field("${p5}" on on5)
  field("${p4}" played played4)
  list(GET keys 5 key5)
  field("${s5}" selected sel5)
  if(NOT on5 EQUAL 0 OR NOT played5 EQUAL played4 OR NOT sel5 STREQUAL key5)
    message(FATAL_ERROR "FAIL (${name}): with the preview off a click was heard or did not select (${played4} -> ${played5}): ${s5}")
  endif()
  # A double-click: replaced, marked, one undo step.
  field("${s6}" sound sound6)
  field("${s6}" marked marked6)
  field("${s6}" channels ch6)
  field("${p6}" undo undo6)
  field("${p5}" undo undo5)
  math(EXPR want "${undo5} + 1")
  if(NOT sound6 STREQUAL row6 OR NOT marked6 STREQUAL row6 OR NOT ch6 EQUAL ch0 OR NOT undo6 EQUAL want)
    message(FATAL_ERROR "FAIL (${name}): a double-click did not replace the track's sound with ${row6} in one step: ${s6} (undo ${undo5} -> ${undo6})")
  endif()
  set(OUTPUT "${out}" PARENT_SCOPE)
endfunction()

check_stopped_or_playing(stopped "transport")
check_stopped_or_playing(playing "key space")
if(NOT OUTPUT MATCHES "transport frame [0-9]+: pos [0-9.]+ playing 1")
  message(FATAL_ERROR "FAIL: the transport was not playing in the playing run")
endif()
message(STATUS "k112d_library_preview: ok")
