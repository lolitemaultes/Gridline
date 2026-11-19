# Gridline - K-106 cp17: the Tracker's sideways zoom in the real app (the user's ask: every track in
# view, scroll across, zoom out to see more, on the Pattern page and the Song page).
#   Pattern: at 1024x640 "Fit all" shows every track of the demo song; zoom in and out round-trips;
#   the plain view returns; the cursor's track is in view; Ctrl+Alt+/ is Fit.
#   Song: the arranger's timeline zooms in and scrolls to the song's end; the lanes zoom in, scroll to
#   a lane far right, and Fit shows them all.
#   The saved song is byte-identical before and after (the zoom is a setting, never in the song).
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k106_zoom_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"tracker\", \"keys.profile\": \"tracker\", \"ui.menuStyle\": \"tracker\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n${out}")
endfunction()

file(WRITE "${OUT}/input.txt" "5 toasts clear
6 save \"${OUT}/before.gridline.json\"
8 tracker focus
10 tracker zoom show
12 tracker zoom fit
16 tracker zoom show
18 tracker zoom in
22 tracker zoom show
23 tracker zoom out
27 tracker zoom show
28 tracker zoom plain
32 tracker zoom show
33 tracker zoom fit
34 tracker cursor 18 0 0 0
36 tracker zoom in
40 tracker zoom show
41 tracker zoom plain
42 sdlkey / down ctrl+alt
43 sdlkey / up ctrl+alt
46 tracker zoom show
48 tracker page Song
52 tracker songzoom arranger
54 tracker songzoom in
55 tracker songzoom in
56 tracker songzoom in
58 tracker songzoom end
62 tracker songzoom show
64 tracker songzoom lanes
66 tracker songzoom in
67 tracker songzoom in
68 tracker songzoom in
70 tracker songzoom lane 18
74 tracker songzoom show
76 tracker songzoom fit
80 tracker songzoom show
82 tracker songzoom arranger
83 tracker songzoom fit
84 tracker page Pattern
86 save \"${OUT}/after.gridline.json\"
88 tracker zoom fit
89 tracker cursor 99 999 0 0
92 tracker zoom show
93 tracker dump
94 tracker order new
96 tracker dump
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1024x640 --layout tracker --dev-input "${OUT}/input.txt"
                        --fixed-dt 16 --frames 100 --screenshot "${OUT}/end.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed (${rc})")
endif()

# ---- Pattern ----
if(NOT out MATCHES "tracker zoom frame 16: step ([0-9]+) fit 1 tracks ([0-9]+) first 0 last ([0-9]+)")
  fail("Fit all did not report a fitted view")
endif()
set(fitStep "${CMAKE_MATCH_1}")
set(nTracks "${CMAKE_MATCH_2}")
set(last "${CMAKE_MATCH_3}")
math(EXPR want "${nTracks} - 1")
if(NOT last EQUAL want)
  fail("at 1024x640 Fit all shows tracks 0..${last} of ${nTracks}, not all of them")
endif()
if(nTracks LESS 20)
  fail("the demo song should have twenty-odd tracks (${nTracks})")
endif()
if(NOT out MATCHES "tracker zoom frame 10: step 0 fit 0 tracks [0-9]+ first 0 last ([0-9]+)")
  fail("the plain view at the start")
endif()
if(NOT CMAKE_MATCH_1 LESS last)
  fail("the plain view already shows every track (the test needs a song wider than the window)")
endif()
if(NOT out MATCHES "tracker zoom frame 22: step ([0-9]+) fit 0")
  fail("zoom in after Fit")
endif()
math(EXPR closer "${fitStep} - 1")
if(NOT CMAKE_MATCH_1 EQUAL closer)
  fail("zoom in from Fit (step ${fitStep}) is not one step closer (${CMAKE_MATCH_1})")
endif()
if(NOT out MATCHES "tracker zoom frame 27: step ${fitStep} fit 0 [^\n]* settings ${fitStep} 0")
  fail("zoom in then out does not come back to the fitted step")
