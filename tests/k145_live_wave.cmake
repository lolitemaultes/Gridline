# Gridline — K-145: a recording's waveform is drawn live, then fitted to the width once it reaches
# the right edge, in Loupe and in Seltmann's recorder. Driven like a user (hidden window, isolated
# config and HOME, fixed frame time, SDL's dummy audio). The inputs are simulated, never a
# microphone: Loupe records GRIDLINE_EDISON_SIM (a generated phrase, frame-time paced), Seltmann
# the GRIDLINE_TEST_INPUT loop.
#   1. Loupe: from the first frame of the take the lanes draw it; while it fits it grows at the
#      normal scale (ten seconds across); past the edge the whole take is fitted every frame (the
#      head stays at the edge less its gap), the span never shrinks; when the take stops the view
#      is the normal one: the whole file, no live look. At 100 % and 200 %, dark and light.
#   2. Seltmann: the same while recording, through a pause and a resume; finished, the whole take.
# Frame sequences of both in <OUT> (shot_*.ppm).
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P k145_live_wave.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput (tail):\n${tail}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# Checks the live lines of a trace ("<prefix> ... span S base B ... gap G fit F"):
# gap >= -0.05 always (the head never past the edge), |gap| < 0.05 once fitted, spans never shrink.
function(check_live tag prefix text)
  string(REGEX MATCHALL "${prefix}[^\n]*" lines "${text}")
  list(LENGTH lines n)
  set(bad "")
  set(fitted 0)
  set(before 0)
  set(last -1)
  set(lastFrames -1)
  foreach(l ${lines})
    string(REGEX MATCH "frames ([0-9]+) shown [0-9.]+ span ([0-9.]+)" _ "${l}")
    set(frames "${CMAKE_MATCH_1}")
    set(span "${CMAKE_MATCH_2}")
    string(REGEX MATCH "gap (-?[0-9.]+) fit ([01])" _ "${l}")
    set(gap "${CMAKE_MATCH_1}")
    set(fit "${CMAKE_MATCH_2}")
    if(gap LESS -0.05)
      set(bad "${bad}\n  head past the edge: ${l}")
    endif()
    if(fit EQUAL 1)
      math(EXPR fitted "${fitted} + 1")
      if(gap GREATER 0.05)
        set(bad "${bad}\n  fitted but the take does not fill the width: ${l}")
      endif()
    else()
      math(EXPR before "${before} + 1")
    endif()
    if(frames GREATER_EQUAL lastFrames AND span LESS last)
      set(bad "${bad}\n  the span shrank: ${l}")
    endif()
    set(last "${span}")
    set(lastFrames "${frames}")
  endforeach()
  set(tail "${text}" PARENT_SCOPE)
  string(LENGTH "${bad}" nb)
  if(nb GREATER 2000)
    string(SUBSTRING "${bad}" 0 2000 bad)
  endif()
  check("${tag}: ${n} live frames drawn" n GREATER 20)
  check("${tag}: grows at the normal scale first (${before} frames)" before GREATER 5)
  check("${tag}: then fitted to the width (${fitted} frames)" fitted GREATER 5)
  check("${tag}: head at the edge once fitted, never past it, the view never shrinks${bad}" nb EQUAL 0)
endfunction()

