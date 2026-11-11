# Gridline: K-77, the order of the main menus and the context menus. Runs the app hidden (isolated
# config and HOME, off-screen 1440x2600 (tall: every row of the longest menu is drawn), SDL's dummy audio driver) once per menu with the menu pinned
# open (--menu <name>, ContextMenus.cpp drawDevMenu). The menu writes its text to stdout (ImGui's
# text log: one row per line, "(shortcut)" after a label, ">" after a submenu, "---" before a
# heading and a dashed line for a separator). For every menu:
#   1. the expected items are all there, in the expected order (other items may sit between them);
#   2. no separator or heading at the end, no plain separator at the start, never two in a row
#      (no empty section), and no item twice in the same menu.
# K-158 (compact menus): the menu also writes its height ("MENUSIZE <name> <width> <height wanted>
# <height drawn>") and its whole tree, every submenu entered ("MENUTREE": "Parent > Item" per row):
#   3. no menu is taller than 860 px at 100 % interface size (it would scroll in a 1440x900 window);
#      a context menu over 650 px (the goal: a windowed 1440x900 or a 1366x768 screen) is reported;
#   4. rows inside submenus are where they belong (checkTree), and every row the menu had before
#      the menus were made compact (tests/k158_menu_rows_before.txt) is still in it somewhere.
# The song is a copy of the test song plus a sampler channel playing a WAV file with an audio clip,
# and playlist track 8 as an audio track, so the longest variants of the menus are measured.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> [-DBEFORE=<rows file>] -P k77_menu_order_app.cmake
cmake_minimum_required(VERSION 3.19)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)  # a copy: never the fixture itself
string(JSON nch LENGTH "${demo}" channels)
string(JSON demo SET "${demo}" channels ${nch} "{\"color\": 5219296, \"id\": 60, \"mixerTrack\": 0, \"name\": \"Take\", \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}")
string(JSON ncl LENGTH "${demo}" playlist)
string(JSON demo SET "${demo}" playlist ${ncl} "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 384, \"length\": 200, \"pattern\": 0, \"gain\": 1.0}")
string(JSON mix2 GET "${demo}" mixer 2 id)
string(JSON demo SET "${demo}" tracks 7 mode 1)
string(JSON demo SET "${demo}" tracks 7 mixer ${mix2})
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")
set(kHardMax 860)   # px: taller scrolls in a full-screen 1440x900 window
set(kGoal 650)      # px: context menus fit a windowed 1440x900 and a 1366x768 screen

set(failures 0)
function(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR f "${failures} + 1")
  set(failures ${f} PARENT_SCOPE)
endfunction()

