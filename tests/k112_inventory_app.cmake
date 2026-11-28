# Gridline - K-112: every window of the homes table opens where its home says, in every layout.
#   For each layout (Studio, and the development shells of Live, Logic and Tracker) the app runs
#   "layout inventory": every row of the homes table is checked. A window is opened in turn (by its
#   flag or its own command) and checked where it lands (docked in its region, floating, a dialog,
#   hosted by a part, or in the status line); a plugin editor is opened on a new channel or slot and
#   its body must be drawn once, by its own window or by the Device chain where the home is Host;
#   bars must be drawn; menus, dialogs, overlays and rows inside other windows are listed with what
#   their home means. A layout home a later phase row builds is listed as planned with that row; only
#   the windows of later rows (and Sync review and Chat, which need a shared session) are skipped. No row may
#   fail, and every row of every layout is accounted for.
#   Also at 200% interface size in the Live shell (the regions keep their windows).
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112_inventory_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(failures 0)

function(inventory tag layout size scale)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true}}\n")
  file(WRITE "${cfg}/input.txt" "5 toasts clear\n10 layout inventory\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${OUT}/${tag}.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${OUT}/${tag}.gridline.json" --hidden --no-ui-sounds --size ${size} --ui-scale ${scale} --layout ${layout}
                          --dev-input "${cfg}/input.txt" --fixed-dt 16 --frames 2000 --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 600)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "FAIL: ${tag}: gridline-app failed (${rc})")
    return()
  endif()
  string(REGEX MATCHALL "inventory [^\n]* FAIL" bad "${out}")
  foreach(b IN LISTS bad)
    message(SEND_ERROR "FAIL: ${tag}: ${b}")
  endforeach()
  string(REGEX MATCH "inventory done layout ${layout} rows ([0-9]+) ok ([0-9]+) failed ([0-9]+) skipped ([0-9]+) planned ([0-9]+)" done "${out}")
  if(NOT done)
    message(SEND_ERROR "FAIL: ${tag}: the inventory did not finish")
    return()
  endif()
  set(rows ${CMAKE_MATCH_1})
  set(ok ${CMAKE_MATCH_2})
  set(failed ${CMAKE_MATCH_3})
  set(skipped ${CMAKE_MATCH_4})
  set(planned ${CMAKE_MATCH_5})
  math(EXPR counted "${ok} + ${failed} + ${skipped} + ${planned}")
  if(NOT failed EQUAL 0 OR NOT counted EQUAL rows OR rows LESS 325 OR skipped GREATER 4)
    message(SEND_ERROR "FAIL: ${tag}: rows ${rows} ok ${ok} failed ${failed} skipped ${skipped} planned ${planned} (every row checked, at most the 4 later windows skipped)")
  endif()
  string(REGEX MATCHALL "inventory [A-Z][0-9]+ [^\n]* skip [^\n]*" skips "${out}")
  foreach(k IN LISTS skips)
    if(NOT k MATCHES " (N6|N7|N12|D29|B15) ")  # (B15 Chat: shown only in a shared session, like Sync review)
      message(SEND_ERROR "FAIL: ${tag}: an unexpected skip: ${k}")
    endif()
  endforeach()
  message(STATUS "${tag}: rows ${rows} ok ${ok} failed ${failed} skipped ${skipped} planned ${planned}")
endfunction()

inventory(studio studio 1440x900 1)
inventory(live live 1440x900 1)
inventory(logic logic 1440x900 1)
inventory(tracker tracker 1440x900 1)
inventory(live_x2 live 2560x1600 2)
inventory(logic_small logic 1280x800 1)
