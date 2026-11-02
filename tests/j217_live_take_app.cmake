# Gridline — J-217: audio recorded into the playlist draws its clip and waveform live, in the
# real app (hidden, isolated config and home, SDL's dummy audio driver: the engine runs in real
# time; the input is GRIDLINE_SYNTH_INPUT's generated square wave, amplitude 0.2 / 0.7 changing
# every second, never a microphone).
#   1. One take at 100 %: while recording, a live clip on insert 1's track starts at the take and
#      grows (its geometry, peak count and envelope traced each check; the incremental summary equals
#      a rescan of the take); after stop the committed clip's geometry and summary equal the last live
#      frame, and its file's own summary too; one undo removes it.
#   2. Loop recording over a one-bar time selection at 200 %: the passes stack on the tracks below
#      while they record, earlier passes muted, exactly as they are committed. The script waits for the
#      third live pass ("livetake-clips:3"), not a fixed time: a busy machine's audio runs late.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j217_live_take_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_SYNTH_INPUT} "1")
set(ENV{GRIDLINE_NO_UI_SOUNDS} "1")

function(run tag scale frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --show playlist --size 1280x720 --ui-scale ${scale}
                          --frames ${frames} --screenshot "${OUT}/${tag}_end.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.log" "${out}\n--- stderr ---\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check name cond_var)
  if(NOT ${cond_var})
    message(FATAL_ERROR "FAIL: ${name}")
  endif()
  message(STATUS "ok - ${name}")
endfunction()

# A trace line: "live take <i> mixer <m> pass <p> track <t> start <s> length <l> pad <p> frames <f> bins <b> bin <w>
# lo <lo> hi <hi> sum <hex> rescan <0|1> done <0|1> muted <0|1> stereo <0|1> drawn <0|1> x <x0> <x1> y <y0> <y1> name "<n>"".
# fields(<line> <prefix>) sets <prefix>_<field> for each of them.
macro(fields line pre)
  foreach(f track start length frames bins hi sum rescan done muted drawn)
    string(REGEX MATCH " ${f} ([^ ]+)" _m "${line}")
    set(${pre}_${f} "${CMAKE_MATCH_1}")
  endforeach()
  string(REGEX MATCH " x ([0-9.-]+) ([0-9.-]+) y ([0-9.-]+) ([0-9.-]+)" _m "${line}")
  set(${pre}_x0 "${CMAKE_MATCH_1}")
  set(${pre}_x1 "${CMAKE_MATCH_2}")
  set(${pre}_y0 "${CMAKE_MATCH_3}")
  set(${pre}_y1 "${CMAKE_MATCH_4}")
endmacro()

# ---------------------------------------------------------------- 1. one take, 100 %
run(one 1 40 "3 liverec watch
4 liverec arm 1
6 liverec start
7 waitfor rec-a 1.3
8 liverec trace
9 shot \"${OUT}/one_mid_a.ppm\"
10 waitfor rec-b 1.3
11 liverec trace
12 shot \"${OUT}/one_mid_b.ppm\"
13 liverec stop
14 waitfor rec-c 1.5
15 liverec trace
16 plgeom
17 liverec clips
18 shot \"${OUT}/one_after.ppm\"
20 undo
22 liverec clips
")
string(REGEX MATCHALL "livetakes frame [0-9]+:[^\n]*\n[^\n]*\n(    env[^\n]*\n)?" traces "${one_out}")
list(LENGTH traces ntr)
check("three traces printed (${ntr})" ntr)
foreach(k 0 1)
  list(GET traces ${k} tk)
  if(NOT tk MATCHES "session 1 finishing 0 views 1" OR NOT tk MATCHES "live take [^\n]*")
    message(FATAL_ERROR "FAIL: no live clip while recording (check ${k}):\n${tk}\nOutput:\n${one_out}")
  endif()
  fields("${CMAKE_MATCH_0}" c${k})
  message(STATUS "  check ${k}: track ${c${k}_track} start ${c${k}_start} length ${c${k}_length} frames ${c${k}_frames} bins ${c${k}_bins} hi ${c${k}_hi} x ${c${k}_x0} ${c${k}_x1}")
endforeach()
list(GET traces 1 t1)
set(ok FALSE)
if(c0_track EQUAL 0 AND c1_track EQUAL 0 AND c0_start EQUAL c1_start)
  set(ok TRUE)
endif()
check("the live clip is on insert 1's track at the take's start" ok)
set(ok FALSE)
if(c1_frames GREATER c0_frames AND c1_bins GREATER c0_bins AND c1_length GREATER c0_length AND c1_x1 GREATER c0_x1 AND c0_x0 STREQUAL c1_x0)
  set(ok TRUE)
endif()
check("the live clip grows (frames ${c0_frames} -> ${c1_frames}, right edge ${c0_x1} -> ${c1_x1}, left edge fixed)" ok)
set(ok FALSE)
if(c0_drawn AND c1_drawn AND c0_rescan AND c1_rescan)
  set(ok TRUE)
endif()
check("drawn every check; the incremental summary equals a rescan of the take" ok)
# The envelope: the peak is the louder amplitude; every 1/8 window reads 0.200 or 0.700.
string(REGEX MATCH "env ([0-9. ]+)" envl "${t1}")
set(ok FALSE)
if(c1_hi STREQUAL "0.7000" AND envl MATCHES "0\\.200" AND envl MATCHES "0\\.700" AND NOT envl MATCHES "0\\.[013-689]")
  set(ok TRUE)
endif()
check("the live peaks follow the input's envelope (hi ${c1_hi}, ${envl})" ok)

# The last live frame (printed as the takes commit) against the committed clip.
if(NOT one_out MATCHES "livetakes final:\n[^\n]*\n *live take [^\n]*")
  message(FATAL_ERROR "FAIL: no final live frame. Output:\n${one_out}")
endif()
string(REGEX MATCH "live take [^\n]*" fl "${CMAKE_MATCH_0}")
fields("${fl}" f)
check("the last live frame shows the closed take" f_done)
list(GET traces 2 t2)
set(ok FALSE)
if(t2 MATCHES "session 0 finishing 0 views 0")
  set(ok TRUE)
endif()
check("after stop no live clip remains" ok)
string(REGEX MATCH "clippeaks frame [0-9]+:\n  clip [^\n]*" cl "${one_out}")
string(REGEX MATCH "clip ([0-9]+) " _m "${cl}")
set(cid ${CMAKE_MATCH_1})
fields("${cl}" k)
string(REGEX MATCH " file ([0-9a-f]+)" _m "${cl}")
set(kfile ${CMAKE_MATCH_1})
set(ok FALSE)
if(cid AND k_track EQUAL 0 AND k_start EQUAL f_start AND k_length EQUAL f_length AND k_frames EQUAL f_frames)
  set(ok TRUE)
endif()
check("the committed clip's track, start, length and frames equal the last live frame (${f_start} ${f_length} ${f_frames})" ok)
set(ok FALSE)
if(k_sum STREQUAL f_sum AND kfile STREQUAL f_sum)
  set(ok TRUE)
endif()
check("the committed clip draws the last live frame's peaks, which equal its file's (${f_sum})" ok)
set(ok FALSE)
if(one_out MATCHES "  clip ${cid} x ${f_x0} ${f_x1} y ${f_y0} ${f_y1} ")
  set(ok TRUE)
endif()
check("the committed clip's rectangle equals the last live one (x ${f_x0} ${f_x1} y ${f_y0} ${f_y1})" ok)
string(REGEX MATCHALL "clippeaks frame [0-9]+:\n(  clip [^\n]*\n)*" cps "${one_out}")
list(GET cps -1 lastcp)
set(ok FALSE)
if(NOT lastcp MATCHES "  clip ")
  set(ok TRUE)
endif()
check("one undo removes the recorded take" ok)

# ---------------------------------------------------------------- 2. loop passes, 200 %
run(loop 2 40 "4 liverec arm 1
5 liverec select 0 1
6 liverec start
7 waitfor livetake-clips:3 120
8 liverec trace
9 shot \"${OUT}/loop_mid.ppm\"
10 liverec stop
11 waitfor rec-b 1.5
12 liverec clips
13 shot \"${OUT}/loop_after.ppm\"
")
string(REGEX MATCH "livetakes frame [0-9]+:[^\n]*views ([0-9]+)" _m "${loop_out}")
set(nv ${CMAKE_MATCH_1})
set(ok FALSE)
if(nv GREATER_EQUAL 3)
  set(ok TRUE)
endif()
check("loop recording: every pass is a live clip (${nv})" ok)
check("loop recording: three passes recorded" loop_out MATCHES "waitfor livetake-clips:3: reached")
set(ok TRUE)
if(NOT loop_out MATCHES "live take [0-9]+ mixer [0-9]+ pass 0 track 0 start [0-9][0-9]? ")  # (the first pass: after the input latency, longer on a busy machine)
  set(ok FALSE)
endif()
foreach(p 1 2)
  if(NOT loop_out MATCHES "live take [0-9]+ mixer [0-9]+ pass ${p} track ${p} start 0 ")
    set(ok FALSE)
  endif()
endforeach()
if(NOT ok)
  message(STATUS "loop trace:\n${loop_out}")
endif()
check("loop recording: the passes stack on the tracks below, later passes from the loop start" ok)
set(ok FALSE)
if(loop_out MATCHES "pass 0 track 0 [^\n]* muted 1 " AND loop_out MATCHES "pass 1 track 1 [^\n]* muted 1 ")
  set(ok TRUE)
endif()
check("loop recording: earlier passes show muted while the next one records" ok)
string(REGEX MATCHALL "  clip [0-9]+ track [0-9]+ start [0-9]+ length [0-9]+ muted [01] take [0-9]+" lclips "${loop_out}")
list(LENGTH lclips nlc)
set(ok FALSE)
if(nlc EQUAL nv)
  set(ok TRUE)
endif()
check("loop recording: one committed clip per live pass (${nlc})" ok)
message(STATUS "screenshots: ${OUT}")