# check(<menu> <expected item>...): the labels must appear in this order (each matched at a row start).
function(check name)
  file(REMOVE_RECURSE "${OUT}/cfg_${name}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${name}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${name}")
  set(ENV{HOME} "${OUT}/cfg_${name}/home")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x2600 --frames 12
                          --menu ${name} --screenshot "${OUT}/${name}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${name}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    fail("${name}: gridline-app failed (${rc}): ${err}")
    set(failures ${failures} PARENT_SCOPE)
    return()
  endif()
  string(FIND "${out}" "MENUDUMP BEGIN ${name}\n" b)
  string(FIND "${out}" "\nMENUDUMP END" e)
  if(b LESS 0 OR e LESS b)
    fail("${name}: no menu dump in the output")
    set(failures ${failures} PARENT_SCOPE)
    return()
  endif()
  string(LENGTH "MENUDUMP BEGIN ${name}\n" hl)
  math(EXPR from "${b} + ${hl}")
  math(EXPR len "${e} - ${from}")
  string(SUBSTRING "${out}" ${from} ${len} dump)
  file(WRITE "${OUT}/${name}.txt" "${dump}")
  # 3. height (K-158)
  if(out MATCHES "\nMENUSIZE ${name} ([0-9]+) ([0-9]+) ([0-9]+)")
    set(h ${CMAKE_MATCH_2})
    set_property(GLOBAL APPEND PROPERTY k158_sizes "${name} ${h}")
    if(h GREATER kHardMax)
      fail("${name}: ${h} px tall, over ${kHardMax} px: it scrolls at 1440x900")
    elseif(h GREATER kGoal AND NOT name MATCHES "^(file|edit|add|patterns|view|options|help|layouts|pianotools)$")
      message(STATUS "note - ${name}: ${h} px, over the ${kGoal} px goal for context menus")
    endif()
  else()
    fail("${name}: no MENUSIZE line")
  endif()
  # the tree (K-158), kept for checkTree and the rows-before check
  string(FIND "${out}" "MENUTREE BEGIN ${name}\n" tb)
  string(FIND "${out}" "MENUTREE END" te)
  if(tb LESS 0 OR te LESS tb)
    fail("${name}: no menu tree in the output")
  else()
    string(LENGTH "MENUTREE BEGIN ${name}\n" tl)
    math(EXPR tf "${tb} + ${tl}")
    math(EXPR tn "${te} - ${tf}")
    string(SUBSTRING "${out}" ${tf} ${tn} tree)
    file(WRITE "${OUT}/${name}.tree" "${tree}")
  endif()
  # Rows: trimmed, empty ones dropped; "|" marks a separator or a heading.
  string(REPLACE ";" "," dump "${dump}")
  string(REPLACE "\n" ";" lines "${dump}")
  set(rows "")
  foreach(l IN LISTS lines)
    string(STRIP "${l}" l)
    if(l STREQUAL "")
      continue()
    endif()
    if(l MATCHES "^-----")
      list(APPEND rows "|sep")
    elseif(l MATCHES "^---(.*)$")
      list(APPEND rows "|head ${CMAKE_MATCH_1}")
    else()
      list(APPEND rows "${l}")
    endif()
  endforeach()
  list(LENGTH rows n)
  if(n EQUAL 0)
    fail("${name}: the menu is empty")
    set(failures ${failures} PARENT_SCOPE)
    return()
  endif()
  # 2. separators: never first (a heading may be: a context menu's title), never last, never two in a row.
  list(GET rows 0 first)
  math(EXPR lastI "${n} - 1")
  list(GET rows ${lastI} last)
  if(first STREQUAL "|sep")
    fail("${name}: starts with a separator")
  endif()
  if(last MATCHES "^\\|")
    fail("${name}: ends with a separator or heading (${last})")
  endif()
  set(prev "")
  set(seen "")
  foreach(r IN LISTS rows)
    if(r MATCHES "^\\|" AND prev MATCHES "^\\|")
      fail("${name}: two separators in a row (${prev}, ${r}): an empty section")
    endif()
    if(NOT r MATCHES "^\\|")
      string(REGEX REPLACE " *\\(.*\\)$" "" key "${r}")  # without the shortcut
      list(FIND seen "${key}" dup)
      if(dup GREATER -1)
        fail("${name}: \"${key}\" is listed twice")
      endif()
      list(APPEND seen "${key}")
    endif()
    set(prev "${r}")
  endforeach()
  # 1. expected order.
  set(at 0)
  foreach(want IN LISTS ARGN)
    set(found -1)
    set(i 0)
    foreach(r IN LISTS rows)
      if(i GREATER_EQUAL at)
        string(LENGTH "${want}" wl)
        string(SUBSTRING "${r}" 0 ${wl} head)
        if(head STREQUAL want)
          set(found ${i})
          break()
        endif()
      endif()
      math(EXPR i "${i} + 1")
    endforeach()
    if(found LESS 0)
      fail("${name}: \"${want}\" missing or out of order\n${dump}")
      set(failures ${failures} PARENT_SCOPE)
      return()
    endif()
    math(EXPR at "${found} + 1")
  endforeach()
  message(STATUS "ok - ${name}: ${n} rows, order and separators")
  set(failures ${failures} PARENT_SCOPE)
