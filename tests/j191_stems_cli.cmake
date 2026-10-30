# Gridline - J-191: `gridline stems` end to end with a tiny model of the same family (no real
# weights): every stem written as a file with the input's length, a chosen subset, files never
# overwritten, a model that is not downloaded reported (nothing fetched), the catalogue listed.
#   cmake -DCLI=<gridline> -DTESTBIN=<gridline_tests_stems> -DSRC=<source dir> -DOUT=<dir> -P j191_stems_cli.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/models" "${OUT}/home" "${OUT}/a" "${OUT}/b")
execute_process(COMMAND "${TESTBIN}" --write-tiny "${OUT}/tiny.th" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: could not write the tiny model (${rc})")
endif()
configure_file("${SRC}/assets/sounds/classic/startup.wav" "${OUT}/Song.wav" COPYONLY)  # 48 kHz stereo, 2.5 s
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/home")
set(ENV{GRIDLINE_STEM_MODELS} "${OUT}/models")
set(ENV{GRIDLINE_STEMS_TEST_MODEL} "${OUT}/tiny.th")

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nstdout:\n${out}\nstderr:\n${err}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
function(cli)
  execute_process(COMMAND "${CLI}" stems ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e TIMEOUT 300)
  set(rc "${rc}" PARENT_SCOPE)
  set(out "${o}" PARENT_SCOPE)
  set(err "${e}" PARENT_SCOPE)
endfunction()

cli(--list)
check("the catalogue lists the three models and the test one" rc EQUAL 0 AND out MATCHES "htdemucs " AND out MATCHES "htdemucs_ft"
      AND out MATCHES "htdemucs_6s" AND out MATCHES "test ")

cli("${OUT}/Song.wav" --model test --out "${OUT}/a" --threads 2)
check("four stems split" rc EQUAL 0)
foreach(s Vocals Drums Bass Other)
  check("... ${s} written" EXISTS "${OUT}/a/Song - ${s}.wav")
  file(SIZE "${OUT}/a/Song - ${s}.wav" sz)
  # 2 ch x 32-bit float x 2.5387 s x 48000 Hz = 974,872 bytes of audio (plus the header)
  check("... ${s} has the input's length, rate and channels (${sz} bytes)" sz GREATER 974000 AND sz LESS 976000)
endforeach()

cli("${OUT}/Song.wav" --model test --out "${OUT}/b" --stems vocals,bass --name Mix)
check("a chosen subset" rc EQUAL 0 AND EXISTS "${OUT}/b/Mix - Vocals.wav" AND EXISTS "${OUT}/b/Mix - Bass.wav"
      AND NOT EXISTS "${OUT}/b/Mix - Drums.wav")
cli("${OUT}/Song.wav" --model test --out "${OUT}/b" --stems vocals --name Mix)
check("an existing file is never overwritten" rc EQUAL 0 AND EXISTS "${OUT}/b/Mix - Vocals 2.wav")

cli("${OUT}/Song.wav" --model test --out "${OUT}/b" --stems guitar)
check("a stem the model does not have is refused" NOT rc EQUAL 0 AND err MATCHES "no stem")
cli("${OUT}/Song.wav" --model htdemucs --out "${OUT}/b")
check("a model that is not downloaded is reported, nothing fetched" NOT rc EQUAL 0 AND err MATCHES "not downloaded")
file(GLOB fetched "${OUT}/models/*")
list(LENGTH fetched nfetched)
check("... and nothing appeared in the models folder" nfetched EQUAL 0)
cli("${OUT}/missing.wav" --model test --out "${OUT}/b")
check("a missing input fails cleanly" NOT rc EQUAL 0)
cli(--import "${OUT}/Song.wav")
check("importing a file that is no catalogue model is refused" NOT rc EQUAL 0)
cli("${OUT}/Song.wav" --model test --out "${OUT}/b" --stems drums --name "Kick/Snare: take")
check("a name with path characters is made safe for a file name" rc EQUAL 0 AND EXISTS "${OUT}/b/Kick_Snare_ take - Drums.wav")
