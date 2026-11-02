# Gridline: K-18: rename and organise files in the browser, in the real app (hidden, isolated
# config and HOME, a copy of the demo song whose Kick channel plays <OUT>/lib/Snap.wav, the folder
# <OUT>/lib added to the browser):
#   1. right-click a file row: the context menu with Rename / New folder / Move to the trash;
#   2. Rename through the inline name field (typed, Enter): the file is renamed on disk and the
#      channel follows, one undo step; Undo puts the file and the reference back, Redo again;
#   3. a name that exists is refused with a message (the field stays), Esc cancels;
#   4. a change of letter case only works (case-insensitive file systems too);
#   5. New folder (its name field), a move into it (a drop on the folder): the reference follows;
#   6. read-only places are refused with the reason (outside the user's / Gridline's folders, a
#      browser root itself);
#   7. Move to the trash (asked first): the file leaves the folder (into the trash under HOME);
#   8. Undo of the move puts the file back where it was.
# Screenshots: <OUT>/menu.ppm, <OUT>/rename.ppm, <OUT>/collision.ppm, <OUT>/trash.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k18_browser_files.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/lib/sub" "${OUT}/elsewhere" "${OUT}/hm" "${OUT}/xdg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{HOME} "${OUT}/hm")  # the trash lands under it (macOS ~/.Trash) or in XDG_DATA_HOME (freedesktop)
set(ENV{XDG_DATA_HOME} "${OUT}/xdg")
if(WIN32)
  set(ENV{GRIDLINE_TRASH_DIR} "${OUT}/trash")  # never the real Recycle Bin from a test
endif()
set(ENV{GRIDLINE_BROWSER_EXPAND} "lib")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/open.wav" "${OUT}/lib/Snap.wav" COPYONLY)
configure_file("${src}/../../assets/sounds/classic/save.wav" "${OUT}/lib/Tick.wav" COPYONLY)
configure_file("${src}/../../assets/sounds/classic/save.wav" "${OUT}/elsewhere/Other.wav" COPYONLY)
file(WRITE "${OUT}/lib/Snap.wav.origin.json" "{\"source\": \"x.wav\"}\n")  # a sidecar: it moves with the file
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
# Kick (channel 20) becomes a sampler playing lib/Snap.wav.
string(REGEX REPLACE "\"plugin\": {[ \n]*\"params\": \\[[ \n]*220,[^]]*\\],[ \n]*\"type\": \"gl.kick\"" "\"plugin\": {\"data\": \"${OUT}/lib/Snap.wav\", \"type\": \"gl.sampler\"" song "${demo}")
if(song STREQUAL demo)
  message(FATAL_ERROR "FAIL: the fixture's Kick channel was not found")