endfunction()

# checkTree(<menu> <row>...): rows of the menu's tree ("Parent > Item", a submenu ends in " >"),
# each matched at a line start, in this order (other rows may sit between them).
function(checkTree name)
  if(NOT EXISTS "${OUT}/${name}.tree")
    fail("${name}: no tree to check")
    set(failures ${failures} PARENT_SCOPE)
    return()
  endif()
  file(STRINGS "${OUT}/${name}.tree" lines)
  set(at 0)
  foreach(want IN LISTS ARGN)
    string(LENGTH "${want}" wl)
    set(found -1)
    set(i 0)
    foreach(l IN LISTS lines)
      if(i GREATER_EQUAL at)
        string(SUBSTRING "${l}" 0 ${wl} head)
        if(head STREQUAL want)
          set(found ${i})
          break()
        endif()
      endif()
      math(EXPR i "${i} + 1")
    endforeach()
    if(found LESS 0)
      fail("${name}: tree row \"${want}\" missing or out of order")
      set(failures ${failures} PARENT_SCOPE)
      return()
    endif()
    math(EXPR at "${found} + 1")
  endforeach()
  message(STATUS "ok - ${name}: submenu rows")
  set(failures ${failures} PARENT_SCOPE)
endfunction()

# Expected orders (K-77 plan: analysis notes PLAN.md). Items between them are allowed.
check(file
      "New" "New from template" "New from loop starter..." "Open..." "Open recent" "Welcome screen" "Save" "Save as..." "Save new version" "Save as template..." "Save to a new project folder..." "Revert to last backup" "Import" "Export" "Locate missing samples..." "New collaborative project..." "Collaboration..." "Project info..." "Quit")
check(edit
      "Undo" "Redo" "Undo history" "Cut" "Copy" "Paste" "Duplicate" "Delete" "Select all" "Deselect" "Rename...")
check(add
      "Instrument channel" "Pattern" "Effect on" "Automation clip for the last moved control" "Clip of the current pattern at the song end" "Mixer track" "8 playlist tracks")
check(patterns
      "New pattern" "Insert one before" "Clone" "Find first empty" "Find next empty" "Go to" "Next pattern" "Previous pattern" "Rename..." "Colour" "Random colour" "Length" "Transpose" "Split by channel" "Select in playlist" "Pattern groups" "Move up" "Move down" "Render pattern to file..." "Render as audio clip..." "Render and replace..." "Delete" "Delete unused patterns")
check(view
      "Playlist" "Channel rack" "Piano roll" "Mixer" "Browser" "Plugin picker" "Project picker" "Channel settings" "Master chain" "Live pads" "Undo history" "Performance" "Zoom in" "Zoom out" "Close plugin windows" "Close all windows" "Bring all windows back into Gridline" "Window layout" "Reset window layout" "Dark theme" "White text and glyphs" "Theme settings..." "Interface size" "Mixer layout" "Show played chords in the hint bar")
check(options
      "Audio settings..." "MIDI settings..." "Theme..." "Plugin manager..." "Song mode" "Live mode" "Arrangements" "Song position jumps" "Metronome" "Plugin delay compensation" "Refreeze out-of-date freezes" "Hibernate unused plugins automatically" "Woken plugins sleep again after" "Send MIDI clock (master sync)" "Record notes" "Step record" "Record automation" "Recording options" "Typing keyboard to piano" "Typing keyboard options" "Multilink to controllers" "Auto scrolling" "Auto scrolling in pattern editors" "Smooth snapping")
check(tools
      "Stop all sound" "Tempo tapper" "Last moved control" "Control moved before it" "Loop starter..." "Swing presets" "Chord progression" "Riff from progression" "Piano roll tools" "Composing" "Render channel to sampled instrument..." "Audio logger" "Cancel recording" "Score logger" "Route channels to free mixer tracks" "Unmute all channels" "Unsolo all mixer tracks" "Close plugin windows" "Macros" "Freeze and hibernate" "Clean up" "AI assistant activity")
