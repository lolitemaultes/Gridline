# Gridline - HUNT-1119: five things found by playing a new song in the Tracker as a tracker user
# would, each clicked with the real mouse or keys in the real app (hidden, an isolated config folder
# and HOME, SDL's dummy audio driver):
#   1. the Channel frame's Lowest/Highest key named keys as the pattern does (60 = C-4; before: an
#      octave up, C-0 to G-10);
#   2. the sample list numbered its slots in hex as the Sample editor and the pattern's instrument
#      column do (before: 09, 10, 11 next to 0A, 0B);
#   3. the Channel frame's Pitch arrows from a pitch between semitones (+192 ct, set with the strip's
#      knob) go to the next whole semitone the way they point (before: up gave +300, +200 skipped);
#   4. the Audio page (its tab or Ctrl+Alt+8) asked for while the Channel frame shows comes to the
#      front (before: the frame closed, the layout rebuilt and the pattern page came back);
#   5. Ctrl+Alt+Z (the header band hidden and back) draws no frame of a broken band: the strip
#      stretched over the whole band, or the pattern's glyphs a size too big;
#   6. the MIDI box's Channel and Program, which act only while the notes go to the MIDI output,
#      draw faint while they do not (before: as bright as Bend beside them, though clicks did
#      nothing).
#   cmake -DAPP=<gridline-app> -DWAV=<a wav file> -DOUT=<dir> -P hunt1119_tracker_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_PLUGIN_WINDOW_SIZE} "1")  # (as ctest runs the gui tests: the plugin window's place and size fixed)
set(ENV{HOME} "${OUT}/home")

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n(the run logs: ${OUT}/*.log)")
endfunction()

# One run of the app on a new song (no song file: what a fresh start opens) and a fresh config.
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

function(mustnot regex what)
  if(out MATCHES "${regex}")
    fail("${what}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

# A key with Ctrl+Alt held (the modifiers released after, as a hand lets go of them).
function(ctrlalt frame key var)
  math(EXPR f1 "${frame} + 1")
  math(EXPR f2 "${frame} + 2")
  set(${var} "${frame} sdlkey ${key} down ctrl+alt
${f1} sdlkey ${key} up ctrl+alt
${f2} sdlkey #224 up
${f2} sdlkey #226 up
" PARENT_SCOPE)
endfunction()

set(E "tracker editors")

# ---- 1 and 3. The Channel frame's key names; its Pitch arrows from +192 ct ----
# The Kick's Channel frame (the lower band, 1440x900; a dev screen since plugins float in the Tracker:
# Ctrl+Alt+7 floats the Kick's window) and its "Open plugin window" float the instrument's window,
# whose eye shows the channel strip; the strip's Pitch knob dragged up to +192 ct. Back in the frame,
# its Pitch up arrow, then its down arrow.
set(k1 "17 ${E} open frame 0\n")
set(k2 "113 ${E} open frame 0\n")
run(pitch 170 "5 toasts clear
${k1}30 ${E} dump
36 move 1297 834
38 down 0
39 up 0
52 move 1011 172
54 down 0
55 up 0
68 move 535 232
72 down 0
76 move 535 230
79 move 535 228
82 move 535 227
85 move 535 226
88 move 535 224
91 up 0
97 knobs
99 ${E} dump
105 plugwin close
${k2}124 ${E} dump
130 move 786 834
132 down 0
133 up 0
139 ${E} dump
145 move 814 834
147 down 0
148 up 0
154 ${E} dump
160 quit discard
")
must("frame 30: channel [^\n]* keys 0 127 type [^ ]+ names C-- G-9" "the Channel frame names its key range as the pattern does (0 to 127: C-- to G-9, 60 = C-4)")
must("win \"channelsettings\" id \"pitch\" name \"Pitch\" value 192 " "the strip's Pitch knob, dragged up, reads +192 ct")
must("frame 99: channel [^\n]* pitch 192 " "the strip's Pitch knob, dragged up, sets +192 ct")
must("frame 139: channel [^\n]* pitch 200 " "the frame's Pitch up arrow from +192 ct goes to +200 ct (the next semitone up)")
must("frame 154: channel [^\n]* pitch 100 " "its down arrow from +200 ct goes to +100 ct")

# ---- 2. The sample list's numbers ----
# "Add instrument..." in the list, then Tracker Sampler from its picker; a sample loaded into its
# first slot (the file dialog's pick). The list beside the instruments numbers its slots.
configure_file("${WAV}" "${OUT}/bell.wav" COPYONLY)
run(samples 100 "20 toasts clear
25 move 943 121
27 down 0
28 up 0
35 move 1019 137
37 down 0
38 up 0
47 tracker tracks
57 tracker loadsample \"${OUT}/bell.wav\"
70 tracker samplelist
80 quit discard
")
must("tracker tracks frame 47: n 6 [^\n]* Tracker Sampler[(]1[)]" "a Tracker Sampler is added from the instrument picker")
must("tracker samplelist frame 70: 01 [^|]+[|]02 [|]03 [|]04 [|]05 [|]06 [|]07 [|]08 [|]09 [|]0A [|]0B" "the sample list numbers its slots in hex (09, 0A, 0B), as the Sample editor does")

# ---- 4. The Audio page while the Channel frame shows ----
set(k1 "17 ${E} open frame 0\n")
set(k2 "48 ${E} open frame 0\n")
ctrlalt(57 8 k3)
# (the page strip's Audio tab found by its label: a page added to the strip moves it)
run(audio 90 "5 toasts clear
${k1}26 atitem \"Audio\"
28 down 0
29 up 0
42 layout dump
${k2}${k3}70 layout dump
80 quit discard
")
mustnot("atitem frame [0-9]+: [^\n]* not shown" "the page strip has an Audio tab")
string(REGEX MATCHALL "region tracker.pages [^\n]*" pages "${out}")
list(LENGTH pages n)
if(NOT n EQUAL 2)
  fail("two layout dumps of the pages (got ${n})")
endif()
list(GET pages 0 p0)
list(GET pages 1 p1)
if(NOT p0 MATCHES "front Loupe")
  fail("the Audio tab clicked with the Channel frame shown brings the Audio page to the front (${p0})")
endif()
message(STATUS "ok - the Audio tab clicked with the Channel frame shown brings the Audio page to the front")
if(NOT p1 MATCHES "front Loupe")
  fail("Ctrl+Alt+8 with the Channel frame shown brings the Audio page to the front (${p1})")
endif()
message(STATUS "ok - Ctrl+Alt+8 with the Channel frame shown brings the Audio page to the front")

# ---- 5. The header band hidden and back, frame by frame ----
set(s "5 toasts clear\n10 tracker band\n")
ctrlalt(20 z k1)
ctrlalt(40 z k2)
string(APPEND s "${k1}${k2}")
foreach(f RANGE 20 50)
  string(APPEND s "${f} tracker band\n")
endforeach()
string(APPEND s "60 quit discard\n")
run(band 70 "${s}")
must("tracker band frame 30: drawn strip height 9[0-9] " "Ctrl+Alt+Z hides the header band to its strip")
must("tracker band frame 50: drawn header height [1-9][0-9][0-9] " "Ctrl+Alt+Z again brings the whole header back")
mustnot("drawn strip height [1-9][0-9][0-9] " "the strip is never drawn over the whole band's height")
mustnot("drawn header height [0-9][0-9] " "the whole header is never drawn in the strip's height")
string(REGEX MATCH "tracker band frame 10: [^\n]* rowh ([0-9]+)" _ "${out}")
set(rowh0 "${CMAKE_MATCH_1}")
string(REGEX MATCHALL "rowh [0-9]+" rows "${out}")
foreach(r IN LISTS rows)
  string(REPLACE "rowh " "" r "${r}")
  if(r GREATER rowh0)
    fail("the pattern's rows never grow past their height with the whole header (${rowh0}) while the band changes (got ${r})")
  endif()
endforeach()
message(STATUS "ok - the pattern's rows keep their size while the band changes (at most ${rowh0})")

# ---- 6. The MIDI box's Channel and Program with the MIDI output off ----
# The brightest pixel (luminance r + 2g + b) in a box of a PPM shot.
function(brightest ppm x0 y0 x1 y1 var)
  file(READ "${ppm}" head LIMIT 32)
  string(REGEX MATCH "^P6\n([0-9]+) ([0-9]+)\n255\n" h "${head}")
  set(w "${CMAKE_MATCH_1}")
  string(LENGTH "${h}" off)
  set(best 0)
  foreach(y RANGE ${y0} ${y1})
    math(EXPR o "${off} + (${y} * ${w} + ${x0}) * 3")
    math(EXPR bytes "(${x1} - ${x0} + 1) * 3")
    file(READ "${ppm}" hex OFFSET ${o} LIMIT ${bytes} HEX)
    string(REGEX MATCHALL "......" pix "${hex}")
    foreach(p ${pix})
      string(SUBSTRING "${p}" 0 2 r)
      string(SUBSTRING "${p}" 2 2 g)
      string(SUBSTRING "${p}" 4 2 b)
      math(EXPR l "0x${r} + 2 * 0x${g} + 0x${b}")
      if(l GREATER best)
        set(best ${l})
      endif()
    endforeach()
  endforeach()
  set(${var} ${best} PARENT_SCOPE)
endfunction()

# A Tracker Sampler added (the list's "Add instrument..."), then the MIDI box (Ctrl+Alt+J): its
# output is off, so Channel and Program wait for it; Bend under them always acts. Each row's up
# arrow (at 1440x900): Channel's at 180,209, Bend's at 180,230.
ctrlalt(50 j k1)
run(midibox 80 "20 toasts clear
25 move 943 121
27 down 0
28 up 0
35 move 1019 137
37 down 0
38 up 0
${k1}60 shot \"${OUT}/midibox.ppm\"
70 quit discard
")
brightest("${OUT}/midibox.ppm" 175 204 185 214 chan)
brightest("${OUT}/midibox.ppm" 175 225 185 235 bend)
math(EXPR limit "${bend} * 3 / 4")
if(NOT chan LESS limit)
  fail("Channel's arrow (waiting for the MIDI output) draws fainter than Bend's (${chan} against ${bend})")
endif()
message(STATUS "ok - with the MIDI output off, Channel's arrow draws faint (${chan} against Bend's ${bend})")
