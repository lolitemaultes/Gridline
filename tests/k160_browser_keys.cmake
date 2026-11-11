# Gridline: browsing the file browser with the keyboard, in the real app (hidden, isolated config
# and HOME, a folder of samples added to the browser):
#   1. Up / Down move through the rows; moving onto a sample selects and previews it;
#   2. W / S do the same while the typing keyboard is off; while it is on, S plays its note and the
#      browser does not move (the arrows still do);
#   3. folders: moving onto one keeps it folded and the preview alone; Right unfolds, then enters;
#      Left goes to the parent, then folds; Enter toggles a folder;
#   4. Enter on a sample makes a sampler channel, as a double-click does;
#   5. the keys do nothing while another window has focus or a name field is open;
#   6. End / Home / Page Down jump, and the row stays in view;
#   7. a held key previews only the sample it stops on.
# Screenshots: <OUT>/file.ppm, <OUT>/folder.ppm, <OUT>/wide.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k160_browser_keys.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/lib/sub" "${OUT}/hm")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{HOME} "${OUT}/hm")
set(ENV{GRIDLINE_BROWSER_EXPAND} "lib")
get_filename_component(src "${DEMO}" DIRECTORY)
set(wav "${src}/../../assets/sounds/classic/open.wav")
foreach(n A B C sub/D)
  configure_file("${wav}" "${OUT}/lib/${n}.wav" COPYONLY)
endforeach()
foreach(i RANGE 10 49)
  configure_file("${wav}" "${OUT}/lib/Z${i}.wav" COPYONLY)
endforeach()
configure_file("${DEMO}" "${OUT}/fixture.gridline.json" COPYONLY)  # the demo song is never opened in place
set(L "${OUT}/lib")