endif()
file(WRITE "${OUT}/fixture.gridline.json" "${song}")

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}/gridline")
  file(WRITE "${OUT}/cfg_${tag}/gridline/browser-folders.txt" "${OUT}/lib\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900
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
# The file reference of channel 20 in the "browser refs" block of frame <frame> -> <var>.
function(kickref var frame)
  string(REGEX MATCH "browserrefs frame ${frame}:\n(  ref [^\n]*\n)*" blk "${out}")
  string(REGEX MATCH "  ref ([^\n]*/lib/[^\n]*\\.wav)\n" _ "${blk}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
function(state var frame)
  string(REGEX MATCH "browserstate frame ${frame}: [^\n]*" line "${out}")
  set(${var} "${line}" PARENT_SCOPE)
endfunction()

# ---- 1. the rows and the context menu
run(geo 16 "6 browser rows on\n8 browser tab 3\n12 browser rows\n")
string(REGEX MATCH "  row \"lib\" x ([0-9.]+) ([0-9.]+) y ([0-9.]+) ([0-9.]+) path [^\n]*/lib/Snap.wav\n" _ "${out}")
set(m1 "${CMAKE_MATCH_1}")
set(m3 "${CMAKE_MATCH_3}")
check("the browser lists lib/Snap.wav under the user folder (x ${m1} y ${m3})" m1 MATCHES "^[0-9]")
string(REGEX REPLACE "\\..*" "" ry "${m3}")
string(REGEX REPLACE "\\..*" "" rx "${m1}")
math(EXPR rx "${rx} + 60")
math(EXPR ry "${ry} + 10")
run(menu 30 "8 browser tab 3\n12 move ${rx} ${ry}\n14 down 1\n15 up 1\n24 shot \"${OUT}/menu.ppm\"\n")
check("screenshot menu.ppm" EXISTS "${OUT}/menu.ppm")

# ---- 2.-8. the operations
set(L "${OUT}/lib")
run(ops 100 "8 browser tab 3
10 browser rename \"${L}/Snap.wav\"
13 text Kick One
15 shot \"${OUT}/rename.ppm\"
16 key enter
20 browser refs
21 browser state
22 undo
24 browser refs
26 redo
28 browser refs
30 browser rename \"${L}/Tick.wav\"
33 text Kick One
34 key enter
36 browser state
37 shot \"${OUT}/collision.ppm\"
38 key escape
40 browser state
42 browser rename \"${L}/Kick One.wav\"
45 text kick one
46 key enter
48 browser refs
50 browser newfolder \"${L}\"
53 text Drums
54 key enter
58 browser move \"${L}/kick one.wav\" \"${L}/Drums\"
60 browser refs
61 browser state
62 browser state \"${OUT}/elsewhere/Other.wav\"
63 browser state \"${L}\"
64 browser state \"${L}/Drums\"
70 browser trash \"${L}/Tick.wav\"
74 shot \"${OUT}/trash.ppm\"
76 browser trashok
80 undo
82 browser refs
83 browser state
")

kickref(r20 20)
kickref(r24 24)
kickref(r28 28)
state(s21 21)
check("rename: the channel follows (${r20})" r20 STREQUAL "${L}/Kick One.wav")
check("... one undo step labelled Renamed (${s21})" s21 MATCHES "undo 1 \"Renamed\"")
check("undo: the reference goes back (${r24})" r24 STREQUAL "${L}/Snap.wav")
check("redo: renamed again (${r28})" r28 STREQUAL "${L}/Kick One.wav")
state(s36 36)
check("a name that exists is refused, the field stays (${s36})" s36 MATCHES "renaming \"[^\"]*Tick.wav\" error \"[^\"]*already exists")
state(s40 40)
check("Esc cancels the field (${s40})" s40 MATCHES "renaming \"\" error \"\" ")
kickref(r48 48)
check("a change of letter case only (${r48})" r48 STREQUAL "${L}/kick one.wav")
kickref(r60 60)
check("New folder + a move into it: the reference follows (${r60})" r60 STREQUAL "${L}/Drums/kick one.wav")
state(s61 61)
check("... one undo step labelled Moved (${s61})" s61 MATCHES "undo [0-9]+ \"Moved\"")
state(s62 62)
check("outside the user's and Gridline's folders: refused with the reason (${s62})" s62 MATCHES "locked \"Read-only here")
state(s63 63)
check("a browser root itself cannot be renamed (${s63})" s63 MATCHES "locked \"This folder is one of the browser's own folders\"")
state(s64 64)
check("a folder inside it can (${s64})" s64 MATCHES "locked \"\" ")
kickref(r82 82)
check("undo of the move: the reference is back in lib (${r82})" r82 STREQUAL "${L}/kick one.wav")
check("... and the file too" EXISTS "${L}/kick one.wav")
check("... not left in Drums" NOT EXISTS "${L}/Drums/kick one.wav")
check("the sidecar moved with the file" EXISTS "${L}/kick one.wav.origin.json")
check("the old name is gone" NOT EXISTS "${L}/Snap.wav")
check("the new folder is there" IS_DIRECTORY "${L}/Drums")
check("trash: Tick.wav left the folder" NOT EXISTS "${L}/Tick.wav")
if(APPLE)
  check("... into the trash under HOME" EXISTS "${OUT}/hm/.Trash/Tick.wav")
elseif(WIN32)
  check("... into the test trash" EXISTS "${OUT}/trash/files/Tick.wav")
else()
  check("... into the freedesktop trash" EXISTS "${OUT}/xdg/Trash/files/Tick.wav")
  check("... with its trashinfo" EXISTS "${OUT}/xdg/Trash/info/Tick.wav.trashinfo")
endif()
foreach(f rename collision trash)
  check("screenshot ${f}.ppm" EXISTS "${OUT}/${f}.ppm")
endforeach()
