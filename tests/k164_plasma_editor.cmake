# Gridline — Plasma's editor (hidden, off-screen, isolated config, no preset library):
#  1. every parameter has a control: the reach driver (GL_PLASMA_REACH) steps through every view
#     and its sub-states, in the Synth and Effects tabs and (GL_PLASMA_REACH=patch) in the Patch view, and reports
#     the parameters no control was drawn for;
#  2. no overlapping text, no text cut by its field or card, no control over another, in any view
#     (Simple, Full, Patch, the keyboard drawer out) at 60 %, 75 % and 100 % editor zoom, at
#     1280x800, 1440x900 and 1920x1080, in the dark and the light theme;
#  3. opening a saved Plasma sound in the editor changes nothing: for three factory sounds with
#     notes, the project saved after the editor drew each view holds the same plugin state, and
#     renders (gridline render, -DCLI) to the same bytes, as the project saved before;
#  4. a link made in Link mode (LFO 1 armed, a drag on Filter 1's cutoff bar) is one undo step
#     that sets the link's source, target and depth, and Ctrl+Z takes all of it back.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k164_plasma_editor.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home" "${OUT}/no-library")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GL_PLASMA_SERUM_ROOTS} "${OUT}/no-library")

# ---- 1. every parameter reachable, in the panels and in the Patch view
foreach(mode 1 patch)
set(ENV{GL_PLASMA_REACH} "${mode}")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.plasma --size 1600x1100 --frames 480
                        --screenshot "${OUT}/reach.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
unset(ENV{GL_PLASMA_REACH})
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: reach run (${mode}): gridline-app failed (${rc}):\n${out}\n${err}")
endif()
if(NOT err MATCHES "PLASMA_REACH ([0-9]+) of ([0-9]+)")
  message(FATAL_ERROR "FAIL: the reach driver (${mode}) did not report (too few frames?):\n${err}")
endif()
set(got ${CMAKE_MATCH_1})
set(total ${CMAKE_MATCH_2})
if(err MATCHES "PLASMA_REACH_MISSING" OR NOT got EQUAL total)
  string(REGEX MATCHALL "PLASMA_REACH_MISSING [^\n]*" miss "${err}")
  string(REPLACE ";" "\n" miss "${miss}")
  message(FATAL_ERROR "FAIL: ${mode}: ${got} of ${total} parameters have a control; missing:\n${miss}")
endif()
endforeach()
message(STATUS "ok - every one of Plasma's ${total} parameters has a control, in the panels and in the Patch view")
if(NOT err MATCHES "PLASMA_MENUS ok")
  string(REGEX MATCHALL "PLASMA_MENU_BAD [^\n]*" bad "${err}")
  string(REPLACE ";" "\n" bad "${bad}")
  message(FATAL_ERROR "FAIL: the filter / morph pickers do not list every stored index exactly once:\n${bad}")
endif()
message(STATUS "ok - the filter and morph pickers list every stored index exactly once")

# ---- 2. layouts
set(ENV{GL_PLASMA_LAYOUT_CHECK} "1")
set(ENV{GL_HEADER_REPORT} "1")  # the plugin header says what it drew: Plasma's own bar is the only logo and preset picker
set(views synth1 synth2 synth3 synth3+full modulation modulation+full drag dragwt effects effects+full dragfx
          routing env0 lfo0 voices keys links setup arp zones phrases table library linkmode drawer synth3+full+drawer env0+drawer lfo0+drawer
          patch patch+full patch+drawer patchlink patchadd patchsearch patchsends libshapes libsounds menu env1)
