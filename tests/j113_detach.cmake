# Gridline — J-113: detachable windows in the real app (hidden, isolated configs, off-screen at
# 1440x900, SDL's dummy audio driver). Off-screen runs get two virtual monitors, the main
# window's and one of the same size to its right, so every position below is the same on
# every machine. Detached windows are real (hidden) OS windows; the scripted pointer drives
# ImGui like the mouse does ("detach" pops a window out to an exact place). (The pop-out button,
# OS moves / resizes / close, docking back and live resize: J-116, tests/j116_detach.cmake.)
#   2. with the mixer detached and focused: Space starts / stops the song; a typed key (the
#      typing keyboard) held while the focus moves to the main window and back is still
#      held; the detached window losing the focus to another application releases it;
#   3. a detached channel rack takes J-97 drops (a file and a plugin replace rows), and a
#      dialog opened from a detached window opens over it (one from the main window: over
#      the main window, as before);
#   4. a detached window is saved in layout.ini (the user's config) and comes back where it
#      was; re-anchored when the main window starts elsewhere; recovered onto a monitor when
#      saved off every monitor; brought back inside with "Allow windows outside" off.
# Where the platform cannot create windows outside the main one (Wayland, no backend
# support) the test is skipped with the reason.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j113_detach.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x (CMP0054: a quoted "keep" is that string, never a variable)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../assets/sounds/open.wav" "${OUT}/Snap.wav" COPYONLY)

