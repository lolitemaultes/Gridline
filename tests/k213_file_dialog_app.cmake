# Gridline: the Open / Save dialog in the real app (hidden, isolated config and HOME; every folder it
# browses is made here):
#   1. File > Open: the places name the scratch home's folders, a drive is listed, a place opens,
#      a folder opens, sorting by size and by name (folders first, natural order), Tab completes a
#      typed path, the current folder is pinned, a typed folder opens, Back returns, the arrows
#      select a file and fill the name, and a typed file path opens that project;
#   2. the next start: the dialog opens in the folder it was last used in, the favourite and the
#      recent folder are listed, and the Open button opens the selected project;
#   3. the layout fits without overlaps at 1280x800 and 1440x900 (the parts inside the dialog, the
#      places beside the list, the footer below it).
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k213_file_dialog_app.cmake
cmake_minimum_required(VERSION 3.18)
file(REMOVE_RECURSE "${OUT}")
set(H "${OUT}/home")
file(MAKE_DIRECTORY "${OUT}/cfg" "${H}/Desktop" "${H}/Documents" "${H}/Downloads" "${H}/Music/Songs/Stems")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${H}")
set(ENV{USERPROFILE} "${H}")
set(ENV{XDG_CONFIG_HOME} "${OUT}/cfg")
set(ENV{XDG_CACHE_HOME} "${OUT}/cache")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

file(READ "${DEMO}" demo)
set(S "${H}/Music/Songs")
file(WRITE "${S}/Beat 2.gridline" "${demo}")
file(WRITE "${S}/Beat 10.gridline" "${demo}")
# The biggest: the same project with padding after it (JSON allows trailing white space).
string(REPEAT " " 40000 pad)
file(WRITE "${S}/big.gridline" "${demo}${pad}")
file(WRITE "${S}/notes.txt" "not a project")
file(WRITE "${S}/Stems/kick.wav" "")

