# Gridline - K-7: an installed plugin inside a Junction, in the real app (hidden, isolated config,
# off-screen, SDL's dummy audio driver) with the in-tree test plugin:
#   1. its own interface opens from the Junction, in a window of its own;
#   2. an edit in that interface (the plugin's "edit in my editor" hook) becomes an edit of the
#      song, as one undo step, and the plugin keeps its instance;
#   3. undo restores the stored value and the plugin itself follows it; redo too.
# Where the test plugin has no interface of its own (Linux, Windows) part 1 is skipped.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DCLAP=<bundle> -DOUT=<dir> -P k7_junction_hosted_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}")
set(ENV{HOME} "${OUT}/home")
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
set(type "clap:org.gridline.test.synth")
set(script "10 junction scan clap ${CLAP}
14 junction add ${type}
24 noteon 0 60
30 junction open last
36 plugwin info
38 junction value last 0
44 junction cc 20 127
60 junction value last 0
70 undo
84 junction value last 0
90 redo
104 junction value last 0
")
file(WRITE "${OUT}/script.txt" "${script}")
execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames 120
                        --screenshot "${OUT}/shot.ppm" --dev-input "${OUT}/script.txt" --plugin gl.patcher
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 100)
file(WRITE "${OUT}/out.txt" "${out}\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${out}")
endif()
if(NOT out MATCHES "junction scan ok 1")
  message(FATAL_ERROR "FAIL: the test plugin was not registered\n${out}")
endif()
if(NOT out MATCHES "junction added ([0-9]+)" OR CMAKE_MATCH_1 EQUAL 0)
  message(FATAL_ERROR "FAIL: the module was not added\n${out}")
endif()
if(NOT out MATCHES "junction open last: ([01]) shown ([01]) instance ([01])")
  message(FATAL_ERROR "FAIL: no result of opening\n${out}")
endif()
set(opened ${CMAKE_MATCH_1})
set(shown ${CMAKE_MATCH_2})
if(NOT CMAKE_MATCH_3 EQUAL 1)
  message(FATAL_ERROR "FAIL: no instance of the plugin inside the Junction\n${out}")
endif()
if(APPLE)
  if(NOT opened EQUAL 1 OR NOT shown EQUAL 1)
    message(FATAL_ERROR "FAIL: its own interface did not open: ${opened} ${shown}\n${out}")
  endif()
  if(NOT out MATCHES "plugwin frame 36: 1 windows")
    message(FATAL_ERROR "FAIL: no plugin window listed\n${out}")
  endif()
  message(STATUS "ok - its own interface opens in a window of its own")
endif()
string(REGEX MATCHALL "junction value last 0 = [-0-9.]+ undo [0-9]+ instance [^\n]*" lines "${out}")
list(LENGTH lines n)
if(NOT n EQUAL 4)
  message(FATAL_ERROR "FAIL: ${n} value lines\n${out}")
endif()
function(val i pre)
  list(GET lines ${i} l)
  string(REGEX MATCH "= ([-0-9.]+) undo ([0-9]+) instance (.*)" _ "${l}")
  set(${pre}_v ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${pre}_u ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${pre}_i "${CMAKE_MATCH_3}" PARENT_SCOPE)
endfunction()
val(0 a)
val(1 b)
val(2 c)
val(3 d)
if(a_v STREQUAL b_v OR NOT b_v EQUAL 1)
  message(FATAL_ERROR "FAIL: the edit in its interface did not reach the song: ${a_v} -> ${b_v}\n${out}")
endif()
math(EXPR want "${a_u} + 1")
if(NOT b_u EQUAL want)
  message(FATAL_ERROR "FAIL: the edit is not one undo step: ${a_u} -> ${b_u}\n${out}")
endif()
message(STATUS "ok - an edit in its interface comes back as one undo step (${a_v} -> ${b_v})")
if(NOT c_v STREQUAL a_v)
  message(FATAL_ERROR "FAIL: undo left the stored value at ${c_v}, expected ${a_v}\n${out}")
endif()
if(a_i STREQUAL b_i)
  message(FATAL_ERROR "FAIL: the plugin's own state did not change with the edit\n${out}")
endif()
if(NOT c_i STREQUAL a_i)
  message(FATAL_ERROR "FAIL: undo did not reach the plugin\nbefore ${a_i}\nafter  ${c_i}\n${out}")
endif()
if(NOT d_v STREQUAL b_v OR NOT d_i STREQUAL b_i)
  message(FATAL_ERROR "FAIL: redo: ${d_v}, plugin\n${d_i}\nexpected\n${b_i}\n${out}")
endif()
message(STATUS "ok - undo and redo reach the plugin itself")
