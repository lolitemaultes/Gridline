# Gridline - K-106: the Tracker's Collaboration page (the Collaboration panel as a page of the header
# band, CollabUiTracker.inc), at 1024x640:
#   1. one window: File > Collaboration... (through the palette) swaps the band to the page, and the
#      panel's window is not drawn; each page (Share, Projects, Profile, Connection) fits its band (no row
#      past a box's foot); Exit closes the panel; another screen swapped in closes it; the band's own
#      swap opens it again; New collaborative project... shows the Share page; Make collaborative on the
#      page makes the project collaborative and the page shows the session's pages, which fit too.
#   2. two windows over loopback (each with its own config; no address servers): the Tracker hosts, a
#      Studio collaborator joins. On the page: the Members page offers the member actions (an admin's
#      too), Follow their playhead follows, Invite creates and cancels an invite, Reconnect... opens
#      the reconnect codes on the Invite page; every page fits with two members.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k106_collab_page_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(failures 0)
macro(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR failures "${failures} + 1")
endmacro()
# The page report printed at a frame: "collab page frame F: ..." (the line's rest in ${out_var}).
function(pageAt text frame out_var)
  string(REGEX MATCH "collab page frame ${frame}: [^\n]*" m "${text}")
  set(${out_var} "${m}" PARENT_SCOPE)
endfunction()
set(SETTINGS "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"collab.stunHost\": \"\", \"collab.stunBackups\": \"\", \"collab.portMapping\": false}}\n")