checkTree(tools
      "Composing > Piano roll selection >" "Composing > Open the last piano roll script again" "Composing > Save the note clipboard as a MIDI file" "Composing > Project scripts >"
      "Macros > Route every channel to its own mixer track" "Macros > Select unused channels" "Macros > Lock every playlist track" "Macros > Unlock every playlist track" "Macros > Prepare the live area" "Macros > Audio clip channels >" "Macros > Declicking of the selected channels >"
      "Freeze and hibernate > Unfreeze all" "Freeze and hibernate > Hibernate unused plugins" "Freeze and hibernate > Wake all"
      "Clean up > Trim unused mixer tracks" "Clean up > Delete unused automation clips" "Clean up > Delete unused patterns")
check(help
      "Keyboard shortcuts" "Load demo song" "Track Receipt..." "Show settings folder" "About Gridline")
check(channel
      "Piano roll" "Channel settings" "Graph editor" "Keyboard editor" "Cut" "Copy" "Paste" "Clone" "Clone with controller links" "Insert instrument below" "Fill" "Rotate left" "Rotate right" "Steps" "Replace instrument" "Route to mixer track" "Route to a free mixer track" "Instrument and routing" "Rename..." "Colour" "Mute" "Properties" "Move up" "Move down" "Select in playlist" "Clear steps and notes" "Delete")
checkTree(channel
      "Fill > Every step" "Fill > Every 2 steps" "Fill > Every 4 steps" "Fill > Every 8 steps" "Fill > Every 4 from step 3" "Fill > Every 8 from step 5" "Fill > Every 2 from step 2" "Fill > Advanced fill..."
      "Steps > Shift left" "Steps > Shift right" "Steps > Reverse" "Steps > Invert" "Steps > Randomize..." "Steps > Insert a step at the start" "Steps > Delete the first step" "Steps > Repeat the first bar of steps" "Steps > Bake note effects into the pattern" "Steps > Steps to piano roll" "Steps > Piano roll to steps" "Steps > Step loop >" "Steps > Truncate swung steps" "Steps > Reset step parameters"
      "Instrument and routing > Make an instrument track for it" "Instrument and routing > Freeze" "Instrument and routing > Hibernate" "Instrument and routing > Render to sampled instrument..." "Instrument and routing > Key range >"
      "Properties > Random colour" "Properties > Icon >" "Properties > Cut itself" "Properties > Zip")
check(chsample
      "Piano roll" "Channel settings" "Graph editor" "Keyboard editor" "Edit sample in Loupe" "Cut" "Copy" "Paste" "Clone" "Clone with controller links" "Insert instrument below" "Fill" "Rotate left" "Rotate right" "Steps" "Replace instrument" "Route to mixer track" "Route to a free mixer track" "Instrument and routing" "Rename..." "Colour" "Mute" "Properties" "Move up" "Move down" "Select in playlist" "Clear steps and notes" "Delete")
checkTree(chsample
      "Instrument and routing > Render to sampled instrument..." "Instrument and routing > Export sample as WAV..." "Instrument and routing > Open the sample in a new channel >" "Instrument and routing > Key range >")
check(rack
      "Add channel" "Clone selected" "Fill selected" "Transpose selected" "Colour selected" "Mute selected" "Unmute selected" "Zip selected" "Route selected to free mixer tracks" "Selected channels" "Move selected up" "Move selected down" "Delete selected" "Select all" "Select unused channels" "Sort by" "Unzip all" "All channels" "Auto scrolling in pattern editors")
