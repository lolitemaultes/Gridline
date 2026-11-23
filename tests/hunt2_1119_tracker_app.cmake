# Gridline - HUNT2-1119: three things a tracker user asked of the Tracker, each pressed with the
# real keys and mouse in the real app on a new song (hidden, an isolated config folder and HOME,
# SDL's dummy audio driver):
#   1. a pattern made with Ctrl+Alt+N takes the keys: the next note typed goes in it, with the
#      instrument's editor open or another page in front (before: the editor or the page kept the keys
#      and the note went nowhere until a click). K-391 (user 2026-11-22): in the Tracker a plugin's
#      editor floats in-app and never opens in the lower band, so Ctrl+Alt+7 on the Kick opens its
#      floating window (the Channel frame is only a dev screen now);
#   2. it is as long as the current pattern (a new song's 16 rows, or 32 after Expand), not always
#      64 rows;
#   3. Esc closes the screen swapped into the header (About, Settings, Help) as its Exit button
#      does, and toggles edit mode only when none is shown;
#   4. an instrument added from the list's "Add instrument..." gets its own free insert, named
#      after it, as the new song's five instruments have (before: it played through the master,
#      and the Mix page's next row was an unused insert), and one undo takes the whole add back.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P hunt2_1119_tracker_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE} "1")
set(ENV{HOME} "${OUT}/home")

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n(the run logs: ${OUT}/*.log)")
endfunction()

function(run tag frames script)
  file(MAKE_DIRECTORY "${OUT}/${tag}/cfg/gridline" "${OUT}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/${tag}/cfg")
  file(WRITE "${OUT}/${tag}/cfg/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"tracker\", \"keys.profile\": \"tracker\", \"ui.menuStyle\": \"tracker\"}}\n")
  file(WRITE "${OUT}/${tag}/input.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --video-clock 60 --size 1440x900
                          --dev-input "${OUT}/${tag}/input.txt" --fixed-dt 16 --frames ${frames}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${o}\n---- stderr ----\n${e}")
  if(NOT rc EQUAL 0)
    fail("gridline-app failed in run ${tag} (${rc})")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

function(must regex what)
  if(NOT out MATCHES "${regex}")
    fail("${what}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

# A key pressed (with Ctrl+Alt when mods is "ctrl+alt"; the modifiers let go after, as hands do).
function(key frame k mods var)
  math(EXPR f1 "${frame} + 1")
  math(EXPR f2 "${frame} + 2")
  if(mods STREQUAL "")
    set(${var} "${frame} sdlkey ${k} down\n${f1} sdlkey ${k} up\n" PARENT_SCOPE)
  else()
    set(${var} "${frame} sdlkey ${k} down ${mods}\n${f1} sdlkey ${k} up ${mods}\n${f2} sdlkey #224 up\n${f2} sdlkey #226 up\n" PARENT_SCOPE)
  endif()
endfunction()

set(D "tracker dump")
# Edit mode on (Esc); the Kick's editor (Ctrl+Alt+7), floating; Ctrl+Alt+N; Z.
# The Song page (Ctrl+Alt+2) in front; Ctrl+Alt+N; Z. Expand (the header's button) makes the pattern
# 32 rows; Ctrl+Alt+N.
key(10 escape "" k0)
key(17 7 ctrl+alt k1)
key(26 n ctrl+alt k2)
key(36 z "" k3)
key(46 2 ctrl+alt k4)
key(54 n ctrl+alt k5)
key(66 z "" k6)
key(96 n ctrl+alt k7)
run(newpat 120 "5 toasts clear
${k0}${k1}24 tracker editors dump
${k2}32 ${D}
${k3}40 ${D}
${k4}52 layout dump
${k5}62 ${D}
${k6}70 ${D}
72 layout dump
80 move 508 120
82 down 0
83 up 0
90 ${D}
${k7}104 ${D}
110 quit discard
")
must("tracker editors frame 24: shown 0 [^\n]* pluginwin 1 " "Ctrl+Alt+7 opens the Kick's editor in a floating window, not in the lower band (K-391)")
must("tracker dump frame 32: pattern 2 rows 16 [^\n]* keyboard 1 " "Ctrl+Alt+N from the Kick's floating editor: pattern 02, 16 rows as the first, and the pattern has the keys")
must("tracker dump frame 40: [^\n]*\ntracker status: [^\n]*\n  cell 0 0 0 C-4" "Z then writes C-4 in the new pattern")
must("region tracker.pages [^\n]* front Playlist" "Ctrl+Alt+2 brings the Song page to the front")
must("tracker dump frame 62: pattern 3 rows 16 [^\n]* keyboard 1 " "Ctrl+Alt+N from the Song page: pattern 03 shown with the keys")
must("tracker dump frame 70: [^\n]*\ntracker status: [^\n]*\n  cell 0 1 0 C-4" "Z then writes C-4 in it (at the cursor's row, which a pattern change keeps)")
must("tracker dump frame 90: pattern 3 rows 32 " "Expand makes the pattern 32 rows")
must("tracker dump frame 104: pattern 4 rows 32 [^\n]* keyboard 1 " "Ctrl+Alt+N after it: a 32-row pattern, as the current one")

# ---- 3. Esc and the header's screens ----
key(30 escape "" e1)
key(44 0 ctrl+alt s2)
key(52 escape "" e2)
key(66 h ctrl+alt s3)
key(74 escape "" e3)
key(86 escape "" e4)
run(esc 110 "5 toasts clear
${k0}16 ${D}
20 move 702 39
22 down 0
23 up 0
28 tracker swapstate
${e1}36 tracker swapstate
38 ${D}
${s2}50 tracker swapstate
${e2}58 tracker swapstate
60 ${D}
${s3}72 tracker swapstate
${e3}80 tracker swapstate
82 ${D}
${e4}92 ${D}
100 quit discard
")
must("tracker dump frame 16: [^\n]* edit 1 " "Esc turns edit mode on (no screen in the header)")
must("tracker swapstate frame 28: 4 " "About Gridline shows About in the header")
must("tracker swapstate frame 36: 0 " "Esc closes About")
must("tracker dump frame 38: [^\n]* edit 1 [^\n]* keyboard 1 " "... and leaves edit mode as it was, the keys with the pattern")
must("tracker swapstate frame 50: 3 " "Ctrl+Alt+0 shows Settings")
must("tracker swapstate frame 58: 0 " "Esc closes Settings")
must("tracker swapstate frame 72: 1 " "Ctrl+Alt+H shows Help")
must("tracker swapstate frame 80: 0 " "Esc closes Help")
must("tracker dump frame 82: [^\n]* edit 1 " "edit mode is still on after the screens closed")
must("tracker dump frame 92: [^\n]* edit 0 " "with no screen shown, Esc turns edit mode off again")

# ---- 4. An added instrument's insert ----
run(route 70 "5 toasts clear
20 move 943 121
22 down 0
23 up 0
30 move 1019 137
32 down 0
33 up 0
44 mixer
48 undo
54 mixer
60 quit discard
")
string(REGEX MATCH "mixer frame 44:.*mixer frame 54:" first "${out}")
if(NOT first MATCHES "mtrack 6 id ([0-9]+) name \\[Tracker Sampler\\]")
  fail("Add instrument... then Tracker Sampler: insert 06 takes its name")
endif()
set(ins "${CMAKE_MATCH_1}")
if(NOT first MATCHES "mchannel \\[Tracker Sampler\\] mixer ${ins}\n")
  fail("the Tracker Sampler plays through insert 06 (id ${ins})")
endif()
message(STATUS "ok - an added Tracker Sampler plays through its own insert 06, named after it")
string(REGEX MATCH "mixer frame 54:.*" second "${out}")
if(second MATCHES "Tracker Sampler" OR NOT second MATCHES "mtrack 6 id [0-9]+ name \\[\\]")
  fail("one undo takes the instrument and its insert's name back")
endif()
message(STATUS "ok - one undo takes the instrument and its insert's name back")
