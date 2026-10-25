# Gridline — J-116: detachable windows, detached only with the title-bar button, in the real app
# (hidden, isolated configs, off-screen at 1440x900, SDL's dummy audio driver, two virtual
# monitors side by side: the same positions on every machine). The scripted pointer drives ImGui
# like the mouse; "osmove" / "osresize" / "osclose" do what the OS does when the user drags a
# detached window's OS title bar or edge or clicks its close button; "liveresize" resizes one
# while the event pump is blocked (as macOS's live resize and Windows' size loop do), through
# SDL's event watch.
#   1. the mixer's tab dragged past the main window's edge and released outside: nothing special,
#      it stays a part of the main window (no OS window, now or later);
#   2. the pop-out button: a decorated, resizable OS window beside the main window, the panel's
#      size; the button flips to dock back;
#   3. resized by the OS (never below the panel's minimum size); moved by the OS over the main
#      window and released there: still detached; its own (Gridline) title bar dragged over the
#      main window and released: it does not move and does not dock;
#   4. the dock-back button docks it where it was; popped out again: at its last place outside;
#   5. its OS close button docks it back where it was (the panel stays open) and Gridline keeps
#      running; a floating window (the channel rack) closed outside floats where it was; its own
#      (Gridline) X in the title bar does the same;
#   6. live resize: each size step of a blocked pump draws exactly one frame, at that step's size;
#   7. a fixed-size editor pops out fixed-size; "Bring all windows back" docks everything; the
#      button is gone with detaching off.
# Skipped, with the reason, where windows cannot be created outside the main one.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j116_detach.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# run(<tag> <cfg> <frames> <script> [extra args...]): a fresh config ${OUT}/cfg_<cfg>.
function(run tag cfg frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${cfg}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${cfg}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${cfg}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
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

# The "viewports" dump of <frame>: <prefix>_detached, and for window <name>: <prefix>_x _y _w _h
# (integers), _vp (viewport index, 0 = the main window), _docked; for a window with an OS window
# of its own: <prefix>_resizable, _decorated.
function(win prefix frame name)
  string(REGEX MATCH "viewports frame ${frame}:[^\n]*\n(  [^\n]*\n)*" d "${out}")
  if(d STREQUAL "")
    fail("no viewports dump at frame ${frame}")
  endif()
  string(REGEX MATCH "detached ([0-9]+)" _ "${d}")
  set(${prefix}_detached ${CMAKE_MATCH_1} PARENT_SCOPE)
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
  string(REGEX MATCH "  viewport [0-9]+ window \"${name}\" [^\n]* resizable ([01]) decorated ([01])" vline "${d}")
  set(${prefix}_resizable "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${prefix}_decorated "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()
# The title-bar button of <name> at <frame> ("titlebtn" dev command): <prefix>_x _y (its centre), _state.
function(btn prefix frame name)
  if(NOT out MATCHES "titlebtn frame ${frame}: ${name} x ([-0-9]+) y ([-0-9]+) size ([0-9]+) state ([a-z]+)")
    fail("no title-bar button for ${name} at frame ${frame}")
  endif()
  math(EXPR cx "${CMAKE_MATCH_1} + ${CMAKE_MATCH_3} / 2")
  math(EXPR cy "${CMAKE_MATCH_2} + ${CMAKE_MATCH_3} / 2")
  set(${prefix}_x ${cx} PARENT_SCOPE)
  set(${prefix}_y ${cy} PARENT_SCOPE)
  set(${prefix}_state ${CMAKE_MATCH_4} PARENT_SCOPE)
endfunction()
function(near a b tol res)
  math(EXPR d "${a} - ${b}")
  if(d LESS -${tol} OR d GREATER ${tol})
    set(${res} OFF PARENT_SCOPE)
  else()
    set(${res} ON PARENT_SCOPE)
  endif()
endfunction()

# ---- 0. geometry, and whether windows can be detached here at all ----
run(geo geo 12 "8 viewports\n9 titlebtn Mixer\n9 titlebtn \"Channel rack\"\n" --chan 5)
if(out MATCHES "available 0")
  string(REGEX MATCH "unavailable: [^\n]*" why "${out}")
  message(STATUS "SKIP: windows cannot be detached on this platform (${why})")
  return()
endif()
win(g 8 "Mixer")
if(NOT g_docked EQUAL 1 OR NOT g_vp EQUAL 0)
  fail("the mixer should start docked in the main window")
endif()
win(rk 8 "Channel rack")
win(cs 8 "channelsettings")
btn(mb 9 "Mixer")
btn(rb 9 "Channel rack")
if(NOT mb_state STREQUAL "popout" OR NOT rb_state STREQUAL "popout")
  fail("docked and floating windows inside Gridline should show the pop-out button")
endif()
ok("docked and floating panels show the pop-out button in their title bar / tab bar")
math(EXPR tabX "${g_x} + 28")  # the mixer's tab
math(EXPR tabY "${g_y} + 12")

# ---- 1. dragged past the edge and released outside: nothing special happens ----
run(out1 drag 50 "10 move ${tabX} ${tabY}
12 down 0
14 move ${tabX} 100
16 move 1300 140
18 move 1450 180
20 move 1600 200
22 move 1700 220
23 viewports
24 up 0
30 viewports
44 viewports
")
win(m 23 "Mixer")
if(NOT m_vp EQUAL 0 OR NOT m_detached EQUAL 0)
  fail("while dragged past the main window's edge the mixer should stay in the main window (viewport ${m_vp}, detached ${m_detached})")
endif()
win(o 30 "Mixer")
win(o2 44 "Mixer")
if(NOT o_vp EQUAL 0 OR NOT o_detached EQUAL 0 OR NOT o2_vp EQUAL 0 OR NOT o2_detached EQUAL 0)
  fail("released outside: the mixer should still be a part of the main window, no OS window (viewport ${o_vp} / ${o2_vp}, detached ${o_detached} / ${o2_detached})")
endif()
ok("dragged past the edge and released outside: it stays in the main window (dragging never detaches)")

# ---- 2. the pop-out button ----
set(popClick "10 move ${mb_x} ${mb_y}
12 down 0
13 up 0
")
run(pop1 btn 30 "${popClick}20 viewports
22 titlebtn Mixer
")
win(p 20 "Mixer")
math(EXPR besideX "1440 + 24")
if(NOT p_vp GREATER 0 OR NOT p_x EQUAL besideX OR NOT p_w EQUAL g_w OR NOT p_detached EQUAL 1)
  fail("the pop-out button: the mixer should open beside the main window with its size (${p_x} ${p_w}x${p_h}, viewport ${p_vp})")
endif()
if(NOT p_decorated EQUAL 1 OR NOT p_resizable EQUAL 1)
  fail("a detached panel should be an OS-decorated, resizable window (decorated ${p_decorated}, resizable ${p_resizable})")
endif()
btn(pb 22 "Mixer")
if(NOT pb_state STREQUAL "dockin")
  fail("popped out: the same button should now dock back (${pb_state})")
endif()
ok("pop-out button: an OS-decorated, resizable window beside the main window, the panel's size; the button flips to dock back")
math(EXPR offX "${pb_x} - ${p_x}")
math(EXPR offY "${pb_y} - ${p_y}")

# ---- 3. resized / moved by the OS; moved over the main window or dragged by its own title bar: no docking ----
math(EXPR rw "${p_w} + 120")
math(EXPR rh "${p_h} - 100")
math(EXPR ttx "300 + 140")  # its own title bar once moved to 300,300
set(ttY 310)
run(os btn 90 "${popClick}20 osresize Mixer ${rw} ${rh}
26 viewports
28 osresize Mixer 20 20
34 viewports
36 osresize Mixer ${rw} ${rh}
38 osmove Mixer 1800 300
44 viewports
46 osmove Mixer 300 300
47 move 340 290
48 up 0
54 viewports
56 move ${ttx} ${ttY}
58 down 0
60 move ${ttx} 400
62 move 700 450
64 viewports
66 up 0
72 viewports
")
win(r 26 "Mixer")
if(NOT r_w EQUAL rw OR NOT r_h EQUAL rh OR NOT r_x EQUAL p_x OR NOT r_vp GREATER 0)
  fail("resized by the OS: want ${rw}x${rh} at x ${p_x}, got ${r_w}x${r_h} at ${r_x} (viewport ${r_vp})")
endif()
ok("resized by the OS (an edge / corner dragged): the panel follows")
win(t 34 "Mixer")
if(t_w LESS 32 OR t_h LESS 32)
  fail("resized below the panel's minimum size: ${t_w}x${t_h}")
endif()
ok("never smaller than the panel's minimum size")
win(v 44 "Mixer")
if(NOT v_x EQUAL 1800 OR NOT v_y EQUAL 300 OR NOT v_vp GREATER 0)
  fail("moved by the OS to 1800,300: it should be there, detached (${v_x},${v_y}, viewport ${v_vp})")
endif()
win(w 54 "Mixer")
if(NOT w_x EQUAL 300 OR NOT w_y EQUAL 300 OR NOT w_vp GREATER 0 OR NOT w_docked EQUAL 0 OR NOT w_detached EQUAL 1)
  fail("moved by its OS title bar over the main window and released there: it should stay detached at 300,300 (${w_x},${w_y}, viewport ${w_vp}, docked ${w_docked})")
endif()
ok("moved by its OS title bar over the main window and released: stays detached (never docks by moving)")
win(f 64 "Mixer")
win(k 72 "Mixer")
if(NOT f_x EQUAL 300 OR NOT f_y EQUAL 300 OR NOT k_x EQUAL 300 OR NOT k_y EQUAL 300 OR NOT k_vp GREATER 0 OR NOT k_docked EQUAL 0 OR NOT k_detached EQUAL 1)
  fail("its own title bar dragged over the main window: it should neither move nor dock (${f_x},${f_y} -> ${k_x},${k_y}, viewport ${k_vp}, docked ${k_docked})")
endif()
ok("its own Gridline title bar dragged over the main window and released: does not move, does not dock")

# ---- 4. the dock-back button; popped out again at the last place outside ----
math(EXPR ax "1700 + ${offX}")
math(EXPR ay "150 + ${offY}")
run(pop2 btn 70 "${popClick}20 osmove Mixer 1700 150
26 viewports
28 move ${ax} ${ay}
30 down 0
31 up 0
38 viewports
39 titlebtn Mixer
40 move ${mb_x} ${mb_y}
42 down 0
43 up 0
50 viewports
")
win(h 38 "Mixer")
near(${h_x} ${g_x} 2 xOk)
if(NOT h_docked EQUAL 1 OR NOT h_detached EQUAL 0 OR NOT xOk)
  fail("the dock-back button: the mixer should be docked where it was (x ${h_x}, docked ${h_docked}, detached ${h_detached})")
endif()
btn(hb 39 "Mixer")
if(NOT hb_state STREQUAL "popout")
  fail("docked back: the button should pop out again (${hb_state})")
endif()
ok("dock-back button: docked where it was; the button flips back to pop out")
win(z 50 "Mixer")
if(NOT z_vp GREATER 0 OR NOT z_x EQUAL 1700 OR NOT z_y EQUAL 150)
  fail("popped out again: the mixer should open at its last place outside, 1700,150 (${z_x},${z_y}, viewport ${z_vp})")
endif()
ok("popped out again: at its last place outside")

# ---- 5. the OS close button docks it back; Gridline keeps running ----
run(close btn 60 "${popClick}18 popout \"Channel rack\"
24 viewports
26 osclose Mixer
27 osclose \"Channel rack\"
34 viewports
40 titlebtn Mixer
50 viewports
" --chan 5)
win(c0 24 "Mixer")
win(cr0 24 "Channel rack")
if(NOT c0_vp GREATER 0 OR NOT cr0_vp GREATER 0)
  fail("set-up: the mixer and the channel rack should be detached (viewports ${c0_vp} ${cr0_vp})")
endif()
win(c1 34 "Mixer")
win(cr1 34 "Channel rack")
near(${c1_x} ${g_x} 2 xOk)
if(NOT c1_docked EQUAL 1 OR NOT c1_vp EQUAL 0 OR NOT xOk OR NOT c1_detached EQUAL 0)
  fail("closed by its OS close button: the mixer should be docked where it was, still open (docked ${c1_docked}, x ${c1_x} vs ${g_x}, detached ${c1_detached})")
endif()
if(NOT cr1_vp EQUAL 0 OR NOT cr1_x EQUAL rk_x OR NOT cr1_y EQUAL rk_y)
  fail("the floating channel rack closed outside: it should float where it was inside, still open (${cr1_x},${cr1_y} vs ${rk_x},${rk_y}, viewport ${cr1_vp})")
endif()
btn(cb 40 "Mixer")
win(c2 50 "Mixer")
if(NOT cb_state STREQUAL "popout" OR NOT c2_docked EQUAL 1)
  fail("after the close: docked, the button pops out again (${cb_state})")
endif()
ok("the OS close button (Cmd+W / X / red button) docks a detached panel back where it was; it stays open, Gridline keeps running")

# Gridline's own close button (the X in a detached panel's title bar) does the same.
run(xq btn 30 "${popClick}22 closebtn Mixer
" )
if(NOT out MATCHES "closebtn frame 22: Mixer x ([-0-9]+) y ([-0-9]+) size ([0-9]+)")
  fail("no close button in the detached mixer's title bar")
endif()
math(EXPR xx "${CMAKE_MATCH_1} + ${CMAKE_MATCH_3} / 2")
math(EXPR xy "${CMAKE_MATCH_2} + ${CMAKE_MATCH_3} / 2")
run(xclose btn 50 "${popClick}22 move ${xx} ${xy}
24 down 0
25 up 0
34 viewports
40 titlebtn Mixer
")
win(x1 34 "Mixer")
near(${x1_x} ${g_x} 2 xOk)
btn(xb 40 "Mixer")
if(NOT x1_docked EQUAL 1 OR NOT x1_vp EQUAL 0 OR NOT xOk OR NOT x1_detached EQUAL 0 OR NOT xb_state STREQUAL "popout")
  fail("closed by its own X: the mixer should be docked where it was, still open (docked ${x1_docked}, x ${x1_x} vs ${g_x}, detached ${x1_detached}, button ${xb_state})")
endif()
ok("Gridline's own X in a detached panel's title bar docks it back too; it stays open")

# ---- 6. live resize: one frame per size step, drawn at that step's size ----
math(EXPR lw "${g_w} + 240")
math(EXPR lh "${g_h} - 120")
run(live btn 60 "${popClick}20 liveresize Mixer ${lw} ${lh} 6
40 livestats
42 viewports
")
set(steps 0)
string(REGEX MATCHALL "liveresize step [0-9]+ size [0-9]+ [0-9]+ drawn [0-9]+ [0-9]+ frames [0-9]+" lines "${out}")
foreach(l IN LISTS lines)
  string(REGEX MATCH "step ([0-9]+) size ([0-9]+) ([0-9]+) drawn ([0-9]+) ([0-9]+) frames ([0-9]+)" _ "${l}")
  if(NOT CMAKE_MATCH_2 EQUAL CMAKE_MATCH_4 OR NOT CMAKE_MATCH_3 EQUAL CMAKE_MATCH_5)
    fail("live resize step ${CMAKE_MATCH_1}: drawn at ${CMAKE_MATCH_4}x${CMAKE_MATCH_5}, the window was ${CMAKE_MATCH_2}x${CMAKE_MATCH_3} (a stretched old frame)")
  endif()
  if(NOT CMAKE_MATCH_6 EQUAL 1)
    fail("live resize step ${CMAKE_MATCH_1}: ${CMAKE_MATCH_6} frames for one step (want exactly one)")
  endif()
  math(EXPR steps "${steps} + 1")
endforeach()
if(NOT steps EQUAL 6)
  fail("live resize: ${steps} of 6 steps reported")
endif()
if(NOT out MATCHES "liveresize frame 40: live-frames 6 recent 6 avg-ms ([0-9.]+) max-ms ([0-9.]+)")
  fail("live resize: the watch should have drawn 6 frames")
endif()
set(avg ${CMAKE_MATCH_1})
set(mx ${CMAKE_MATCH_2})
win(lr 42 "Mixer")
if(NOT lr_w EQUAL lw OR NOT lr_h EQUAL lh OR NOT lr_vp GREATER 0)
  fail("after the live resize the mixer should be ${lw}x${lh} (${lr_w}x${lr_h})")
endif()
ok("live resize (blocked event pump): every size step drew exactly one frame at its new size (avg ${avg} ms, max ${mx} ms per frame)")

# ---- 7. a fixed-size editor; "Bring all windows back"; detaching off ----
run(pop3 btn 70 "6 popout channelsettings
10 move ${rb_x} ${rb_y}
12 down 0
13 up 0
20 viewports
22 titlebtn \"Channel rack\"
24 dockback \"Channel rack\"
30 viewports
32 popout Mixer
38 viewports
40 gather
48 viewports
50 detaching off
60 titlebtn Mixer
60 titlebtn \"Channel rack\"
" --chan 5)
win(fr 20 "Channel rack")
win(fe 20 "channelsettings")
if(NOT fr_vp GREATER 0 OR NOT fr_w EQUAL rk_w)
  fail("the floating channel rack's pop-out button: a window of its own with its size (${fr_w} vs ${rk_w}, viewport ${fr_vp})")
endif()
if(NOT fe_vp GREATER 0 OR NOT fe_resizable EQUAL 0 OR NOT fe_w EQUAL cs_w)
  fail("a popped-out channel settings editor keeps its fixed size (resizable ${fe_resizable}, ${fe_w} vs ${cs_w}, viewport ${fe_vp})")
endif()
ok("a floating window and a fixed-size editor pop out (the editor's OS window is not resizable)")
win(fb 30 "Channel rack")
if(NOT fb_vp EQUAL 0 OR NOT fb_x EQUAL rk_x OR NOT fb_y EQUAL rk_y)
  fail("docked back: the channel rack should float where it was (${fb_x},${fb_y} vs ${rk_x},${rk_y}, viewport ${fb_vp})")
endif()
ok("a floating window docks back where it floated")
win(ga 48 "Mixer")
win(ge 48 "channelsettings")
if(NOT ga_detached EQUAL 0 OR NOT ga_docked EQUAL 1 OR NOT ge_vp EQUAL 0)
  fail("Bring all windows back: nothing should stay detached (detached ${ga_detached}, mixer docked ${ga_docked}, editor viewport ${ge_vp})")
endif()
ok("\"Bring all windows back into Gridline\" docks / brings back every detached window")
if(NOT out MATCHES "titlebtn frame 60: Mixer none" OR NOT out MATCHES "titlebtn frame 60: Channel rack none")
  fail("detaching off: no pop-out button")
endif()
ok("detaching off: the button is gone")