checkTree(rack
      "Selected channels > Copy selected to a new pattern" "Selected channels > Move selected to a new pattern" "Selected channels > Create layer from selected" "Selected channels > Swing mix for selected >" "Selected channels > Swing mix for selected (type in)..." "Selected channels > Declicking of selected >" "Selected channels > Step loops >" "Selected channels > Truncate swung steps >" "Selected channels > Cut itself (selected)" "Selected channels > Freeze selected" "Selected channels > Hibernate selected" "Selected channels > Select their mixer tracks"
      "All channels > Audio clip channels >" "All channels > Automation clips in this pattern >")
check(pattern
      "Insert one before" "Clone" "Rename..." "Colour" "Random colour" "Length" "Transpose" "Split by channel" "Select in playlist" "Automation clips" "Move up" "Move down" "Delete")
check(track
      "Insert one above" "Clone" "Add 8 tracks" "Rename..." "Colour" "Properties" "Mute" "Lock" "Live settings" "Group" "Track mode" "Audio input" "Select all clips on track" "Clips" "Move up" "Move down" "Delete all clips on track" "Delete track")
checkTree(track
      "Properties > Random colour" "Properties > Icon >" "Properties > Name after its first clip" "Properties > Size >" "Properties > Reset name, colour and icon"
      "Group > Hold the next 1 track" "Group > Group with the track above" "Group > Auto colour group" "Group > Collapse grouped tracks" "Group > Fold" "Group > Ungroup"
      "Clips > Send" "Clips > Merge pattern clips" "Clips > Mute all clips" "Clips > Unmute all clips")
check(clip
      "Edit pattern" "Duplicate" "Make unique" "Split at pattern repeats" "Slice at the song position" "Chop" "Split by channel" "Merge selected clips" "Fit length to pattern" "Rename pattern..." "Pattern colour" "Select all clips of this pattern" "Delete")
check(arrclip
      "Mute clip" "Loop" "Rename..." "Edit in Loupe" "Export clip as WAV..." "Reverse" "Channel settings" "Audio" "Clip properties..." "Time stretch" "Render clip to audio (consolidate)" "Bounce this track to audio")
checkTree(arrclip
      "Audio > Follow the project tempo" "Audio > Fit to tempo" "Audio > Normalize" "Audio > Make unique as sample" "Audio > Pitch a semitone up" "Audio > Pitch a semitone down")
check(playlist
      "Edit" "Select" "Time selection" "Clip groups" "Time markers" "Arrangements" "Live area" "Tool" "Snap" "Zoom" "View" "Add 8 tracks" "Collapse all grouped tracks" "Expand all grouped tracks" "Sort patterns" "Reset empty track names and colours" "Delete unused patterns")
check(piano
      "File" "Edit" "Select" "Tools" "Chord progression" "Riff from progression" "Scripts" "Tool" "Snap" "Stamp" "View")
check(mixer
      "Insert one" "Insert multiple..." "Rename, colour and icon" "File" "Mute" "Solo" "Solo alone" "Disable track" "Arm for disk recording" "Record from" "Stereo and polarity" "Render selected tracks to wave files..." "Send to" "Route only to" "Routing" "Group" "Insert effect" "Effect chain" "Dock to" "CPU" "Move left" "Move right" "Reset track" "Trim unused tracks at the end" "Delete")
checkTree(mixer
      "File > Open mixer track state..." "File > Save mixer track state as..." "File > Insert from state..."
      "Stereo and polarity > Make mono" "Stereo and polarity > Swap left and right" "Stereo and polarity > Reverse polarity" "Stereo and polarity > Stereo separation >"
      "Routing > Channel routing >" "Routing > Track routing >" "Routing > Select >" "Routing > Make an audio track for it" "Routing > Unroute tracks routed to this track" "Routing > Reset routing to master"
      "Group > Create group..." "Group > Add to group >" "Group > Remove from group" "Group > Auto colour group"
      "CPU > Freeze" "CPU > Hibernate" "CPU > Allow threaded processing" "CPU > Track latency >")
check(slot
      "Open editor" "Replace" "Presets and state" "Bypass" "Enable switch" "Mix" "Move up" "Move down" "Move to track" "Reset to default" "Remove")