endif()
if(NOT out MATCHES "tracker zoom frame 32: step 0 fit 0 [^\n]* settings 0 0")
  fail("the plain view did not come back (settings 0, no fit)")
endif()
if(NOT out MATCHES "tracker zoom frame 40: [^\n]* cursor 18 field [0-9]+ inview 1")
  fail("the cursor's track is not in view after zooming in")
endif()
if(NOT out MATCHES "tracker zoom frame 46: step [0-9]+ fit 1 ")
  fail("Ctrl+Alt+/ did not fit all tracks")
endif()

# ---- Song ----
if(NOT out MATCHES "tracker songzoom frame 62: view arranger [^|]*\\| timeline zoom ([0-9.]+) start ([0-9]+) end ([0-9]+) len ([0-9]+)")
  fail("the Song page's timeline did not report")
endif()
set(tz "${CMAKE_MATCH_1}")
set(tEnd "${CMAKE_MATCH_3}")
set(tLen "${CMAKE_MATCH_4}")
if(tz VERSION_LESS 3.0)
  fail("three zoom-ins on the timeline gave zoom ${tz}")
endif()
math(EXPR tGap "${tLen} - ${tEnd}")
if(tGap GREATER 1 OR tGap LESS -1)
  fail("the timeline did not scroll to the song's end (end ${tEnd}, length ${tLen})")
endif()
if(NOT out MATCHES "tracker songzoom frame 74: view lanes lanes ([0-9]+) lanew ([0-9]+) first ([0-9]+) last ([0-9]+) scroll ([0-9]+)")
  fail("the lanes did not report")
endif()
set(nLanes "${CMAKE_MATCH_1}")
if(CMAKE_MATCH_3 GREATER 18 OR CMAKE_MATCH_4 LESS 18 OR CMAKE_MATCH_5 EQUAL 0)
  fail("the lanes did not scroll to lane 18 (first ${CMAKE_MATCH_3}, last ${CMAKE_MATCH_4}, scroll ${CMAKE_MATCH_5})")
endif()
math(EXPR lastLane "${nLanes} - 1")
if(NOT out MATCHES "tracker songzoom frame 80: view lanes lanes ${nLanes} lanew [0-9]+ first 0 last ${lastLane} scroll 0")
  fail("Fit on the lanes does not show all ${nLanes}")
endif()

# cp21: Fit with the cursor on the last track's last row: every track still in view, the cursor's
# track whole and the cursor on the pattern's last row (clamped there, not past it).
math(EXPR lastTrack "${nTracks} - 1")
if(NOT out MATCHES "tracker zoom frame 92: step [0-9]+ fit 1 tracks ${nTracks} first 0 last ${lastTrack} cursor ${lastTrack} field 0 inview 1")
  fail("Fit with the cursor on the last track and row does not keep every track and the cursor in view")
endif()
if(NOT out MATCHES "tracker dump frame 93: pattern [0-9]+ rows ([0-9]+) [^\n]* cursor ${lastTrack} ([0-9]+) ")
  fail("the dump after Fit on the last row")
endif()
set(curRows "${CMAKE_MATCH_1}")
math(EXPR lastRow "${CMAKE_MATCH_1} - 1")
if(NOT CMAKE_MATCH_2 EQUAL lastRow)
  fail("Fit on the last row: the cursor is on row ${CMAKE_MATCH_2}, not the last (${lastRow})")
endif()

# cp21, HUNT2-1119: a pattern made in the Tracker (the Song page's New) is as long as the current
# one (before HUNT2-1119: always the classic 64 rows).
if(NOT out MATCHES "tracker dump frame 96: pattern [0-9]+ rows ${curRows} ")
  fail("a new pattern made in the Tracker is not as long as the current one")
endif()

# ---- the song is untouched ----
file(READ "${OUT}/before.gridline.json" before)
file(READ "${OUT}/after.gridline.json" after)
if(NOT before STREQUAL after)
  fail("zooming changed the saved song")
endif()
message(STATUS "k106_zoom_app: ${nTracks} tracks fit at step ${fitStep}; timeline zoom ${tz} to the end; ${nLanes} lanes")
