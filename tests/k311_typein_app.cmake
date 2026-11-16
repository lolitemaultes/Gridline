# Gridline - K-311: the type-in menu on every plugin control, in the real app (hidden, isolated
# config and HOME). The Bass channel's editor (Tercet) is opened at 1920x1080 and placed at 600,100:
#   1. a right-click on a cell of oscillator 1's shape selector (a control with no menu of its
#      own) opens the shared menu for "Osc 1 shape" (typein: ... found 1);
#   2. a right-click on the filter type box (a shared control with its own menu) leaves that menu
#      alone (typein: release ... popup 1, no second menu).
# Screenshots: <OUT>/shape.ppm, <OUT>/filter.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k311_typein_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/hm" "${OUT}/cfg/gridline")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_TYPEIN_DEBUG} "1")
set(ENV{HOME} "${OUT}/hm")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
configure_file("${DEMO}" "${OUT}/fixture.gridline.json" COPYONLY)
file(WRITE "${OUT}/input.txt"
     "5 toasts clear\n6 closeplugins\n10 plugwin chan 5\n12 plugwin at 600 100\n40 move 708 315\n42 down 1\n44 up 1\n52 shot \"${OUT}/shape.ppm\"\n54 key escape\n"
     "60 move 852 740\n62 down 1\n64 up 1\n72 shot \"${OUT}/filter.ppm\"\n80 quit discard\n")
execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --layout-dev --no-ui-sounds --fixed-dt 16 --size 1920x1080
                        --dev-input "${OUT}/input.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
file(WRITE "${OUT}/out.txt" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${out}")
endif()
string(REGEX MATCHALL "typein: [^\n]*" lines "${out}")
list(LENGTH lines n)
if(n LESS 3)
  message(FATAL_ERROR "FAIL: expected the type-in trace of two right-clicks, got ${n} lines\n${out}")
endif()
list(GET lines 0 r1)
list(GET lines 1 f1)
list(GET lines 2 r2)
if(NOT r1 MATCHES "clicked 1 edited 0 popup 0 hovered-window 1" OR NOT f1 MATCHES "found 1$")
  message(FATAL_ERROR "FAIL: the shape selector cell offers its parameter (${r1} / ${f1})\n${out}")
endif()
message(STATUS "ok - a selector cell without a menu of its own gets the type-in menu")
if(NOT r2 MATCHES "popup 1")
  message(FATAL_ERROR "FAIL: the filter type box keeps its own menu (${r2})\n${out}")
endif()
if(n GREATER 3)
  message(FATAL_ERROR "FAIL: a second menu over the filter type box's own\n${out}")
endif()
message(STATUS "ok - a control with its own menu keeps it")
