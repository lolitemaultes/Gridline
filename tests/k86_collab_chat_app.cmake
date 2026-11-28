# K-86 part 2 (0.7.9): Automated UI test for Collaboration Chat + "Send to chat" / "View" Flash.
# Verifies:
#   1) Non-collab project: Chat stays hidden (even if chat-open is requested).
#   2) Collab session: peer message arrives with unread badge, undo stack untouched.
#   3) Opening Chat panel renders it docked in Studio and marks messages read.
#   4) Posting an attachment + invoking View opens the target window and triggers element flash.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED APP OR NOT DEFINED DEMO OR NOT DEFINED OUT)
  message(FATAL_ERROR "APP, DEMO and OUT are required")
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(CFG_NON "${OUT}/cfg_noncollab")
set(CFG_COL "${OUT}/cfg_collab")
set(HOME_TMP "${OUT}/home")
file(MAKE_DIRECTORY "${CFG_NON}" "${CFG_COL}" "${HOME_TMP}")

set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_QUIET_WINDOWS} "1")
set(ENV{GRIDLINE_NO_AUTO_UPDATE} "1")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

function(read_kv FILE PREFIX)
  if(NOT EXISTS "${FILE}")
    message(FATAL_ERROR "Missing report file: ${FILE}")
  endif()
  file(STRINGS "${FILE}" _lines)
  foreach(_l IN LISTS _lines)
    if(_l MATCHES "^([^=]+)=(.*)$")
      set("${PREFIX}_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}" PARENT_SCOPE)
    endif()
  endforeach()
endfunction()

# ---- Phase 1: Non-collab session must never show Chat ----
set(NON_SCRIPT "${OUT}/noncollab.txt")
set(NON_REPORT "${OUT}/noncollab_report.txt")
file(WRITE "${NON_SCRIPT}"
"3 collab chat-open\n"
"5 collab chat-report \"${NON_REPORT}\"\n"
"6 quit discard\n"
)
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "GRIDLINE_CONFIG_HOME=${CFG_NON}" "HOME=${HOME_TMP}"
          "${APP}" --hidden --no-ui-sounds --size 1440x900
          --dev-input "${NON_SCRIPT}" "${DEMO}"
  RESULT_VARIABLE rc1
  OUTPUT_VARIABLE out1
  ERROR_VARIABLE err1
)
if(NOT rc1 EQUAL 0)
  message(FATAL_ERROR "non-collab run failed (${rc1}):\n${out1}\n${err1}")
endif()
read_kv("${NON_REPORT}" NON)
if(NOT NON_active STREQUAL "0" OR NOT NON_showChat STREQUAL "0" OR NOT NON_chatDrawn STREQUAL "0")
  message(FATAL_ERROR "Chat must stay hidden outside collab sessions: active=${NON_active} showChat=${NON_showChat} chatDrawn=${NON_chatDrawn}")
endif()

# ---- Phase 2: Collab session (Send to chat -> Peer sync -> Open Chat -> View + Flash) ----
set(COL_SCRIPT "${OUT}/collab.txt")
set(REP_BEFORE "${OUT}/rep_before.txt")
set(REP_OPEN   "${OUT}/rep_open.txt")
set(REP_CLIP   "${OUT}/rep_view_clip.txt")
set(REP_MIXER  "${OUT}/rep_view_mixer.txt")
set(REP_PROMPT "${OUT}/rep_prompt.txt")
file(WRITE "${COL_SCRIPT}"
"4 waitfor collab-synced 45\n"
"5 collab chat-peer-ref clip 0 -1 Check this clip timing\n"
"6 waitfor never 0.5\n"
"8 collab chat-report \"${REP_BEFORE}\"\n"
"9 collab chat-send-ref mixer 1 -1 Check insert 1 level\n"
"10 collab chat-open\n"
"13 collab chat-report \"${REP_OPEN}\"\n"
"14 collab chat-view 0\n"
"15 collab chat-report \"${REP_CLIP}\"\n"
"16 collab chat-view 1\n"
"17 collab chat-report \"${REP_MIXER}\"\n"
"18 collab chat-prompt channel 0 -1 Look at this synth\n"
"20 collab chat-report \"${REP_PROMPT}\"\n"
"21 quit discard\n"
)
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "GRIDLINE_CONFIG_HOME=${CFG_COL}" "HOME=${HOME_TMP}"
          "${APP}" --hidden --no-ui-sounds --size 1440x900
          --show collab-cursors --dev-input "${COL_SCRIPT}" "${DEMO}"
  RESULT_VARIABLE rc2
  OUTPUT_VARIABLE out2
  ERROR_VARIABLE err2
)
if(NOT rc2 EQUAL 0)
  message(FATAL_ERROR "collab run failed (${rc2}):\n${out2}\n${err2}")
endif()

read_kv("${REP_BEFORE}" BEF)
if(NOT BEF_active STREQUAL "1")
  message(FATAL_ERROR "Expected active collab session, got active=${BEF_active}")
endif()
if(BEF_messages LESS 1 OR BEF_unread LESS 1)
  message(FATAL_ERROR "Expected peer message with unread >= 1 before opening chat: messages=${BEF_messages} unread=${BEF_unread}")
endif()

read_kv("${REP_OPEN}" OPN)
if(NOT OPN_showChat STREQUAL "1" OR NOT OPN_chatDrawn STREQUAL "1")
  message(FATAL_ERROR "Expected Chat window drawn when opened: showChat=${OPN_showChat} chatDrawn=${OPN_chatDrawn}")
endif()
if(NOT OPN_unread STREQUAL "0")
  message(FATAL_ERROR "Expected unread count to clear when viewing Chat, got unread=${OPN_unread}")
endif()
if(OPN_messages LESS 2)
  message(FATAL_ERROR "Expected at least 2 messages after local attachment send, got ${OPN_messages}")
endif()
if(NOT OPN_undoSize STREQUAL BEF_undoSize)
  message(FATAL_ERROR "Sending chat must not change undo history: before=${BEF_undoSize} after=${OPN_undoSize}")
endif()

read_kv("${REP_CLIP}" VCL)
if(NOT VCL_lastViewOk STREQUAL "1" OR NOT VCL_showPlaylist STREQUAL "1" OR NOT VCL_flashActive STREQUAL "1")
  message(FATAL_ERROR "Viewing clip attachment must open Playlist and flash clip: ok=${VCL_lastViewOk} pl=${VCL_showPlaylist} flash=${VCL_flashActive} key=${VCL_lastViewFlashKey}")
endif()
if(NOT VCL_lastViewFlashKey MATCHES "^playlist/#")
  message(FATAL_ERROR "Expected playlist/# flash key, got ${VCL_lastViewFlashKey}")
endif()

read_kv("${REP_MIXER}" VMX)
if(NOT VMX_lastViewOk STREQUAL "1" OR NOT VMX_showMixer STREQUAL "1" OR NOT VMX_flashActive STREQUAL "1")
  message(FATAL_ERROR "Viewing mixer attachment must open Mixer and flash track: ok=${VMX_lastViewOk} mx=${VMX_showMixer} flash=${VMX_flashActive} key=${VMX_lastViewFlashKey}")
endif()
if(NOT VMX_lastViewFlashKey MATCHES "^mixer/#")
  message(FATAL_ERROR "Expected mixer/# flash key, got ${VMX_lastViewFlashKey}")
endif()

message(STATUS "k86_collab_chat_app PASS: non-collab hidden, peer sync + unread badge, docked Chat, undo untouched, View+flash verified (clip=${VCL_lastViewFlashKey}, mixer=${VMX_lastViewFlashKey})")
