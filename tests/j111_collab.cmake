# Gridline - J-111: two Gridline processes collaborate over real ICE (libjuice) on this computer,
# each with its own config (identity, profile). No server is contacted (no STUN: host candidates).
#   1. Alex creates a collaborative project from the demo and writes an invite code to a file;
#   2. Sam joins with the code into his own folder;
#   3. both edit (Alex: notes, a mixer level, a native plugin's parameter, an audio file; Sam:
#      notes, a channel pan, a pattern name);
#   4. Alex solos, arms, monitors, launches a clip and folds a track: no op, Sam sees none of it;
#   5. both songs and both saved project files are identical, the audio file arrived (no
#      placeholder left), and an edit reached the other process within the latency budget;
#   6. both run again on their folders (--open): they reconnect by themselves (stored addresses and
#      ports), edit again and converge, still two members;
#   7. a join whose ICE connects only after 32 s (--ice-delay: past the old 30 s handshake budget,
#      which made the inviter admit a joiner that had already given up) connects and converges.
#   cmake -DCLI=<gridline> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j111_collab.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x (CMP0054: a quoted "keep" is that string, never a variable)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND "${CLI}" render "${DEMO}" -o "${OUT}/loop.wav" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "rendering the audio file failed (${rc}): ${err}")
endif()
# The two processes run at the same time (a pipeline: Alex's stdout is Sam's unused stdin).
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgA
          "${CLI}" collab-run --folder "${OUT}/Alex" --name Alex --create "${DEMO}" --invite-out "${OUT}/invite.txt"
          --seconds 20 --edits host --audio "${OUT}/loop.wav" --report "${OUT}/alex.json"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgB
          "${CLI}" collab-run --folder "${OUT}/Sam" --name Sam --join "${OUT}/invite.txt"
          --seconds 20 --edits guest --report "${OUT}/sam.json"
  RESULTS_VARIABLE rcs OUTPUT_QUIET ERROR_VARIABLE err TIMEOUT 120)
foreach(who alex sam)
  if(NOT EXISTS "${OUT}/${who}.json")
    message(FATAL_ERROR "${who} wrote no report (exit codes ${rcs}): ${err}")
  endif()
  file(READ "${OUT}/${who}.json" j)
  foreach(k song file members audioFiles loading latencyMs ready sessionOps soloSeen)
    string(JSON v_${who}_${k} GET "${j}" ${k})
  endforeach()
endforeach()
message(STATUS "alex: song ${v_alex_song} members ${v_alex_members} audio ${v_alex_audioFiles}")
message(STATUS "sam:  song ${v_sam_song} members ${v_sam_members} audio ${v_sam_audioFiles} latency ${v_sam_latencyMs} ms")
if(NOT v_alex_ready EQUAL 1 OR NOT v_sam_ready EQUAL 1)
  message(FATAL_ERROR "a session never became ready (${err})")
endif()
if(NOT v_alex_members EQUAL 2 OR NOT v_sam_members EQUAL 2)
  message(FATAL_ERROR "membership differs: ${v_alex_members} / ${v_sam_members}")
endif()
if(NOT v_alex_song STREQUAL v_sam_song)
  message(FATAL_ERROR "the two songs differ after both edited")
endif()
if(NOT v_alex_file STREQUAL v_sam_file OR NOT v_alex_file STREQUAL v_alex_song)
  message(FATAL_ERROR "the saved project files differ (${v_alex_file} / ${v_sam_file})")
endif()
if(NOT v_sam_audioFiles EQUAL 1 OR NOT v_sam_loading EQUAL 0)
  message(FATAL_ERROR "the audio file did not arrive (files ${v_sam_audioFiles}, still loading ${v_sam_loading})")
endif()
# (shared CI runners, busy with other tests, get more room)
set(budget 100)
if(DEFINED ENV{CI})
  set(budget 300)
endif()
if(v_sam_latencyMs LESS 0 OR v_sam_latencyMs GREATER budget)
  message(FATAL_ERROR "edit latency ${v_sam_latencyMs} ms (budget ${budget} ms)")