function(run tag size frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}/gridline")
  file(WRITE "${OUT}/cfg_${tag}/gridline/browser-folders.txt" "${OUT}/lib\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size ${size}
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
# The "browser nav" line of a frame.
function(nav var frame)
  string(REGEX MATCH "browsernav frame ${frame}: [^\n]*" line "${out}")
  if(line STREQUAL "")
    message(FATAL_ERROR "FAIL: no browser nav line at frame ${frame}\nOutput:\n${out}")
  endif()
  set(${var} "${line}" PARENT_SCOPE)
endfunction()
function(moves var frame)
  nav(l ${frame})
  string(REGEX MATCH " moves ([0-9]+) " _ "${l}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
function(cursor var frame)
  nav(l ${frame})
  string(REGEX MATCH " cursor (-?[0-9]+) " _ "${l}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
function(expect_nav frame msg regex)
  nav(l ${frame})
  check("${msg} (${l})" l MATCHES "${regex}")
endfunction()

# ---- where the first sample's row is drawn
run(geo 1440x900 16 "6 browser rows on\n8 browser tab 3\n12 browser rows\n")
string(REGEX MATCH "  row \"lib\" x ([0-9]+)[0-9.]* [0-9.]+ y ([0-9]+)[0-9.]* [0-9.]+ path [^\n]*/lib/A.wav\n" _ "${out}")
set(rx "${CMAKE_MATCH_1}")
set(ry "${CMAKE_MATCH_2}")
check("the sample's row is drawn (x ${rx} y ${ry})" rx MATCHES "^[0-9]+$")
math(EXPR rx "${rx} + 60")
math(EXPR ry "${ry} + 10")

run(keys 1440x900 240 "6 browser rows on
8 browser tab 3
12 move ${rx} ${ry}
14 down 0
15 up 0
18 browser nav
20 key down
23 browser nav
23 shot \"${OUT}/file.ppm\"
25 key down
28 browser nav
30 key up
33 browser nav
35 key w
38 browser nav
40 key s
43 browser nav
45 typing on
47 sdlkey s down
49 typing show
50 browser nav
51 sdlkey s up
53 key down
56 browser nav
58 typing off
60 key up
63 key up
66 key up
69 browser nav
69 shot \"${OUT}/folder.ppm\"
71 key right
74 browser nav
76 key right
79 browser nav
81 key left
84 browser nav
86 key left
89 browser nav
91 key left
94 browser nav
96 key enter
99 browser nav
101 key enter
104 browser nav
106 key right
109 key down
112 browser nav
114 key enter
118 channels
120 focus Playlist
124 browser nav
126 key down
130 browser nav
132 focus Browser
136 key end
140 browser rows
141 browser nav
144 key home
148 browser rows
149 browser nav
152 key pagedown
156 browser nav
158 key end
162 browser nav
165 keyhold up 1
200 browser nav
201 keyhold up 0
206 browser nav
210 browser rename \"${L}/A.wav\"
214 key down
218 browser nav
220 key escape
")

expect_nav(18 "a click focuses the browser and selects the sample, no key has moved yet" "focused 1 moves 0 sel \"[^\"]*/lib/A.wav\" previewed \"\"")
expect_nav(23 "Down: the next sample is selected and previewed" "moves 1 sel \"[^\"]*/lib/B.wav\" previewed \"[^\"]*/lib/B.wav\" previewing 1")
expect_nav(28 "Down again" "moves 2 sel \"[^\"]*/lib/C.wav\" previewed \"[^\"]*/lib/C.wav\"")
expect_nav(33 "Up" "moves 3 sel \"[^\"]*/lib/B.wav\" previewed \"[^\"]*/lib/B.wav\"")
expect_nav(38 "W moves up while the typing keyboard is off" "moves 4 sel \"[^\"]*/lib/A.wav\"")
expect_nav(43 "S moves down while the typing keyboard is off" "moves 5 sel \"[^\"]*/lib/B.wav\"")
string(REGEX MATCH "typing notes [0-9]+" played "${out}")
check("typing keyboard on: S plays its note (${played})" played MATCHES "typing notes [0-9]+")
expect_nav(50 "... and the browser does not move" "moves 5 sel \"[^\"]*/lib/B.wav\"")
expect_nav(56 "the arrows still move with the typing keyboard on" "moves 6 sel \"[^\"]*/lib/C.wav\"")
expect_nav(69 "a folder takes the cursor without unfolding, the selection and preview stay" "folder 1 open 0 focused 1 moves 9 sel \"[^\"]*/lib/A.wav\" previewed \"[^\"]*/lib/A.wav\"")
expect_nav(74 "Right unfolds it" "folder 1 open 1 focused 1 moves 9 ")
expect_nav(79 "Right again enters it and previews the first sample" "moves 10 sel \"[^\"]*/lib/sub/D.wav\" previewed \"[^\"]*/lib/sub/D.wav\"")
expect_nav(84 "Left on a file goes to its folder" "folder 1 open 1 focused 1 moves 11 ")
expect_nav(89 "Left folds it" "folder 1 open 0 focused 1 moves 11 ")
expect_nav(94 "Left on a folded folder goes to its parent" "folder 1 open 1 focused 1 moves 12 ")
expect_nav(99 "Enter folds a folder" "folder 1 open 0 ")
expect_nav(104 "Enter unfolds it" "folder 1 open 1 ")
expect_nav(112 "Right and Down reach a sample again" "sel \"[^\"]*/lib/A.wav\" previewed \"[^\"]*/lib/A.wav\"")

string(REGEX MATCH "channels frame 118[^\n]*\n([^\n]*\n)*" blk "${out}")
check("Enter on a sample makes a sampler channel" blk MATCHES "type gl.sampler data A\\.wav")

moves(m124 124)
moves(m130 130)
expect_nav(124 "another window focused" "focused 0 ")
check("the keys leave the browser alone then (${m124} ${m130})" m124 STREQUAL "${m130}")

# End: the last row is in view (the tree holds far more rows than fit).
string(REGEX MATCH "browserrows frame 140: [0-9]+\n(  row [^\n]*\n)*" blk "${out}")
string(REGEX MATCH "y ([0-9.]+) ([0-9.]+) path [^\n]*/lib/Z49.wav\n" _ "${blk}")
set(y1 "${CMAKE_MATCH_2}")
string(REGEX REPLACE "\\..*" "" y1 "${y1}")
check("End scrolls the last row into view (bottom ${y1})" y1 GREATER 200 AND y1 LESS 900)
expect_nav(141 "End selects the last sample" "sel \"[^\"]*/lib/Z49.wav\" previewed \"[^\"]*/lib/Z49.wav\"")
cursor(c149 149)
check("Home goes to the first row (${c149})" c149 EQUAL 0)
cursor(c156 156)
check("Page Down jumps a page of rows (${c156})" c156 GREATER 8)

expect_nav(162 "End again" "sel \"[^\"]*/lib/Z49.wav\" previewed \"[^\"]*/lib/Z49.wav\"")
moves(m162 162)
moves(m200 200)
math(EXPR held "${m200} - ${m162}")
check("a held key repeats (${held} moves)" held GREATER 3)
expect_nav(200 "... the first press previews, the repeats do not" "previewed \"[^\"]*/lib/Z48.wav\"")
nav(l206 206)
string(REGEX MATCH "sel \"([^\"]*)\" previewed \"([^\"]*)\"" _ "${l206}")
set(s "${CMAKE_MATCH_1}")
set(p "${CMAKE_MATCH_2}")
check("letting go previews the sample it stopped on (${p})" s STREQUAL p AND NOT p MATCHES "Z49")

moves(m206 206)
moves(m218 218)
check("the keys leave the browser alone while a name field is open (${m206} ${m218})" m206 STREQUAL "${m218}")
check("screenshot file.ppm" EXISTS "${OUT}/file.ppm")
check("screenshot folder.ppm" EXISTS "${OUT}/folder.ppm")

# ---- the same at a wider window, for the screenshot
run(geo2 1920x1080 16 "6 browser rows on\n8 browser tab 3\n12 browser rows\n")
string(REGEX MATCH "  row \"lib\" x ([0-9]+)[0-9.]* [0-9.]+ y ([0-9]+)[0-9.]* [0-9.]+ path [^\n]*/lib/A.wav\n" _ "${out}")
set(rx "${CMAKE_MATCH_1}")
set(ry "${CMAKE_MATCH_2}")
math(EXPR rx "${rx} + 60")
math(EXPR ry "${ry} + 10")
run(wide 1920x1080 40 "8 browser tab 3\n12 move ${rx} ${ry}\n14 down 0\n15 up 0\n20 key down\n24 key down\n30 browser nav\n")
expect_nav(30 "1920x1080: the keys move too" "moves 2 ")
check("screenshot wide.ppm" EXISTS "${OUT}/wide.ppm")
message(STATUS "k160 browser keys: all checks passed")
