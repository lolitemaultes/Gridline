# Gridline 0.7.8: the settled small picks in the real app (hidden, isolated config and HOME, dummy audio).
#   Q1. A render to the library: the sheet's tag chip says loop for a whole pattern; a click on
#       One-shot flips it; the render is saved with the tag chosen.
#   Q2. Studio's piano roll in a cramped toolbar keeps its zoom buttons (the row's end folds into "...");
#       wide, both show.
#   Q3. Tracker: Alt+P on the Audio page plays the clip in the audio clip editor; again stops it.
#   Q4. Loupe's warp markers on a channel's sample: saved with the project, back on load, one undo step.
#   Q6. Tracker Browser at 1024 wide and 200%: no tab label is cut short.
#   Q8. Tracker: Alt+F9 shows the LFO page, again keeps it; Esc leaves it; Exit leaves it.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k078_picks2_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)
set(chanJson "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clipJson "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chanJson}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clipJson}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

# run(<tag> <layout> <frames> <script> [extra args...]): the app on a copy of the song in its own folder.
function(run tag layout frames script)
  set(dir "${OUT}/${tag}")
  file(MAKE_DIRECTORY "${dir}/cfg/gridline" "${dir}/home")
  file(WRITE "${dir}/cfg/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\", \"tracker.sampleView\": \"advanced\"}}\n")
  if(NOT EXISTS "${dir}/song.gridline.json")
    configure_file("${OUT}/song.gridline.json" "${dir}/song.gridline.json" COPYONLY)
  endif()
  set(ENV{GRIDLINE_CONFIG_HOME} "${dir}/cfg")
  set(ENV{HOME} "${dir}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${dir}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --frames ${frames} --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# ---- Q1. Render to the library: the tag chip
run(q1auto studio 200 "3 typing off\n6 render sheet pattern\n8 render dest library\n60 render state\n62 render go\n150 render state\n160 quit discard\n" --size 1440x900)
string(REGEX MATCH "render sheet dest library tag ([a-z]+) ([a-z]+) chip ([-0-9]+) ([-0-9]+) ([-0-9]+) ([-0-9]+)" m "${out}")
check("Q1: the sheet shows the library tag chip, automatic (${CMAKE_MATCH_1} ${CMAKE_MATCH_2})" CMAKE_MATCH_1 STREQUAL "loop" AND CMAKE_MATCH_2 STREQUAL "auto")
check("Q1: the whole pattern renders to the library tagged loop" "${out}" MATCHES "render state job 0 sheet 0 finished 1 last [^\n]* tag loop")

run(q1flip studio 220 "3 typing off\n6 render sheet pattern\n8 render dest library\n40 render state\n160 quit discard\n" --size 1440x900)
string(REGEX MATCH "render sheet dest library tag [a-z]+ [a-z]+ chip ([-0-9]+) ([-0-9]+) ([-0-9]+) ([-0-9]+)" m "${out}")
math(EXPR cx "${CMAKE_MATCH_3} - 12")
math(EXPR cy "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
run(q1flip studio 220
    "3 typing off\n6 render sheet pattern\n8 render dest library\n40 sdlmove ${cx} ${cy}\n42 sdldown 0\n43 sdlup 0\n50 render state\n52 shot \"${OUT}/q1_sheet.ppm\"\n56 render go\n150 render state\n160 quit discard\n"
    --size 1440x900)
check("Q1: a click on One-shot flips the tag (your choice)" "${out}" MATCHES "render sheet dest library tag oneshot chosen")
check("Q1: the render is saved tagged one-shot" "${out}" MATCHES "render state job 0 sheet 0 finished 1 last [^\n]* tag oneshot")

# ---- Q2. Studio's piano roll: zoom keeps its place
run(q2narrow studio 60 "30 pianobar\n32 shot \"${OUT}/q2_narrow.ppm\"\n40 quit discard\n" --size 1080x800 --edit 0 6 --hide rack)
string(REGEX MATCH "pianobar frame 30: vibrato icon (-?[0-9]+) zoom buttons (-?[0-9]+) [^\n]* fold row ([0-9]+) more ([01])" m "${out}")
# (polish3: the tools that do not fit fold into the "..." button; k078_polish3_app checks the list)
check("Q2: cramped, the zoom buttons show and the row's end folds (${CMAKE_MATCH_2} ${CMAKE_MATCH_3} ${CMAKE_MATCH_4})" CMAKE_MATCH_2 EQUAL 1 AND CMAKE_MATCH_4 EQUAL 1)
run(q2narrowv studio 60 "20 key v\n30 pianobar\n40 quit discard\n" --size 1080x800 --edit 0 6 --hide rack)
check("Q2: cramped, V still picks the Vibrato tool" "${out}" MATCHES "pianobar frame 30: [^\n]* vibrato tool 1")
run(q2wide studio 60 "30 pianobar\n32 shot \"${OUT}/q2_wide.ppm\"\n40 quit discard\n" --size 1920x1000 --edit 0 6 --hide rack)
string(REGEX MATCH "pianobar frame 30: vibrato icon (-?[0-9]+) zoom buttons (-?[0-9]+)" m "${out}")
check("Q2: wide, both show (${CMAKE_MATCH_1} ${CMAKE_MATCH_2})" CMAKE_MATCH_2 EQUAL 1 AND CMAKE_MATCH_1 EQUAL 1)

# ---- Q3. Tracker: Alt+P plays the clip in the audio clip editor (its Advanced Sample editor view: the
# settings pin it, as the classic view is the default)
run(q3 tracker 90
    "6 clipeditor dbl 70\n30 mod alt 1\n30 key p\n31 mod alt 0\n36 clipeditor state\n38 shot \"${OUT}/q3_audio.ppm\"\n50 mod alt 1\n50 key p\n51 mod alt 0\n56 clipeditor state\n70 quit discard\n"
    --size 1440x900)
string(REGEX MATCHALL "clipeditor state frame [^\n]*last \"[^\"]*\"" st "${out}")
list(LENGTH st nst)
check("Q3: two clip editor reports (${nst})" nst EQUAL 2)
list(GET st 0 st0)
list(GET st 1 st1)
check("Q3: Alt+P plays the clip" st0 MATCHES "host pane shown 1 clip 70 .*last \"alt\\+p play\"")
check("Q3: Alt+P again stops it" st1 MATCHES "last \"alt\\+p stop\"")

# ---- Q4. Loupe's warp markers on a channel's sample: saved, loaded, one undo step
run(q4 studio 140
    "6 loupe channel 60\n40 loupe warpadd 0.2\n44 loupe warps\n46 save \"${OUT}/q4_marked.gridline.json\"\n48 shot \"${OUT}/q4_loupe.ppm\"\n50 undo\n56 loupe warps\n58 save \"${OUT}/q4_undone.gridline.json\"\n60 redo\n66 loupe warps\n80 quit discard\n"
    --size 1440x900)
check("Q4: the marker is placed" "${out}" MATCHES "loupe warps: 1 ")
file(READ "${OUT}/q4_marked.gridline.json" marked)
file(READ "${OUT}/q4_undone.gridline.json" undone)
file(READ "${OUT}/q4/song.gridline.json" orig)
check("Q4: the marker is saved with the project" marked MATCHES "\"loupeWarp\"")
check("Q4: undo takes it back (Loupe and the project)" "${out}" MATCHES "loupe warps: 0\n" AND NOT undone MATCHES "loupeWarp")
string(REGEX MATCHALL "loupe warps: [0-9]+" ws "${out}")
list(GET ws 2 w3)
check("Q4: redo brings it back (${w3})" w3 STREQUAL "loupe warps: 1")
file(MAKE_DIRECTORY "${OUT}/q4load")
configure_file("${OUT}/q4_marked.gridline.json" "${OUT}/q4load/song.gridline.json" COPYONLY)
run(q4load studio 80 "6 loupe channel 60\n40 loupe warps\n60 quit discard\n" --size 1440x900)
check("Q4: the marker is back when the project opens again" "${out}" MATCHES "loupe warps: 1 ")

# ---- Q6. Tracker Browser at 1024 and 200%: no tab label cut short
run(q6 tracker 90 "4 tracker labels on\n5 toasts clear\n8 layout open Browser\n50 tracker labels\n52 shot \"${OUT}/q6_browser.ppm\"\n60 quit discard\n" --size 2048x1536 --ui-scale 2)
string(REGEX MATCH "tracker labels frame 50: drawn ([0-9]+) clipped ([0-9]+) tight ([0-9]+)([^\n]*)" m "${out}")
check("Q6: labels drawn ${CMAKE_MATCH_1}, none cut short (${CMAKE_MATCH_4})" CMAKE_MATCH_1 GREATER 0 AND CMAKE_MATCH_2 EQUAL 0)
check("Q6: no Browser tab is cut short or tight" NOT "${CMAKE_MATCH_4}" MATCHES " (clip|tight) tab ")

# ---- Q8. Tracker: Alt+F9 shows the LFO page; again keeps it; Esc leaves it; Exit leaves it
run(q8 tracker 120
    "6 mod alt 1\n6 key f9\n7 mod alt 0\n20 lfo\n22 shot \"${OUT}/q8_lfo.ppm\"\n24 mod alt 1\n24 key f9\n25 mod alt 0\n34 lfo\n36 key escape\n44 lfo\n46 tracker geometry\n70 quit discard\n"
    --size 1440x900)
string(REGEX MATCHALL "\\[lfo\\] frame [0-9]+ lfos [0-9]+ targets [0-9]+ open [01]" reps "${out}")
string(REGEX MATCHALL "open [01]" opens "${reps}")
string(REPLACE ";" "," opens "${opens}")
check("Q8: Alt+F9 shows the page, again keeps it, Esc leaves it (${opens})" opens STREQUAL "open 1,open 1,open 0")
check("Q8: Esc goes back to the pattern" "${out}" MATCHES "tracker page Pattern editor: [^\n]* shown 1 ")
# Exit: the page's title bar ends with it (the board's window: its top right corner).
string(REGEX MATCH "\\[lfo\\] frame [0-9]+ lfos [^\n]* win ([-0-9]+) ([-0-9]+) ([-0-9]+) ([-0-9]+)" m "${out}")
math(EXPR ex "${CMAKE_MATCH_3} - 12")
math(EXPR ey "${CMAKE_MATCH_2} + 10")
run(q8exit tracker 120
    "6 mod alt 1\n6 key f9\n7 mod alt 0\n20 lfo\n22 tracker geometry\n24 sdlmove ${ex} ${ey}\n26 sdldown 0\n27 sdlup 0\n34 tracker geometry\n70 quit discard\n"
    --size 1440x900)
string(REGEX MATCHALL "tracker page Pattern editor: [^\n]* shown [01] " pe "${out}")
string(REGEX REPLACE "tracker page Pattern editor: [^;]* (shown [01]) " "\\1" pe "${pe}")
string(REPLACE ";" "," pe "${pe}")
check("Q8: Exit goes back to the pattern (${pe})" pe STREQUAL "shown 0,shown 1")
