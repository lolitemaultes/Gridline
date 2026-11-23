# Gridline - TRK-1118: four things a user of the 0.7.7 release candidate found in the Tracker, each
# clicked with the real mouse or keys in the real app (hidden, an isolated config folder and HOME,
# SDL's dummy audio driver):
#   1. "Open plugin window" opens the instrument's editor: a double-click on the instrument (its
#      window floats, as in Studio), the Channel frame's button (a dev screen now; before: its first
#      click sent the keys back to the pattern, whose focus took the press from the button, so
#      nothing opened), the instrument list's menu and the View menu's item;
#   2. Duplicate pattern: the pattern box's right-click menu, Ctrl+Alt+U and "into the song" (a new
#      order entry after the current one), each one undo step;
#   3. the instrument picker ("Add instrument..." in the list: a Tracker Sampler or any instrument)
#      and the Mix page's effects (a click on a track's effects: its menu; add, open, bypass, remove);
#   4. MIDI notes enter the pattern as note keys do in edit mode (the velocity in the volume column,
#      the cursor moving on by the edit step, one undo step each) and only play with edit mode off.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P trk1118_tracker_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{HOME} "${OUT}/home")

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n(the run logs: ${OUT}/*.log)")
endfunction()

# One run of the app on a fresh copy of the song and a fresh config.
function(run tag frames script)
  file(MAKE_DIRECTORY "${OUT}/${tag}/cfg/gridline" "${OUT}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/${tag}/cfg")
  file(WRITE "${OUT}/${tag}/cfg/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"tracker\", \"keys.profile\": \"tracker\", \"ui.menuStyle\": \"tracker\"}}\n")
  configure_file("${DEMO}" "${OUT}/${tag}/song.gridline.json" COPYONLY)
  file(WRITE "${OUT}/${tag}/input.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/${tag}/song.gridline.json" --hidden --no-ui-sounds --video-clock 60 --size 1440x900 --layout tracker
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

set(E "tracker editors")

# ---- 1. Open plugin window ----
# Plasma (a built-in synth) in the first slot; a double-click on it floats Plasma's editor (nothing in
# the lower band); the dev Channel frame's "Open plugin window" button (at its right, 1440x900) too.
run(plugwin 120 "5 toasts clear
8 replace gl.plasma
12 ${E} close
20 move 930 41
22 down 0
23 up 0
24 down 0
25 up 0
34 ${E} windows
36 plugwin close
38 ${E} open frame 0
40 move 1297 834
42 down 0
43 up 0
50 ${E} windows
52 plugwin close
56 ${E} windows
60 move 930 57
62 down 1
63 up 1
70 ${E} menu at \"Open plugin window\"
72 sdldown 0
73 sdlup 0
80 ${E} windows
82 ${E} dump
84 ${E} close
90 palette run menu:View/Open plugin window
96 ${E} windows
110 quit discard
")
must("tracker windows frame 34: watched [0-9]+ shown 0 hosted 0 [^\n]* plugin open band closed" "a double-click on the instrument floats its editor, nothing in the band")
must("tracker windows frame 50: watched [0-9]+ shown 0 hosted 0 [^\n]* plugin open band closed" "the Channel frame's Open plugin window opens the editor on its first click")
must("tracker windows frame 56: [^\n]* plugin closed" "closed again")
must("tracker editors frame 70: menu row \"Open plugin window\" at [0-9]+ [0-9]+" "the instrument list's menu has an Open plugin window row (found by its label)")
must("tracker windows frame 80: [^\n]* plugin open band closed" "the instrument list's Open plugin window opens the editor")
must("tracker editors frame 82: [^\n]* channel 1 tracker 0" "... of the instrument right-clicked (Clap)")
must("tracker windows frame 96: [^\n]* plugin open band closed" "View > Open plugin window (run from the command palette) opens the selected instrument's editor")

# ---- 2. Duplicate pattern ----
run(dup 140 "5 toasts clear
10 tracker order dump
20 move 500 79
22 down 1
23 up 1
30 move 578 95
32 down 0
33 up 0
40 tracker order dump
42 ${E} dump
44 tracker dump
50 sdlkey u down ctrl+alt
51 sdlkey u up ctrl+alt
52 sdlkey #224 up
53 sdlkey #226 up
60 tracker order dump
62 ${E} dump
70 undo
72 undo
76 tracker order dump
78 palette dump
80 palette run menu:Pattern/Duplicate into the song
86 tracker order dump
88 ${E} dump
92 undo
96 tracker order dump
130 quit discard
")
if(NOT out MATCHES "order frame 10 \\(dump\\): n ([0-9]+) \\|([^|]*)\\| patterns ([0-9]+)")
  fail("no order report at the start")
endif()
set(n0 ${CMAKE_MATCH_1})
set(order0 "${CMAKE_MATCH_2}")
set(p0 ${CMAKE_MATCH_3})
math(EXPR p1 "${p0} + 1")
math(EXPR p2 "${p0} + 2")
math(EXPR n1 "${n0} + 1")
must("order frame 40 \\(dump\\): n ${n0} \\|[^|]*\\| patterns ${p1}" "the pattern box's menu: Duplicate pattern adds a pattern (no order entry)")
must("tracker editors frame 42: [^\n]* undo \"duplicate pattern\"" "... as one undo step named for it")
must("tracker dump frame 44: pattern 2 " "... and shows the copy (the next number)")
must("order frame 60 \\(dump\\): n ${n0} \\|[^|]*\\| patterns ${p2}" "Ctrl+Alt+U duplicates the pattern shown")
must("tracker editors frame 62: [^\n]* undo \"duplicate pattern\"" "... one undo step")
must("order frame 76 \\(dump\\): n ${n0} \\|${order0}\\| patterns ${p0}" "two undos take both copies away")
must("ROW\tcommand\tcmd:pattern.clone\tpattern.clone\t1\tDuplicate pattern\t[^\n]*\\+U" "the command palette lists Duplicate pattern with its key")
must("ROW\tcommand\tmenu:Pattern/Duplicate into the song\t" "... and Duplicate into the song")
must("order frame 86 \\(dump\\): n ${n1} \\|[^|]*\\| patterns ${p1}" "Duplicate into the song adds the copy and an order entry")
must("tracker editors frame 88: [^\n]* undo \"duplicate pattern into the song\"" "... as one undo step")
must("order frame 96 \\(dump\\): n ${n0} \\|${order0}\\| patterns ${p0}" "one undo takes the copy and its entry away")

# ---- 3. The instrument picker and the Mix page's effects ----
run(add 160 "5 toasts clear
10 tracker tracks
20 move 930 169
22 down 0
23 up 0
28 windows 1
30 move 1006 185
32 down 0
33 up 0
40 tracker tracks
42 ${E} dump
50 tracker pickins gl.plasma
60 tracker tracks
62 ${E} windows
64 ${E} close
70 tracker pickins gl.synth 1
76 ${E} dump
80 tracker page Mix
86 move 1130 389
88 down 0
89 up 0
94 move 1180 405
98 move 1300 405
100 down 0
101 up 0
106 tracker fx dump 2
110 tracker fx add 2 gl.delay
112 tracker fx bypass 2 1
114 tracker fx remove 2 1
120 tracker fx dump 2
150 quit discard
")
must("tracker tracks frame 10: [^\n]* rack 8 " "eight instruments at the start")
must("window \"##Popup_[0-9a-f]+\" pos 9[0-9][0-9]\\.0,1[0-9][0-9]\\.0" "a click on Add instrument... opens the instrument picker (a popup at the slot)")
must("tracker tracks frame 40: [^\n]* rack 9 " "its Tracker Sampler adds an instrument")
must("tracker editors frame 42: [^\n]* channel 8 tracker 1 " "... a Tracker Sampler at the list's end, selected")
must("tracker tracks frame 60: [^\n]* rack 10 " "a picked built-in instrument is added")
must("tracker windows frame 62: [^\n]* plugin open" "... with its editor open")
must("tracker editors frame 76: [^\n]* type gl.synth" "Replace with puts another instrument in an instrument's place")
must("tracker fx frame 106 \\(dump\\): track 2 slots 0:gl.reverb \\| editor 2 0" "the Mix page's effect menu opens an effect's editor")
must("tracker fx frame 110 \\(add\\): track 2 slots 0:gl.reverb 1:gl.delay \\| editor 2 1 \\| undo [0-9]+ \"insert effect\"" "Add effect puts the effect in the first free slot, its editor open")
must("tracker fx frame 112 \\(bypass\\): track 2 slots 0:gl.reverb 1:gl.delay\\(off\\)" "Bypass turns it off")
must("tracker fx frame 114 \\(remove\\): track 2 slots 0:gl.reverb \\|" "Remove takes it out")

# ---- 4. MIDI note entry ----
# A new, empty pattern; the cursor on the Chords track's first row; MIDI notes from the input queue.
run(midi 120 "5 toasts clear
10 tracker order new
14 tracker page Pattern
16 tracker cursor Chords 0 0
18 tracker step 2
20 tracker edit on
24 tracker midi on 60 90
26 tracker midi off 60
30 tracker dump
34 tracker midi on 62 100
35 tracker midi on 65 80
38 tracker midi off 62
39 tracker midi off 65
42 tracker dump
44 ${E} dump
50 tracker edit off
52 tracker midi on 67 90
54 tracker midi off 67
58 tracker dump
100 quit discard
")
must("tracker dump frame 30: [^\n]* edit 1 step 2 [^\n]* cursor 6 2 " "a MIDI note moves the cursor on by the edit step")
must("cell 6 0 0 C-4 07 5A " "... after entering it at the cursor with its velocity (90)")
must("cell 6 2 0 D-4 07 64 " "a second note goes on the next row by the step (velocity 100)")
must("cell 6 4 0 F-4 07 50 " "... and a third after it")
must("tracker editors frame 44: [^\n]* pos 4 undo \"type note\"" "each note is one undo step (after the new pattern)")
must("tracker dump frame 58: [^\n]* edit 0 " "edit mode off")
if(out MATCHES "cell 6 [0-9]+ 0 G-4")
  fail("a MIDI note with edit mode off was entered (it should only play)")
endif()
message(STATUS "ok - with edit mode off a MIDI note only plays")
message(STATUS "trk1118_tracker_app: ok")