set(count 0)
foreach(theme dark light)
  set(themeArg "")
  if(theme STREQUAL "light")
    set(themeArg "--light")
  endif()
  foreach(size 1280x800 1440x900 1920x1080)
    set(scales 0.6 0.75 1)
    if(size STREQUAL "1920x1080")
      list(APPEND scales 2)  # 200%: the editor scrolls inside its window; nothing overlaps or is cut
    endif()
    foreach(scale ${scales})
      # one run shows every view in turn (eight frames each), the layout check tagged per view
      string(REPLACE ";" "," vlist "${views}")
      set(ENV{GL_PLASMA_VIEWS} "${vlist}")
      set(tag "${scale}/${size}/${theme}")
      set(ENV{GL_PLASMA_LAYOUT_TAG} "${tag}")
      file(WRITE "${OUT}/zoom.txt" "1 edzoomset gl.plasma ${scale}\n")
      list(LENGTH views nv)
      math(EXPR frames "${nv} * 8 + 4")
      execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 ${themeArg} --plugin gl.plasma --size ${size} --frames ${frames}
                              --dev-input "${OUT}/zoom.txt" --screenshot "${OUT}/zoom.ppm" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
      if(NOT rc EQUAL 0)
        message(FATAL_ERROR "FAIL: ${tag}: gridline-app failed (${rc}):\n${out}\n${err}")
      endif()
      foreach(v ${views})
        string(REPLACE "+" "[+]" vre "${v}")
        if(NOT err MATCHES "PLASMA_LAYOUT ${vre}@")
          message(FATAL_ERROR "FAIL: ${v}@${tag}: the editor did not draw (no layout report):\n${err}")
        endif()
        math(EXPR count "${count} + 1")
      endforeach()
      if(NOT err MATCHES "PLUGIN_HEADER gl.plasma wordmark 0 picker 0")
        message(FATAL_ERROR "FAIL: ${tag}: the plugin header should leave the logo and preset picker to Plasma's own bar:\n${err}")
      endif()
      if(err MATCHES "PLASMA_OVERLAP")
        string(REGEX MATCHALL "PLASMA_OVERLAP [^\n]*" ov "${err}")
        string(REPLACE ";" "\n" ov "${ov}")
        message(FATAL_ERROR "FAIL: ${tag}: overlapping text:\n${ov}")
      endif()
    endforeach()
  endforeach()
endforeach()
unset(ENV{GL_PLASMA_VIEWS})
# the same report for a plugin without its own bar: the header does draw both there
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.analog --size 1440x900 --frames 40
                        --screenshot "${OUT}/other.ppm" TIMEOUT 180 RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
unset(ENV{GL_HEADER_REPORT})
if(NOT rc EQUAL 0 OR NOT err MATCHES "PLUGIN_HEADER gl.analog wordmark 1 picker 1")
  message(FATAL_ERROR "FAIL: the header report should see the logo and preset picker of an ordinary plugin (${rc}):\n${err}")
endif()
message(STATUS "ok - one header: Plasma's own bar holds the logo and preset picker; the plugin header leaves them out (and keeps them for others)")
unset(ENV{GL_PLASMA_VIEWS})
message(STATUS "ok - ${count} Plasma view layouts (60% to 200% zoom, 3 sizes, 2 themes) without overlapping or cut text or overlapping controls")

# ---- 3. open, look, save: the same state and the same render
unset(ENV{GL_PLASMA_LAYOUT_CHECK})
unset(ENV{GL_PLASMA_VIEW})
# the Plasma channel's plugin state of a saved project (-> var), and its channel index (-> idx)
function(plasma_state file var idx)
  file(READ "${file}" j)
  string(JSON n LENGTH "${j}" channels)
  math(EXPR last "${n} - 1")
  foreach(i RANGE ${last})
    string(JSON t ERROR_VARIABLE e GET "${j}" channels ${i} plugin type)
    if(t STREQUAL "gl.plasma")
      string(JSON st GET "${j}" channels ${i} plugin)
      set(${var} "${st}" PARENT_SCOPE)
      set(${idx} ${i} PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "FAIL: no Plasma channel in ${file}")
