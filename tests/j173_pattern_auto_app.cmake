# Gridline — J-173: automation clips inside a pattern, in the app. Runs the app hidden (off-screen,
# isolated config, fixed frame time: nothing depends on real-time speed) on a copy of the demo song
# whose "Bassline" pattern (id 29, channel 25 "Bass") holds two automation clips: "Bass level"
# (volume, a curve) and an unnamed pan lane (steps, as an older project's pattern lane loads).
#   1. the playlist draws both curves over every clip of the pattern (dev geometry "pcurve"
#      lines: a point on each drawn curve);
#   2. a double-click on a curve opens that clip in the automation editor (the editor's target:
#      the pattern and the control, no playlist clip);
#   3. the channel rack lists the pattern's clips as rows below the channels; a click on a
#      row's name opens that clip in the editor;
#   4. "Automation only" (playlist View menu setting) still draws the curves.
# Screenshots: <OUT>/playlist.ppm (notes and curves), <OUT>/editor.ppm, <OUT>/rack.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j173_pattern_auto_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
set(anchor "\"id\": 29,\n      \"name\": \"Bassline\",")
string(FIND "${demo}" "${anchor}" at)
if(at LESS 0)
  message(FATAL_ERROR "FAIL: the demo song has no Bassline pattern (id 29)")
endif()
set(auto "\"automation\": [
        {\"owner\": 25, \"index\": 0, \"name\": \"Bass level\", \"color\": 15909198, \"flCurves\": true,
         \"points\": [[0, 0.15, 0, 0], [288, 0.9, 0.45, 0], [576, 0.35, -0.3, 0], [768, 0.8, 0, 0]]},
        {\"owner\": 25, \"index\": 1,
         \"points\": [[0, 0.5, 0, 2], [192, 0.2, 0, 2], [384, 0.8, 0, 2], [576, 0.5, 0, 2]]}
      ],")
string(REPLACE "${anchor}" "${anchor}\n      ${auto}" demo "${demo}")
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  if(DEFINED preview)  # the playlist's "Pattern clips show" setting (0 both, 1 notes, 2 automation)
    file(WRITE "${OUT}/cfg_${tag}/gridline/settings.json" "{\"version\": 1, \"values\": {\"playlist.patternPreview\": ${preview}}}\n")
  endif()
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames ${frames}
                          ${ARGN} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
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

# ---- 1. the curves on the pattern's playlist clips
run(playlist 24 "4 selpattern 29\n12 patauto\n20 plgeom\n22 autoedit\n" --hide rack)
set(last_out "${playlist_out}")
check("the pattern holds the two clips (loaded: names kept, an unnamed lane stays unnamed)"
      playlist_out MATCHES "pa 29 25 0 name \"Bass level\" points 4" AND playlist_out MATCHES "pa 29 25 1 name \"\" points 4")
string(REGEX MATCHALL "pcurve [0-9]+ 25 0 x [0-9.]+ y [0-9.]+" vol "${playlist_out}")
string(REGEX MATCHALL "pcurve [0-9]+ 25 1 x [0-9.]+ y [0-9.]+" pan "${playlist_out}")
list(LENGTH vol nvol)
list(LENGTH pan npan)
message(STATUS "curves drawn: ${nvol} volume, ${npan} pan")
check("the playlist draws both curves on the pattern's clips" nvol GREATER 0 AND npan GREATER 0)
check("... on as many clips as each other" nvol EQUAL npan)
check("the editor is closed at first" playlist_out MATCHES "autoedit frame 22: open 0")
list(GET vol 0 v0)
string(REGEX MATCH "pcurve ([0-9]+) 25 0 x ([0-9.]+) y ([0-9.]+)" _ "${v0}")
set(clip "${CMAKE_MATCH_1}")
set(cx "${CMAKE_MATCH_2}")
set(cy "${CMAKE_MATCH_3}")
string(REGEX REPLACE "\\..*" "" cx "${cx}")
string(REGEX REPLACE "\\..*" "" cy "${cy}")
string(REGEX MATCH "clip ${clip} x ([0-9.]+) ([0-9.]+) y ([0-9.]+) ([0-9.]+)" _ "${playlist_out}")
check("the curve's point is inside its clip" CMAKE_MATCH_1 LESS_EQUAL cx AND CMAKE_MATCH_2 GREATER_EQUAL cx)

# ---- 2. a double-click on the volume curve opens it in the editor
run(editor 40 "4 selpattern 29\n14 move ${cx} ${cy}\n18 move ${cx} ${cy}\n20 down 0\n21 up 0\n22 down 0\n23 up 0\n30 autoedit\n" --hide rack)
set(last_out "${editor_out}")
check("a double-click on the curve opens the pattern's volume clip in the editor"
      editor_out MATCHES "autoedit frame 30: open 1 automation 0 pattern 29 target 25 0")
check("... and changes nothing (no undo step)" NOT editor_out MATCHES "undo [1-9]")

# ---- 3. the channel rack's rows
run(rackgeo 14 "4 selpattern 29\n10 rack\n" --rack w1300,h560)
string(REGEX MATCH "geom originX ([0-9.]+) row0Y ([0-9.]+) viewX0 [-0-9.]+ graphY [-0-9.]+ rowPitch ([0-9.]+)" _ "${rackgeo_out}")
set(last_out "${rackgeo_out}")
check("the rack's geometry" CMAKE_MATCH_1 MATCHES "[0-9]")
set(ox "${CMAKE_MATCH_1}")
set(oy "${CMAKE_MATCH_2}")
set(pitch "${CMAKE_MATCH_3}")
string(REGEX REPLACE "\\..*" "" ox "${ox}")
string(REGEX REPLACE "\\..*" "" oy "${oy}")
string(REGEX REPLACE "\\..*" "" pitch "${pitch}")
set(nch 8)  # the demo song's channels (the rack's rows above the clips)
math(EXPR x "${ox} + 120")
math(EXPR y "${oy} + (${nch} + 1) * ${pitch}")  # the second clip row: the pan lane
run(rack 30 "4 selpattern 29\n14 move ${x} ${y}\n18 move ${x} ${y}\n20 down 0\n21 up 0\n26 autoedit\n" --rack w1300,h560)
set(last_out "${rack_out}")
check("a click on the rack's second clip row opens the pan lane in the editor"
      rack_out MATCHES "autoedit frame 26: open 1 automation 0 pattern 29 target 25 1")

# ---- 4. the "Pattern clips show" setting: automation only draws the curves, notes only does not
set(preview 2)
run(autoonly 24 "4 selpattern 29\n20 plgeom\n" --hide rack)
set(last_out "${autoonly_out}")
string(REGEX MATCHALL "pcurve [0-9]+ 25 0 " vol2 "${autoonly_out}")
list(LENGTH vol2 nvol2)
check("\"Automation only\": the curves are drawn" nvol2 EQUAL nvol)
set(preview 1)
run(notesonly 24 "4 selpattern 29\n20 plgeom\n" --hide rack)
set(last_out "${notesonly_out}")
check("\"Notes only\": no curve is drawn" notesonly_out MATCHES "plgeom frame 20" AND NOT notesonly_out MATCHES "pcurve ")
unset(preview)
foreach(s playlist editor rack autoonly)
  check("screenshot ${s}.ppm written" EXISTS "${OUT}/${s}.ppm")
endforeach()
