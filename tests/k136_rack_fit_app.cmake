# Gridline - K-136: the channel rack's height follows its content, in the real app (hidden,
# off-screen, isolated configs, SDL's dummy audio driver):
#   1. a floating rack of 5 channels asked to be 800 px tall is exactly as tall as its
#      header, rows and add button, at UI scale 100 % and 200 % (the same logical height);
#   2. the add button sits right under the last row, the window ends right under it;
#   3. a new channel raises the limit by one row (a rack at its full height grows with it);
#      deleting one lowers it and the window shrinks to fit;
#   4. a zipped row (a thin strip) and a piano-roll preview row are counted as drawn;
#   5. docked in a tall dock node the rack keeps the add button under the last row and
#      leaves the rest of the node empty.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k136_rack_fit_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag spec script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1920x1080 --frames 60 --rack ${spec}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

# geom(<tag> <n> <var>): fields of the n-th (0-based) rack dump's geometry line -> <var>_<field>
# (whole pixels: the fractions are dropped).
function(geom tag n var)
  string(REGEX MATCHALL "rack frame [^\n]*\n  geom [^\n]*" dumps "${${tag}_out}")
  list(LENGTH dumps count)
  if(count LESS_EQUAL n)
    message(FATAL_ERROR "FAIL (${tag}): no rack dump ${n}. Output:\n${${tag}_out}")
  endif()
  list(GET dumps ${n} d)
  foreach(key rows rowPitch docked winH fitH winBottom lastRowBottom rowsContent)
    string(REGEX MATCH " ${key} (-?[0-9]+)" _ "${d}")
    set(${var}_${key} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  endforeach()
  string(REGEX MATCH " addrow ([-0-9.]+) ([0-9]+)[.0-9]* ([-0-9.]+) ([0-9]+)" _ "${d}")
  set(${var}_addTop "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${var}_addBottom "${CMAKE_MATCH_4}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# under(<var> <what>): the add button sits right under the last row, the window ends under it.
function(under v what)
  math(EXPR gap "${${v}_addTop} - ${${v}_lastRowBottom}")
  math(EXPR tail "${${v}_winBottom} - ${${v}_addBottom}")
  check("${what}: add button ${gap} px under the last row, window ends ${tail} px under it"
        gap GREATER_EQUAL 0 AND gap LESS_EQUAL 20 AND tail GREATER_EQUAL 0 AND tail LESS_EQUAL 12)
endfunction()

# The demo song has 8 channels (the last one plays piano-roll notes: a preview row). Three
# deleted leave 5; the rack is then asked to be 800 px tall (--rack h800, frame 3).
set(five "1 rmchan 2\n2 rmchan 3\n3 rmchan 4\n")

# ---- 1 + 2. five channels, 100 % and 200 %
run(f100 h800 "${five}20 rack\n")
geom(f100 0 a)
check("5 channels: 5 rows drawn" a_rows EQUAL 5)
check("100 %: the rack asked for 800 px is as tall as its content (${a_winH} = ${a_fitH})" a_winH EQUAL a_fitH AND a_winH LESS 800)
under(a "100 %")
run(f200 h800 "${five}20 rack\n" --ui-scale 2)
geom(f200 0 b)
check("200 %: the same logical height as at 100 % (${b_winH} = ${b_fitH} = ${a_fitH})" b_winH EQUAL b_fitH AND b_fitH EQUAL a_fitH)
under(b "200 %")

# ---- 3. a channel added raises the limit (the full-height rack grows), one deleted lowers it
run(addrm h800 "${five}20 rack\n22 addvia rack gl.hat\n24 closeplugins\n32 rack\n34 rmchan 0\n35 rmchan 0\n46 rack\n")
geom(addrm 0 c0)
geom(addrm 1 c1)
geom(addrm 2 c2)
math(EXPR up "${c1_fitH} - ${c0_fitH}")
math(EXPR down "${c1_fitH} - ${c2_fitH}")
math(EXPR two "2 * ${c0_rowPitch}")
check("a channel added: the limit rises by one row (${up} px, pitch ${c0_rowPitch}); the rack grows with it (${c1_winH})"
      c1_rows EQUAL 6 AND up EQUAL c0_rowPitch AND c1_winH EQUAL c1_fitH)
check("two channels deleted: the limit drops by two rows (${down} px) and the window shrinks to fit (${c2_winH} = ${c2_fitH})"
      c2_rows EQUAL 4 AND down EQUAL two AND c2_winH EQUAL c2_fitH)
under(c2 "after deleting")

# ---- 4. zipped and preview rows (all 8 channels; the last one shows its piano-roll notes)
run(zip h900 "20 rack\n22 rackzip 1\n23 rackzip 2\n34 rack\n")
geom(zip 0 z0)
geom(zip 1 z1)
math(EXPR saved "${z0_fitH} - ${z1_fitH}")
check("8 channels with a preview row: as tall as the content (${z0_winH} = ${z0_fitH})" z0_winH EQUAL z0_fitH)
check("two rows zipped: the rack is shorter by less than two rows (${saved} px)" saved GREATER 0 AND saved LESS two AND z1_winH EQUAL z1_fitH)
under(z1 "zipped rows")

# ---- 5. docked beside the browser (a tall dock node): the add button still under the last row
run(dock dock "20 rack\n")
geom(dock 0 d)
check("docked: the dock node sets the size (${d_winH} > ${d_fitH})" d_docked EQUAL 1 AND d_winH GREATER d_fitH)
math(EXPR gap "${d_addTop} - ${d_lastRowBottom}")
# (a narrow dock node adds the rows' sideways scrollbar between the last row and the add button)
check("docked: the add button ${gap} px under the last row, the rest of the node is empty" gap GREATER_EQUAL 0 AND gap LESS_EQUAL 34)