check(plugin
      "Presets" "Load preset from file..." "Save preset to file..." "Copy state" "Paste state" "Randomize" "Reset to default" "Piano roll" "Replace instrument" "Route to a free mixer track" "Clone channel" "Rename..." "Colour" "Delete channel" "Close window")
check(record
      "Audio, into the playlist as audio clips" "Disarm tracks after recording" "Drop silent takes" "Name takes after the audio input" "Note recording" "Overdub (keep existing notes)" "Quantize note starts to the snap" "Count-in")
check(knob
      "Copy value" "Paste value")
# (the marker menu needs a song with markers: the fixture has none; its order is unchanged)
# The rest only for height and tree (K-158).
foreach(n audioclip pianotools pianoselect typing chop timesel audiosource patternsource markers layouts)  # (no automation clip in the song: autosource is empty)
  check(${n})
endforeach()

# 4. every row the menus had before they were made compact is still in them (K-158). The rows file:
# "<menu>|<row label>" per line, taken from the menus before the change. A few rows were renamed or
# are hidden where they can never apply (only one of the make / drop pair, the sample rows without a
# sample, Disarm with nothing armed); they are listed here.
set(kGone "channel|Fill every step" "channel|Fill every 2 steps" "channel|Fill every 4 steps" "channel|Fill every 8 steps"
          "channel|Fill offbeats" "channel|Drop its instrument track link" "channel|Edit sample in Loupe" "channel|Export sample as WAV..."
          "channel|Open the sample in a new channel" "channel|Granular" "channel|Kerf" "mixer|Disarm selected tracks" "mixer|Drop its audio track link"
          # moved: into the clip menu's Audio submenu (checked in the arrclip tree above)
          "audioclip|Fit to tempo" "audioclip|Normalize")
if(BEFORE AND EXISTS "${BEFORE}")
  file(STRINGS "${BEFORE}" before)
  set(cur "")
  set(missing 0)
  set(checked 0)
  foreach(b IN LISTS before)
    if(NOT b MATCHES "^([a-z]+)\\|(.*)$")
      continue()
    endif()
    set(menu "${CMAKE_MATCH_1}")
    set(label "${CMAKE_MATCH_2}")
    if(label STREQUAL "Show in Finder" AND NOT CMAKE_HOST_APPLE)  # the rows file was taken on macOS; the label names the platform's file manager
      if(CMAKE_HOST_WIN32)
        set(label "Show in Explorer")
      else()
        set(label "Show in Files")
      endif()
    endif()
    if(NOT menu STREQUAL cur)
      set(cur "${menu}")
      set(segs "\n")
      if(EXISTS "${OUT}/${menu}.tree")
        file(STRINGS "${OUT}/${menu}.tree" tl)
        foreach(l IN LISTS tl)
          string(REGEX REPLACE " >$" "" l "${l}")
          string(REGEX REPLACE "^.* > " "" l "${l}")
          string(REGEX REPLACE "^ +" "" l "${l}")  # rows drawn with a leading swatch are padded with spaces
          string(APPEND segs "${l}\n")
        endforeach()
      endif()
    endif()
    list(FIND kGone "${menu}|${label}" gone)
    if(gone GREATER -1)
      continue()
    endif()
    math(EXPR checked "${checked} + 1")
    string(FIND "${segs}" "\n${label}\n" at)
    if(at LESS 0)
      fail("${menu}: \"${label}\" was in this menu before and is gone")
      math(EXPR missing "${missing} + 1")
    endif()
  endforeach()
  message(STATUS "ok - ${checked} rows from before, ${missing} missing")
else()
  fail("no rows-before file (-DBEFORE)")
endif()
get_property(sizes GLOBAL PROPERTY k158_sizes)
string(REPLACE ";" ", " sizes "${sizes}")
message(STATUS "menu heights (px): ${sizes}")

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} menu check(s) failed")
endif()