# ---- 1. one window ----
set(D "${OUT}/one")
file(MAKE_DIRECTORY "${D}/cfg/gridline" "${D}/home")
file(WRITE "${D}/cfg/gridline/settings.json" "${SETTINGS}")
configure_file("${DEMO}" "${D}/song.gridline.json" COPYONLY)
file(WRITE "${D}/input.txt" "5 toasts clear
6 collab profile Alex
8 palette run menu:File/Collaboration...
11 collab panel
12 layout where Collaboration
13 collab page share
15 collab page
16 collab page projects
18 collab page
19 collab page profile
21 collab page
22 collab page connection
24 collab page
25 collab press tcollabexit
27 collab panel
28 palette run menu:File/Collaboration...
31 collab panel
32 tracker swap help
34 collab panel
35 tracker swap collab
37 collab panel
38 palette run menu:File/New collaborative project...
43 collab page
44 collab press tsharemake
47 collab report \"${D}/made.json\"
48 collab page
49 collab page members
51 collab page
52 collab page invite
54 collab page
55 collab page session
57 collab page
60 quit discard
")
execute_process(COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfg HOME=${D}/home
                        "${APP}" "${D}/song.gridline.json" --hidden --layout-dev --no-ui-sounds --fixed-dt 16 --layout tracker --size 1024x640
                        --dev-input "${D}/input.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${D}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  fail("one window: gridline-app failed (${rc})")
endif()
if(NOT out MATCHES "collab frame 11 panel open 1 tracker-page 1")
  fail("one window: File > Collaboration... did not show the page in the band")
endif()
if(NOT out MATCHES "where Collaboration layout tracker drawn 0")
  fail("one window: the panel's window is drawn in the Tracker")
endif()
foreach(fp "15;share;tsharemake" "18;projects;tprojfolder" "21;profile;tcolother" "24;connection;tnettest" "51;members;tfollow" "54;invite;tinvitecreate"
        "57;session;tstop")
  list(GET fp 0 f)
  list(GET fp 1 pg)
  list(GET fp 2 btn)
  pageAt("${out}" ${f} line)
  if(NOT line MATCHES "shown 1 page ${pg} ")
    fail("one window: frame ${f}: the ${pg} page is not shown: '${line}'")
  elseif(NOT line MATCHES "overflow 0")
    fail("one window: the ${pg} page does not fit at 1024x640: '${line}'")
  elseif(NOT line MATCHES "[ ,]${btn}[,( ]")
    fail("one window: the ${pg} page has no ${btn} button: '${line}'")
  endif()
endforeach()
if(NOT out MATCHES "collab frame 27 panel open 0")
  fail("one window: Exit did not close the page")
endif()
if(NOT out MATCHES "collab frame 31 panel open 1 tracker-page 1")
  fail("one window: File > Collaboration... did not open the page again")
endif()
if(NOT out MATCHES "collab frame 34 panel open 0")
  fail("one window: another screen (Help) did not replace the page")
endif()
if(NOT out MATCHES "collab frame 37 panel open 1 tracker-page 1")
  fail("one window: the band's swap did not open the panel")
endif()
pageAt("${out}" 43 line)
if(NOT line MATCHES "shown 1 page share ")
  fail("one window: New collaborative project... did not show the Share page: '${line}'")
endif()
if(NOT out MATCHES "collab page press tsharemake: done")
  fail("one window: Make collaborative was not pressed")
endif()
if(EXISTS "${D}/made.json")
  file(READ "${D}/made.json" j)
  string(JSON act GET "${j}" active)
  if(NOT act EQUAL 1)
    fail("one window: Make collaborative on the page did not make the project collaborative")
  endif()
else()
  fail("one window: no report after Make collaborative")
endif()
pageAt("${out}" 48 line)
if(NOT line MATCHES "pages session,members,invite,profile,connection")
  fail("one window: the page does not show the session's pages once shared: '${line}'")
endif()

# ---- 2. two windows over loopback ----
set(D "${OUT}/two")
foreach(who A B)
  file(MAKE_DIRECTORY "${D}/cfg${who}/gridline" "${D}/home${who}")
  file(WRITE "${D}/cfg${who}/gridline/settings.json" "${SETTINGS}")
endforeach()
file(WRITE "${D}/a.txt" "5 toasts clear
6 collab profile Alex
8 palette run menu:File/Collaboration...
10 collab new \"${D}/Alex/Song\"
20 collab invite \"${D}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
32 waitfor never 2
33 collab page members
34 waitfor never 1
35 collab page
36 collab press tfollow
37 waitfor never 1
38 collab page
39 collab page invite
40 waitfor never 1
41 collab press tinviteother
42 waitfor never 1
43 collab press tinvitecreate
44 waitfor never 3
45 collab page
46 collab press tinvitecancel
47 waitfor never 1
48 collab page
49 collab page members
50 collab press treconnect
51 waitfor never 2
52 collab page
53 collab page session
54 waitfor never 1
55 collab page
56 collab page profile
57 waitfor never 1
58 collab page
59 collab page connection
60 waitfor never 1
61 collab page
62 collab report \"${D}/a-done.json\"
63 waitfor never 2
90 quit discard
")
file(WRITE "${D}/b.txt" "5 collab profile Sam
10 collab join \"${D}/invite.txt\" \"${D}/Sam/Song\"
11 waitfor collab-joined 120
12 waitfor collab-synced 60
13 waitfor \"file:${D}/a-done.json\" 300
90 quit discard
")
# (the Tracker's window last: only the last command's output reaches the file)
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfgB HOME=${D}/homeB
          "${APP}" --hidden --layout-dev --no-ui-sounds --layout studio --size 1280x800 --dev-input "${D}/b.txt"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfgA HOME=${D}/homeA
          "${APP}" --hidden --layout-dev --no-ui-sounds --layout tracker --size 1024x640 --dev-input "${D}/a.txt"
  RESULTS_VARIABLE rcs OUTPUT_FILE "${D}/run.out" ERROR_FILE "${D}/run.err" TIMEOUT 420)
if(NOT rcs STREQUAL "0;0")
  fail("two windows: exit codes ${rcs}")
endif()
file(READ "${D}/run.out" out)
pageAt("${out}" 35 line)
if(NOT line MATCHES "shown 1 page members " OR NOT line MATCHES "overflow 0")
  fail("two windows: the Members page is not shown whole: '${line}'")
endif()
foreach(btn tfollow tgoto treconnect tmakeadmin tfingerprint tremove)
  if(NOT line MATCHES "[ ,]${btn}[, ]")
    fail("two windows: the Members page's ${btn} is missing or off for the collaborator: '${line}'")
  endif()
endforeach()
pageAt("${out}" 38 line)
if(NOT line MATCHES "follow 1")
  fail("two windows: Follow their playhead did not follow: '${line}'")
endif()
pageAt("${out}" 45 line)
if(NOT line MATCHES "invite (preparing|ready)")
  fail("two windows: Create invite on the page made no invite: '${line}'")
endif()
pageAt("${out}" 48 line)
if(NOT line MATCHES "invite none")
  fail("two windows: Cancel invite on the page did not cancel it: '${line}'")
endif()
pageAt("${out}" 52 line)
if(NOT line MATCHES "page invite " OR NOT line MATCHES "reconnect 1" OR NOT line MATCHES "[ ,]trcclose[, ]")
  fail("two windows: Reconnect... did not show the reconnect codes on the Invite page: '${line}'")
endif()
foreach(f 52 55 58 61)
  pageAt("${out}" ${f} line)
  if(NOT line MATCHES "overflow 0")
    fail("two windows: frame ${f}: a page does not fit at 1024x640 with two members: '${line}'")
  endif()
endforeach()
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - the Tracker's Collaboration page opens from the menu, fits at 1024x640, and its actions work")
