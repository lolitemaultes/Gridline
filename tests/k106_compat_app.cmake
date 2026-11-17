# Gridline - K-106 (checkpoint 16): a song arranged in the Tracker is the same song in every layout.
#   1. In the Tracker: a song of seven entries built from four patterns on the Song page (append, set,
#      move, insert, delete, a clone and its length), a note typed into a pattern, a section looped;
#      saved. The app then switches to Studio, Clips, Smooth and back to the Tracker, saving in
#      each: every save is byte for byte the Tracker's.
#   2. That file opened again (in the Tracker, then in Studio) and saved: the same bytes, and the
#      order list reads back as it was arranged.
#   3. The pattern's tracks: one per rack channel, empty ones too, in the rack's order after a move
#      made in Studio, the same in every pattern; at 1024x640 the last one scrolls into view.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k106_compat_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(failures 0)
set(runSize 1440x900)
macro(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR failures "${failures} + 1")
endmacro()

# run(<tag> <layout> <project or ""> <frames> <script> args...): a hidden run in its own config folder.
function(run tag lay project frames script)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${lay}\", \"keys.profile\": \"${lay}\", \"ui.menuStyle\": \"${lay}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  file(WRITE "${cfg}/input.txt" "${script}")
  set(p "")
  if(NOT project STREQUAL "")
    set(p "${project}")
  endif()
  execute_process(COMMAND "${APP}" ${p} --hidden --layout-dev --no-ui-sounds --size ${runSize} --layout ${lay} --dev-input "${cfg}/input.txt" --fixed-dt 16
                          --frames ${frames} ${ARGN}
                  OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.log" "${o}\n--- stderr\n${e}")
  if(NOT rc EQUAL 0)
    fail("${tag}: exit ${rc}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

# ---- 1: arrange in the Tracker, then every layout saves the same bytes ----
run(prep tracker "" 80 "10 template tracker\n40 tracker order dump\n60 save \"${OUT}/base.gridline.json\"\n70 quit discard\n")
string(REGEX MATCH "order frame 40 \\(dump\\): n ([0-9]+)" _ "${out}")
if(NOT CMAKE_MATCH_1)
  fail("the template has no order list: ${out}")
endif()

# Build the seven entries on an empty song: the template's patterns, its arrangement cleared first.
set(S "10 tracker page Song
20 tracker order dump
")
math(EXPR f 22)
# Remove every entry the template has (at most 16), then append 1 2 3 2 4 3 3, then edit.
foreach(i RANGE 15)
  string(APPEND S "${f} tracker order del 0\n")
  math(EXPR f "${f} + 2")
endforeach()
string(APPEND S "${f} tracker order new\n")
math(EXPR f "${f} + 2")
foreach(p 1 2 3 2 4 3 3)
  string(APPEND S "${f} tracker order append ${p}\n")
  math(EXPR f "${f} + 2")
endforeach()
# Reorder: entry 4 (pattern 4) up one; insert after 0 (a second 1), delete it again; set entry 6 to 1.
string(APPEND S "${f} tracker order move 4 -1\n")
math(EXPR f "${f} + 2")
string(APPEND S "${f} tracker order ins 0\n")
math(EXPR f "${f} + 2")
string(APPEND S "${f} tracker order del 1\n")
math(EXPR f "${f} + 2")
string(APPEND S "${f} tracker order set 6 1\n")
math(EXPR f "${f} + 2")
# Clone pattern 2 (it becomes pattern 3; the later ones move up) and give the clone a bar more.
string(APPEND S "${f} tracker order clone 2\n")
math(EXPR f "${f} + 2")
string(APPEND S "${f} tracker order bars 3 1\n")
math(EXPR f "${f} + 2")
# A note typed into the shown pattern (the clone), on track 0, row 0.
string(APPEND S "${f} tracker page Pattern
${f} tracker focus\n")
math(EXPR f "${f} + 4")
string(APPEND S "${f} tracker cursor 0 0 0 0
${f} tracker edit on\n")
math(EXPR f "${f} + 2")
string(APPEND S "${f} sdlkey q down\n")
math(EXPR f "${f} + 1")
string(APPEND S "${f} sdlkey q up\n")
math(EXPR f "${f} + 3")
# Loop entries 2 to 4; play from entry 2 for a moment, stop.
string(APPEND S "${f} tracker order loop 2 4\n")
math(EXPR f "${f} + 2")
string(APPEND S "${f} tracker order play 2\n")
math(EXPR f "${f} + 30")
string(APPEND S "${f} tracker order dump\n")
math(EXPR f "${f} + 2")
string(APPEND S "${f} key space\n")
math(EXPR f "${f} + 10")
string(APPEND S "${f} tracker order dump\n")
math(EXPR f "${f} + 2")
string(APPEND S "${f} save \"${OUT}/tracker.gridline.json\"\n")
set(saveT ${f})
# The other layouts, each saving once it is on screen; then the Tracker again.
math(EXPR swS "${f} + 10")
math(EXPR svS "${f} + 30")
math(EXPR swC "${f} + 40")
math(EXPR svC "${f} + 60")
math(EXPR swL "${f} + 70")
math(EXPR svL "${f} + 90")
math(EXPR swT "${f} + 100")
math(EXPR svT "${f} + 120")
math(EXPR dT "${f} + 122")
math(EXPR frames0 "${f} + 128")  # (a run ends by quitting: an idle window waits for events, frames or not)
string(APPEND S "${svS} save \"${OUT}/studio.gridline.json\"
${svC} save \"${OUT}/clips.gridline.json\"
${svL} save \"${OUT}/smooth.gridline.json\"
${svT} save \"${OUT}/back.gridline.json\"
${dT} tracker order dump
${frames0} quit discard
")
math(EXPR frames "${f} + 135")
run(arrange tracker "${OUT}/base.gridline.json" ${frames} "${S}" --switch-layout studio@${swS} --switch-layout live@${swC} --switch-layout logic@${swL}
    --switch-layout tracker@${swT})
set(arrangeLog "${out}")
string(REGEX MATCH "order frame ${saveT} \\(dump\\)[^\n]*" _ "${out}")
string(REGEX MATCH "order frame [0-9]+ \\(dump\\): n ([0-9]+) \\|([^|]*)\\| patterns ([0-9]+) \\| loop ([0-9-]+)[^\n]*playing 1" playingLine "${out}")
if(NOT playingLine)
  fail("the song did not play from entry 2")
elseif(NOT CMAKE_MATCH_1 STREQUAL "7" OR NOT CMAKE_MATCH_2 STREQUAL " 01 02 04 05 02 04 01 " OR NOT CMAKE_MATCH_3 STREQUAL "6" OR NOT CMAKE_MATCH_4 STREQUAL "2-4")
  fail("the arranged order is not 7 entries 01 02 04 05 02 04 01 of 6 patterns, looped 2-4: ${playingLine}")
endif()
# (The clone goes in after its source, so the patterns after it renumber: 03 Theme is 04 now.)
# Studio's save is the Tracker's, byte for byte. Clips, once shown, keeps its own view in the song
# (N48-07: "live": {"view": ...}, the one key it adds); past that key Clips, Smooth and the Tracker
# again save the same bytes.
file(READ "${OUT}/tracker.gridline.json" tText)
foreach(tag studio clips smooth back)
  if(NOT EXISTS "${OUT}/${tag}.gridline.json")
    fail("${tag}: nothing saved")
    continue()
  endif()
  file(READ "${OUT}/${tag}.gridline.json" h)
  if(NOT tag STREQUAL "studio")
    string(REGEX REPLACE "\n  \"live\": {\n    \"view\": \"[a-z]+\"\n  }," "" h "${h}")
  endif()
  if(NOT h STREQUAL tText)
    fail("${tag}: the save differs from the Tracker's (diff ${OUT}/tracker.gridline.json ${OUT}/${tag}.gridline.json)")
  endif()
endforeach()
file(SHA256 "${OUT}/clips.gridline.json" c)
foreach(tag smooth back)
  file(SHA256 "${OUT}/${tag}.gridline.json" h)
  if(NOT h STREQUAL c)
    fail("${tag}: the save differs from Clips' (diff ${OUT}/clips.gridline.json ${OUT}/${tag}.gridline.json)")
  endif()
endforeach()
string(REGEX MATCH "order frame ${dT} \\(dump\\): n 7 \\| 01 02 04 05 02 04 01 \\|" back "${arrangeLog}")
if(NOT back)
  fail("back in the Tracker the order list reads otherwise")
endif()

# ---- 2: reopened in the Tracker and in Studio, saved again: the same bytes ----
run(reopenT tracker "${OUT}/tracker.gridline.json" 60 "30 tracker order dump\n40 save \"${OUT}/reopenT.gridline.json\"\n50 quit discard\n")
string(REGEX MATCH "n 7 \\| 01 02 04 05 02 04 01 \\|" ok "${out}")
if(NOT ok)
  fail("reopened, the order list reads otherwise: ${out}")
endif()
run(reopenS studio "${OUT}/tracker.gridline.json" 60 "40 save \"${OUT}/reopenS.gridline.json\"\n50 quit discard\n")
foreach(tag reopenT reopenS)
  file(SHA256 "${OUT}/${tag}.gridline.json" h)
  file(SHA256 "${OUT}/tracker.gridline.json" t)
  if(NOT h STREQUAL t)
    fail("${tag}: the save differs from the Tracker's (diff ${OUT}/tracker.gridline.json ${OUT}/${tag}.gridline.json)")
  endif()
endforeach()

# ---- 3: the pattern's tracks are the rack's channels, all of them, in its order (cp16, user) ----
# At 1024x640 in Studio a channel moves in the rack (the rack's own Document::moveChannel); in the
# Tracker the tracks follow it: one per channel (the empty ones too), every pattern the same, and
# the cursor taken to the last channel scrolls it into view.
set(runSize 1024x640)
run(columns studio "${OUT}/base.gridline.json" 140
    "20 tracker rack move 0 3\n60 tracker tracks\n62 tracker order go 4\n70 tracker tracks\n72 tracker order go 2\n80 tracker tracks\n90 quit discard\n"
    --switch-layout tracker@40)
set(cols "${out}")
string(REGEX MATCHALL "tracker tracks frame [0-9]+: [^\n]*" lines "${cols}")
list(LENGTH lines nLines)
if(NOT nLines EQUAL 3)
  fail("columns: ${nLines} track dumps, not 3: ${cols}")
else()
  list(GET lines 0 l0)
  list(GET lines 1 l1)
  list(GET lines 2 l2)
  string(REGEX MATCH "n ([0-9]+) rack ([0-9]+) first [0-9-]+ last [0-9-]+ cursor [0-9-]+ width [0-9]+ \\|(.*)" _ "${l0}")
  set(n0 "${CMAKE_MATCH_1}")
  set(names0 "${CMAKE_MATCH_3}")
  if(NOT CMAKE_MATCH_1 STREQUAL CMAKE_MATCH_2)
    fail("columns: ${CMAKE_MATCH_1} tracks for ${CMAKE_MATCH_2} channels: ${l0}")
  endif()
  if(NOT names0 MATCHES "^ Snare\\([0-9]+\\) Clap\\([0-9]+\\) Hat\\([0-9]+\\) Kick\\([0-9]+\\) ")
    fail("columns: the tracks do not follow the rack after Kick moved to 4th: ${l0}")
  endif()
  foreach(l IN ITEMS "${l1}" "${l2}")
    string(REGEX MATCH "\\|(.*)" _ "${l}")
    if(NOT CMAKE_MATCH_1 STREQUAL names0)
      fail("columns: another pattern shows other tracks or widths:\n  ${l0}\n  ${l}")
    endif()
  endforeach()
endif()
# The cursor on the last track: it is in view (the sideways scroll reached it).
run(lastcol tracker "${OUT}/base.gridline.json" 60 "20 tracker cursor Thunder 0\n30 tracker tracks\n40 quit discard\n")
string(REGEX MATCH "n ([0-9]+) rack [0-9]+ first ([0-9]+) last ([0-9]+) cursor ([0-9]+)" _ "${out}")
math(EXPR lastT "${CMAKE_MATCH_1} - 1")
if(NOT CMAKE_MATCH_4 STREQUAL "${lastT}" OR NOT CMAKE_MATCH_3 STREQUAL "${lastT}" OR CMAKE_MATCH_2 STREQUAL "0")
  fail("columns: the last track is not scrolled into view at 1024x640: ${out}")
endif()
set(runSize 1440x900)

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - arranged in the Tracker; Studio, Clips, Smooth and the Tracker again save the same bytes; reopened, the order holds; the tracks are the rack's channels")
