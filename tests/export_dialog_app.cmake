# Gridline: the Export audio dialog in the real app (hidden, isolated config and HOME, dummy audio
# driver). For each layout (Studio, Clips, Smooth, Tracker) on a song with three markers:
#   - File > Export audio (render) opens the one dialog on the whole song; Export marker areas opens
#     the same dialog with Marker areas chosen;
#   - the scope cards (song, selection, marker range, areas, pattern) switch what is exported;
#   - the endings (cut, ring out up to N s, a fixed tail of N s, fold into the start);
#   - name pieces: old %-patterns read as pieces, pieces added and removed, the example and the
#     footer's line follow them;
# and in Studio a render with only Opus ticked is not dropped (an .opus file, or the encoder's
# message when none is installed: never "Choose at least one output format").
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P export_dialog_app.cmake
cmake_minimum_required(VERSION 3.19)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

file(READ "${DEMO}" song)
string(JSON song SET "${song}" markers
       "[{\"pos\": 0, \"name\": \"Opening\"}, {\"pos\": 1536, \"name\": \"Second song with a long title for the list\"}, {\"pos\": 3840, \"name\": \"Finale\"}]")

function(expect_line out what regex)
  if(NOT out MATCHES "${regex}")
    message(FATAL_ERROR "FAIL: ${what} (see the log)")
  endif()
endfunction()

foreach(L studio live logic tracker)
  set(cfg "${OUT}/cfg_${L}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${L}\", \"keys.profile\": \"${L}\", \"ui.menuStyle\": \"${L}\"}}\n")
  file(WRITE "${cfg}/song.gridline.json" "${song}")
  set(script "5 toasts clear\n")
  # the menu's two rows open the one dialog
  string(APPEND script "8 exportdlg open\n14 exportdlg dump\n16 exportdlg open areas\n22 exportdlg dump\n24 markerareas dump\n")
  # scope cards
  string(APPEND script "26 exportdlg scope song\n30 exportdlg dump\n32 exportdlg scope pattern\n36 exportdlg dump\n38 exportdlg scope markers\n42 exportdlg dump\n")
  string(APPEND script "44 exportdlg scope selection\n48 exportdlg dump\n50 exportdlg scope areas\n54 exportdlg dump\n")
  # endings
  string(APPEND script "56 exportdlg tail cut\n58 exportdlg dump\n60 exportdlg tail ring 12\n62 exportdlg dump\n64 exportdlg tail fixed 1.5\n66 exportdlg dump\n68 exportdlg tail wrap\n70 exportdlg dump\n72 exportdlg tail fixed 1.5\n")
  # name pieces: an old part pattern, an added and a removed piece, text
  string(APPEND script "74 exportdlg name part set \"%P - %N %T\"\n76 exportdlg name area add N\n80 exportdlg dump\n")
  string(APPEND script "82 exportdlg name area remove 4\n84 exportdlg name file text \" final\"\n88 exportdlg dump\n")
  string(APPEND script "90 exportdlg format 5 on\n94 exportdlg dump\n96 exportdlg format 5 off\n")
  if(L STREQUAL "studio")
    string(APPEND script "100 exportdlg scope song\n102 exportdlg tail cut\n104 exportstart 32 \"${cfg}/opus\"\n400 waitfor render-finished 120\n402 export\n404 toastlist\n")
  endif()
  string(APPEND script "410 quit discard\n")
  file(WRITE "${cfg}/input.txt" "${script}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  execute_process(COMMAND "${APP}" "${cfg}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames 600 --dev-input "${cfg}/input.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${L}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL: ${L}: gridline-app failed (${rc}): ${err}")
  endif()
  expect_line("${out}" "${L}: File > Export audio opens the dialog on the whole song" "exportdlg frame 14: open 1 areas 0 scope 0")
  expect_line("${out}" "${L}: Export marker areas opens the same dialog on the areas" "exportdlg frame 22: open 1 areas 1 scope 0 dest 0")
  expect_line("${out}" "${L}: the areas are listed, ticked, full names" "row Second song with a long title for the list \\| [0-9.]+ \\| [0-9:.]+ \\| on")
  expect_line("${out}" "${L}: the Whole song card" "exportdlg frame 30: open 1 areas 0 scope 0")
  expect_line("${out}" "${L}: the pattern card" "exportdlg frame 36: open 1 areas 0 scope 1")
  expect_line("${out}" "${L}: between two markers" "exportdlg frame 42: open 1 areas 0 scope 3")
  expect_line("${out}" "${L}: the selection card" "exportdlg frame 48: open 1 areas 0 scope 2")
  expect_line("${out}" "${L}: back to the areas card" "exportdlg frame 54: open 1 areas 1 scope 2 dest 0")
  expect_line("${out}" "${L}: Cut" "exportdlg frame 58: [^\n]* tail cut ")
  expect_line("${out}" "${L}: ring out up to 12 s" "exportdlg frame 62: [^\n]* tail ring ring 12.0 ")
  expect_line("${out}" "${L}: a fixed tail of 1.5 s" "exportdlg frame 66: [^\n]* tail fixed ring 12.0 fixed 1.5 ")
  expect_line("${out}" "${L}: fold into the start" "exportdlg frame 70: [^\n]* tail wrap ")
  expect_line("${out}" "${L}: the old part pattern as pieces" "part names %P - %N %T = \\[Project\\]' - '\\[Number\\]' '\\[Track\\]")
  expect_line("${out}" "${L}: a Number piece added to the area names" "area names %P - %M - %N = \\[Project\\]' - '\\[Marker\\]' - '\\[Number\\]")
  expect_line("${out}" "${L}: the example follows the pieces" "example [^\n]* - Opening - 01\\.wav \\| and 2 more, one per area")
  expect_line("${out}" "${L}: the Number piece removed again" "exportdlg frame 88: [^\n]*\n  file name %P final = \\[Project\\]' final'\n  area names %P - %M = ")
  expect_line("${out}" "${L}: the footer counts the areas' files" "exportdlg frame 88: [^\n]*\n[^\n]*\n[^\n]*\n[^\n]*\n[^\n]*\n  summary 3 files")
  expect_line("${out}" "${L}: Opus is counted with WAV" "exportdlg frame 94: [^\n]* formats WAV,Opus\n[^\n]*\n[^\n]*\n[^\n]*\n[^\n]*\n  summary 6 files")
  expect_line("${out}" "${L}: the footer was drawn" "drawn summary [0-9]+ files")
  if(L STREQUAL "studio")
    file(GLOB opus "${cfg}/opus/*.opus")
    if(NOT opus AND NOT out MATCHES "Opus: ")
      message(FATAL_ERROR "FAIL: studio: an Opus-only render wrote no .opus file and said nothing about the encoder (see ${OUT}/studio.log)")
    endif()
    if(out MATCHES "Choose at least one output format")
      message(FATAL_ERROR "FAIL: studio: the ticked Opus format was dropped")
    endif()
  endif()
  message(STATUS "ok - ${L}: one dialog from both rows, scope cards, endings, name pieces, footer")
endforeach()
file(REMOVE_RECURSE "${OUT}")
message(STATUS "ok - export_dialog_app")
