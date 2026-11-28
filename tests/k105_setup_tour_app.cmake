# Gridline - K-105: First-run setup wizard + interactive guided tour quiet app test.
#   1. Quiet/hidden launch with fresh config does NOT auto-open a blocking setup wizard.
#   2. Dev-input hooks drive Setup Wizard steps 1..4, live theme switching, test tone, and close
#      (persisting ui.setupDone=true and ui.layoutChosen=true).
#   3. Dev-input hooks start the Guided Tour inside the demo project, step through targets,
#      verify live non-zero spotlight & non-clipping card rects, and stop cleanly.
#   4. Help menu includes both "Setup wizard..." and "Guided tour..." across Studio, Clips, Smooth, and Tracker.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k105_setup_tour_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_QUIET_WINDOWS} "1")

function(run tag script)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900 --ui-scale 1 ${ARGN}
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
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

# ---- 1 & 2. Quiet fresh start must NOT block, then exercise wizard steps + persistence ----
run(wizard
    "4 wizard state\n6 wizard open\n10 wizard step 2\n12 wizard look light\n14 wizard state\n16 wizard look dark\n18 wizard step 3\n20 wizard tone\n22 wizard step 4\n24 wizard state\n26 wizard close\n28 wizard state\n32 quit discard\n")
expectMatch(wizard "${OUT_wizard}" "wizard frame 4: open 0" "fresh hidden/quiet launch does not auto-open wizard")
expectMatch(wizard "${OUT_wizard}" "wizard frame 14: open 1 step 2/4 layout studio look light" "wizard step 2 switches theme live to light")
expectMatch(wizard "${OUT_wizard}" "wizard frame 24: open 1 step 4/4 layout studio look dark" "wizard steps cleanly to step 4 and restores dark theme")
expectMatch(wizard "${OUT_wizard}" "wizard frame 28: open 0 .* setupDone 1" "closing wizard marks setupDone=1")
expectMatch(wizard "${SET_wizard}" "\"ui.setupDone\": true" "ui.setupDone persisted in settings.json")
expectMatch(wizard "${SET_wizard}" "\"ui.layoutChosen\": true" "ui.layoutChosen persisted in settings.json")

# ---- 3. Guided tour inside real demo project ----
run(tour
    "4 tour start demo\n10 tour state\n12 tour next\n18 tour state\n20 tour next\n26 tour state\n28 tour back\n32 tour state\n34 tour stop\n38 tour state\n42 quit discard\n")
expectMatch(tour "${OUT_tour}" "tour frame 10: active 1 askEmpty 0 step 1/6 target transport" "tour starts at step 1/6 (transport)")
expectMatch(tour "${OUT_tour}" "tour frame 18: active 1 askEmpty 0 step 2/6 target browser" "tour next advances to step 2/6 (browser)")
expectMatch(tour "${OUT_tour}" "tour frame 26: active 1 askEmpty 0 step 3/6 target rack" "tour next advances to step 3/6 (channel rack)")
expectMatch(tour "${OUT_tour}" "tour frame 32: active 1 askEmpty 0 step 2/6 target browser" "tour back returns to step 2/6")
expectMatch(tour "${OUT_tour}" "tour frame 38: active 0" "tour stop exits cleanly")

# Verify live card & spotlight geometry stay strictly inside 1440x900
string(REGEX MATCH "tour frame 26:[^\n]*spot ([0-9]+),([0-9]+),([0-9]+),([0-9]+) card ([0-9]+),([0-9]+),([0-9]+),([0-9]+)" m "${OUT_tour}")
if(NOT m)
  message(FATAL_ERROR "FAIL (tour): could not parse spot/card rect from frame 26:\n${OUT_tour}")
endif()
set(sx ${CMAKE_MATCH_1})
set(sy ${CMAKE_MATCH_2})
set(sw ${CMAKE_MATCH_3})
set(sh ${CMAKE_MATCH_4})
set(cx ${CMAKE_MATCH_5})
set(cy ${CMAKE_MATCH_6})
set(cw ${CMAKE_MATCH_7})
set(ch ${CMAKE_MATCH_8})
math(EXPR cx1 "${cx} + ${cw}")
math(EXPR cy1 "${cy} + ${ch}")
if(sw LESS 100 OR sh LESS 100)
  message(FATAL_ERROR "FAIL (tour): spotlight rect too small: ${sw}x${sh}")
endif()
if(cx LESS 0 OR cy LESS 0 OR cx1 GREATER 1440 OR cy1 GREATER 900)
  message(FATAL_ERROR "FAIL (tour): coachmark card clipped outside 1440x900: ${cx},${cy}..${cx1},${cy1}")
endif()
message(STATUS "ok - tour: live spotlight (${sx},${sy} ${sw}x${sh}) and card (${cx},${cy} ${cw}x${ch}) inside 1440x900")

# ---- 4. Help menu exposes Setup wizard... & Guided tour... in every layout ----
foreach(L IN ITEMS studio live logic tracker)
  set(cfg "${OUT}/cfg_help_${L}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.setupDone\": true, \"ui.layout\": \"${L}\", \"keys.profile\": \"${L}\", \"ui.menuStyle\": \"${L}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900 --frames 12 --menu help
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (help_${L}): gridline-app failed (${rc}):\n${err}")
  endif()
  expectMatch("help_${L}" "${out}" "Setup wizard\\.\\.\\." "Help menu has Setup wizard... in ${L}")
  expectMatch("help_${L}" "${out}" "Guided tour\\.\\.\\." "Help menu has Guided tour... in ${L}")
endforeach()

message(STATUS "k105_setup_tour_app: OK")
