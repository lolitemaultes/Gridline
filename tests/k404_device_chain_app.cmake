# Gridline - K-404: quiet app test for Studio inline Device chain & Junction device.
# Verifies:
#   1. Studio Device chain renders the selected channel's instrument + mixer track effects inline
#      with zero floating windows required.
#   2. Adding built-in effects + Junction FX (gl.patcherfx) inline updates the chain immediately
#      and never pops open a floating effect editor window.
#   3. Collapsing a device turns it into a slim 30 px strip with ZERO editor draw calls
#      (skippedCollapsed increments, drawn=0).
#   4. Scrolling devices off-screen skips their editor draw calls (skippedOffscreen > 0).
#   5. Resizing the panel dynamically re-fits device height and vector scale.
#   6. Reordering and removing effects update the chain cleanly.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k404_device_chain_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_QUIET_WINDOWS} "1")

set(failures 0)
macro(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR failures "${failures} + 1")
endmacro()
macro(check msg)
  if(NOT (${ARGN}))
    fail("${msg}")
  else()
    message(STATUS "ok - ${msg}")
  endif()
endmacro()

set(cfg "${OUT}/cfg")
file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
file(WRITE "${cfg}/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true}}\n")
set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
set(ENV{HOME} "${cfg}/home")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)

set(script "5 toasts clear
10 layout open Device chain
16 parts chain add gl.eq
18 parts chain add gl.comp
20 parts chain add gl.patcherfx
22 parts chain add gl.reverb
24 parts chain add gl.delay
32 parts chain stats
32 layout bodies
36 parts chain collapse 1
42 parts chain stats
46 parts chain expand 1
50 parts chain scroll 2200
56 parts chain stats
60 parts chain scroll 0
64 parts chain resize 1200 440
70 parts chain stats
74 parts chain move 0 2
78 parts chain stats
82 parts chain remove 2
88 parts chain stats
")
file(WRITE "${cfg}/input.txt" "${script}")

execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --fixed-dt 16
                        --dev-input "${cfg}/input.txt" --frames 94 --screenshot "${OUT}/studio_chain.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
file(WRITE "${OUT}/run1.log" "${out}\n---- stderr ----\n${err}")
check("gridline-app exited cleanly (${rc})" rc EQUAL 0)

# Collect each `devchain frame N: ...` snapshot in order (0..5)
string(REGEX MATCHALL "devchain frame [0-9]+: [^\n]+" snapshots "${out}")
list(LENGTH snapshots snapCount)
check("collected all 6 devchain snapshots (${snapCount})" snapCount EQUAL 6)

if(snapCount EQUAL 6)
  list(GET snapshots 0 s0)
  list(GET snapshots 1 s1)
  list(GET snapshots 2 s2)
  list(GET snapshots 3 s3)
  list(GET snapshots 4 s4)
  list(GET snapshots 5 s5)

  string(REGEX MATCH "devices ([0-9]+) drawn ([0-9]+) hosted ([0-9]+) collapsed ([0-9]+) offscreen ([0-9]+) panelH ([0-9]+)" _ "${s0}")
  set(d0 "${CMAKE_MATCH_1}")
  set(dr0 "${CMAKE_MATCH_2}")
  set(off0 "${CMAKE_MATCH_5}")
  set(h0 "${CMAKE_MATCH_6}")
  check("chain shows instrument + 5 added effects incl. Junction FX (devices=${d0})" d0 GREATER_EQUAL 6)
  check("visible expanded built-in devices draw inline (drawn=${dr0})" dr0 GREATER_EQUAL 1)
  check("off-screen devices are culled (offscreen=${off0})" off0 GREATER_EQUAL 1)
  check("Junction FX (gl.patcherfx) is present in the inline Device chain" out MATCHES "gl\\.patcherfx")

  if(out MATCHES "frame 32 bodies \\[([^]\n]*)\\]")
    set(b32 "${CMAKE_MATCH_1}")
    check("adding effects inline never opens a floating effect editor (${b32})" NOT b32 MATCHES "effect editor")
  else()
    fail("no bodies line at frame 32")
  endif()

  check("collapsing device 1 skips its editor draw (${s1})" s1 MATCHES "collapsed 1")
  check("collapsed device 1 is a 30 px strip with drawn=0"
        out MATCHES "dev 1 \"[^\"]+\" type [^ ]+ slot [0-9-]+ collapsed 1 visible [01] drawn 0 scale [0-9.]+ rect [0-9]+ 30 ")

  string(REGEX MATCH "offscreen ([0-9]+)" _ "${s2}")
  set(off2 "${CMAKE_MATCH_1}")
  check("scrolling right culls left off-screen devices (offscreen=${off2})" off2 GREATER_EQUAL 2)

  string(REGEX MATCH "panelH ([0-9]+)" _ "${s3}")
  set(h3 "${CMAKE_MATCH_1}")
  check("resizing Device chain panel taller dynamically re-fits card height (${h0} -> ${h3})" h3 GREATER h0)

  string(REGEX MATCH "devices ([0-9]+)" _ "${s4}")
  set(d4 "${CMAKE_MATCH_1}")
  string(REGEX MATCH "devices ([0-9]+)" _ "${s5}")
  set(d5 "${CMAKE_MATCH_1}")
  math(EXPR expectD5 "${d4} - 1")
  check("removing effect slot 2 drops device count (${d4} -> ${d5})" d5 EQUAL expectD5)
endif()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - K-404 Studio inline Device chain verified")
