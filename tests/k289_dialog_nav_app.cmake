# Gridline - K-289: a change of folder in the Open / Save dialog takes the rest of the mouse gesture
# that made it, in the real app (hidden, isolated config and HOME, SDL's dummy audio driver; every
# folder it browses is made here):
#   1. a double-click on a folder opens it, and the second click's release (over the first row of the
#      folder entered) selects nothing there;
#   2. a single click after that still selects a file;
#   3. Enter on a selected folder still opens it;
#   4. a double-click on a folder held down for a while: nothing is selected when it is let go.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k289_dialog_nav_app.cmake
cmake_minimum_required(VERSION 3.18)
file(REMOVE_RECURSE "${OUT}")
set(H "${OUT}/home")
set(D "${OUT}/lib")
file(MAKE_DIRECTORY "${OUT}/cfg" "${H}" "${D}/a_dir" "${D}/b_dir")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${H}")
set(ENV{USERPROFILE} "${H}")
set(ENV{XDG_CONFIG_HOME} "${OUT}/cfg")
set(ENV{XDG_CACHE_HOME} "${OUT}/cache")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
file(READ "${DEMO}" demo)
# In lib: a_dir, b_dir, z (folders first); in a_dir: a, b. Projects, so the Open dialog lists them.
file(WRITE "${D}/z.gridline" "${demo}")
file(WRITE "${D}/a_dir/a.gridline" "${demo}")
file(WRITE "${D}/a_dir/b.gridline" "${demo}")
file(WRITE "${D}/b_dir/c.gridline" "${demo}")

function(run tag script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --size 1440x900 --fixed-dt 16 --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.out" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()
# One "filedialog frame <n>" report, as text.
function(report tag frame outvar)
  string(REGEX MATCH "filedialog frame ${frame} [^\n]*\n([^\n]*\n)*filedialog end" r "${${tag}_out}")
  string(REGEX REPLACE "filedialog end.*" "" r "${r}")
  if(r STREQUAL "")
    message(FATAL_ERROR "FAIL (${tag}): no dialog report at frame ${frame}\n${${tag}_out}")
  endif()
  set(${outvar} "${r}" PARENT_SCOPE)
endfunction()
# The folder and the selected row of a report.
function(where tag frame)
  report(${tag} ${frame} r)
  string(REGEX MATCH "  dir ([^\n]*)\n" _ "${r}")
  set(w_dir "${CMAKE_MATCH_1}" PARENT_SCOPE)
  string(REGEX MATCH "selected \"([^\"]*)\"" _ "${r}")
  set(w_sel "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
function(expect tag frame dir sel what)
  where(${tag} ${frame})
  if(NOT w_dir STREQUAL "${dir}" OR NOT w_sel STREQUAL "${sel}")
    report(${tag} ${frame} r)
    message(FATAL_ERROR "FAIL (${tag}): ${what}: folder ${w_dir}, selected '${w_sel}' (wanted ${dir}, '${sel}')\n${r}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

# Where the list is: the first row is below the 24 px header, rows are 24 px.
run(geo "3 filedialog open \"${D}\"
4 waitfor \"filedialog:${D}\" 60
8 filedialog state
10 quit discard
")
report(geo 8 r)
if(NOT r MATCHES "layout list (-?[0-9]+) (-?[0-9]+) (-?[0-9]+) (-?[0-9]+)")
  message(FATAL_ERROR "FAIL (geo): no list layout\n${r}")
endif()
math(EXPR x "${CMAKE_MATCH_1} + 80")
math(EXPR y "${CMAKE_MATCH_2} + 36")
if(NOT r MATCHES "  row dir a_dir [^\n]*\n  row dir b_dir [^\n]*\n  row file z.gridline")
  message(FATAL_ERROR "FAIL (geo): the rows are not a_dir, b_dir, z.gridline\n${r}")
endif()

# 1-3. A double-click on a_dir (the first row), let go over a.gridline (the first row inside).
run(dbl "3 filedialog open \"${D}\"
4 waitfor \"filedialog:${D}\" 60
6 move ${x} ${y}
8 down 0
9 up 0
10 down 0
16 up 0
20 filedialog state
22 down 0
23 up 0
26 filedialog state
28 filedialog up
32 filedialog select b_dir
34 key enter
40 filedialog state
44 quit discard
")
expect(dbl 20 "${D}/a_dir" "" "a double-click opens the folder and its release there selects nothing")
expect(dbl 26 "${D}/a_dir" "a.gridline" "a single click then selects the file")
expect(dbl 40 "${D}/b_dir" "" "Enter on a selected folder opens it")

# 4. The second press held over several frames before it is let go.
run(hold "3 filedialog open \"${D}\"
4 waitfor \"filedialog:${D}\" 60
6 move ${x} ${y}
8 down 0
9 up 0
10 down 0
14 move ${x} ${y}
30 up 0
34 filedialog state
36 quit discard
")
expect(hold 34 "${D}/a_dir" "" "a held double-click selects nothing when let go")
message(STATUS "k289: a change of folder in the file dialog takes the click that made it")
