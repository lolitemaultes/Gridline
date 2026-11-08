# Gridline - K-119: files the system asks Gridline to open (a double click in the file manager).
# macOS sends an open request (SDL: a drop with no window); Linux and Windows pass the file as
# the argument. Hidden runs, isolated config, paths with spaces and accented letters:
#   1. started by an open request: the project opens (not the start-up project), once;
#   2. started with the file as the argument: it opens, and is not opened a second time when
#      AppKit repeats it as an open request;
#   3. the running Gridline: a request opens the project in place of the open one, a request
#      for a tracker module converts and opens it, and with unsaved changes it asks first.
#   cmake -DAPP=<gridline-app> -DDEMO=<project.gridline.json> -DPY=<python3> -DOUT=<dir> -P k119_os_open.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(A "${OUT}/Song A.gridline.json")
set(B "${OUT}/Été mix 2.gridline.json")
set(M "${OUT}/Módulo tést.mod")
configure_file("${DEMO}" "${A}" COPYONLY)
configure_file("${DEMO}" "${B}" COPYONLY)
# A four channel module: one sample, one note (the tracker tests' layout).
execute_process(COMMAND "${PY}" -c [=[
import sys
b = bytearray(1084)
b[0:8] = b"k119 mod"
b[20 + 23] = 200; b[20 + 25] = 48; b[20 + 27] = 50; b[20 + 29] = 100
b[950] = 1; b[951] = 127
b[1080:1084] = b"M.K."
pat = bytearray(64 * 4 * 4)
pat[0:4] = bytes([0x01, 0xAC, 0x1C, 0x20])
b += pat + bytes(i & 0x7F for i in range(400))
open(sys.argv[1], "wb").write(b)
]=] "${M}" RESULT_VARIABLE prc)
if(NOT prc EQUAL 0)
  message(FATAL_ERROR "FAIL: could not write the test module")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(common --hidden --no-ui-sounds --no-update-check --size 1280x800 --ui-scale 1)

function(expectMatch tag text re)
  string(REGEX MATCH "${re}" got "${text}")
  if(NOT got)
    message(FATAL_ERROR "FAIL (${tag}): no \"${re}\" in:\n${text}")
  endif()
  message(STATUS "ok - ${tag}")
endfunction()
function(countOf text re outVar)
  string(REGEX MATCHALL "${re}" all "${text}")
  list(LENGTH all n)
  set(${outVar} ${n} PARENT_SCOPE)
endfunction()

# 1. Started by an open request.
file(WRITE "${OUT}/1.txt" "12 launcher\n16 quit discard\n")
execute_process(COMMAND "${APP}" ${common} --dev-os-open "${B}" --dev-input "${OUT}/1.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/1.log" "${out}\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (1): gridline-app exited with ${rc}:\n${out}\n${err}")
endif()
expectMatch("1: the request is taken while starting" "${out}" "os open: [^\n]*Été mix 2\\.gridline\\.json \\(starting\\)")
expectMatch("1: the requested project is open" "${out}" "launcher visible=0 open=1 [^\n]*prompt=0 modified=0 project=Été mix 2\\.gridline\\.json")
countOf("${out}" "os open: " n)
if(NOT n EQUAL 1)
  message(FATAL_ERROR "FAIL (1): ${n} open requests handled, expected 1:\n${out}")
endif()

# 2. The file as the argument (Linux and Windows start Gridline so). On macOS AppKit also
#    repeats it as an open request: it must not open a second time.
file(WRITE "${OUT}/2.txt" "12 launcher\n16 quit discard\n")
execute_process(COMMAND "${APP}" "${B}" ${common} --dev-input "${OUT}/2.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/2.log" "${out}\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (2): gridline-app exited with ${rc}:\n${out}\n${err}")
endif()
expectMatch("2: the argument's project is open" "${out}" "launcher visible=0 open=1 [^\n]*project=Été mix 2\\.gridline\\.json")
string(FIND "${out}" "os open:" again)
if(NOT again EQUAL -1)
  message(FATAL_ERROR "FAIL (2): the command line's file was opened a second time as a request:\n${out}")
endif()
message(STATUS "ok - 2: the command line's file opens once")

# 3. The running Gridline.
file(WRITE "${OUT}/3.txt"
  "8 launcher\n"
  "10 osopen \"${B}\"\n"
  "16 launcher\n"
  "18 osopen \"${M}\"\n"
  "40 launcher\n"
  "42 osopen \"${A}\"\n"
  "48 launcher\n"
  "50 mknotes 4 96 48\n"
  "52 osopen \"${B}\"\n"
  "58 launcher\n"
  "62 quit discard\n")
execute_process(COMMAND "${APP}" "${A}" ${common} --dev-input "${OUT}/3.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/3.log" "${out}\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (3): gridline-app exited with ${rc}:\n${out}\n${err}")
endif()
string(REGEX MATCHALL "launcher visible=[^\n]*" reps "${out}")
list(LENGTH reps nrep)
if(NOT nrep EQUAL 5)
  message(FATAL_ERROR "FAIL (3): ${nrep} launcher reports, expected 5:\n${out}")
endif()
list(GET reps 0 r0)
list(GET reps 1 r1)
list(GET reps 2 r2)
list(GET reps 3 r3)
list(GET reps 4 r4)
expectMatch("3: started with Song A" "${r0}" "project=Song A\\.gridline\\.json")
expectMatch("3: a request opens the other project in its place" "${r1}" "prompt=0 modified=0 project=Été mix 2\\.gridline\\.json")
expectMatch("3: a request for a tracker module converts and opens it" "${r2}" "prompt=0 [^\n]*title=[^\n]*Módulo tést")
expectMatch("3: and back to a project" "${r3}" "prompt=0 modified=0 project=Song A\\.gridline\\.json")
expectMatch("3: with unsaved changes it asks first, the open project stays" "${r4}" "prompt=1 modified=1 project=Song A\\.gridline\\.json")
countOf("${out}" "os open: [^\n]*\\(running\\)" n)
if(NOT n EQUAL 4)
  message(FATAL_ERROR "FAIL (3): ${n} requests handled while running, expected 4:\n${out}")
endif()
message(STATUS "ok - 3: four requests handled while running")