# ---- 1. Loupe
set(ENV{GRIDLINE_EDISON_STATE} "rec")
set(ENV{GRIDLINE_EDISON_SIM} "6")
set(ENV{GRIDLINE_EDISON_SIM_STOP} "36")
set(ENV{GRIDLINE_EDISON_LIVETRACE} "1")
foreach(v "1;1440x900;" "1;1920x1080;" "2;2880x1800;" "1;1440x900;--light")
  list(GET v 0 scale)
  list(GET v 1 size)
  list(GET v 2 light)
  set(tag "loupe_${size}_x${scale}${light}")
  string(REPLACE "--" "_" tag "${tag}")
  # frames: ~0.1 s of take each; the edge (10 s) near frame 105, the stop (36 s) near 375
  set(script "")
  foreach(f 30 80 120 200 300 360)
    string(APPEND script "${f} shot \"${OUT}/shot_${tag}_${f}.ppm\"\n")
  endforeach()
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  set(extra "")
  if(light)
    list(APPEND extra --light)
  endif()
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.edison --size ${size} --ui-scale ${scale} ${extra}
                          --frames 420 --screenshot "${OUT}/shot_${tag}_after.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  set(tail "${err}")
  check("${tag}: gridline-app ran (${rc})" rc EQUAL 0)
  check_live("${tag}" "loupe: live " "${out}")
  # the last live frame, then the view once the take is the file
  string(REGEX MATCHALL "loupe: live frames [0-9]+" lf "${out}")
  list(GET lf -1 lastLine)
  string(REGEX MATCH "[0-9]+$" lastLive "${lastLine}")
  string(FIND "${out}" "${lastLine}" at REVERSE)
  string(SUBSTRING "${out}" ${at} -1 after)
  string(REGEX MATCH "loupe: view start ([0-9]+) len ([0-9]+) frames ([0-9]+) spectral 0" _ "${after}")
  set(vstart "${CMAKE_MATCH_1}")
  set(vlen "${CMAKE_MATCH_2}")
  set(vframes "${CMAKE_MATCH_3}")
  check("${tag}: after the stop the view is the normal one: the whole take (start ${vstart} len ${vlen} frames ${vframes}, last live ${lastLive})"
        vstart EQUAL 0 AND vlen EQUAL vframes AND vframes EQUAL lastLive AND vframes GREATER 1500000)
  foreach(f 30 80 120 200 300 360 after)
    check("${tag}: frame ${f} written" EXISTS "${OUT}/shot_${tag}_${f}.ppm")
  endforeach()
endforeach()
unset(ENV{GRIDLINE_EDISON_STATE})
unset(ENV{GRIDLINE_EDISON_SIM})
unset(ENV{GRIDLINE_EDISON_SIM_STOP})

# ---- 2. Seltmann's recorder (real-time test input)
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/input.wav" RESULT_VARIABLE rc)
check("the synthetic input was written" rc EQUAL 0 AND EXISTS "${OUT}/input.wav")
set(ENV{GRIDLINE_TEST_INPUT} "${OUT}/input.wav")
set(ENV{GRIDLINE_SELTMANN_TRACE} "1")
function(srun tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.seltmann --size 1440x960 --frames ${frames}
                          --screenshot "${OUT}/shot_${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  set(tail "${err}")
  check("${tag}: gridline-app ran (${rc})" rc EQUAL 0)
  set(out "${out}" PARENT_SCOPE)
endfunction()
# record past the edge: pause at 6 s for a second, resume, finish at 15 s (the recorder drives
# itself through GRIDLINE_SELTMANN_AUTOREC, wherever the window sits); frames along the way. The
# input runs in real time, so a slow runner draws few frames between the edge (10 s) and 15 s:
# the take goes on until 10 frames have shown it fitted (the check below wants more than 5).
set(ENV{GRIDLINE_SELTMANN_AUTOREC} "6 1 15 10")
srun(s_rec 90 "26 waitfor seltmann-take-4 20\n27 shot \"${OUT}/shot_seltmann_4s.ppm\"\n28 waitfor seltmann-take-12 30\n29 shot \"${OUT}/shot_seltmann_12s.ppm\"\n30 waitfor seltmann-take-14 30\n31 shot \"${OUT}/shot_seltmann_14s.ppm\"\n32 waitfor seltmann-autorec-done 30\n")
unset(ENV{GRIDLINE_SELTMANN_AUTOREC})
string(REGEX MATCHALL "seltmann: take (recording|paused) [^\n]*live 1" live "${out}")
string(REPLACE ";" "\n" live "${live}")
check_live("seltmann" "seltmann: take (recording|paused) " "${live}")
string(REGEX MATCHALL "seltmann: take finished frames ([0-9]+) shown [0-9]+ span ([0-9]+)[^\n]*live 0" fin "${out}")
list(LENGTH fin nfin)
check("seltmann: finished frames drawn without the live fit" nfin GREATER 0)
list(GET fin -1 f)
string(REGEX MATCH "frames ([0-9]+) shown ([0-9]+) span ([0-9]+) head ([0-9.]+) width ([0-9.]+)" _ "${f}")
check("seltmann: finished, the whole take fills the width (${f})" CMAKE_MATCH_1 EQUAL CMAKE_MATCH_3 AND CMAKE_MATCH_1 GREATER 600000)
check("seltmann: frame sequence written" EXISTS "${OUT}/shot_seltmann_4s.ppm" AND EXISTS "${OUT}/shot_seltmann_12s.ppm" AND EXISTS "${OUT}/shot_seltmann_14s.ppm"
      AND EXISTS "${OUT}/shot_s_rec.ppm")