# run(<tag> <frames> <script> [keep] [extra args...]): config ${OUT}/cfg_<cfg>, fresh unless "keep".
function(run tag cfg frames script)
  set(extra ${ARGN})
  list(LENGTH extra n)
  set(keep OFF)
  if(n GREATER 0)
    list(GET extra 0 first)
    if(first STREQUAL "keep")
      set(keep ON)
      list(REMOVE_AT extra 0)
    endif()
  endif()
  if(NOT keep)
    file(REMOVE_RECURSE "${OUT}/cfg_${cfg}")
  endif()
  file(MAKE_DIRECTORY "${OUT}/cfg_${cfg}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${cfg}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${extra}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
endfunction()
function(ok msg)
  message(STATUS "ok - ${msg}")
endfunction()

# The "viewports" dump of <frame>: <prefix>_detached, and for window <name>:
# <prefix>_x _y _w _h (integers), _vp (viewport index, 0 = the main window), _docked.
function(win prefix frame name)
  string(REGEX MATCH "viewports frame ${frame}:[^\n]*\n(  [^\n]*\n)*" d "${out}")
  if(d STREQUAL "")
    fail("no viewports dump at frame ${frame}")
  endif()
  string(REGEX MATCH "detached ([0-9]+)" _ "${d}")
  set(${prefix}_detached ${CMAKE_MATCH_1} PARENT_SCOPE)
  string(REGEX MATCH "mainos ([-0-9]+) ([-0-9]+)" _ "${d}")
  set(${prefix}_mainx ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${prefix}_mainy ${CMAKE_MATCH_2} PARENT_SCOPE)
  string(REGEX MATCH "  window \"${name}\" pos ([-0-9]+)[.0-9]* ([-0-9]+)[.0-9]* size ([0-9]+)[.0-9]* ([0-9]+)[.0-9]* viewport ([-0-9]+) docked ([01])" line "${d}")
  if(line STREQUAL "")
    fail("window ${name} missing at frame ${frame}:\n${d}")
  endif()
  set(${prefix}_x ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${prefix}_y ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${prefix}_w ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(${prefix}_h ${CMAKE_MATCH_4} PARENT_SCOPE)
  set(${prefix}_vp ${CMAKE_MATCH_5} PARENT_SCOPE)
  set(${prefix}_docked ${CMAKE_MATCH_6} PARENT_SCOPE)
endfunction()

# ---- 0. geometry, and whether windows can be detached here at all ----
run(geo geo 12 "8 viewports\n")
if(out MATCHES "available 0")
  string(REGEX MATCH "unavailable: [^\n]*" why "${out}")
  message(STATUS "SKIP: windows cannot be detached on this platform (${why})")
  return()
endif()
win(g 8 "Mixer")
if(NOT g_docked EQUAL 1 OR NOT g_vp EQUAL 0)
  fail("the mixer should start docked in the main window")
endif()

# ---- 2. keys from a detached window, focus moving between Gridline's windows ----
run(keys keys 140 "8 detach Mixer 1600 120 900 380
16 viewports
18 move 1900 300
20 down 0
21 up 0
24 focusmove Mixer
26 transport
28 sdlkeyin Mixer space down
29 sdlkeyin Mixer space up
34 transport
36 sdlkeyin Mixer space down
37 sdlkeyin Mixer space up
42 transport
44 typing on
46 sdlkeyin Mixer z down
50 waitfor j113-none 0.3
51 sound
52 focusmove main
54 waitfor j113-none 0.3
55 sound
56 focusmove Mixer
58 waitfor j113-none 0.3
59 sound
60 focuslost Mixer
62 waitfor j113-none 0.3
63 sound
70 quit discard
" --chan 5)
win(k 16 "Mixer")
if(NOT k_vp GREATER 0)
  fail("keys: the mixer should be detached")
endif()
foreach(f 26 34 42)
  if(NOT out MATCHES "transport frame ${f}: pos [0-9.]+ playing ([01])")
    fail("no transport report at frame ${f}")
  endif()
  set(play${f} ${CMAKE_MATCH_1})
endforeach()
if(NOT play26 EQUAL 0 OR NOT play34 EQUAL 1 OR NOT play42 EQUAL 0)
  fail("Space typed into the detached mixer should start then stop the song (${play26} ${play34} ${play42})")
endif()
ok("Space typed into the detached mixer starts and stops the song")
function(live frame var)
  if(NOT out MATCHES "sound frame ${frame}: peak [0-9.]+ live ([0-9]+)")
    fail("no sound report at frame ${frame}")
  endif()
  set(${var} ${CMAKE_MATCH_1} PARENT_SCOPE)
endfunction()
live(51 l51)
live(55 l55)
live(59 l59)
live(63 l63)
if(NOT l51 GREATER 0)
  fail("a key typed into the detached mixer should play a held note (live ${l51})")
endif()
ok("the typing keyboard plays from the detached mixer")
if(NOT l55 GREATER 0 OR NOT l59 GREATER 0)
  fail("focus moving between Gridline's windows must not release held keys (live ${l55}, ${l59})")
endif()
ok("focus moving to the main window and back keeps the held key")
if(NOT l63 EQUAL 0)
  fail("the detached window losing the focus to another application should release held keys (live ${l63})")
endif()
ok("focus lost to another application (from the detached window): the held key is released")

# ---- 3. J-97 drops into a detached channel rack; dialogs over the window they come from ----
set(rackStart "6 detach \"Channel rack\" 1480 100
16 rack
18 viewports
")
run(rackgeo rack 20 "${rackStart}" --rack steps16,w1300,h560)
win(r 18 "Channel rack")
if(NOT r_vp GREATER 0 OR NOT r_x EQUAL 1480)
  fail("rack: the channel rack should be detached at x 1480 (viewport ${r_vp}, x ${r_x})")
endif()
string(REGEX MATCH "geom originX ([0-9]+)[.0-9]* row0Y ([0-9]+)[.0-9]*[^\n]* rowPitch ([0-9]+)" _ "${out}")
set(ox "${CMAKE_MATCH_1}")
set(oy "${CMAKE_MATCH_2}")
set(pitch "${CMAKE_MATCH_3}")
if(ox STREQUAL "" OR NOT ox GREATER 1440)
  fail("rack: no geometry for the detached rack (originX '${ox}')")
endif()
math(EXPR x "${ox} + 160")  # over a row, between the name button and the steps (as J-97)
math(EXPR y2 "${oy} + ${pitch}")
run(rack rack 64 "${rackStart}20 dragenter ${x} ${oy} \"${OUT}/Snap.wav\"
22 dragmove ${x} ${oy}
24 dragmove ${x} ${oy}
26 drop
32 channels
34 dragenter ${x} ${y2} plugin:gl.synth
36 dragmove ${x} ${y2}
38 dragmove ${x} ${y2}
40 drop
46 channels
48 dialogfrom \"Channel rack\" about
54 dialogfrom Playlist info
60 viewports
" --rack steps16,w1300,h560)
if(NOT out MATCHES "channels frame 32:[^\n]*undo 1\n  channel 20 Snap type gl.sampler data Snap.wav mixer 1")
  fail("rack: a file dropped on the detached rack's first row should replace it with a sampler, routing kept")
endif()
ok("J-97: a file dropped on a row of the detached rack replaces it (mixer track kept)")
if(NOT out MATCHES "channels frame 46:[^\n]*undo 2\n[^\n]*\n  channel 21 [^\n]* type gl.synth data  mixer 2")
  fail("rack: a plugin dropped on the detached rack's second row should replace its instrument")
endif()
ok("J-97: a plugin dropped on a row of the detached rack replaces its instrument")
win(ab 60 "About Gridline")
win(pi 60 "Project info")
math(EXPR abCx "${ab_x} + ${ab_w} / 2")
math(EXPR rackCx "${r_x} + ${r_w} / 2")
math(EXPR dx "${abCx} - ${rackCx}")
if(dx LESS -2 OR dx GREATER 2 OR NOT ab_vp GREATER 0)
  fail("a dialog opened from the detached rack should open centred over it (centre x ${abCx} vs ${rackCx}, viewport ${ab_vp})")
endif()
ok("a dialog opened from a detached window opens over it")
if(NOT pi_vp EQUAL 0 OR pi_x LESS 0 OR pi_x GREATER 1440)
  fail("a dialog opened from the main window should open in the main window (viewport ${pi_vp}, x ${pi_x})")
endif()
ok("a dialog opened from the main window opens over the main window, as before")

# ---- 4. saved in layout.ini, re-anchored, recovered, brought back ----
set(ini "${OUT}/cfg_persist/gridline/layout.ini")
run(save persist 20 "8 detach Mixer 1600 120 900 380\n16 viewports\n")
win(sv 16 "Mixer")  # sv_mainx / sv_mainy: where the OS put the main window this run
if(NOT EXISTS "${ini}")
  file(GLOB_RECURSE found "${OUT}/cfg_persist/*.ini")
  fail("no layout.ini in the config folder (${found})")
endif()
file(READ "${ini}" text)
if(NOT text MATCHES "\\[GridlineViewports\\]\\[Main\\]\nMainOs=[-0-9]+,[-0-9]+\nScale=1.0000")
  fail("layout.ini should record the main window detached windows are relative to:\n${text}")
endif()
if(NOT text MATCHES "\\[Window\\]\\[Mixer\\]\nViewportPos=1600,120\n")
  fail("layout.ini should hold the detached mixer's position:\n${text}")
endif()
if(NOT text MATCHES "\\[GridlineViewports\\]\\[Home:Mixer\\]\nDock=0x[0-9A-F]+,0x[0-9A-F]+,[-0-3]+,[0-9.]+\n")
  fail("layout.ini should hold where the detached mixer docks back (J-114):\n${text}")
endif()
if(NOT text MATCHES "\\[GridlineViewports\\]\\[Out:Mixer\\]\nRect=[-0-9]+,[-0-9]+,900,380\n")
  fail("layout.ini should hold the mixer's last place outside, in OS units (J-114):\n${text}")
endif()
if(EXISTS "${OUT}/fixture.gridline.json")
  file(READ "${OUT}/fixture.gridline.json" proj)
  if(proj MATCHES "GridlineViewports|ViewportPos")
    fail("window positions must never be saved in the project")
  endif()
endif()
ok("a detached window is saved in the user's layout.ini, never in the project")
run(load persist 20 "16 viewports\n" keep)
win(l 16 "Mixer")
# The same OS position: 1600,120 relative to the main window, which the OS may place a pixel
# elsewhere from one run to the next.
math(EXPR wantX "1600 + ${sv_mainx} - ${l_mainx}")
math(EXPR wantY "120 + ${sv_mainy} - ${l_mainy}")
if(NOT l_vp GREATER 0 OR NOT l_x EQUAL wantX OR NOT l_y EQUAL wantY OR NOT l_w EQUAL 900 OR NOT l_h EQUAL 380)
  fail("the next start should bring the mixer back detached at ${wantX},${wantY} 900x380 (${l_x},${l_y} ${l_w}x${l_h}, viewport ${l_vp})")
endif()
ok("the next start brings the detached mixer back where it was")
# The main window of the saved session 100 px further right: the mixer keeps its OS position.
math(EXPR savedX "${l_mainx} + 100")
string(REGEX REPLACE "MainOs=[-0-9]+," "MainOs=${savedX}," text2 "${text}")
file(WRITE "${ini}" "${text2}")
run(anchor persist 20 "16 viewports\n" keep)
win(a 16 "Mixer")
math(EXPR wantX "1700 + ${l_mainx} - ${a_mainx}")
if(NOT a_x EQUAL wantX OR NOT a_vp GREATER 0)
  fail("saved with the main window 100 px further right: the mixer should be re-anchored to x ${wantX} (${a_x}, viewport ${a_vp})")
endif()
ok("re-anchored to the main window's new position (same OS position)")
# Saved far off every monitor (a monitor that is gone): recovered onto the nearest one.
file(READ "${ini}" text3)
string(REGEX REPLACE "(\\[Window\\]\\[Mixer\\]\n)ViewportPos=[-0-9]+,[-0-9]+" "\\1ViewportPos=9000,9000" text3 "${text3}")
file(WRITE "${ini}" "${text3}")
run(recover persist 20 "16 viewports\n" keep)
win(v 16 "Mixer")
math(EXPR right "${v_x} + ${v_w}")
math(EXPR bottom "${v_y} + ${v_h}")
if(NOT v_vp GREATER 0 OR v_x LESS 1440 OR right GREATER 2880 OR v_y LESS 0 OR bottom GREATER 900)
  fail("saved off every monitor: the mixer should be recovered fully onto the second monitor (${v_x},${v_y} ${v_w}x${v_h})")
endif()
ok("saved off every monitor: recovered onto the nearest monitor")
# "Allow windows outside the main window" off: everything comes back inside, as before J-113.
run(off persist 30 "4 detaching off\n20 viewports\n" keep)
win(f 20 "Mixer")
math(EXPR right "${f_x} + ${f_w}")
if(NOT f_vp EQUAL 0 OR NOT f_detached EQUAL 0 OR f_x LESS 0 OR right GREATER 1440)
  fail("detaching off: the mixer should be back inside the main window (${f_x} ${f_w}, viewport ${f_vp})")
endif()
if(NOT out MATCHES "enabled 0")
  fail("detaching off: viewports should be off")
endif()
ok("\"Allow windows outside the main window\" off: windows come back inside")
# Started with the setting off while layout.ini holds a detached window: it opens inside.
run(again persist 20 "2 detaching on\n8 detach Mixer 1600 120 900 380\n16 viewports\n" keep)
set(settingsFile "${OUT}/cfg_persist/gridline/settings.json")
file(READ "${settingsFile}" st)
string(REGEX REPLACE "\"ui.detachWindows\": *true" "\"ui.detachWindows\": false" st "${st}")
file(WRITE "${settingsFile}" "${st}")
run(offstart persist 20 "16 viewports\n" keep)
win(s 16 "Mixer")
math(EXPR right "${s_x} + ${s_w}")
if(NOT s_vp EQUAL 0 OR s_x LESS 0 OR right GREATER 1440 OR NOT out MATCHES "enabled 0")
  fail("started with detaching off: the saved detached mixer should open inside the main window (${s_x} ${s_w}, viewport ${s_vp})")
endif()
ok("started with \"Allow windows outside\" off: a window saved outside opens inside")
