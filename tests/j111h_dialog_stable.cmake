# Gridline - J-111h: windows keep their size once open. A section (Widgets BeginSection) reserved one
# pixel less than the width it was given, so a window that sizes itself to its content
# (AlwaysAutoResize, e.g. the old "Make this project collaborative" name window, Project info) lost a
# pixel of width every frame until it reached its minimum: it visibly shrank for a second or more
# after opening. Here, in a hidden window at a large and a small main-window size:
#   1. the Collaboration panel with "Share this project" (the name field focused),
#   2. a collaborative project: renaming in the panel's header,
#   3. Project info with the Shared name row being renamed,
#   4. a window that sizes itself to a full-width section ("collab section-probe", as the old name
#      window did: it shrank from 520 to 440 px, a pixel a frame),
# and for each, every window's position and size and the main window's size over 60 frames after it
# settled: none may change.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j111h_dialog_stable.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

# The "windows" dumps of <log> from frame <from> on: each window keeps one position and size, the
# main window one size; at least <minWindows> windows named <expect> were seen.
function(expectStable tag log from expect)
  file(STRINGS "${log}" lines)
  set(frame -1)
  set(names "")
  set(seen 0)
  set(mainSize "")
  foreach(l IN LISTS lines)
    if(l MATCHES "^windows frame ([0-9]+): main ([0-9]+x[0-9]+) viewport")
      set(frame ${CMAKE_MATCH_1})
      if(frame GREATER_EQUAL from)
        if(mainSize STREQUAL "")
          set(mainSize "${CMAKE_MATCH_2}")
        elseif(NOT mainSize STREQUAL "${CMAKE_MATCH_2}")
          message(FATAL_ERROR "FAIL (${tag}): the main window changed size: ${mainSize} -> ${CMAKE_MATCH_2} (frame ${frame})")
        endif()
      endif()
    elseif(frame GREATER_EQUAL from AND l MATCHES "^  window \"([^\"]*)\" pos (.*)$")
      set(n "${CMAKE_MATCH_1}")
      set(geo "${CMAKE_MATCH_2}")
      string(MAKE_C_IDENTIFIER "${n}" key)
      if(n STREQUAL "${expect}")
        math(EXPR seen "${seen} + 1")
      endif()
      if(NOT DEFINED geo_${key})
        set(geo_${key} "${geo}")
        list(APPEND names "${n}")
      elseif(NOT geo_${key} STREQUAL "${geo}")
        message(FATAL_ERROR "FAIL (${tag}): window '${n}' changed from ${geo_${key}} to ${geo} (frame ${frame})")
      endif()
    endif()
  endforeach()
  if(seen LESS 50)
    message(FATAL_ERROR "FAIL (${tag}): '${expect}' was seen in ${seen} frames only")
  endif()
  string(MAKE_C_IDENTIFIER "${expect}" key)
  message(STATUS "${tag}: main ${mainSize}, ${expect} ${geo_${key}}; stable: ${names}")
endfunction()

foreach(size 2200x1300 1000x640)
  set(D "${OUT}/${size}")
  file(MAKE_DIRECTORY "${D}/work")
  file(WRITE "${D}/script.txt" "5 collab profile Alex
10 collab name-window
11 collab share-folder \"${D}/work\"
40 windows 60
110 collab share-name \"Stable\"
111 collab share
130 collab rename-window
160 windows 60
230 collab rename-type \"Stable: take 2\" folder
260 windows 60
330 collab project-info rename
360 windows 60
430 collab section-probe
460 windows 60
")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfg XDG_CONFIG_HOME=${D}/cfg
            "${APP}" --hidden --no-ui-sounds --size ${size} --ui-scale 1 --frames 530 --screenshot "${D}/shot.ppm" --dev-input "${D}/script.txt"
    RESULT_VARIABLE rc OUTPUT_FILE "${D}/out.txt" ERROR_FILE "${D}/err.txt" TIMEOUT 240)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (${size}): exit code ${rc}")
  endif()
  # (one log per phase: the frames of each "windows" run)
  file(STRINGS "${D}/out.txt" all)
  foreach(phase share:40:100 rename:160:220 options:260:320 info:360:420 probe:460:520)
    string(REPLACE ":" ";" p "${phase}")
    list(GET p 0 name)
    list(GET p 1 from)
    list(GET p 2 to)
    set(keep OFF)
    set(text "")
    foreach(l IN LISTS all)
      if(l MATCHES "^windows frame ([0-9]+):")
        if(CMAKE_MATCH_1 GREATER_EQUAL from AND CMAKE_MATCH_1 LESS to)
          set(keep ON)
        else()
          set(keep OFF)
        endif()
      elseif(NOT l MATCHES "^  window ")
        continue()
      endif()
      if(keep)
        string(APPEND text "${l}\n")
      endif()
    endforeach()
    file(WRITE "${D}/${name}.txt" "${text}")
    if(name STREQUAL "info")
      set(expect "Project info")
    elseif(name STREQUAL "probe")
      set(expect "Section probe")
    else()
      set(expect "Collaboration")
    endif()
    expectStable("${size} ${name}" "${D}/${name}.txt" ${from} "${expect}")
  endforeach()
endforeach()
message(STATUS "ok - no window changes size after it opened (share, rename, Project info, a self-sizing section), large and small main window")
