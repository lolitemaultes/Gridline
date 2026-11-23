# Gridline 0.7.8 polish, round 3, in the real app (hidden, isolated config and HOME, dummy audio).
#   P1. Tracker: the LFO page's Exit closes the board as Esc does (its open flag clear, the pattern
#       in front); Alt+F9 then opens it again. A header screen's Exit (Help) closes it, nothing left open.
#   P2. Studio's piano roll toolbar: the zoom buttons always show. At 1024x640 (100%) and 2048x1280
#       (200%) the tools that do not fit fold into one "..." button whose list holds them in the Tool
#       menu's order, the tool in use picked; a click in the list picks a tool. At 1920 wide nothing folds.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k078_polish3_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

# run(<tag> <layout> <frames> <script> [extra args...]): the app on a copy of the song in its own folder.
function(run tag layout frames script)
  set(dir "${OUT}/${tag}")
  file(MAKE_DIRECTORY "${dir}/cfg/gridline" "${dir}/home")
  file(WRITE "${dir}/cfg/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  configure_file("${DEMO}" "${dir}/song.gridline.json" COPYONLY)
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

# ---- P1. Tracker: the LFO page's Exit closes the board, as Esc does
run(p1find tracker 40 "6 mod alt 1\n6 key f9\n7 mod alt 0\n20 lfo\n30 quit discard\n" --size 1440x900)
string(REGEX MATCH "\\[lfo\\] frame [0-9]+ lfos [^\n]* win ([-0-9]+) ([-0-9]+) ([-0-9]+) ([-0-9]+)" m "${out}")
math(EXPR ex "${CMAKE_MATCH_3} - 12")
math(EXPR ey "${CMAKE_MATCH_2} + 10")
run(p1exit tracker 120
    "6 mod alt 1\n6 key f9\n7 mod alt 0\n20 lfo\n22 shot \"${OUT}/p1_lfo.ppm\"\n24 sdlmove ${ex} ${ey}\n26 sdldown 0\n27 sdlup 0\n34 lfo\n36 tracker geometry\n40 mod alt 1\n40 key f9\n41 mod alt 0\n54 lfo\n70 quit discard\n"
    --size 1440x900)
string(REGEX MATCHALL "\\[lfo\\] frame [0-9]+ lfos [0-9]+ targets [0-9]+ open [01]" reps "${out}")
string(REGEX MATCHALL "open [01]" opens "${reps}")
string(REPLACE ";" "," opens "${opens}")
check("P1: the LFO page shows, Exit closes the board, Alt+F9 opens it again (${opens})" opens STREQUAL "open 1,open 0,open 1")
check("P1: Exit brings the pattern to the front" "${out}" MATCHES "tracker page Pattern editor: [^\n]* shown 1 ")

# A header screen's Exit (Help at 1440x900: the subject "Song page keys" at 70 76, Exit at 80 386)
# closes it with nothing left open, the keys with the pattern.
run(p1help tracker 90
    "6 tracker swap help\n20 tracker swapstate\n22 shot \"${OUT}/p1_help.ppm\"\n24 sdlmove 70 76\n26 sdldown 0\n27 sdlup 0\n32 tracker dump\n34 sdlmove 80 386\n36 sdldown 0\n37 sdlup 0\n46 tracker swapstate\n48 tracker dump\n70 quit discard\n"
    --size 1440x900)
check("P1: Help shows in the header" "${out}" MATCHES "tracker swapstate frame 20: 1 ")
check("P1: its Exit closes it" "${out}" MATCHES "tracker swapstate frame 46: 0 ")
check("P1: and the pattern has the keys" "${out}" MATCHES "tracker dump frame 48: [^\n]* keyboard 1 ")

# ---- P2. Studio's piano roll toolbar: zoom always shows; the tools that do not fit fold
set(kMenuOrder "Draw,Paint,Delete,Select,Slice,Mute,Playback,Zoom,Detune,Pitch bend,Vibrato")
# folded(<hidden list>): the list keeps the Tool menu's order.
function(menu_order hidden)
  string(REPLACE "," ";" hl "${hidden}")
  string(REPLACE "," ";" ml "${kMenuOrder}")
  set(last -1)
  foreach(n IN LISTS hl)
    list(FIND ml "${n}" i)
    if(i LESS 0 OR NOT i GREATER last)
      set(ok 0 PARENT_SCOPE)
      return()
    endif()
    set(last ${i})
  endforeach()
  set(ok 1 PARENT_SCOPE)
endfunction()

foreach(cfg "small;1024x640;1" "big;2048x1280;2")
  list(GET cfg 0 tag)
  list(GET cfg 1 size)
  list(GET cfg 2 scale)
  run(p2${tag} studio 60 "20 pianotool zoom\n30 pianobar\n32 shot \"${OUT}/p2_${tag}.ppm\"\n40 quit discard\n" --size ${size} --ui-scale ${scale} --edit 0 6 --hide rack)
  string(REGEX MATCH "pianobar frame 30: vibrato icon (-?[0-9]+) zoom buttons (-?[0-9]+) [^\n]* fold row ([0-9]+) more ([01]) at ([-0-9]+) ([-0-9]+) ([-0-9]+) ([-0-9]+) list [-0-9 ]+ pitch [0-9]+ hidden ([^\n]*)" m "${out}")
  check("P2 ${size} at ${scale}x: the zoom buttons show (${CMAKE_MATCH_2})" CMAKE_MATCH_2 EQUAL 1)
  check("P2 ${size} at ${scale}x: tools fold into the more button (row ${CMAKE_MATCH_3}, more ${CMAKE_MATCH_4})" CMAKE_MATCH_4 EQUAL 1)
  set(hidden "${CMAKE_MATCH_9}")
  menu_order("${hidden}")
  check("P2 ${size} at ${scale}x: the folded tools in the Tool menu's order (${hidden})" ok EQUAL 1)
  check("P2 ${size} at ${scale}x: the Zoom tool (in use) is folded here" hidden MATCHES "Zoom")
  math(EXPR mx "(${CMAKE_MATCH_5} + ${CMAKE_MATCH_7}) * ${scale} / 2")  # (window pixels: the interface scale)
  math(EXPR my "(${CMAKE_MATCH_6} + ${CMAKE_MATCH_8}) * ${scale} / 2")
  # Open the list: the tool in use (Zoom) is picked; a click on its first row picks that tool.
  run(p2${tag}open studio 90
      "20 pianotool zoom\n30 sdlmove ${mx} ${my}\n32 sdldown 0\n33 sdlup 0\n40 pianobar\n42 shot \"${OUT}/p2_${tag}_list.ppm\"\n60 quit discard\n"
      --size ${size} --ui-scale ${scale} --edit 0 6 --hide rack)
  string(REGEX MATCH "pianobar frame 40: [^\n]* list ([-0-9]+) ([-0-9]+) ([-0-9]+) ([-0-9]+) pitch ([0-9]+) hidden ([^\n]*)" m "${out}")
  check("P2 ${size} at ${scale}x: a click on the more button opens its list (${CMAKE_MATCH_1})" CMAKE_MATCH_1 GREATER 0)
  set(lx0 ${CMAKE_MATCH_1})
  set(ly0 ${CMAKE_MATCH_2})
  set(lx1 ${CMAKE_MATCH_3})
  set(ly1 ${CMAKE_MATCH_4})
  string(REPLACE "," ";" hl "${CMAKE_MATCH_6}")
  list(GET hl 0 first)
  math(EXPR rx "(${lx0} + ${lx1}) * ${scale} / 2")
  math(EXPR ry "(${ly0} + ${ly1}) * ${scale} / 2")
  run(p2${tag}pick studio 90
      "20 pianotool zoom\n30 sdlmove ${mx} ${my}\n32 sdldown 0\n33 sdlup 0\n40 sdlmove ${rx} ${ry}\n42 sdldown 0\n43 sdlup 0\n50 pianobar\n60 quit discard\n"
      --size ${size} --ui-scale ${scale} --edit 0 6 --hide rack)
  check("P2 ${size} at ${scale}x: a click on the list's first row (${first}) picks it and closes the list"
        "${out}" MATCHES "pianobar frame 50: [^\n]* list -1 " AND NOT "${out}" MATCHES "pianobar frame 50: [^\n]* tool 8 fold")
endforeach()

# Wide: nothing folds, the whole row and the zoom buttons as before.
run(p2wide studio 60 "30 pianobar\n32 shot \"${OUT}/p2_wide.ppm\"\n40 quit discard\n" --size 1920x1000 --edit 0 6 --hide rack)
string(REGEX MATCH "pianobar frame 30: vibrato icon (-?[0-9]+) zoom buttons (-?[0-9]+) [^\n]* fold row ([0-9]+) more ([01])" m "${out}")
check("P2 1920: the whole row (${CMAKE_MATCH_3} tools), no more button, the zoom buttons" CMAKE_MATCH_3 EQUAL 11 AND CMAKE_MATCH_4 EQUAL 0 AND CMAKE_MATCH_2 EQUAL 1)
# Other layouts keep their own bar: Clips reports no Studio fold.
run(p2clips live 60 "30 pianobar\n40 quit discard\n" --size 1024x640 --edit 0 6)
check("P2: Clips keeps its own tool row (no fold)" "${out}" MATCHES "pianobar frame 30: [^\n]* fold row -1 more 0")
