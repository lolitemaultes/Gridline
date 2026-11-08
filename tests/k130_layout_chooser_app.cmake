# Gridline: the window layout chooser in the running app (hidden, isolated config, real input).
#   1. The welcome screen's layout page: the keyboard moves between the cards, Enter picks one
#      (the layout switches and ui.layout is saved), Esc closes the page.
#   2. The welcome screen's page with the mouse: a click on a card picks it.
#   3. Settings > Interface: a click on a card picks it; with "Matching shortcuts" switched off
#      first, only the layout and the menus change.
#   4. View > Window layout: the menu lists every layout.
# Every pick is checked in the app's own report and in the settings file written at exit.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k130_layout_chooser_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

# run(<tag> <script text> <args>...): a fresh config; the output in OUT_<tag>, the saved settings in SET_<tag>.
function(run tag script)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900 --ui-scale 1 ${ARGN}
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (${tag}): gridline-app exited with ${rc}:\n${err}")
  endif()
  set(OUT_${tag} "${out}" PARENT_SCOPE)
  set(s "")
  if(EXISTS "${cfg}/gridline/settings.json")
    file(READ "${cfg}/gridline/settings.json" s)
  endif()
  set(SET_${tag} "${s}" PARENT_SCOPE)
endfunction()

function(expectMatch tag text regex what)
  if(NOT text MATCHES "${regex}")
    message(FATAL_ERROR "FAIL (${tag}): ${what} (no match for \"${regex}\")\n${text}")
  endif()
  message(STATUS "ok - ${tag}: ${what}")
endfunction()

# The centre of a reported card or option ("layoutcard 2 logic x=.. y=.. w=.. h=..").
function(centreOf text regex outVar)
  string(REGEX MATCH "${regex} x=([0-9-]+) y=([0-9-]+) w=([0-9]+) h=([0-9]+)" m "${text}")
  if(NOT m)
    message(FATAL_ERROR "FAIL: no rectangle for \"${regex}\" in:\n${text}")
  endif()
  math(EXPR cx "${CMAKE_MATCH_1} + ${CMAKE_MATCH_3} / 2")
  math(EXPR cy "${CMAKE_MATCH_2} + ${CMAKE_MATCH_4} / 2")
  set(${outVar} "${cx} ${cy}" PARENT_SCOPE)
endfunction()

# ---- 1. the welcome page with the keyboard ----
# The page opens on the layout in use (Studio). The first arrow shows the focus ring, the next
# ones move it: right, right = Logic, left = Live; Enter picks Live. Esc closes the page.
run(keys "8 launcher\n10 key right\n12 key right\n14 key right\n16 key left\n18 key enter\n24 launcher\n26 key escape\n30 launcher\n34 quit\n"
    --show launcher --pane launcher-layout)
string(REGEX MATCHALL "launcher visible=[^\n]*" reports "${OUT_keys}")
list(LENGTH reports n)
if(NOT n EQUAL 3)
  message(FATAL_ERROR "FAIL (keys): expected 3 launcher reports, got ${n}\n${OUT_keys}")
endif()
list(GET reports 0 r0)
list(GET reports 1 r1)
list(GET reports 2 r2)
expectMatch(keys "${r0}" "view=3" "the layout page is shown")
expectMatch(keys "${OUT_keys}" "layoutcard 0 studio x=[0-9]+ y=[0-9]+ w=[0-9]+ h=[0-9]+ in=##launcher" "the cards are drawn on the welcome screen")
expectMatch(keys "${OUT_keys}" "layoutcard 2 logic" "every layout has a card")
expectMatch(keys "${r1}" "view=3" "the page stays after a pick")
string(REGEX MATCHALL "layout active=[a-z]+ setting=[a-z]+" states "${OUT_keys}")
list(GET states 0 s0)
list(GET states 1 s1)
expectMatch(keys "${s0}" "active=studio setting=studio" "Studio in use at first")
expectMatch(keys "${s1}" "active=live setting=live" "arrows and Enter picked Live, and the window switched")
expectMatch(keys "${r2}" "view=0" "Esc closed the page")
expectMatch(keys "${SET_keys}" "\"ui.layout\": \"live\"" "ui.layout saved as live")
expectMatch(keys "${SET_keys}" "\"keys.profile\": \"live\"" "the matching shortcuts came with it")

# ---- 2. the welcome page with the mouse ----
centreOf("${OUT_keys}" "layoutcard 2 logic" logicCard)
run(mouse "8 move ${logicCard}\n10 down 0\n11 up 0\n16 launcher\n20 quit\n" --show launcher --pane launcher-layout)
expectMatch(mouse "${OUT_mouse}" "layout active=logic setting=logic keys=logic menus=logic" "a click on the Logic card picked it")
expectMatch(mouse "${SET_mouse}" "\"ui.layout\": \"logic\"" "ui.layout saved as logic")

# ---- 3. Settings > Interface ----
run(settingsProbe "10 launcher\n14 quit discard\n" "${DEMO}" --show interface)
expectMatch(settingsProbe "${OUT_settingsProbe}" "layoutcard 1 live x=[0-9]+ y=[0-9]+ w=[0-9]+ h=[0-9]+ in=Settings" "the cards are drawn in Settings")
expectMatch(settingsProbe "${OUT_settingsProbe}" "layoutoption keys [^\n]* on=1" "Matching shortcuts is on by default")
centreOf("${OUT_settingsProbe}" "layoutcard 1 live" liveCard)
centreOf("${OUT_settingsProbe}" "layoutoption keys" keysOpt)
run(settings "8 move ${keysOpt}\n10 down 0\n11 up 0\n14 move ${liveCard}\n16 down 0\n17 up 0\n22 launcher\n26 quit discard\n" "${DEMO}" --show interface)
expectMatch(settings "${OUT_settings}" "layoutoption keys [^\n]* on=0" "a click switched Matching shortcuts off")
expectMatch(settings "${OUT_settings}" "layout active=live setting=live keys=studio menus=live" "the card picked Live; the shortcuts stayed")
expectMatch(settings "${SET_settings}" "\"ui.layoutLinkKeys\": false" "the option is saved")
expectMatch(settings "${SET_settings}" "\"ui.layout\": \"live\"" "ui.layout saved as live")

# ---- 4. View > Window layout ----
run(menu "12 quit discard\n" "${DEMO}" --menu layouts)
string(REGEX MATCH "MENUDUMP BEGIN layouts\n([^\n]*\n)*MENUDUMP END" dump "${OUT_menu}")
expectMatch(menu "${dump}" "Studio" "the menu lists Studio")
expectMatch(menu "${dump}" "Live[^\n]*Preview" "Live, marked as a preview")
expectMatch(menu "${dump}" "Logic[^\n]*Preview" "Logic, marked as a preview")
expectMatch(menu "${dump}" "Layout settings" "and the settings item")

message(STATUS "k130_layout_chooser_app: OK")
