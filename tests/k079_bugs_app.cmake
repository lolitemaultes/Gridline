# Gridline - 0.7.9 Track 1 (K-395A, K-395B, K-409) quiet app regression test.
#   1. K-395A: Clips layout track deletion (Delete key, consecutive deletes, session/arrangement sync, undo).
#   2. K-395B: Theme switching stability in Clips & Smooth (live dark/light/brightness/whitetext round-trip).
#   3. K-409: Tempo double-click + Escape cancel + right-click > Type a value across ALL 4 layouts
#             (Studio, Clips, Smooth, Tracker), decimals preserved, single undo step.
#
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k079_bugs_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{GRIDLINE_QUIET_WINDOWS} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# run_app(<tag> <layout> <extra_settings_json> <dev_input> <frames> <dump_frame>)
function(run_app tag layout extra_settings script frames dump_frame)
  set(cfg "${OUT}/${tag}/cfg")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${OUT}/${tag}/home")
  configure_file("${DEMO}" "${OUT}/${tag}/song.gridline.json" COPYONLY)
  set(vals "\"ui.layoutChosen\": true, \"ui.layoutDev\": true")
  if(layout STREQUAL "live")
    string(APPEND vals ", \"clips.arrangementSets\": \"${OUT}/${tag}/song.gridline.json\"")
  endif()
  if(NOT extra_settings STREQUAL "")
    string(APPEND vals ", ${extra_settings}")
  endif()
  file(WRITE "${cfg}/gridline/settings.json" "{\"version\": 1, \"values\": {${vals}}}\n")
  file(WRITE "${OUT}/${tag}/input.txt" "3 toasts clear\n${script}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${OUT}/${tag}/home")
  if(dump_frame GREATER 0)
    set(ENV{GL_THEME_DUMP} "${dump_frame}")
  else()
    unset(ENV{GL_THEME_DUMP})
  endif()
  execute_process(
    COMMAND "${APP}" "${OUT}/${tag}/song.gridline.json"
            --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900
            --layout "${layout}" --layout-dev
            --dev-input "${OUT}/${tag}/input.txt" --frames "${frames}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}/log.txt" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL: ${tag}: gridline-app exited ${rc}:\n${err}\n${out}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
  set(${tag}_err "${err}" PARENT_SCOPE)
endfunction()

function(names var log prefix)
  string(REGEX MATCHALL "${prefix} [0-9]+ \"[^\"]*\"" lines "${log}")
  set(r "")
  foreach(l ${lines})
    string(REGEX REPLACE ".*\"([^\"]*)\"" "\\1" n "${l}")
    list(APPEND r "${n}")
  endforeach()
  set(${var} "${r}" PARENT_SCOPE)
endfunction()

function(point xv yv log prefix i)
  string(REGEX MATCH "${prefix} ${i} \"[^\"]*\" ([-0-9]+) ([-0-9]+)" m "${log}")
  if(NOT m)
    message(FATAL_ERROR "FAIL: no '${prefix} ${i}' in output:\n${log}")
  endif()
  set(${xv} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${yv} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

function(theme_key dump k var)
  if(NOT dump MATCHES "(^| )${k}=([#0-9A-Fa-f]+)")
    message(FATAL_ERROR "FAIL: no ${k} in theme dump: ${dump}")
  endif()
  set(${var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

function(verify_tempo_flow lay out)
  string(REGEX MATCHALL "tempo frame [0-9]+: [0-9.]+ undo [0-9]+" tlines "${out}")
  list(LENGTH tlines nt)
  check("K-409 (${lay}): got 4 tempo readings (${nt})" nt EQUAL 4)
  list(GET tlines 0 tl0)
  list(GET tlines 1 tl1)
  list(GET tlines 2 tl2)
  list(GET tlines 3 tl3)
  string(REGEX MATCH "tempo frame [0-9]+: ([0-9.]+) undo ([0-9]+)" _ "${tl0}")
  set(u0 "${CMAKE_MATCH_2}")
  string(REGEX MATCH "tempo frame [0-9]+: ([0-9.]+) undo ([0-9]+)" _ "${tl1}")
  set(v1 "${CMAKE_MATCH_1}")
  set(u1 "${CMAKE_MATCH_2}")
  string(REGEX MATCH "tempo frame [0-9]+: ([0-9.]+) undo ([0-9]+)" _ "${tl2}")
  set(v2 "${CMAKE_MATCH_1}")
  set(u2 "${CMAKE_MATCH_2}")
  string(REGEX MATCH "tempo frame [0-9]+: ([0-9.]+) undo ([0-9]+)" _ "${tl3}")
  set(v3 "${CMAKE_MATCH_1}")
  set(u3 "${CMAKE_MATCH_2}")
  math(EXPR uexp1 "${u0} + 1")
  math(EXPR uexp3 "${u1} + 1")
  check("K-409 (${lay}): double-click commit sets 131.250 in 1 undo step (got ${v1}, undo ${u0}->${u1})"
        v1 STREQUAL "131.250" AND u1 EQUAL uexp1)
  check("K-409 (${lay}): Escape cancels without changing tempo or undo (got ${v2}, undo ${u2})"
        v2 STREQUAL "131.250" AND u2 EQUAL u1)
  check("K-409 (${lay}): right-click Type a value sets 142.750 in 1 undo step (got ${v3}, undo ${u2}->${u3})"
        v3 STREQUAL "142.750" AND u3 EQUAL uexp3)
endfunction()

# Shared K-409 script (frames 6..46): double-click -> 131.25 + Enter, double-click -> 99.5 + Escape,
# right-click -> Type a value... -> 142.75 + Enter.
set(TEMPO_SCRIPT
  "6 tempo\n8 sdlat tempoint\n10 sdldown 0\n11 sdlup 0\n13 sdldown 0\n14 sdlup 0\n17 text 131.25\n19 key enter\n21 tempo\n"
  "23 sdlat tempoint\n25 sdldown 0\n26 sdlup 0\n28 sdldown 0\n29 sdlup 0\n31 text 99.5\n33 key escape\n35 tempo\n"
  "37 sdlat tempoint\n39 sdldown 1\n40 sdlup 1\n42 sdlat tempotype\n44 sdldown 0\n45 sdlup 0\n48 text 142.75\n50 key enter\n52 tempo\n")
string(JOIN "" TEMPO_SCRIPT ${TEMPO_SCRIPT})

set(darkMode "\"theme.mode\": \"dark\", \"ui.darkTheme\": true")

# ============================================================================
# 1. Studio: K-409 double-click + Escape + right-click Type a value
# ============================================================================
run_app(studio_tempo studio "" "${TEMPO_SCRIPT}54 quit discard\n" 60 0)
verify_tempo_flow(studio "${studio_tempo_out}")

# ============================================================================
# 2. Tracker: K-409 double-click + Escape + right-click Type a value
# ============================================================================
run_app(tracker_tempo tracker "" "${TEMPO_SCRIPT}54 quit discard\n" 60 0)
verify_tempo_flow(tracker "${tracker_tempo_out}")

# ============================================================================
# 3. Smooth: K-409 + K-395B live theme round-trip
# ============================================================================
run_app(logic_all logic "${darkMode}"
  "${TEMPO_SCRIPT}54 theme light\n57 theme brightness -1\n60 theme whitetext on\n63 theme dark\n66 theme brightness 0\n69 theme whitetext off\n"
  80 75)
verify_tempo_flow(logic "${logic_all_out}")
if(NOT logic_all_err MATCHES "THEME_DUMP frame [0-9]+ ([^\n]*)")
  message(FATAL_ERROR "FAIL: K-395B (logic): no THEME_DUMP:\n${logic_all_err}")
endif()
set(td_logic "${CMAKE_MATCH_1}")
theme_key("${td_logic}" chrome tc_logic)
theme_key("${td_logic}" text tt_logic)
theme_key("${td_logic}" frame.dark tfd_logic)
check("K-395B (logic): live theme round-trip restores clean dark chrome (${tc_logic}) and frame.dark (${tfd_logic})"
      tc_logic STREQUAL "#262C30" AND tfd_logic EQUAL 1 AND NOT tt_logic STREQUAL tc_logic)

# ============================================================================
# 4. Clips: K-409 + K-395B live theme round-trip + initial track list
# ============================================================================
run_app(live_all live "${darkMode}"
  "${TEMPO_SCRIPT}54 plrows\n56 key tab\n60 livegrid\n62 theme light\n64 theme brightness -1\n66 theme whitetext on\n68 theme dark\n70 theme brightness 0\n72 theme whitetext off\n"
  82 78)
verify_tempo_flow(live "${live_all_out}")
if(NOT live_all_err MATCHES "THEME_DUMP frame [0-9]+ ([^\n]*)")
  message(FATAL_ERROR "FAIL: K-395B (live): no THEME_DUMP:\n${live_all_err}")
endif()
set(td_live "${CMAKE_MATCH_1}")
theme_key("${td_live}" chrome tc_live)
theme_key("${td_live}" text tt_live)
theme_key("${td_live}" frame.dark tfd_live)
check("K-395B (live): live theme round-trip restores clean dark chrome (${tc_live}) and frame.dark (${tfd_live})"
      tc_live STREQUAL "#262C30" AND tfd_live EQUAL 1 AND NOT tt_live STREQUAL tc_live)

names(c_rows0 "${live_all_out}" "plrow")
names(c_cols0 "${live_all_out}" "livegrid title")
list(LENGTH c_rows0 n0)
check("K-395A: initial Clips arrangement and grid match (${n0} tracks)" n0 GREATER_EQUAL 3 AND c_rows0 STREQUAL c_cols0)

# ============================================================================
# 5. Clips: K-395A consecutive track deletion + grid sync + undo
# ============================================================================
point(hx0 hy0 "${live_all_out}" "plrow" 0)
run_app(clips_del live ""
  "8 move ${hx0} ${hy0}\n10 down 0\n12 up 0\n15 key delete\n19 plrows\n22 key delete\n26 plrows\n29 key tab\n34 livegrid\n37 undo\n42 livegrid\n46 quit discard\n"
  52 0)

string(REGEX MATCHALL "plrows n [0-9]+[^\n]*(\nplrow [^\n]*)*" pl_blocks "${clips_del_out}")
list(LENGTH pl_blocks n_plb)
check("K-395A: captured two plrows snapshots after deletes (${n_plb})" n_plb EQUAL 2)
list(GET pl_blocks 0 pl_after1)
list(GET pl_blocks 1 pl_after2)
names(r_after1 "${pl_after1}" "plrow")
names(r_after2 "${pl_after2}" "plrow")
list(LENGTH r_after1 n1)
list(LENGTH r_after2 n2)
math(EXPR exp1 "${n0} - 1")
math(EXPR exp2 "${n0} - 2")
check("K-395A: first Delete removed a track in Clips (${n0} -> ${n1})" n1 EQUAL exp1)
check("K-395A: consecutive second Delete removed another track (${n1} -> ${n2})" n2 EQUAL exp2)

string(FIND "${clips_del_out}" "livegrid cols" lg_first)
string(FIND "${clips_del_out}" "livegrid cols" lg_last REVERSE)
check("K-395A: captured livegrid snapshots before and after undo" lg_first GREATER_EQUAL 0 AND lg_last GREATER lg_first)
math(EXPR lg_len "${lg_last} - ${lg_first}")
string(SUBSTRING "${clips_del_out}" ${lg_first} ${lg_len} lg_before_undo)
string(SUBSTRING "${clips_del_out}" ${lg_last} -1 lg_after_undo)
names(g_after2 "${lg_before_undo}" "livegrid title")
names(g_undo "${lg_after_undo}" "livegrid title")
list(LENGTH g_after2 ng2)
list(LENGTH g_undo ngu)
check("K-395A: clip grid matches arrangement after two deletes (${ng2})" g_after2 STREQUAL r_after2)
check("K-395A: undo restores deleted Clips track (${ng2} -> ${ngu})" ngu EQUAL exp1)