function(run tag size script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --size ${size} --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.out" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()
function(must tag regex what)
  if(NOT "${${tag}_out}" MATCHES "${regex}")
    message(FATAL_ERROR "FAIL (${tag}): ${what}\n${${tag}_out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()
function(mustnot tag regex what)
  if("${${tag}_out}" MATCHES "${regex}")
    message(FATAL_ERROR "FAIL (${tag}): ${what}\n${${tag}_out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()
# One "filedialog frame <n>" report, as text.
function(report tag frame outvar)
  string(REGEX MATCH "filedialog frame ${frame} [^\n]*\n([^\n]*\n)*filedialog end" r "${${tag}_out}")
  string(REGEX REPLACE "filedialog end.*" "" r "${r}")
  set(${outvar} "${r}" PARENT_SCOPE)
endfunction()
# The layout lines of a report: every part inside the dialog, places left of the list, footer below.
function(layout tag frame)
  report(${tag} ${frame} r)
  foreach(part dialog bar side list foot)
    string(REGEX MATCH "layout ${part} (-?[0-9]+) (-?[0-9]+) (-?[0-9]+) (-?[0-9]+)" m "${r}")
    if(NOT m)
      message(FATAL_ERROR "FAIL (${tag}): no layout for ${part}\n${r}")
    endif()
    set(${part}_x0 ${CMAKE_MATCH_1})
    set(${part}_y0 ${CMAKE_MATCH_2})
    set(${part}_x1 ${CMAKE_MATCH_3})
    set(${part}_y1 ${CMAKE_MATCH_4})
  endforeach()
  foreach(part bar side list foot)
    if(${part}_x0 LESS dialog_x0 OR ${part}_y0 LESS dialog_y0 OR ${part}_x1 GREATER dialog_x1 OR ${part}_y1 GREATER dialog_y1)
      message(FATAL_ERROR "FAIL (${tag}): ${part} leaves the dialog\n${r}")
    endif()
  endforeach()
  if(side_x1 GREATER list_x0 OR bar_y1 GREATER side_y0 OR bar_y1 GREATER list_y0 OR list_y1 GREATER foot_y0 OR side_y1 GREATER foot_y0)
    message(FATAL_ERROR "FAIL (${tag}): parts overlap\n${r}")
  endif()
  math(EXPR listW "${list_x1} - ${list_x0}")
  if(listW LESS 300)
    message(FATAL_ERROR "FAIL (${tag}): the list is only ${listW} px wide\n${r}")
  endif()
  message(STATUS "ok - ${tag}: the layout fits (dialog ${dialog_x0},${dialog_y0} to ${dialog_x1},${dialog_y1})")
endfunction()

# 1. One session through the dialog. Each step waits until the dialog reports the folder's rows
# ("filedialog:<folder>"), so a loaded machine only makes it slower, never different.
run(first 1280x800 "2 loadhold off
3 filedialog open
4 waitfor \"filedialog:${H}\" 60
6 filedialog state
7 filedialog place Music
8 waitfor \"filedialog:${H}/Music\" 60
9 filedialog state
10 filedialog go \"${S}\"
11 waitfor \"filedialog:${S}\" 60
12 filedialog sort size desc
14 filedialog state
15 filedialog sort name
17 filedialog state
18 filedialog complete \"${S}/St\"
19 filedialog pin
20 forget \"filedialog:${H}/Music\"
21 filedialog path \"${H}/Music\"
22 waitfor \"filedialog:${H}/Music\" 60
23 filedialog state
24 forget \"filedialog:${S}\"
25 filedialog back
26 waitfor \"filedialog:${S}\" 60
27 key down
29 key down
31 filedialog state
32 toasts clear
33 filedialog path \"${S}/Beat 2.gridline\"
35 waitfor project-loaded 60
38 loadstate
40 quit discard
")
report(first 6 r6)
string(FIND "${r6}" "side place \"Home\" ${H}\n" i)
if(i LESS 0)
  message(FATAL_ERROR "FAIL: Home is not a place\n${r6}")
endif()
foreach(p Desktop Documents Music Downloads)
  string(FIND "${r6}" "side place \"${p}\" ${H}/${p}" i)
  if(i LESS 0)
    message(FATAL_ERROR "FAIL: ${p} is not a place\n${r6}")
  endif()
endforeach()
message(STATUS "ok - the places are the scratch home's folders")
must(first "filedialog frame 6 shown 1\n  dir [^\n]*home\n" "File > Open starts at home (no project, no remembered folder)")
must(first "  side drive \"[^\"]+\" /" "a drive is listed")
must(first "filedialog frame 9 shown 1\n  dir [^\n]*home/Music\n" "a place opens its folder")
report(first 14 r14)
if(NOT r14 MATCHES "row dir Stems [^\n]*\n  row file big.gridline [^\n]*\n  row file Beat (2|10).gridline [^\n]*\n  row file Beat (2|10).gridline")
  message(FATAL_ERROR "FAIL: sorted by size (folders first, biggest first)\n${r14}")
endif()
if(r14 MATCHES "notes.txt")
  message(FATAL_ERROR "FAIL: a file of another type is listed\n${r14}")
endif()
message(STATUS "ok - by size, biggest first, folders first; other types hidden")
must(first "filedialog frame 17 [^\n]*\n[^\n]*\n  sort name asc[^\n]*\n[^\n]*\n  row dir Stems [^\n]*\n  row file Beat 2.gridline [^\n]*\n  row file Beat 10.gridline [^\n]*\n  row file big.gridline"
     "by name, natural order")
must(first "filedialog complete: [^\n]*/Songs/Stems/\n" "Tab completes a typed folder")
must(first "filedialog frame 23 shown 1\n  dir [^\n]*home/Music\n" "a typed folder opens")
must(first "filedialog frame 23 [^\n]*\n([^\n]*\n)*  side favourite \"Songs\" [^\n]*/Songs\n" "the pinned folder is a favourite")
must(first "filedialog frame 31 shown 1\n  dir [^\n]*/Songs\n[^\n]*\n  name \"Beat 2.gridline\" selected \"Beat 2.gridline\"" "Back returns; the arrows select a file and fill the name")
must(first "waitfor project-loaded: reached" "a typed file path opens the project")
must(first "toast \"Opened Beat 2" "the project typed is the one opened")
layout(first 6)

file(READ "${OUT}/cfg/gridline/settings.json" settings)
if(NOT settings MATCHES "dialogFavourites\": \"[^\"]*Music/Songs" OR NOT settings MATCHES "dialogLast\": \"Open project\\\\t[^\"]*Music/Songs"
   OR NOT settings MATCHES "dialogRecent\": \"[^\"]*Music/Songs" OR NOT settings MATCHES "dialogSort\": 0")
  message(FATAL_ERROR "FAIL: the dialog's settings were not kept:\n${settings}")
endif()
message(STATUS "ok - favourites, the last folder, recent folders and the sort are saved")

# 2. The next start.
run(second 1440x900 "2 loadhold off
3 filedialog open
4 waitfor \"filedialog:${S}\" 60
6 filedialog state
7 filedialog select \"Beat 10.gridline\"
8 toasts clear
9 filedialog accept
11 waitfor project-loaded 60
14 loadstate
16 quit discard
")
must(second "filedialog frame 6 shown 1\n  dir [^\n]*/Music/Songs\n" "the dialog opens where it was last used")
must(second "side favourite \"Songs\" [^\n]*/Songs\n" "the favourite is kept")
must(second "side recent \"Songs\" [^\n]*/Songs\n" "the recent folder is listed")
must(second "toast \"Opened Beat 10" "the Open button opens the selected project")
layout(second 6)

# 3. Save as: the name box holds the name only, the type's extension shows beside it and is added
# when saving; a new type changes it; saving over a file asks first.
set(W "${H}/Saves")
file(MAKE_DIRECTORY "${W}")
run(save 1280x800 "2 loadhold off
3 filedialog saveas \"${W}\"
4 waitfor \"filedialog:${W}\" 60
6 filedialog state
7 text \"My Song\"
9 filedialog state
10 filedialog type 1
12 filedialog state
13 filedialog accept
14 waitfor \"file:${W}/My Song.gridlinez\" 60
16 filedialog saveas \"${W}\"
20 text Plain
21 filedialog type 0
23 filedialog state
24 filedialog accept
25 waitfor \"file:${W}/Plain.gridline\" 60
27 filedialog saveas \"${W}\"
31 text Plain
32 filedialog type 0
34 filedialog accept
36 filedialog state
37 toasts clear
38 filedialog accept
40 filedialog state
41 loadstate
43 quit discard
")
must(save "filedialog frame 6 shown 1\n  dir [^\n]*/Saves\n[^\n]*\n  name \"Untitled.gridline\"[^\n]*\n([^\n]*\n)*  namebox \"Untitled\" ext \".gridline\" type 0"
     "Save as: the box shows the name without its extension, the extension beside it")
must(save "filedialog frame 9 [^\n]*\n[^\n]*\n[^\n]*\n  name \"My Song.gridline\"[^\n]*\n([^\n]*\n)*  namebox \"My Song\" ext \".gridline\""
     "a typed name without an extension takes the type's extension")
must(save "filedialog frame 12 [^\n]*\n[^\n]*\n[^\n]*\n  name \"My Song.gridlinez\"[^\n]*\n([^\n]*\n)*  namebox \"My Song\" ext \".gridlinez\" type 1"
     "another type changes the extension, not the name in the box")
must(save "waitfor file:[^\n]*/My Song.gridlinez: reached" "saved with the chosen type's extension")
must(save "filedialog frame 23 [^\n]*\n[^\n]*\n[^\n]*\n  name \"Plain.gridline\"[^\n]*\n([^\n]*\n)*  namebox \"Plain\" ext \".gridline\" type 0"
     "back to the first type: its extension")
must(save "waitfor file:[^\n]*/Plain.gridline: reached" "a typed name with no extension saves with it")
must(save "filedialog frame 36 shown 1\n([^\n]*\n)*  namebox \"Plain\" ext \".gridline\" type 0 confirm \"[^\"]*/Saves/Plain.gridline\""
     "saving over a file asks first (the box still shows the name only)")
must(save "filedialog frame 40 shown 0" "Replace saves")
must(save "toast \"Saved Plain.gridline" "the file replaced is the one named")
file(GLOB saved RELATIVE "${W}" "${W}/*")
list(SORT saved)
if(NOT saved STREQUAL "My Song.gridlinez;Plain.gridline")
  message(FATAL_ERROR "FAIL: the saved files are ${saved}")
endif()
message(STATUS "ok - exactly the two files, each with its type's extension")
