# Gridline - K-172: a window's main action is a primary button (raised, filled), never the
# toggle-on look. Fails when any source still draws a button with the toggle look switched on
# for good (`TextButton(<label>, true ...)`): such a button looked pressed when it was only the
# default. Toggles pass their real on/off state, which this check leaves alone.
#   cmake -DSRC=<gridline/src> -P k172_primary_audit.cmake
cmake_minimum_required(VERSION 3.16)
file(GLOB_RECURSE files "${SRC}/app/*.cpp" "${SRC}/app/*.h" "${SRC}/packs/*.cpp" "${SRC}/packs/*.h" "${SRC}/plugin/*.cpp")
set(bad "")
set(nprimary 0)
foreach(f IN LISTS files)
  file(READ "${f}" text)
  # A literal `true` as the "active" argument: TextButton(<anything without a comma at depth 0>, true[,)]
  string(REGEX MATCHALL "TextButton\\(([^;,()]|\\([^;()]*\\))*,[ \n]*true[ \n]*[,)]" hits "${text}")
  foreach(h IN LISTS hits)
    list(APPEND bad "${f}: ${h}")
  endforeach()
  # Plugin skin buttons lit for good (a selector that cycles shows its value in the caption).
  string(REGEX MATCHALL "(skin|iskin|sk)::button\\([^;]*,[ ]*true[ ]*[,)]" sk "${text}")
  foreach(h IN LISTS sk)
    list(APPEND bad "${f}: ${h}")
  endforeach()
  # The same pattern in look drawing: a button face lit for good (look::On with no state).
  string(REGEX MATCHALL "look::button\\([^;]*,[ ]*look::On[ ]*,[^;]*\\)" lit "${text}")
  foreach(h IN LISTS lit)
    list(APPEND bad "${f}: ${h}")
  endforeach()
  string(REGEX MATCHALL "PrimaryButton\\(" p "${text}")
  list(LENGTH p n)
  math(EXPR nprimary "${nprimary} + ${n}")
endforeach()
list(LENGTH bad nbad)
if(nbad GREATER 0)
  string(REPLACE ";" "\n" bad "${bad}")
  message(FATAL_ERROR "FAIL: ${nbad} button(s) drawn lit for good (use ui::PrimaryButton for a main action):\n${bad}")
endif()
message(STATUS "ok - no button drawn with the toggle look for good; ${nprimary} primary buttons")
if(nprimary LESS 40)
  message(FATAL_ERROR "FAIL: only ${nprimary} primary buttons found (the dialogs' main actions should use them)")
endif()
