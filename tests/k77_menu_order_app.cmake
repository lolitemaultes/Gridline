# Gridline: K-77, the order of the main menus and the context menus. Runs the app hidden (isolated
# config and HOME, off-screen 1440x2600 (tall: every row of the longest menu is drawn), SDL's dummy audio driver) once per menu with the menu pinned
# open (--menu <name>, ContextMenus.cpp drawDevMenu). The menu writes its text to stdout (ImGui's
# text log: one row per line, "(shortcut)" after a label, ">" after a submenu, "---" before a
# heading and a dashed line for a separator). For every menu:
#   1. the expected items are all there, in the expected order (other items may sit between them);
#   2. no separator or heading at the end, no plain separator at the start, never two in a row
#      (no empty section), and no item twice in the same menu.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k77_menu_order_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)  # a copy: never the fixture itself

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
      "Playlist" "Channel rack" "Piano roll" "Mixer" "Browser" "Plugin picker" "Project picker" "Channel settings" "Master chain" "Live pads" "Undo history" "Performance" "Zoom in" "Zoom out" "Close plugin windows" "Close all windows" "Bring all windows back into Gridline" "Window layout" "Reset window layout" "Dark theme" "Theme settings..." "Interface size" "Mixer layout" "Show played chords in the hint bar")
check(options
      "Audio settings..." "MIDI settings..." "Theme..." "Plugin manager..." "Song mode" "Live mode" "Arrangements" "Song position jumps" "Metronome" "Plugin delay compensation" "Refreeze out-of-date freezes" "Hibernate unused plugins automatically" "Woken plugins sleep again after" "Send MIDI clock (master sync)" "Record notes" "Step record" "Record automation" "Recording options" "Typing keyboard to piano" "Typing keyboard options" "Multilink to controllers" "Auto scrolling" "Auto scrolling in pattern editors" "Smooth snapping")
check(tools
      "Stop all sound" "Tempo tapper" "Last moved control" "Control moved before it" "Loop starter..." "Chord progression" "Riff from progression" "Piano roll tools" "Piano roll selection" "Open the last piano roll script again" "Save the note clipboard as a MIDI file" "Project scripts" "Render channel to sampled instrument..." "Audio logger" "Cancel recording" "Score logger" "Route channels to free mixer tracks" "Route every channel to its own mixer track" "Select unused channels" "Unmute all channels" "Unsolo all mixer tracks" "Unfreeze all" "Hibernate unused plugins" "Wake all" "Lock every playlist track" "Unlock every playlist track" "Prepare the live area" "Audio clip channels" "Declicking of the selected channels" "Close plugin windows" "Trim unused mixer tracks" "Delete unused automation clips" "Delete unused patterns" "AI assistant activity")
check(help
      "Keyboard shortcuts" "Load demo song" "Track Receipt..." "Show settings folder" "About Gridline")
check(channel
      "Piano roll" "Channel settings" "Graph editor" "Keyboard editor" "Edit sample in Loupe" "Cut" "Copy" "Paste" "Clone" "Clone with controller links" "Insert instrument below" "Fill every step" "Fill every 2 steps" "Fill every 4 steps" "Fill every 8 steps" "Fill offbeats" "Advanced fill..." "Rotate left" "Rotate right" "Steps" "Repeat the first bar of steps" "Bake note effects into the pattern" "Steps to piano roll" "Piano roll to steps" "Step loop" "Key range" "Truncate swung steps" "Replace instrument" "Route to mixer track" "Route to a free mixer track" "Freeze" "Hibernate" "Render to sampled instrument..." "Export sample as WAV..." "Open the sample in a new channel" "Rename..." "Colour" "Random colour" "Icon" "Mute" "Cut itself" "Zip" "Move up" "Move down" "Select in playlist" "Clear steps and notes" "Delete")
check(rack
      "Add channel" "Clone selected" "Copy selected to a new pattern" "Move selected to a new pattern" "Create layer from selected" "Fill selected" "Transpose selected" "Swing mix for selected" "Colour selected" "Mute selected" "Unmute selected" "Cut itself (selected)" "Zip selected" "Route selected to free mixer tracks" "Freeze selected" "Hibernate selected" "Select their mixer tracks" "Move selected up" "Move selected down" "Delete selected" "Select all" "Select unused channels" "Sort by" "Unzip all" "Auto scrolling in pattern editors")
check(pattern
      "Insert one before" "Clone" "Rename..." "Colour" "Random colour" "Length" "Transpose" "Split by channel" "Select in playlist" "Automation clips" "Move up" "Move down" "Delete")
check(track
      "Insert one above" "Clone" "Add 8 tracks" "Rename..." "Colour" "Random colour" "Icon" "Name after its first clip" "Size" "Reset name, colour and icon" "Mute" "Lock" "Live settings" "Track mode" "Group" "Group with the track above" "Auto colour group" "Collapse grouped tracks" "Select all clips on track" "Send" "Merge pattern clips" "Mute all clips" "Unmute all clips" "Move up" "Move down" "Delete all clips on track" "Delete track")
check(clip
      "Edit pattern" "Duplicate" "Make unique" "Split at pattern repeats" "Slice at the song position" "Chop" "Split by channel" "Merge selected clips" "Fit length to pattern" "Rename pattern..." "Pattern colour" "Select all clips of this pattern" "Delete")
check(playlist
      "Edit" "Select" "Time selection" "Clip groups" "Time markers" "Arrangements" "Live area" "Tool" "Snap" "Zoom" "View" "Add 8 tracks" "Collapse all grouped tracks" "Expand all grouped tracks" "Sort patterns" "Reset empty track names and colours" "Delete unused patterns")
check(piano
      "File" "Edit" "Select" "Tools" "Chord progression" "Riff from progression" "Scripts" "Tool" "Snap" "Stamp" "View")
check(mixer
      "Insert one" "Insert multiple..." "Insert from state..." "Rename, colour and icon" "File" "Mute" "Solo" "Disable track" "Freeze" "Hibernate" "Make mono" "Swap left and right" "Reverse polarity" "Stereo separation" "Render selected tracks to wave files..." "Channel routing" "Track routing" "Send to" "Route only to" "Select" "Unroute tracks routed to this track" "Reset routing to master" "Create group..." "Insert effect" "Effect chain" "Dock to" "Move left" "Move right" "Allow threaded processing" "Track latency" "Reset track" "Trim unused tracks at the end" "Delete")
check(slot
      "Open editor" "Replace" "Presets and state" "Bypass" "Enable switch" "Mix" "Move up" "Move down" "Move to track" "Reset to default" "Remove")
check(plugin
      "Presets" "Load preset from file..." "Save preset to file..." "Copy state" "Paste state" "Randomize" "Reset to default" "Piano roll" "Replace instrument" "Route to a free mixer track" "Clone channel" "Rename..." "Colour" "Delete channel" "Close window")
check(record
      "Audio, into the playlist as audio clips" "Disarm tracks after recording" "Drop silent takes" "Name takes after the audio input" "Note recording" "Overdub (keep existing notes)" "Quantize note starts to the snap" "Count-in")
check(knob
      "Copy value" "Paste value")
# (the marker menu needs a song with markers: the fixture has none; its order is unchanged)

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} menu check(s) failed")
endif()