endif()
# Solo, record arm, input monitoring, a launched clip and a folded track stay on Alex's computer.
if(NOT v_alex_sessionOps EQUAL 0 OR NOT v_sam_soloSeen EQUAL 0)
  message(FATAL_ERROR "session state was shared (ops ${v_alex_sessionOps}, solo / arm seen by Sam ${v_sam_soloSeen})")
endif()
# The audio file is byte-identical in both folders.
file(GLOB a_audio "${OUT}/Alex/Audio/*")
file(GLOB s_audio "${OUT}/Sam/Audio/*")
file(SHA256 "${a_audio}" ha)
file(SHA256 "${s_audio}" hs)
if(NOT ha STREQUAL hs)
  message(FATAL_ERROR "the audio files differ")
endif()

# 6. Both reopen their folders: they find each other without a code and converge again.
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgA
          "${CLI}" collab-run --folder "${OUT}/Alex" --name Alex --open --seconds 20 --edits host --report "${OUT}/alex2.json"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgB
          "${CLI}" collab-run --folder "${OUT}/Sam" --name Sam --open --seconds 20 --edits guest --report "${OUT}/sam2.json"
  RESULTS_VARIABLE rcs2 OUTPUT_QUIET ERROR_VARIABLE err2 TIMEOUT 120)
foreach(who alex2 sam2)
  if(NOT EXISTS "${OUT}/${who}.json")
    message(FATAL_ERROR "${who} wrote no report (exit codes ${rcs2}): ${err2}")
  endif()
  file(READ "${OUT}/${who}.json" j)
  foreach(k song connected members)
    string(JSON v_${who}_${k} GET "${j}" ${k})
  endforeach()
endforeach()
if(NOT v_alex2_connected EQUAL 1 OR NOT v_sam2_connected EQUAL 1 OR NOT v_alex2_members EQUAL 2 OR NOT v_sam2_members EQUAL 2)
  message(FATAL_ERROR "reopened: not reconnected (connected ${v_alex2_connected} / ${v_sam2_connected}, members ${v_alex2_members} / ${v_sam2_members}): ${err2}")
endif()
if(NOT v_alex2_song STREQUAL v_sam2_song OR v_alex2_song STREQUAL v_alex_song)
  message(FATAL_ERROR "reopened: the songs differ or the new edits are missing")
endif()
message(STATUS "reopened: reconnected, song ${v_alex2_song}")

# 7. ICE that connects late (32 s after joining) still joins. Sam has 28 s after that to connect,
# sync and report, Alex stays 12 s longer (a loaded runner); the connection log (GRIDLINE_COLLAB_DEBUG)
# goes into the failure message with the timeline of both.
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgC GRIDLINE_COLLAB_DEBUG=1
          "${CLI}" collab-run --folder "${OUT}/Alex3" --name Alex --create "${DEMO}" --invite-out "${OUT}/invite3.txt"
          --seconds 72 --edits host --report "${OUT}/alex3.json"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgD GRIDLINE_COLLAB_DEBUG=1
          "${CLI}" collab-run --folder "${OUT}/Sam3" --name Sam --join "${OUT}/invite3.txt" --ice-delay 32
          --seconds 60 --edits guest --report "${OUT}/sam3.json"
  RESULTS_VARIABLE rcs3 OUTPUT_QUIET ERROR_VARIABLE err3 TIMEOUT 170)
# the log without the repeated packets
string(REGEX REPLACE "[^\n]*(H2 repeated|send of [0-9]+ bytes failed|Send while ICE is not connected|poll gap)[^\n]*\n" "" err3 "${err3}")
foreach(who alex3 sam3)
  if(NOT EXISTS "${OUT}/${who}.json")
    message(FATAL_ERROR "${who} wrote no report (exit codes ${rcs3}): ${err3}")
  endif()
  file(READ "${OUT}/${who}.json" j)
  foreach(k song ready connected)
    string(JSON v_${who}_${k} GET "${j}" ${k})
  endforeach()
endforeach()
if(NOT v_sam3_ready EQUAL 1 OR NOT v_sam3_connected EQUAL 1 OR NOT v_alex3_song STREQUAL v_sam3_song)
  message(FATAL_ERROR "late ICE: the join failed (ready ${v_sam3_ready}, connected ${v_sam3_connected}): ${err3}")
endif()
message(STATUS "late ICE: joined after 32 s, song ${v_sam3_song}")
