# Gridline: the audio clip menus' Audio tools, driven through the real clip menu in every layout.
# Runs the app hidden (isolated config, fixed frame time) on a copy of the test song with
# two audio clips added on one sample (Take). Per layout (Studio, Clips, Smooth, Tracker):
#   A. the clip's right-click menu (the arrangement clip menu, as a right-click opens it) has an
#      "Audio tools" submenu; hovering it opens it with Loupe's tools; clicking Clean up opens the
#      tool's window on the clip;
#   B. Accept: the clip plays a new file (the source untouched) on a copy of the channel named
#      "Take (edited)", the other clip keeps Take; one undo step brings it back;
#   C. Reverse (the dev "open" entry, as the menu calls it) applies at once with no window;
#   D. (scale 1) both clips selected, clicking Reverse in the menu applies to both at once, no
#      window, one undo step.
# With SHOTS set, the menu and the window are captured at 1x and 2x into SHOTS.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> [-DSHOTS=<dir>] -P k078_cliptools_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(SHA256 "${OUT}/Take.wav" takeHash)
file(READ "${DEMO}" demo)
set(chanJson "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clipJson "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, {\"id\": 71, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 1536, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chanJson}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clipJson}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

function(run tag layout scale frames script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --ui-scale ${scale}
                          --frames ${frames} --menu arrclip --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${last_out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# The dev menu opens at frame 2; its rows settle by frame 6.
set(drive "8 cliptools reveal\n11 cliptools hover\n16 cliptools rows\n18 SHOTMENU\n20 cliptools click denoise\n26 cliptools state\n27 cliptools wait\n36 SHOTDLG\n38 cliptools accept\n40 cliptools wait\n44 cliptools state\n46 save \"@OUT@/after_@TAG@.gridline.json\"\n48 undo\n50 save \"@OUT@/undone_@TAG@.gridline.json\"\n52 cliptools open 70 reverse\n53 cliptools state\n54 cliptools wait\n64 cliptools state\n66 save \"@OUT@/reversed_@TAG@.gridline.json\"\n70 quit discard\n")

foreach(pair "studio;studio" "clips;live" "smooth;logic" "tracker;tracker")
  list(GET pair 0 tag)
  list(GET pair 1 layout)
  foreach(scale 1.0 2.0)
    set(t "${tag}_${scale}")
    string(REPLACE "." "" t "${t}")
    string(REPLACE "@OUT@" "${OUT}" script "${drive}")
    string(REPLACE "@TAG@" "${t}" script "${script}")
    if(SHOTS)
      string(REPLACE "SHOTMENU" "shot \"${SHOTS}/${t}_menu.ppm\"" script "${script}")
      string(REPLACE "SHOTDLG" "shot \"${SHOTS}/${t}_dialog.ppm\"" script "${script}")
    else()
      string(REPLACE "18 SHOTMENU\n" "" script "${script}")
      string(REPLACE "36 SHOTDLG\n" "" script "${script}")
    endif()
    run(${t} ${layout} ${scale} 80 "${script}")
    set(o "${${t}_out}")
    set(last_out "${o}")
    check("${t}: the Audio tools submenu opens with Loupe's tools" o MATCHES "cliptools rows frame 16:[^\n]* denoise@[^\n]* reverse@")
    check("${t}: Clean up opens its window on the clip" o MATCHES "cliptools state frame 26: open 1 tool Clean up clip 70")
    check("${t}: Accept applied it to the clip" o MATCHES "cliptools state frame 44: open 0 [^\n]*last \"applied 1 [^\"]+Edited audio/[^\"]+\\.wav")
    file(READ "${OUT}/after_${t}.gridline.json" after)
    file(READ "${OUT}/undone_${t}.gridline.json" undone)
    file(READ "${OUT}/reversed_${t}.gridline.json" reversed)
    check("${t}: the clip plays the new file, the other clip the original" after MATCHES "Take.Clean.up-[0-9a-f]+\\.wav" AND after MATCHES "\"data\": \"${OUT}/Take.wav\"")
    check("${t}: the channel's copy is named Take (edited)" after MATCHES "\"name\": \"Take \\(edited\\)\"")
    check("${t}: undo, the original again" undone MATCHES "\"data\": \"${OUT}/Take.wav\"" AND NOT undone MATCHES "\\(edited\\)")
    check("${t}: Reverse runs at once, no window" o MATCHES "cliptools state frame 53: open 0 tool Reverse")
    check("${t}: Reverse wrote another new file" o MATCHES "cliptools state frame 64: open 0 [^\n]*last \"applied 1 [^\"]+Take.Reverse-[0-9a-f]+\\.wav" AND reversed MATCHES "\"name\": \"Take \\(edited\\)\"")
    file(SHA256 "${OUT}/Take.wav" nowHash)
    check("${t}: the source file is untouched" nowHash STREQUAL takeHash)
  endforeach()
  # D. Both clips selected; Reverse clicked in the menu.
  set(t "${tag}_multi")
  set(script "4 cliptools select 70 71\n8 cliptools reveal\n11 cliptools hover\n16 cliptools click reverse\n19 cliptools state\n20 cliptools wait\n26 cliptools state\n28 save \"${OUT}/after_${t}.gridline.json\"\n30 undo\n32 save \"${OUT}/undone_${t}.gridline.json\"\n36 quit discard\n")
  run(${t} ${layout} 1.0 50 "${script}")
  set(o "${${t}_out}")
  set(last_out "${o}")
  file(READ "${OUT}/after_${t}.gridline.json" after)
  file(READ "${OUT}/undone_${t}.gridline.json" undone)
  check("${t}: Reverse from the menu opens no window" o MATCHES "cliptools state frame 19: open 0 tool Reverse clip 70 clips 2")
  check("${t}: both clips reversed at once" o MATCHES "cliptools state frame 26: open 0 [^\n]*running 0 last \"applied 2 [^\"]*Reverse-[0-9a-f]+\\.wav [^\"]*Reverse-[0-9a-f]+\\.wav\"")
  check("${t}: neither clip plays the original" NOT after MATCHES "\"data\": \"${OUT}/Take.wav\"")
  check("${t}: one undo brings both back" undone MATCHES "\"data\": \"${OUT}/Take.wav\"" AND NOT undone MATCHES "Reverse-[0-9a-f]+\\.wav")
endforeach()
