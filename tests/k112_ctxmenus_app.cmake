# Gridline - K-112: the order of the track header and clip context menus, pinned per menu style.
#   Live orders these menus its own way (the object menu skeleton: edit rows, rename, freeze, insert,
#   content, look last); Studio keeps the order it had. For Studio and Live the app opens each menu
#   (--menu <name>, ContextMenus.cpp drawDevMenu: track, clip, patclip; Live also device: the device band's
#   title bar menu) and writes its rows as drawn
#   (MENUDUMP: one row per line, separators as dashed lines) and its whole tree (MENUTREE: every
#   submenu entered). Both must equal the committed text (tests/k112/ctxmenu_<style>_<menu>.txt), so a
#   change to Studio's order fails here, not only by eye.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DEXPECT=<dir> -DOUT=<dir> [-DRECORD=1] -P k112_ctxmenus_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

set(failures 0)
foreach(L studio live)
  foreach(M track clip patclip device knob)
    if(L STREQUAL "studio" AND M STREQUAL "device")
      continue()  # the device band (and its menu) is the Live layout's
    endif()
    set(cfg "${OUT}/cfg_${L}_${M}")
    file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
    file(WRITE "${cfg}/gridline/settings.json"
         "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${L}\", \"keys.profile\": \"${L}\", \"ui.menuStyle\": \"${L}\"}}\n")
    set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
    set(ENV{HOME} "${cfg}/home")
    configure_file("${DEMO}" "${cfg}/song.gridline.json" COPYONLY)
    execute_process(COMMAND "${APP}" "${cfg}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x2600 --frames 12 --menu ${M}
                            --screenshot "${cfg}/menu.ppm"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
    file(WRITE "${OUT}/${L}_${M}.log" "${out}\n---- stderr ----\n${err}")
    if(NOT rc EQUAL 0)
      message(SEND_ERROR "FAIL: ${L} ${M}: gridline-app failed (${rc})")
      math(EXPR failures "${failures} + 1")
      continue()
    endif()
    string(REGEX MATCH "MENUDUMP BEGIN ${M}\n(.*)\nMENUDUMP END" m1 "${out}")
    set(rows "${CMAKE_MATCH_1}")
    string(REGEX MATCH "MENUTREE BEGIN ${M}\n(.*)MENUTREE END" m2 "${out}")
    set(tree "${CMAKE_MATCH_1}")
    if(NOT m1 OR NOT m2)
      message(SEND_ERROR "FAIL: ${L} ${M}: no menu dump")
      math(EXPR failures "${failures} + 1")
      continue()
    endif()
    # The tree two levels deep: each submenu's own rows (deeper lists, such as every instrument, change
    # with the catalogue and are not this menu's order).
    string(REGEX REPLACE "\n$" "" tree "${tree}")
    string(REPLACE "\n" ";" lines "${tree}")
    set(tree "")
    foreach(l IN LISTS lines)
      if(NOT l MATCHES " > .* > ")
        string(APPEND tree "${l}\n")
      endif()
    endforeach()
    set(got "${rows}\n==== tree ====\n${tree}")
    file(WRITE "${OUT}/ctxmenu_${L}_${M}.txt" "${got}")
    if(RECORD)
      file(WRITE "${EXPECT}/ctxmenu_${L}_${M}.txt" "${got}")
    elseif(NOT EXISTS "${EXPECT}/ctxmenu_${L}_${M}.txt")
      message(SEND_ERROR "FAIL: ${L} ${M}: no committed menu (${EXPECT}/ctxmenu_${L}_${M}.txt)")
      math(EXPR failures "${failures} + 1")
    else()
      file(READ "${EXPECT}/ctxmenu_${L}_${M}.txt" want)
      if(NOT got STREQUAL want)
        message(SEND_ERROR "FAIL: ${L} ${M}: the menu changed (diff ${EXPECT}/ctxmenu_${L}_${M}.txt ${OUT}/ctxmenu_${L}_${M}.txt)")
        math(EXPR failures "${failures} + 1")
      endif()
    endif()
  endforeach()
endforeach()
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} context menu check(s) failed")
endif()
