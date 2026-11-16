# Gridline - the assistant tools inside the app (MCP).
#   The app runs tool calls from its dev input ("mcp call <tool> <json>"). Checks:
#     - list_commands lists every row of the command palette (each row id of the palette dump);
#     - every window row of the palette is in the windows tool, and run_command opens a window;
#     - patch edits the open project and inspect reads it back;
#     - get_settings lists settings with their access; run_command refuses "new project".
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k281_mcp_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/cfg/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg/home")
file(WRITE "${OUT}/cfg/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true}}\n")
file(WRITE "${OUT}/input.txt"
     "5 toasts clear\n"
     "7 palette dump\n"
     "9 mcp call list_commands {'limit': 500}\n"
     "10 mcp call list_commands {'limit': 500, 'offset': 500}\n"
     "11 mcp call list_commands {'limit': 500, 'offset': 1000}\n"
     "12 mcp call windows {'close': 'Undo history'}\n"
     "13 mcp call run_command {'id': 'win:Undo history'}\n"
     "15 mcp call windows {}\n"
     "16 mcp call patch {'path': 'channel/1', 'set': {'name': 'Probe'}}\n"
     "17 mcp call inspect {'path': 'channel/1'}\n"
     "18 mcp call get_settings {'prefix': 'mcp.', 'limit': 50}\n"
     "19 mcp call run_command {'id': 'cmd:file.new'}\n"
     "21 quit discard\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --dev-input "${OUT}/input.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
file(WRITE "${OUT}/app.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app failed (${rc}), see ${OUT}/app.log")
endif()

# One reply line per call.
macro(reply tool n var)
  string(REGEX MATCHALL "MCP RESULT ${tool} [^\n]*" _all "${out}")
  list(LENGTH _all _len)
  if(_len LESS_EQUAL ${n})
    message(SEND_ERROR "FAIL: no reply ${n} from ${tool}")
    set(${var} "")
  else()
    list(GET _all ${n} ${var})
  endif()
endmacro()

# Every palette row is listed.
string(REGEX MATCHALL "ROW\t[^\n]*" rows "${out}")
list(LENGTH rows nrows)
reply(list_commands 0 lc)
reply(list_commands 1 lc2)
reply(list_commands 2 lc3)  # (three pages: more than 1000 commands since the 0.7.7 merges)
string(REGEX MATCH "\"total\": *([0-9]+)" m "${lc}")
if(NOT m OR CMAKE_MATCH_1 LESS nrows OR nrows LESS 100 OR CMAKE_MATCH_1 GREATER 1500)
  message(SEND_ERROR "FAIL: list_commands total (${CMAKE_MATCH_1}) against the palette's ${nrows} rows")
endif()
set(listed "${lc}${lc2}${lc3}")
set(missing 0)
foreach(r IN LISTS rows)
  string(REGEX REPLACE "^ROW\t[^\t]*\t([^\t]*)\t.*" "\\1" id "${r}")
  string(FIND "${listed}" "\"id\":\"${id}\"" at)
  if(at EQUAL -1)
    string(FIND "${listed}" "\"id\": \"${id}\"" at)
  endif()
  if(at EQUAL -1)
    math(EXPR missing "${missing} + 1")
    if(missing LESS 6)
      message(SEND_ERROR "FAIL: palette row not listed by list_commands: ${id}")
    endif()
  endif()
endforeach()

# Every window row is a window of the windows tool, and run_command opened one.
reply(windows 1 win)
string(REGEX MATCHALL "ROW\twindow\twin:[^\t]*" wrows "${out}")
set(nwin 0)
foreach(r IN LISTS wrows)
  string(REGEX REPLACE "^ROW\twindow\twin:" "" name "${r}")
  if(name MATCHES "^(chan|fx):")
    continue()  # a channel's or an effect's own window: opened through run_command
  endif()
  math(EXPR nwin "${nwin} + 1")
  string(FIND "${win}" "\"name\":\"${name}\"" at)
  if(at EQUAL -1)
    string(FIND "${win}" "\"name\": \"${name}\"" at)
  endif()
  if(at EQUAL -1)
    message(SEND_ERROR "FAIL: window row not in the windows tool: ${name}")
  endif()
endforeach()
if(nwin LESS 10)
  message(SEND_ERROR "FAIL: only ${nwin} window rows")
endif()
if(NOT win MATCHES "\"name\": *\"Undo history\", *\"open\": *true")
  message(SEND_ERROR "FAIL: run_command did not open the undo history: ${win}")
endif()

reply(patch 0 p)
if(p MATCHES "\"isError\": *true" OR p STREQUAL "")
  message(SEND_ERROR "FAIL: patch failed: ${p}")
endif()
reply(inspect 0 i)
if(NOT i MATCHES "\"name\": *\"Probe\"")
  message(SEND_ERROR "FAIL: inspect does not show the patched name: ${i}")
endif()
reply(get_settings 0 g)
if(NOT g MATCHES "\"key\": *\"mcp.allowEdits\"" OR NOT g MATCHES "user only")
  message(SEND_ERROR "FAIL: get_settings: ${g}")
endif()
reply(run_command 1 n)
if(NOT n MATCHES "\"isError\": *true" OR NOT n MATCHES "ask the user")
  message(SEND_ERROR "FAIL: run_command did not refuse a new project: ${n}")
endif()
message(STATUS "${nrows} palette rows, ${nwin} windows")
