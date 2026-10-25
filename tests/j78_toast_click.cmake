# Gridline — J-78: clicks on a notification card never reach the view below.
# Runs the app hidden (off-screen, isolated config) on the demo song with one
# extra pattern clip on track 13 (bars 11-15), right under the bottom-right
# notification card, and scripts left click, right click (draw tool: delete),
# a left drag and a wheel step on that spot. With a notification up (a save at
# frame 3 shows one) the clip must be unchanged; without one (control run) the
# same input must change it, so the test cannot pass vacuously.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j78_toast_click.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x (CMP0054: a quoted "keep" is that string, never a variable)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
file(READ "${DEMO}" demo)
set(clip "{\"id\": 9001, \"length\": 1536, \"pattern\": 30, \"start\": 3840, \"track\": 12}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clip}, " demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 9002" demo "${demo}")
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
# Pointer on the card (single-line card: x 1100..1268, y 732..766 at 1280x800)
# and on the clip (track 13 row y 709..755, bars 11-15 x 984..1250).
set(clicks "8 move 1180 745\n10 down 0\n11 up 0\n13 down 1\n14 up 1\n16 down 0\n17 move 1120 600\n19 up 0\n20 move 1180 745\n21 wheel -1\n")
file(WRITE "${OUT}/control.txt" "5 toasts clear\n${clicks}")  # the "Opened" card goes too

# One run: `saveAt` ("" = none) is saved at frame 3, so its notification is up
# when the scripted input arrives; the project is saved again at exit.
function(run tag saveAt)
  set(args "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --show playlist --size 1280x800 --frames 30
           --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" --save-at-exit "${OUT}/${tag}.gridline.json")
  if(saveAt)
    list(APPEND args --save-at "${saveAt}")
  endif()
  execute_process(COMMAND "${APP}" ${args} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  file(READ "${OUT}/${tag}.gridline.json" saved)
  string(REGEX REPLACE "[ \t\r\n]" "" saved "${saved}")
  set(${tag}_saved "${saved}" PARENT_SCOPE)
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

set(want "{\"id\":9001,\"length\":1536,\"pattern\":30,\"start\":3840,\"track\":12}")
# A notification run: the clip stays, and the clipboard holds every expected piece.
function(expectToast tag)
  string(FIND "${${tag}_saved}" "${want}" kept)
  if(kept LESS 0)
    message(FATAL_ERROR "FAIL (${tag}): a click on the notification reached the playlist (the clip under it changed)")
  endif()
  foreach(piece IN LISTS ARGN)
    string(FIND "${${tag}_out}" "${piece}" at)
    if(at LESS 0)
      message(FATAL_ERROR "FAIL (${tag}): the clipboard lacks \"${piece}\". Output:\n${${tag}_out}")
    endif()
  endforeach()
  message(STATUS "ok - ${tag}: click / right click / drag / wheel stay on the notification; the clipboard has its full text")
endfunction()

set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
foreach(t short flp error)
  file(WRITE "${OUT}/${t}.txt" "${clicks}")
endforeach()
# Short info card.
run(short "${OUT}/toast.gridline.json")
expectToast(short "clipboard: Saved toast.gridline.json" "${OUT}/toast.gridline.json")
# Long two-line card: an FL Studio project saved with Gridline instruments
# rendered to samples (J-80: the summary; the full report is its details).
run(flp "${OUT}/toast.flp")
expectToast(flp "clipboard: Saved toast.flp: " "rendered to samples" "${OUT}/toast.flp")
# Long multi-line error card: a read-only destination (not where permissions do not stop the
# process: root in CI containers).
file(MAKE_DIRECTORY "${OUT}/read only")
execute_process(COMMAND chmod 555 "${OUT}/read only")
execute_process(COMMAND sh -c "echo x > probe" WORKING_DIRECTORY "${OUT}/read only" RESULT_VARIABLE probeRc OUTPUT_QUIET ERROR_QUIET)
if(EXISTS "${OUT}/read only/probe")
  file(REMOVE "${OUT}/read only/probe")
  execute_process(COMMAND chmod 755 "${OUT}/read only")
  message(STATUS "skip - error: folder permissions are not enforced for this user")
else()
  run(error "${OUT}/read only/x.flp")
  execute_process(COMMAND chmod 755 "${OUT}/read only")
  expectToast(error "clipboard: Error: Could not save the FL Studio project: cannot write x.flp: no permission to write in the folder \"read only\""
              "Choose another folder." "${OUT}/read only/x.flp")
endif()
# Control: the same input with no notification up does reach the clip.
run(control "")
string(FIND "${control_saved}" "${want}" controlKept)
if(NOT controlKept LESS 0)
  message(FATAL_ERROR "FAIL (control): the same input without a notification did not change the clip; the test is not exercising the playlist")
endif()
message(STATUS "ok - control: without a notification the same input changes the clip")