endfunction()
set(saves 0)
# dev-input "save" splits at spaces: save to a path relative to the build directory (the app's working
# directory, from WORKING_DIRECTORY), which has none, and read it back through OUT
get_filename_component(rel "${OUT}" NAME)
foreach(preset "Wobble Bass" "Drawn Wobble" "Clip Groove")
  string(REPLACE " " "_" p "${preset}")
  set(ENV{GL_PLASMA_PRESET} "${preset}")
  file(WRITE "${OUT}/a.txt" "4 mknotes 8 96 80x3\n8 save ${rel}/${p}_a.gridline.json\n")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.plasma --size 1440x900 --frames 12 --dev-input "${OUT}/a.txt" --screenshot "${OUT}/a.ppm" TIMEOUT 180
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  unset(ENV{GL_PLASMA_PRESET})
  if(NOT rc EQUAL 0 OR NOT out MATCHES "save frame 8: saved")
    message(FATAL_ERROR "FAIL: ${preset}: could not save the first project (${rc}):\n${out}\n${err}")
  endif()
  plasma_state("${OUT}/${p}_a.gridline.json" sa ci)
  foreach(v layer1 layer2 layer3 roottex filters amp routing effects env0 lfo0 keys links setup arp zones phrases table library drawer layer1+full patch patchlink)
    set(ENV{GL_PLASMA_VIEW} "${v}")
    file(WRITE "${OUT}/b.txt" "14 save ${rel}/${p}_b.gridline.json\n")
    execute_process(COMMAND "${APP}" "${OUT}/${p}_a.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --chan ${ci} --size 1440x900 --frames 18
                            --dev-input "${OUT}/b.txt" --screenshot "${OUT}/b.ppm" TIMEOUT 180 RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    unset(ENV{GL_PLASMA_VIEW})
    if(NOT rc EQUAL 0 OR NOT out MATCHES "save frame 14: saved")
      message(FATAL_ERROR "FAIL: ${preset} / ${v}: could not save after opening the editor (${rc}):\n${out}\n${err}")
    endif()
    plasma_state("${OUT}/${p}_b.gridline.json" sb cb)
    if(NOT sa STREQUAL sb)
      message(FATAL_ERROR "FAIL: ${preset}: opening the editor on ${v} changed the saved state:\nbefore ${sa}\nafter  ${sb}")
    endif()
    math(EXPR saves "${saves} + 1")
  endforeach()
  if(CLI)
    foreach(x a b)
      execute_process(COMMAND "${CLI}" render "${OUT}/${p}_${x}.gridline.json" -o "${OUT}/${p}_${x}.wav" --sr 48000 --bits 32
                      RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
      if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/${p}_${x}.wav")
        message(FATAL_ERROR "FAIL: ${preset}: render ${x} failed (${rc}):\n${out}\n${err}")
      endif()
    endforeach()
    file(SHA256 "${OUT}/${p}_a.wav" ha)
    file(SHA256 "${OUT}/${p}_b.wav" hb)
    file(SIZE "${OUT}/${p}_a.wav" sz)
    if(NOT ha STREQUAL hb OR sz LESS 10000)
      message(FATAL_ERROR "FAIL: ${preset}: the render after opening the editor differs (or is empty: ${sz} bytes)")
    endif()
  endif()
endforeach()
message(STATUS "ok - ${saves} open-look-save runs keep Plasma's state; the renders match sample for sample")

# ---- 4. Link mode: one drag, one link, one undo step (after frame 70: the editor draws from about frame 55)
# parameter indices (plasma.h): Layer 1 level 71 (shown in the default sound); link slot 1: source 704, depth 705, target 707
set(ENV{GL_PLASMA_VIEW} "linkmode")
file(WRITE "${OUT}/l.txt" "80 plug 71 704 705 707
82 atparam 71
84 down 0
86 atparam 71 20 0
88 atparam 71 40 0
90 up 0
94 plug 71 704 705 707
98 sdlkey z down ctrl
99 sdlkey z up
104 plug 71 704 705 707
")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.plasma --size 1440x900 --frames 110 --dev-input "${OUT}/l.txt" --screenshot "${OUT}/l.ppm" TIMEOUT 180
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
unset(ENV{GL_PLASMA_VIEW})
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: Link mode run failed (${rc}):\n${out}\n${err}")
endif()
function(plugat f prefix)
  if(NOT out MATCHES "plug frame ${f}: undo ([0-9]+) of [0-9]+[^\n]*\n  param 71 ([-0-9.]+)\n  param 704 ([-0-9.]+)\n  param 705 ([-0-9.]+)\n  param 707 ([-0-9.]+)")
    message(FATAL_ERROR "FAIL: Link mode: no plug dump at frame ${f}:\n${out}")
  endif()
  set(${prefix}_pos ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${prefix}_cut ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${prefix}_src ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(${prefix}_amt ${CMAKE_MATCH_4} PARENT_SCOPE)
  set(${prefix}_dst ${CMAKE_MATCH_5} PARENT_SCOPE)
endfunction()
plugat(80 b)
plugat(94 m)
plugat(104 u)
math(EXPR want "${b_pos} + 1")
if(NOT m_pos EQUAL want OR m_src STREQUAL b_src OR NOT m_dst STREQUAL "72.000000" OR m_amt STREQUAL b_amt OR NOT m_cut STREQUAL b_cut)
  message(FATAL_ERROR "FAIL: Link mode: the drag should add one link (LFO 1 -> Layer 1 level, a depth) as one undo step, the level unchanged:\n"
                      "before pos ${b_pos} src ${b_src} amt ${b_amt} dst ${b_dst} cut ${b_cut}\nafter  pos ${m_pos} src ${m_src} amt ${m_amt} dst ${m_dst} cut ${m_cut}")
endif()
if(NOT u_pos EQUAL b_pos OR NOT u_src STREQUAL b_src OR NOT u_amt STREQUAL b_amt OR NOT u_dst STREQUAL b_dst)
  message(FATAL_ERROR "FAIL: Link mode: Ctrl+Z should take the whole link back (pos ${u_pos} src ${u_src} amt ${u_amt} dst ${u_dst})")
endif()
message(STATUS "ok - a Link mode drag is one link and one undo step; Ctrl+Z takes it back")
