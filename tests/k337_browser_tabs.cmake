# Gridline: browser tabs of your own (hidden, isolated config and HOME, a fixed 16 ms frame time).
#   1. Studio: a folder added as a tab shows last and is shown when clicked; a middle-click closes
#      the Project tab; the folder tab is renamed from its menu; the + menu brings Project back in
#      its usual place; All closes from its menu. The list is saved in browser-tabs.txt.
#   2. A second start reads the same tabs back.
#   3. Clips and Smooth list the folder tab as a place (clicked: its folder shows); Tracker shows
#      the tab strip like Studio.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k337_browser_tabs.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/hm/Loops/Breaks")
file(WRITE "${OUT}/hm/Loops/notes.txt" "a file in the folder\n")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/hm")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(dir "${OUT}/hm/Loops")

function(settings layout)
  file(WRITE "${OUT}/cfg/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"${layout}\"}}\n")
endfunction()

function(run name script)
  file(WRITE "${OUT}/${name}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --dev-input "${OUT}/${name}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 240)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${o}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

function(expect re what)
  if(NOT out MATCHES "${re}")
    message(FATAL_ERROR "FAIL: ${what}\n(looked for: ${re})\n${out}")
  endif()
  message(STATUS "ok: ${what}")
endfunction()

# The tabs printed at a frame, one line: "kind:label kind:label ..." (labels of folder tabs only).
function(tabs_at frame var)
  string(REGEX MATCH "browsertabs frame ${frame}: [^\n]*\n(  tab [^\n]*\n)*" block "${out}")
  string(REGEX MATCHALL "  tab [0-9]+ [a-z]+ \"[^\"]*\"" lines "${block}")
  set(r "")
  foreach(l ${lines})
    string(REGEX REPLACE "  tab [0-9]+ ([a-z]+) \"([^\"]*)\"" "\\1:\\2" l "${l}")
    string(APPEND r "${l} ")
  endforeach()
  string(REGEX MATCH "shown (-?[0-9]+)" s "${block}")
  set(${var} "${r}| ${s}" PARENT_SCOPE)
endfunction()

function(expect_tabs frame want what)
  tabs_at(${frame} got)
  if(NOT got STREQUAL "${want}")
    message(FATAL_ERROR "FAIL: ${what}\n  want: ${want}\n  got:  ${got}\n${out}")
  endif()
  message(STATUS "ok: ${what}")
endfunction()

# 1. Studio.
settings(studio)
run(studio "6 browser tabs
8 browser addtab ${dir}
10 browser tabs
11 sdlat brtab0
12 sdldown 0
13 sdlup 0
15 sdlat brtab5
16 browser tabs
17 sdldown 0
18 sdlup 0
20 browser tabs
22 shot \"${OUT}/studio-folder.ppm\"
24 sdlat brtab1
26 sdldown 2
27 sdlup 2
32 browser tabs
34 sdlat brtab4
36 sdldown 1
37 sdlup 1
42 sdlat brtabmrename
44 sdldown 0
45 sdlup 0
52 sdlat brtabname
54 key enter
58 browser tabs
60 sdlat braddtab
62 sdldown 0
63 sdlup 0
68 shot \"${OUT}/studio-addmenu.ppm\"
70 sdlat brreopenproject
72 sdldown 0
73 sdlup 0
78 browser tabs
80 sdlat brtab0
82 sdldown 1
83 sdlup 1
88 sdlat brtabmclose
90 sdldown 0
91 sdlup 0
96 browser tabs
98 shot \"${OUT}/studio-final.ppm\"
100 quit discard
")
expect_tabs(6 "all: project: plugins: sounds: starred: | shown 0" "five built-in tabs at first")
expect_tabs(10 "all: project: plugins: sounds: starred: folder:Loops | shown 5" "a folder added as a tab goes last, named after the folder, and shows")
expect_tabs(16 "all: project: plugins: sounds: starred: folder:Loops | shown 0" "clicking All shows All")
expect_tabs(20 "all: project: plugins: sounds: starred: folder:Loops | shown 5" "clicking the folder tab shows it")
expect_tabs(32 "all: plugins: sounds: starred: folder:Loops | shown 4" "a middle-click closes the Project tab; the folder tab stays shown")
expect("sdlat frame 42: brtabmrename at " "a folder tab's menu offers Rename")
expect("sdlat frame 52: brtabname at " "Rename opens the name field")
expect_tabs(58 "all: plugins: sounds: starred: folder:Loops | shown 4" "Enter keeps the name as it was")
expect("sdlat frame 70: brreopenproject at " "the + menu lists the closed Project tab")
expect_tabs(78 "all: project: plugins: sounds: starred: folder:Loops | shown 1" "Project comes back in its usual place, and shows")
expect("sdlat frame 88: brtabmclose at " "a tab's menu offers Close")
expect_tabs(96 "project: plugins: sounds: starred: folder:Loops | shown 0" "All closes from its menu")
file(READ "${OUT}/cfg/gridline/browser-tabs.txt" saved)
if(NOT saved MATCHES "^gridline-browser-tabs 1\nproject\nplugins\nsounds\nstarred\nfolder\t[^\n]*/hm/Loops\t\n$")
  message(FATAL_ERROR "FAIL: the saved tabs\n${saved}")
endif()
message(STATUS "ok: the tabs are saved per user")

# 2. A second start reads them back.
run(again "6 browser tabs
8 quit discard
")
expect_tabs(6 "project: plugins: sounds: starred: folder:Loops | shown 0" "a second start shows the same tabs")

# 3. The other layouts.
foreach(lay live logic)
  settings(${lay})
  run(${lay} "4 layout open Browser
6 browser tabs
10 sdlat brplace0
12 sdldown 0
13 sdlup 0
18 browser tabs
20 shot \"${OUT}/${lay}-place.ppm\"
22 quit discard
")
  expect("sdlat frame 10: brplace0 at " "${lay}: the folder tab is listed as a place")
  expect("browsertabs frame 18: count 5 shown -?[0-9]+ view [0-9]+ cat 14" "${lay}: clicking the place shows its folder")  # (the first place: past the 14 categories)
endforeach()
settings(tracker)
run(tracker "4 layout open Browser
6 browser tabs
10 sdlat brtab4
12 sdldown 0
13 sdlup 0
18 browser tabs
20 shot \"${OUT}/tracker-tabs.ppm\"
22 quit discard
")
expect("sdlat frame 10: brtab4 at " "tracker: the folder tab is in the strip")
expect_tabs(18 "project: plugins: sounds: starred: folder:Loops | shown 4" "tracker: clicking it shows the folder")
