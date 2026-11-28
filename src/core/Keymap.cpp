// Gridline - command / shortcut registry (see Keymap.h).
#include "Keymap.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include "I18n.h"
#include "Json.h"

namespace gl::keys {

using i18n::N_;

namespace {

const char* const kKeyNames[] = {
    "",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "Space", "Enter", "Esc", "Tab", "Backspace", "Ins", "Del", "Home", "End", "PgUp", "PgDn",
    "Up", "Down", "Left", "Right",
    "Num +", "Num -", "Num *", "Num /", "Num Enter",
    "Num 0", "Num 1", "Num 2", "Num 3", "Num 4", "Num 5", "Num 6", "Num 7", "Num 8", "Num 9",
    "-", "=", ",", ".", "/", ";", "'", "[", "]", "\\", "`",
};
static_assert(sizeof(kKeyNames) / sizeof(kKeyNames[0]) == static_cast<size_t>(Key::Count), "key name table");

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

Chord ch(Key k, uint8_t m = 0) { return Chord{k, m}; }
constexpr uint8_t C = kCtrl, S = kShift, A = kAlt, H = C | A | S;  // H: the free Ctrl+Alt+Shift space of the K-79 commands

std::vector<Command> buildCommands() {
  using K = Key;
  const Context G = Context::Global, PL = Context::Playlist, PR = Context::PianoRoll, CR = Context::ChannelRack,
                MX = Context::Mixer, AU = Context::Automation, ST = Context::Stage, WF = Context::Waveform;
  return {
      // ---- global: file ----
      {"file.new", G, N_("New project"), {ch(K::N, C)}},
      {"file.open", G, N_("Open project"), {ch(K::O, C)}},
      {"file.save", G, N_("Save"), {ch(K::S, C)}},
      {"file.saveAs", G, N_("Save as"), {ch(K::S, C | S)}},
      {"file.saveNewVersion", G, N_("Save new version"), {ch(K::S, C | A)}},
      {"file.render", G, N_("Render to audio file"), {ch(K::R, C)}},
      {"file.renderStems", G, N_("Render mixer tracks (stems)"), {ch(K::R, C | S)}},
      {"file.renderMarkerAreas", G, N_("Export marker areas"), {}},  // each marker to the next as its own file
      {"file.exportMidi", G, N_("Export song as MIDI file"), {ch(K::M, C | S)}},
      {"file.importUrl", G, N_("Import audio from a link (URL)"), {ch(K::U, C | S)}},  // J-192
      {"file.projectInfo", G, N_("Project info"), {ch(K::F11)}},
      {"file.quit", G, N_("Quit"), {ch(K::Q, C)}},
      // ---- global: edit ----
      {"edit.undo", G, N_("Undo"), {ch(K::Z, C)}},
      {"edit.redo", G, N_("Redo"), {ch(K::Y, C), ch(K::Z, C | S)}},
      {"edit.history", G, N_("Undo history"), {ch(K::Z, A), ch(K::Z, C | A)}},  // K-141: Alt+Z zips in the channel rack
      {"edit.rename", G, N_("Rename (pattern, channel or mixer track)"), {ch(K::F2)}},
      // ---- global: transport and switches ----
      {"transport.play", G, N_("Play / stop"), {ch(K::Space)}},
      {"transport.songMode", G, N_("Switch pattern / song mode"), {ch(K::L)}},
      {"transport.tapTempo", G, N_("Tap tempo"), {ch(K::T, C | S)}},  // K-48: every command has a default chord
      {"options.typingKeyboard", G, N_("Typing keyboard to piano"), {ch(K::T, C)}},
      {"options.metronome", G, N_("Metronome"), {ch(K::M, C)}},
      {"tools.stopSound", G, N_("Stop all sound"), {ch(K::H, C)}},
      // K-79: transport commands the other shortcut sets put on single keys.
      {"transport.record", G, N_("Record on / off"), {ch(K::R, H)}},
      {"transport.toStart", G, N_("Go to the start"), {ch(K::Home, H)}},
      {"transport.rewindBar", G, N_("Back one bar"), {ch(K::Left, H)}, true},
      {"transport.forwardBar", G, N_("Forward one bar"), {ch(K::Right, H)}, true},
      {"options.countIn", G, N_("Count-in on / off"), {ch(K::K, H)}},
      // K-112: the typing keyboard's octave picked directly. No keys outside the Tracker set
      // (it puts them on F1 to F8); anyone can give them keys in the shortcut editor.
      {"typing.octave1", G, N_("Typing keyboard octave 1"), {}},
      {"typing.octave2", G, N_("Typing keyboard octave 2"), {}},
      {"typing.octave3", G, N_("Typing keyboard octave 3"), {}},
      {"typing.octave4", G, N_("Typing keyboard octave 4"), {}},
      {"typing.octave5", G, N_("Typing keyboard octave 5"), {}},
      {"typing.octave6", G, N_("Typing keyboard octave 6"), {}},
      {"typing.octave7", G, N_("Typing keyboard octave 7"), {}},
      {"typing.octave8", G, N_("Typing keyboard octave 8"), {}},
      // ---- global: patterns ----
      {"pattern.new", G, N_("New pattern"), {ch(K::F4)}},
      {"pattern.findFirstEmpty", G, N_("Find first empty pattern"), {ch(K::F4, S)}},
      {"pattern.findNextEmpty", G, N_("Find next empty pattern"), {ch(K::F4, C)}},
      {"pattern.clone", G, N_("Clone pattern"), {ch(K::C, C | S)}},
      {"pattern.insert", G, N_("Insert pattern"), {ch(K::Insert, C | S)}},
      {"pattern.delete", G, N_("Delete pattern"), {ch(K::Delete, C | S)}},
      {"pattern.moveUp", G, N_("Move pattern up"), {ch(K::Up, C | S)}, true},
      {"pattern.moveDown", G, N_("Move pattern down"), {ch(K::Down, C | S)}, true},
      {"pattern.next", G, N_("Next pattern"), {ch(K::NumAdd)}, true},
      {"pattern.previous", G, N_("Previous pattern"), {ch(K::NumSubtract)}, true},
      // ---- global: arrangements (K-406) ----
      {"arrangement.previous", G, N_("Previous arrangement"), {ch(K::Left, C | A)}, true},
      {"arrangement.next", G, N_("Next arrangement"), {ch(K::Right, C | A)}, true},
      // ---- global: windows ----
      {"view.playlist", G, N_("Playlist"), {ch(K::F5)}},
      {"view.channelRack", G, N_("Channel rack"), {ch(K::F6)}},
      {"view.pianoRoll", G, N_("Piano roll"), {ch(K::F7)}},
      {"view.pluginPicker", G, N_("Plugin picker"), {ch(K::F8)}},
      {"view.browser", G, N_("Browser"), {ch(K::F8, A)}},
      {"view.mixer", G, N_("Mixer"), {ch(K::F9)}},
      {"view.lfoBoard", G, N_("LFO board"), {ch(K::F9, A)}},  // K-247
      {"view.stage", G, N_("Stage (place the mixer tracks in a room)"), {}},  // no default key (bind one in the shortcut editor)
      // K-106: the Tracker pages and header screens (keys in the Tracker set only).
      {"view.patternEditor", G, N_("Pattern editor"), {}},
      {"view.pads", G, N_("Pads"), {}},
      {"view.channelSettings", G, N_("Channel settings"), {}},
      {"view.audioEditor", G, N_("Audio editor"), {}},
      {"view.files", G, N_("Files"), {}},
      {"view.headerBand", G, N_("Header band"), {}},
      {"view.transposeBox", G, N_("Transpose box"), {}},
      {"view.advancedEdit", G, N_("Advanced edit"), {}},
      {"view.cleanUp", G, N_("Clean up box"), {}},
      {"view.midiBox", G, N_("MIDI box"), {}},
      {"view.sampleTools", G, N_("Sample tools box"), {}},
      // K-106 cp17: the Tracker's Sample editor (the instrument's samples in the lower band) and the
      // sideways zoom of the page in front (the pattern's tracks, the Song page's lanes or timeline).
      {"view.sampleEditor", G, N_("Sample editor"), {}},
      {"view.zoomTracksIn", G, N_("Wider tracks (zoom in)"), {}},
      {"view.zoomTracksOut", G, N_("Narrower tracks (zoom out)"), {}},
      {"view.zoomTracksFit", G, N_("Every track in view"), {}},
      {"help.trackerKeys", G, N_("Tracker help"), {}},
      {"view.settings", G, N_("Settings (MIDI page)"), {ch(K::F10)}},
      {"view.backToPlaylist", G, N_("Back to the playlist (from an editor or plugin window)"), {ch(K::Escape)}},  // K-124
      {"view.closeAll", G, N_("Close all windows"), {ch(K::F12)}},
      {"view.closePlugins", G, N_("Close plugin windows"), {ch(K::F12, A)}},
      // K-112: no default key (Studio as before); the Live set gives it Ctrl+Alt+P, the Tracks set (K-112b) V.
      {"view.hidePlugins", G, N_("Hide or show plugin windows (they stay open)"), {}},
      // K-297: no default key (Studio as before); the Tracks set (Smooth) gives it Cmd+W.
      {"window.close", G, N_("Close the front window"), {}},
      {"view.resetLayout", G, N_("Reset window layout"), {ch(K::H, C | S)}},
      // K-237: the layout panes' tab strips (Ctrl+Tab stays the window switcher).
      {"view.paneNextTab", G, N_("Next tab in the pane"), {ch(K::PageDown, C)}},
      {"view.panePrevTab", G, N_("Previous tab in the pane"), {ch(K::PageUp, C)}},
      {"view.reopenTab", G, N_("Reopen the last closed pane tab"), {ch(K::T, C | A)}},
      // K-237: workspaces of the layout. Ctrl+1 .. 9 where the shortcut set leaves them free.
      {"view.workspace1", G, N_("Workspace 1"), {ch(K::D1, C)}},
      {"view.workspace2", G, N_("Workspace 2"), {ch(K::D2, C)}},
      {"view.workspace3", G, N_("Workspace 3"), {ch(K::D3, C)}},
      {"view.workspace4", G, N_("Workspace 4"), {ch(K::D4, C)}},
      {"view.workspace5", G, N_("Workspace 5"), {ch(K::D5, C)}},
      {"view.workspace6", G, N_("Workspace 6"), {ch(K::D6, C)}},
      {"view.workspace7", G, N_("Workspace 7"), {ch(K::D7, C)}},
      {"view.workspace8", G, N_("Workspace 8"), {ch(K::D8, C)}},
      {"view.workspace9", G, N_("Workspace 9"), {ch(K::D9, C)}},
      {"view.workspaceNext", G, N_("Next workspace"), {ch(K::PageDown, C | A)}},
      {"view.workspacePrevious", G, N_("Previous workspace"), {ch(K::PageUp, C | A)}},
      {"help.shortcuts", G, N_("Keyboard shortcuts"), {ch(K::F1)}},
      // K-93: the command palette. Ctrl+K (Cmd+K on macOS) everywhere except the channel rack, whose
      // Ctrl+K stays the graph editor (the editor wins); Ctrl+Shift+P works there too. The logic set
      // keeps its own Cmd+K (typing keyboard), so the palette keeps Ctrl+Shift+P there.
      {"view.commandPalette", G, N_("Command palette: find and run any command, menu item, window, setting or plugin"),
       {ch(K::K, C), ch(K::P, C | S)}},
      // K-79: layout presets.
      {"view.toggleMain", G, N_("Switch the main view (playlist / pads)"), {ch(K::Tab, H)}},
      {"view.editors", G, N_("Editors in the bottom pane (show / hide)"), {ch(K::E, H)}},
      // K-112b: the Tracks layout's panes (no key outside its set; they do nothing in the other layouts).
      {"view.library", G, N_("Library pane (show / hide)"), {}},
      {"view.inspector", G, N_("Inspector pane (show / hide)"), {}},
      {"view.quickHelp", G, N_("Quick help pane (show / hide)"), {}},
      {"view.smartControls", G, N_("Smart controls (show / hide)"), {}},  // R01-07
      {"view.automation", G, N_("Automation of the selected tracks (show / hide)"), {}},  // R02-15
      {"view.globalTracks", G, N_("Global tracks (show / hide)"), {}},  // R01-15  // R02-08: a key in the Tracks set only
      {"playlist.toolMenu", G, N_("Tool menu"), {}},  // R01-18: T in the Tracks set
      {"view.toolbar", G, N_("Toolbar under the control bar (show / hide)"), {}},
      {"view.lists", G, N_("Lists in the right pane (show / hide)"), {}},
      {"view.notes", G, N_("Notes in the right pane (show / hide)"), {}},
      // K-112B (checkpoint 39): the Tracks layout's Track and Navigate menu commands (keys only in its set).
      {"track.newTracks", G, N_("New tracks..."), {}},
      {"track.duplicate", PL, N_("New track with the same settings"), {}},
      {"track.delete", PL, N_("Delete track"), {}},
      {"track.mute", PL, N_("Mute track"), {}},
      {"track.solo", PL, N_("Solo track"), {}},
      {"transport.cycle", G, N_("Cycle"), {}},
      // R02-13 / R02-15: the reference's Navigate commands (keys only in the Tracks set).
      {"transport.cycleSelection", G, N_("Set the cycle to the selected regions"), {}},
      {"transport.prevMarker", G, N_("Go to the previous marker"), {}},
      {"transport.nextMarker", G, N_("Go to the next marker"), {}},
      {"transport.goToPosition", G, N_("Go to position..."), {}},
      // Checkpoint 47: the reference's most used tracks-area keys (keys only in the Tracks set).
      {"playlist.prevRegion", PL, N_("Select the previous region"), {}, true},
      {"playlist.nextRegion", PL, N_("Select the next region"), {}, true},
      {"playlist.prevTrack", PL, N_("Select the previous track"), {}, true},
      {"playlist.nextTrack", PL, N_("Select the next track"), {}, true},
      {"playlist.nudgeLeft", PL, N_("Nudge the selected regions left"), {}, true},
      {"playlist.nudgeRight", PL, N_("Nudge the selected regions right"), {}, true},
      {"playlist.loopRegion", PL, N_("Loop the selected regions on or off"), {}},
      // K-112c: new tracks (no key outside the Clips set, which gives them Ctrl+T and Ctrl+Shift+T).
      {"track.newAudio", G, N_("New audio track"), {}},
      {"track.newMidi", G, N_("New MIDI track"), {}},
      // N47-11: the Clips set's return track, detail bands and Session record keys (no key elsewhere).
      {"track.newReturn", G, N_("New return track"), {}},
      {"clips.insertMidiClip", G, N_("Insert MIDI clip"), {}},  // N49-07: Clips only (Cmd+Shift+M)
      {"clips.captureScene", G, N_("Capture and insert scene"), {}},  // grid-1118: Clips only (Cmd+Shift+I)
      {"clips.backToArrangement", G, N_("Back to arrangement"), {}},  // grid-1118: Clips only (F10)
      // grid2-1118: Clips only (Cmd+1 .. 5): the Arrangement's and the clip editor's grid, and (Cmd+6 .. 0)
      // the global launch quantisation.
      {"clips.gridNarrower", G, N_("Narrow the grid"), {}},
      {"clips.gridWider", G, N_("Widen the grid"), {}},
      {"clips.gridTriplet", G, N_("Triplet grid"), {}},
      {"clips.gridSnap", G, N_("Snap to grid"), {}},
      {"clips.gridFixed", G, N_("Fixed grid"), {}},
      {"clips.quant16", G, N_("Global quantisation 1/16"), {}},
      {"clips.quant8", G, N_("Global quantisation 1/8"), {}},
      {"clips.quant4", G, N_("Global quantisation 1/4"), {}},
      {"clips.quantBar", G, N_("Global quantisation 1 bar"), {}},
      {"clips.quantNone", G, N_("Global quantisation off"), {}},
      {"track.group", G, N_("Group tracks"), {}},      // checkpoint 51: Clips only (Cmd+G)
      {"track.ungroup", G, N_("Ungroup tracks"), {}},  // checkpoint 51: Clips only (Cmd+Shift+G)
      {"view.detail", G, N_("Clip and device bands (show / hide)"), {}},
      {"view.io", G, N_("In/Out section (show / hide)"), {}},  // checkpoint 55: Clips only (Cmd+Alt+I)
      {"transport.sessionRecord", G, N_("Session record"), {}},
      // ---- playlist ----
      {"playlist.delete", PL, N_("Delete selected clips"), {ch(K::Delete), ch(K::Backspace)}},
      {"playlist.selectAll", PL, N_("Select all clips"), {ch(K::A, C)}},
      {"playlist.deselect", PL, N_("Deselect"), {ch(K::D, C)}},
      {"playlist.copy", PL, N_("Copy clips"), {ch(K::C, C)}},
      {"playlist.cut", PL, N_("Cut clips"), {ch(K::X, C)}},  // R02-22; K-297 (round 8): Studio and Clips too (Edit > Cut shows it)
      {"playlist.paste", PL, N_("Paste clips"), {ch(K::V, C)}},
      {"playlist.duplicate", PL, N_("Duplicate selected clips"), {ch(K::B, C)}},
      {"playlist.merge", PL, N_("Merge selected clips"), {ch(K::G, C)}},
      {"playlist.mute", PL, N_("Mute / unmute selected clips"), {ch(K::M)}},
      {"playlist.toolDraw", PL, N_("Draw tool"), {ch(K::P)}},
      {"playlist.toolPaint", PL, N_("Paint tool"), {ch(K::B)}},
      {"playlist.toolDelete", PL, N_("Delete tool"), {ch(K::D)}},
      {"playlist.toolMute", PL, N_("Mute tool"), {ch(K::T)}},
      {"playlist.toolSlip", PL, N_("Slip tool"), {ch(K::S)}},
      {"playlist.toolSlice", PL, N_("Slice tool"), {ch(K::C)}},
      {"playlist.toolSelect", PL, N_("Select tool"), {ch(K::E)}},
      {"playlist.toolZoom", PL, N_("Zoom tool"), {ch(K::Z)}},
      {"playlist.toolPlay", PL, N_("Playback tool"), {ch(K::Y)}},
      {"playlist.toolWarp", PL, N_("Warp tool"), {ch(K::W)}},  // K-85
      {"wave.warpMode", WF, N_("Warp mode (audio clip editor, sampler, Kerf, pads, Loupe)"), {ch(K::W)}},  // K-248
      {"wave.spectrum", WF, N_("Spectrum view (Loupe, audio clip editor)"), {ch(K::W, S)}},  // K-248: was W in Loupe
      {"playlist.marker", PL, N_("Add a marker at the song position"), {ch(K::T, A)}},
      {"playlist.loopMarkers", PL, N_("Place loop: around the time selection, else a loop marker at the song position"), {ch(K::T, S)}},
      {"playlist.meterChange", PL, N_("Add a meter change at the song position"), {ch(K::T, A | S)}},  // K-179
      {"playlist.stretchResize", PL, N_("Resizing audio clips stretches them (on / off)"), {ch(K::M, S)}},
      {"playlist.fades", PL, N_("Show fade and volume handles on audio clips (on / off)"), {ch(K::F, S)}},
      {"playlist.zeroCross", PL, N_("Audio clip edits land on zero crossings (on / off)"), {ch(K::X, S)}},  // J-79 (FL has no key for it)
      {"playlist.picker", PL, N_("Picker panel"), {ch(K::P, A)}},
      {"playlist.quickZoom1", PL, N_("Quick zoom 1"), {ch(K::D1, S)}},
      {"playlist.quickZoom2", PL, N_("Quick zoom 2"), {ch(K::D2, S)}},
      {"playlist.quickZoom3", PL, N_("Quick zoom 3"), {ch(K::D3, S)}},
      {"playlist.zoomSelection", PL, N_("Zoom to the selection"), {ch(K::D4, S)}},
      {"playlist.splitAtSongPos", PL, N_("Split selected clips at the song position"), {ch(K::T, H)}},  // K-79
      // PL-13: chop, stutter and time selection editing.
      {"playlist.chopWindow", PL, N_("Chop and stutter window"), {ch(K::C, A)}},
      {"playlist.sliceGrid", PL, N_("Slice selected clips at the snap grid"), {ch(K::C, S)}},
      {"playlist.duplicateTime", PL, N_("Duplicate the time selection"), {ch(K::B, A)}},
      {"playlist.insertSpace", PL, N_("Insert space at the time selection"), {ch(K::Insert, C)}},
      {"playlist.deleteSpace", PL, N_("Delete the time selection's space"), {ch(K::Delete, C)}},
      {"playlist.consolidateTime", PL, N_("Consolidate the time selection to audio"), {ch(K::C, C | A)}},
      {"playlist.zoomIn", PL, N_("Zoom in"), {ch(K::PageUp)}, true},
      {"playlist.zoomOut", PL, N_("Zoom out"), {ch(K::PageDown)}, true},
      {"playlist.zoomFit", PL, N_("Zoom to fit the song"), {ch(K::Home)}},
      {"playlist.uniqueCopy", PL, N_("While Shift+dragging copies: make them unique (new patterns)"), {ch(K::V, S)}},  // Q-3 (FL: drag + Shift+V)
      {"playlist.cancelDrag", PL, N_("Cancel the clip drag (copies, moves, resizes)"), {ch(K::Escape)}},  // Q-3
      {"playlist.moveTrackUp", PL, N_("Move selected tracks up"), {ch(K::Up, A)}, true},      // GitHub #46
      {"playlist.moveTrackDown", PL, N_("Move selected tracks down"), {ch(K::Down, A)}, true},
      // ---- piano roll ----
      {"piano.delete", PR, N_("Delete selected notes"), {ch(K::Delete), ch(K::Backspace)}},
      {"piano.selectAll", PR, N_("Select all notes"), {ch(K::A, C)}},
      {"piano.deselect", PR, N_("Deselect"), {ch(K::D, C)}},
      {"piano.copy", PR, N_("Copy notes"), {ch(K::C, C)}},
      {"piano.cut", PR, N_("Cut notes"), {ch(K::X, C)}},
      {"piano.paste", PR, N_("Paste notes"), {ch(K::V, C)}},
      {"piano.duplicate", PR, N_("Duplicate notes"), {ch(K::B, C)}},
      {"piano.cancelDrag", PR, N_("Cancel the note drag (copies, moves, resizes)"), {ch(K::Escape)}},  // Q-3
      {"piano.quickQuantize", PR, N_("Quick quantize start times"), {ch(K::Q, C)}},
      {"piano.transposeUp", PR, N_("Transpose up a semitone"), {ch(K::Up)}, true},
      {"piano.transposeDown", PR, N_("Transpose down a semitone"), {ch(K::Down)}, true},
      {"piano.octaveUp", PR, N_("Transpose up an octave"), {ch(K::Up, S)}, true},
      {"piano.octaveDown", PR, N_("Transpose down an octave"), {ch(K::Down, S)}, true},
      {"piano.moveRight", PR, N_("Move notes right by the snap"), {ch(K::Right)}, true},
      {"piano.moveLeft", PR, N_("Move notes left by the snap"), {ch(K::Left)}, true},
      {"piano.legato", PR, N_("Legato"), {ch(K::L, C)}},
      {"piano.chopSteps", PR, N_("Chop into steps"), {ch(K::U, C)}},
      {"piano.glue", PR, N_("Glue notes"), {ch(K::G, C)}},
      {"piano.quantize", PR, N_("Quantize tool"), {ch(K::Q, A)}},
      {"piano.randomize", PR, N_("Randomize tool"), {ch(K::R, A)}},
      {"piano.chop", PR, N_("Chop tool"), {ch(K::U, A)}},
      {"piano.strum", PR, N_("Strum tool"), {ch(K::S, A)}},
      {"piano.arpeggiate", PR, N_("Arpeggiate tool"), {ch(K::A, A)}},
      {"piano.articulate", PR, N_("Articulate tool"), {ch(K::L, A)}},
      {"piano.limit", PR, N_("Limit key range tool"), {ch(K::K, A)}},
      {"piano.lfo", PR, N_("Note LFO tool"), {ch(K::O, A)}},
      {"piano.scaleLevels", PR, N_("Scale levels tool"), {ch(K::X, A)}},
      {"piano.flam", PR, N_("Grace notes tool"), {ch(K::G, A)}},
      {"piano.properties", PR, N_("Note properties"), {ch(K::N, A)}},
      {"piano.humanize", PR, N_("Humanize"), {ch(K::R, A | S)}},
      {"piano.invertSelection", PR, N_("Invert selection"), {ch(K::I, S)}},
      {"piano.selectRandom", PR, N_("Select random notes"), {ch(K::R, S)}},
      {"piano.toolDraw", PR, N_("Draw tool"), {ch(K::P)}},
      {"piano.toolPaint", PR, N_("Paint tool"), {ch(K::B)}},
      {"piano.toolDelete", PR, N_("Delete tool"), {ch(K::D)}},
      {"piano.toolSelect", PR, N_("Select tool"), {ch(K::E)}},
      {"piano.toolSlice", PR, N_("Slice tool"), {ch(K::C)}},
      {"piano.toolMute", PR, N_("Mute tool"), {ch(K::T)}},
      {"piano.toolPlay", PR, N_("Playback tool"), {ch(K::Y)}},
      {"piano.toolZoom", PR, N_("Zoom tool"), {ch(K::Z)}},
      {"piano.toolDetune", PR, N_("Detune tool"), {ch(K::U)}},
      {"piano.toolBend", PR, N_("Pitch bend tool"), {ch(K::N)}},  // K-25
      {"piano.toolVibrato", PR, N_("Vibrato tool"), {ch(K::V)}},  // K-262
      {"piano.slideMode", PR, N_("Slide mode (new notes are slide notes)"), {ch(K::S, S)}},
      {"piano.chordStamp", PR, N_("Chord stamp: pick a chord to stamp"), {ch(K::C, A)}},
      {"piano.zoomIn", PR, N_("Zoom in"), {ch(K::PageUp)}, true},
      {"piano.zoomOut", PR, N_("Zoom out"), {ch(K::PageDown)}, true},
      {"piano.zoomFit", PR, N_("Zoom to fit the pattern"), {ch(K::Home)}},
      // ---- channel rack ----
      {"rack.clone", CR, N_("Clone selected channels"), {ch(K::C, A)}},
      {"rack.cloneEmpty", CR, N_("Clone selected channels without their notes"), {ch(K::C, A | S)}},  // GitHub #74
      {"rack.randomColours", CR, N_("Random colours for selected channels"), {ch(K::C, C | A)}},
      {"rack.delete", CR, N_("Delete selected channels"), {ch(K::Delete, A), ch(K::Delete, S)}},
      {"rack.moveUp", CR, N_("Move selected channels up"), {ch(K::Up, A)}, true},
      {"rack.moveDown", CR, N_("Move selected channels down"), {ch(K::Down, A)}, true},
      {"rack.routeFree", CR, N_("Route selected channels to free mixer tracks"), {ch(K::L, C)}},
      {"rack.zip", CR, N_("Zip selected channels"), {ch(K::Z, A)}},
      {"rack.unzipAll", CR, N_("Unzip all channels"), {ch(K::U, A)}},
      {"rack.rotateLeft", CR, N_("Rotate steps left"), {ch(K::Left, C | S)}, true},
      {"rack.rotateRight", CR, N_("Rotate steps right"), {ch(K::Right, C | S)}, true},
      {"rack.copy", CR, N_("Copy"), {ch(K::C, C)}},
      {"rack.cut", CR, N_("Cut"), {ch(K::X, C)}},
      {"rack.paste", CR, N_("Paste"), {ch(K::V, C)}},
      {"rack.selectAll", CR, N_("Select all channels"), {ch(K::A, C)}},
      {"rack.graphEditor", CR, N_("Graph editor (step velocities)"), {ch(K::K, C)}},
      {"rack.insertStep", CR, N_("Insert a step (over a step)"), {ch(K::Insert)}},
      {"rack.deleteStep", CR, N_("Remove a step (over a step)"), {ch(K::Delete)}},
      // ---- mixer ----
      {"mixer.routeHere", MX, N_("Route selected channels to this track"), {ch(K::L, C)}},
      {"mixer.routeFromHere", MX, N_("Route selected channels from this track on"), {ch(K::L, C | S)}},
      {"mixer.moveLeft", MX, N_("Move track left"), {ch(K::Left, A)}, true},
      {"mixer.moveRight", MX, N_("Move track right"), {ch(K::Right, A)}, true},
      {"mixer.solo", MX, N_("Solo selected track"), {ch(K::S)}},
      {"mixer.renderArmed", MX, N_("Render armed tracks"), {ch(K::R, A)}},
      {"mixer.renderSelected", MX, N_("Render selected tracks"), {ch(K::R, A | S)}},
      {"mixer.waveforms", MX, N_("Show waveforms"), {ch(K::W, A)}},
      {"mixer.selectAll", MX, N_("Select all insert tracks"), {ch(K::A, C)}},
      {"mixer.deselect", MX, N_("Deselect tracks (keeps the focused one)"), {ch(K::D, C)}},
      {"mixer.selectRoutedChannels", MX, N_("Select the channels routed to the focused track"), {ch(K::L, A)}},
      {"mixer.selectRoutedTo", MX, N_("Select the tracks routed to the focused track"), {ch(K::L, C | A)}},
      // ---- Stage (app/StageView.cpp): the arrows take Shift (fine) and Ctrl (coarse) as they are pressed ----
      {"stage.left", ST, N_("Pan the selected tracks left"), {ch(K::Left)}, true},
      {"stage.right", ST, N_("Pan the selected tracks right"), {ch(K::Right)}, true},
      {"stage.up", ST, N_("Move the selected tracks one row further back"), {ch(K::Up)}, true},
      {"stage.down", ST, N_("Move the selected tracks one row nearer"), {ch(K::Down)}, true},
      {"stage.levelUp", ST, N_("Level of the selected tracks up"), {ch(K::Up, A)}, true},
      {"stage.levelDown", ST, N_("Level of the selected tracks down"), {ch(K::Down, A)}, true},
      {"stage.narrower", ST, N_("Narrower (stereo width)"), {ch(K::LeftBracket)}, true},
      {"stage.wider", ST, N_("Wider (stereo width)"), {ch(K::RightBracket)}, true},
      {"stage.next", ST, N_("Focus the next track"), {ch(K::Tab)}, true},
      {"stage.previous", ST, N_("Focus the previous track"), {ch(K::Tab, S)}, true},
      {"stage.mute", ST, N_("Mute the selected tracks"), {ch(K::M)}},
      {"stage.solo", ST, N_("Solo the selected tracks"), {ch(K::S)}},
      {"stage.reset", ST, N_("Reset the position of the selected tracks"), {ch(K::D0)}},
      // K-297 (round 9): no key of its own: F2 (Edit > Rename, every set's rename key) renames the
      // focused track here too. Enter opened a rename prompt whenever the mixer in its Stage view had
      // the keys (Studio and Clips), where Enter renames nothing else.
      {"stage.rename", ST, N_("Rename the focused track"), {}},
      {"stage.selectAll", ST, N_("Select every track shown"), {ch(K::A, C)}},
      {"stage.deselect", ST, N_("Deselect tracks (keeps the focused one)"), {ch(K::D, C)}},
      {"stage.roomsPanel", ST, N_("Open or close the rooms column"), {ch(K::Backslash)}},
      // Tracks from the playlist (app/StagePickUi.cpp): the panel that finds them and acts on them. No keys.
      {"stage.fromSelection", ST, N_("Tracks from the playlist selection..."), {}},
      {"stage.fromMarkers", ST, N_("Tracks from a marker area..."), {}},
      // ---- automation clip editor ----
      {"auto.delete", AU, N_("Delete selected points"), {ch(K::Delete), ch(K::Backspace)}},
      {"auto.selectAll", AU, N_("Select all points"), {ch(K::A, C)}},
      {"auto.deselect", AU, N_("Deselect"), {ch(K::D, C)}},
      {"auto.invertSelection", AU, N_("Invert selection"), {ch(K::I, C)}},
      {"auto.copy", AU, N_("Copy points"), {ch(K::C, C)}},
      {"auto.cut", AU, N_("Cut points"), {ch(K::X, C)}},
      {"auto.paste", AU, N_("Paste points"), {ch(K::V, C)}},
      {"auto.pasteReplace", AU, N_("Paste, replacing points"), {ch(K::R, C)}},
      {"auto.toolDraw", AU, N_("Draw tool"), {ch(K::P)}},
      {"auto.toolSelect", AU, N_("Select tool"), {ch(K::E)}},
      {"auto.toolDelete", AU, N_("Delete tool"), {ch(K::D)}},
      {"auto.lfo", AU, N_("LFO tool"), {ch(K::L)}},
      {"auto.quantize", AU, N_("Quantize points"), {ch(K::Q)}},
      {"auto.cancel", AU, N_("Close the tool / clear the selection"), {ch(K::Escape)}},
  };
}

const std::vector<Chord> kNone;

}  // namespace

bool playsOnTypingKeyboard(const Chord& c) {
  if (c.mods & (kCtrl | kAlt | kCmd)) return false;
  return (c.key >= Key::A && c.key <= Key::D9) || (c.key >= Key::NumAdd && c.key <= Key::NumDivide) ||
         (c.key >= Key::Minus && c.key <= Key::Grave);
}

const char* keyName(Key k) {
  size_t i = static_cast<size_t>(k);
  return i < static_cast<size_t>(Key::Count) ? kKeyNames[i] : "";
}

Key keyFromName(const std::string& name) {
  std::string n = lower(name);
  if (n.empty()) return Key::None;
  for (size_t i = 1; i < static_cast<size_t>(Key::Count); ++i)
    if (lower(kKeyNames[i]) == n) return static_cast<Key>(i);
  // A few spellings people type.
  if (n == "escape") return Key::Escape;
  if (n == "insert") return Key::Insert;
  if (n == "delete") return Key::Delete;
  if (n == "pageup") return Key::PageUp;
  if (n == "pagedown") return Key::PageDown;
  if (n == "return") return Key::Enter;
  return Key::None;
}

std::string format(const Chord& c) {
  if (!c.valid()) return {};
  std::string s;
  if (c.mods & kCtrl) s += "Ctrl+";
  if (c.mods & kCmd) s += "Cmd+";
  if (c.mods & kAlt) s += "Alt+";
  if (c.mods & kShift) s += "Shift+";
  return s + keyName(c.key);
}

Chord parse(const std::string& text) {
  Chord c;
  std::string rest = text;
  for (;;) {
    std::string l = lower(rest);
    auto strip = [&](const char* p, uint8_t m) {
      size_t n = std::char_traits<char>::length(p);
      if (l.size() > n && l.compare(0, n, p) == 0) {
        c.mods |= m;
        rest = rest.substr(n);
        return true;
      }
      return false;
    };
    if (strip("ctrl+", kCtrl) || strip("cmd+", kCmd) || strip("alt+", kAlt) || strip("shift+", kShift)) continue;
    break;
  }
  c.key = keyFromName(rest);
  if (!c.valid()) c.mods = 0;
  return c;
}

const char* contextName(Context c) {
  switch (c) {
    case Context::Global: return N_("Global");
    case Context::Playlist: return N_("Playlist");
    case Context::PianoRoll: return N_("Piano roll");
    case Context::ChannelRack: return N_("Channel rack");
    case Context::Mixer: return N_("Mixer");
    case Context::Automation: return N_("Automation clip editor");
    case Context::Stage: return N_("Stage");
    case Context::Waveform: return N_("Waveform");
    default: return "";
  }
}

const std::vector<Command>& commands() {
  static const std::vector<Command> c = buildCommands();
  return c;
}

const Command* findCommand(const std::string& id) {
  static const std::unordered_map<std::string, const Command*> index = [] {
    std::unordered_map<std::string, const Command*> m;
    for (const auto& c : commands()) m.emplace(c.id, &c);
    return m;
  }();
  auto it = index.find(id);
  return it == index.end() ? nullptr : it->second;
}

const std::vector<Chord>& Keymap::bindings(const std::string& id) const {
  auto it = overrides_.find(id);
  if (it != overrides_.end()) return it->second;
  return defaults(id);
}

const std::vector<Chord>& Keymap::defaults(const std::string& id) const {
  if (profileDefaults_) {
    auto it = profileDefaults_->find(id);
    if (it != profileDefaults_->end()) return it->second;
  }
  const Command* c = findCommand(id);
  return c ? c->defaults : kNone;
}

void Keymap::setProfile(Profile p, bool separateCmd) {
  profile_ = p;
  separateCmd_ = p == Profile::Logic && separateCmd;
  profileDefaults_ = p == Profile::Studio ? nullptr : &profileDefaults(p, separateCmd_);
  ++version_;
}

bool Keymap::set(const std::string& id, std::vector<Chord> chords) {
  const Command* c = findCommand(id);
  if (!c) return false;
  std::vector<Chord> clean;
  for (const auto& x : chords)
    if (x.valid() && std::find(clean.begin(), clean.end(), x) == clean.end()) clean.push_back(x);
  if (clean == defaults(id))
    overrides_.erase(id);
  else
    overrides_[id] = std::move(clean);
  ++version_;
  return true;
}

void Keymap::reset(const std::string& id) {
  if (overrides_.erase(id)) ++version_;
}

void Keymap::resetAll() {
  overrides_.clear();
  ++version_;
}

bool Keymap::customized(const std::string& id) const { return overrides_.count(id) != 0; }

std::vector<const Command*> Keymap::lookup(Context ctx, const Chord& chord) const {
  std::vector<const Command*> out;
  if (!chord.valid()) return out;
  for (const auto& c : commands()) {
    if (c.context != ctx) continue;
    const auto& b = bindings(c.id);
    if (std::find(b.begin(), b.end(), chord) != b.end()) out.push_back(&c);
  }
  return out;
}

std::vector<const Command*> Keymap::conflicts(const std::string& id, const Chord& chord) const {
  std::vector<const Command*> out;
  const Command* self = findCommand(id);
  if (!self) return out;
  for (const Command* c : lookup(self->context, chord))
    if (id != c->id) out.push_back(c);
  return out;
}

std::vector<const Command*> Keymap::shadows(const std::string& id, const Chord& chord) const {
  std::vector<const Command*> out;
  const Command* self = findCommand(id);
  if (!self || !chord.valid()) return out;
  for (const auto& c : commands()) {
    if (c.context == self->context) continue;
    if (self->context != Context::Global && c.context != Context::Global) continue;  // editors never overlap
    const auto& b = bindings(c.id);
    if (std::find(b.begin(), b.end(), chord) != b.end()) out.push_back(&c);
  }
  return out;
}

std::vector<std::string> Keymap::allConflicts() const {
  std::vector<std::string> out;
  const auto& cmds = commands();
  for (size_t i = 0; i < cmds.size(); ++i)
    for (const auto& chord : bindings(cmds[i].id))
      for (size_t j = i + 1; j < cmds.size(); ++j) {
        if (cmds[j].context != cmds[i].context) continue;
        const auto& b = bindings(cmds[j].id);
        if (std::find(b.begin(), b.end(), chord) != b.end())
          out.push_back(std::string(cmds[i].id) + "|" + cmds[j].id + "|" + format(chord));
      }
  return out;
}

std::vector<std::string> Keymap::assign(const std::string& id, const Chord& chord, bool replaceAll) {
  std::vector<std::string> lost;
  if (!findCommand(id) || !chord.valid()) return lost;
  for (const Command* c : conflicts(id, chord)) {
    std::vector<Chord> b = bindings(c->id);
    b.erase(std::remove(b.begin(), b.end(), chord), b.end());
    set(c->id, b);
    lost.push_back(c->id);
  }
  std::vector<Chord> mine = replaceAll ? std::vector<Chord>{} : bindings(id);
  mine.push_back(chord);
  set(id, mine);
  return lost;
}

std::string Keymap::serialize() const {
  if (overrides_.empty()) return {};  // the settings default: no overrides
  Json j = Json::makeObject();
  for (const auto& [id, chords] : overrides_) {
    Json arr = Json::makeArray();
    for (const auto& c : chords) arr.array.push_back(Json::makeString(format(c)));
    j.object[id] = std::move(arr);
  }
  return jsonStringify(j, 0);
}

bool Keymap::load(const std::string& json, std::string* err) {
  overrides_.clear();
  ++version_;
  if (json.empty()) return true;
  Json j;
  if (!jsonParse(json, j) || !j.is(Json::Type::Object)) {
    if (err) *err = "shortcut map is not a JSON object";
    return false;
  }
  for (const auto& [id, v] : j.object) {
    const Command* c = findCommand(id);
    if (!c || !v.is(Json::Type::Array)) continue;
    std::vector<Chord> chords;
    for (const auto& x : v.array)
      if (x.is(Json::Type::String))
        if (Chord cc = parse(x.str); cc.valid()) chords.push_back(cc);
    if (chords != defaults(c->id)) overrides_[id] = std::move(chords);
  }
  return true;
}

// ---- K-79: shortcut sets ----

const char* profileId(Profile p) {
  switch (p) {
    case Profile::Live: return "live";
    case Profile::Logic: return "logic";
    case Profile::Tracker: return "tracker";
    default: return "studio";
  }
}

Profile profileFromId(const std::string& id) {
  const std::string t = lower(id);
  if (t == "live" || t == "performance") return Profile::Live;
  if (t == "logic" || t == "tracks") return Profile::Logic;
  if (t == "tracker") return Profile::Tracker;
  return Profile::Studio;
}

namespace {

using Rows = std::vector<std::pair<const char*, std::vector<Chord>>>;

// The tools on single letters are left to the toolbars in both sets.
const char* const kToolCommands[] = {"playlist.toolDraw", "playlist.toolPaint", "playlist.toolDelete", "playlist.toolMute", "playlist.toolSlip",
                                     "playlist.toolSlice", "playlist.toolSelect", "playlist.toolZoom", "playlist.toolPlay", "playlist.toolWarp", "piano.toolDraw",
                                     "piano.toolPaint", "piano.toolDelete", "piano.toolSelect", "piano.toolSlice", "piano.toolMute",
                                     "piano.toolPlay", "piano.toolZoom", "piano.toolDetune", "piano.toolBend", "piano.toolVibrato", "auto.toolDraw",
                                     "auto.toolSelect", "auto.toolDelete"};

// Performance: view keys on Ctrl+Alt, single-key switches (O, M, 0), Tab
// for the main view, the editing keys most other music programs share
// (Ctrl+D duplicate, Ctrl+J join, Ctrl+L loop, Ctrl+E split), B for draw.
Rows liveRows() {
  using K = Key;
  Rows r;
  for (const char* t : kToolCommands) r.push_back({t, {}});
  auto set = [&](const char* id, std::vector<Chord> c) {
    for (auto& row : r)
      if (std::string(row.first) == id) {
        row.second = std::move(c);
        return;
      }
    r.push_back({id, std::move(c)});
  };
  set("view.toggleMain", {ch(K::Tab)});
  set("wave.warpMode", {ch(K::W, A)});  // K-248: single-letter tools are cleared here (Ctrl+W closes the window)
  set("view.automation", {ch(K::A)});  // checkpoint 53 (N52-10): A shows or hides the automation lanes, as the reference
  set("view.browser", {ch(K::B, C | A)});
  set("view.mixer", {ch(K::M, C | A)});
  set("view.pianoRoll", {ch(K::D3, C | A)});
  set("view.editors", {ch(K::E, C | A)});
  set("edit.history", {ch(K::Z, C | A)});
  set("edit.redo", {ch(K::Z, C | S), ch(K::Y, C)});  // N49-12: Cmd+Shift+Z first, as on the Mac
  set("view.settings", {ch(K::Comma, C)});
  set("window.close", {ch(K::W, C)});  // K-297's front-window close (Cmd+W on a Mac, the reference's Close Window)
  set("file.render", {ch(K::R, C | S)});
  set("file.renderStems", {});
  set("file.exportMidi", {ch(K::E, C | S)});
  set("edit.rename", {ch(K::R, C), ch(K::F2)});
  set("options.metronome", {ch(K::O)});
  set("options.typingKeyboard", {ch(K::M)});
  set("transport.record", {ch(K::F9)});
  set("transport.songMode", {});
  set("help.shortcuts", {ch(K::F1), ch(K::Slash, C)});
  set("view.hidePlugins", {ch(K::P, C | A)});  // K-112: plugin windows hide and show again
  set("track.newAudio", {ch(K::T, C)});      // K-112c: as in the Create menu; tap tempo keeps its button
  set("track.newMidi", {ch(K::T, C | S)});
  set("track.newReturn", {ch(K::T, C | A)});  // N47-11
  set("clips.insertMidiClip", {ch(K::M, C | S)});  // N49-07
  set("clips.captureScene", {ch(K::I, C | S)});    // grid-1118
  set("clips.backToArrangement", {ch(K::F10)});
  // grid2-1118: Cmd+1 .. 5 the edit grid and Cmd+6 .. 0 the global quantisation, as the reference's; the
  // workspaces move to Alt (Option)+1 .. 9, which no command of the set takes.
  set("clips.gridNarrower", {ch(K::D1, C)});
  set("clips.gridWider", {ch(K::D2, C)});
  set("clips.gridTriplet", {ch(K::D3, C)});
  set("clips.gridSnap", {ch(K::D4, C)});
  set("clips.gridFixed", {ch(K::D5, C)});
  set("clips.quant16", {ch(K::D6, C)});
  set("clips.quant8", {ch(K::D7, C)});
  set("clips.quant4", {ch(K::D8, C)});
  set("clips.quantBar", {ch(K::D9, C)});
  set("clips.quantNone", {ch(K::D0, C)});
  {
    const K digits[9] = {K::D1, K::D2, K::D3, K::D4, K::D5, K::D6, K::D7, K::D8, K::D9};
    const char* const ws[9] = {"view.workspace1", "view.workspace2", "view.workspace3", "view.workspace4", "view.workspace5",
                               "view.workspace6", "view.workspace7", "view.workspace8", "view.workspace9"};
    for (int i = 0; i < 9; ++i) {
      set(ws[i], {ch(digits[i], A)});
    }
  }
  set("track.group", {ch(K::G, C)});         // checkpoint 51
  set("track.ungroup", {ch(K::G, C | S)});
  set("view.detail", {ch(K::L, C | A)});
  set("view.quickHelp", {ch(K::Slash, S)});  // checkpoint 55 (gap 9): Shift+?, the Info View
  set("view.io", {ch(K::I, C | A)});         // checkpoint 55 (gap 2): Cmd+Alt+I, the In/Out section
  set("transport.sessionRecord", {ch(K::F9, S)});
  set("tools.stopSound", {ch(K::Period, C | A)});  // checkpoint 59 (N58-12): Cmd+H is the Mac's Hide; Cmd+Alt+. stops all sound
  // playlist
  set("playlist.duplicate", {ch(K::D, C)});
  set("playlist.deselect", {ch(K::A, C | S)});
  set("playlist.merge", {ch(K::J, C)});
  set("playlist.loopMarkers", {ch(K::L, C)});  // checkpoint 56 (N55-04): in Clips, Loop selection (the loop brace), as the reference's
  set("playlist.insertSpace", {ch(K::I, C)});
  set("playlist.deleteSpace", {ch(K::Delete, C | S)});
  set("pattern.delete", {ch(K::Backspace, C | S)});  // K-314 (C-08): its own chord (Cmd+Shift+Delete deletes time in the arrangement)
  set("playlist.duplicateTime", {ch(K::D, C | S)});
  set("playlist.mute", {ch(K::D0)});
  set("playlist.toolDraw", {ch(K::B)});
  set("playlist.zoomSelection", {ch(K::Z)});
  set("playlist.zoomIn", {ch(K::Equal), ch(K::PageUp)});
  set("playlist.zoomOut", {ch(K::Minus), ch(K::PageDown)});
  set("playlist.splitAtSongPos", {ch(K::E, C)});
  // piano roll
  set("piano.duplicate", {ch(K::D, C)});
  set("piano.deselect", {ch(K::A, C | S)});
  set("piano.quickQuantize", {ch(K::U, C)});
  set("piano.chopSteps", {});  // N49-12: Cmd+E is Split (Edit menu) alone
  set("piano.glue", {ch(K::J, C)});
  set("piano.toolDraw", {ch(K::B)});
  set("piano.zoomIn", {ch(K::Equal), ch(K::PageUp)});
  set("piano.zoomOut", {ch(K::Minus), ch(K::PageDown)});
  set("auto.toolDraw", {ch(K::B)});
  return r;
}

// Tracks: single letters open the panes and drive the transport (B browser,
// X mixer, E editors, P piano roll, R record, K metronome, L mode), Return
// goes to the start, comma and full stop move a bar. `M` is the command
// modifier (Cmd on macOS when told apart, else Ctrl) and `LC` the set's
// control modifier (Ctrl on macOS, Ctrl+Alt elsewhere).
Rows logicRows(bool sep) {
  using K = Key;
  const uint8_t M = sep ? kCmd : kCtrl, LC = sep ? static_cast<uint8_t>(kCtrl) : static_cast<uint8_t>(kCtrl | kAlt);
  Rows r;
  for (const char* t : kToolCommands) r.push_back({t, {}});
  auto add = [&](const char* id, std::vector<Chord> c) { r.push_back({id, std::move(c)}); };
  add("transport.play", {ch(K::Space), ch(K::NumEnter), ch(K::Num0)});
  add("transport.record", {ch(K::R)});
  add("transport.toStart", {ch(K::Enter)});
  add("wave.warpMode", {ch(K::W, A)});  // K-248
  add("transport.rewindBar", {ch(K::Comma)});
  add("transport.forwardBar", {ch(K::Period)});
  add("options.metronome", {ch(K::K)});
  add("options.countIn", {ch(K::K, S)});
  add("transport.songMode", {});  // checkpoint 47: L loops the selected regions, as the reference's
  add("playlist.loopRegion", {ch(K::L)});
  add("playlist.prevRegion", {ch(K::Left)});
  add("playlist.nextRegion", {ch(K::Right)});
  add("playlist.prevTrack", {ch(K::Up)});
  add("playlist.nextTrack", {ch(K::Down)});
  add("playlist.nudgeLeft", {ch(K::Left, A)});
  add("playlist.nudgeRight", {ch(K::Right, A)});
  // R01-07 / R01-15: as the reference's keys: B the browser on the left (the library), Y the Smart
  // controls, O the project's files, W the audio editor. The right pane's file browser has no key.
  add("view.browser", {});
  add("view.playlist", {ch(K::D1, M)});  // K-112B: Cmd+1, the main window (the Window menu's first item)
  add("view.mixer", {ch(K::X), ch(K::D2, M)});
  add("view.editors", {ch(K::E)});
  add("view.library", {ch(K::B)});
  add("view.smartControls", {ch(K::Y)});
  add("view.automation", {ch(K::A)});  // R02-15: A shows or hides the automation lanes
  add("view.globalTracks", {ch(K::G)});  // R01-15: G, the global tracks (the markers lane)
  add("playlist.toolMenu", {ch(K::T)});  // R01-18: T opens the tool menu
  add("view.files", {ch(K::O), ch(K::D8, M)});
  add("view.audioEditor", {ch(K::W), ch(K::D6, M)});
  add("view.inspector", {ch(K::I)});
  add("view.quickHelp", {ch(K::Slash, S)});
  add("view.toolbar", {sep ? ch(K::T, kCtrl | kAlt | kCmd) : ch(K::T, kCtrl | kAlt | kShift)});
  add("view.lists", {ch(K::D), ch(K::D7, M)});
  add("view.notes", {ch(K::P, M | A)});
  add("view.hidePlugins", {ch(K::V)});
  add("window.close", {ch(K::W, M)});  // K-297: the front window (K-301 R02-08)
  add("view.pianoRoll", {ch(K::P), ch(K::D4, M)});
  add("view.channelRack", {ch(K::D3, M)});
  add("view.settings", {ch(K::Comma, M)});
  add("help.shortcuts", {ch(K::K, A)});
  add("file.render", {ch(K::B, M)});
  add("file.renderStems", {ch(K::E, M | S)});
  add("file.exportMidi", {ch(K::E, M | A)});
  add("edit.history", {sep ? ch(K::Z, kCtrl | kAlt | kCmd) : ch(K::Z, A)});
  add("edit.redo", {ch(K::Z, M | S), ch(K::Y, M)});  // K-112B: Shift+Cmd+Z first, as the reference's Edit menu shows
  add("track.newTracks", {ch(K::N, M | A)});
  add("track.duplicate", {ch(K::D, M)});
  add("track.delete", {ch(K::Backspace, M)});
  add("track.mute", {ch(K::M)});  // R04-11: M and S mute and solo the selected tracks, as the reference's
  add("track.solo", {ch(K::S)});
  add("transport.cycle", {ch(K::C)});
  add("edit.rename", {ch(K::Enter, S), ch(K::F2)});
  add("tools.stopSound", {sep ? ch(K::Period, kCtrl | kAlt | kCmd) : ch(K::H, C)});
  add("options.typingKeyboard", {ch(K::K, M)});
  add("playlist.splitAtSongPos", {ch(K::T, M)});
  add("playlist.merge", {ch(K::J, M)});
  add("playlist.duplicate", {ch(K::R, M)});
  add("playlist.mute", {ch(K::M, LC)});
  add("playlist.loopMarkers", {});
  add("transport.cycleSelection", {ch(K::U, M)});  // R02-13: Cmd+U sets the cycle to the selected regions
  add("transport.prevMarker", {ch(K::Comma, A)});
  add("transport.nextMarker", {ch(K::Period, A)});
  add("transport.goToPosition", {ch(K::Slash)});
  add("playlist.zoomFit", {ch(K::Z)});
  add("playlist.cut", {ch(K::X, M)});
  add("playlist.zoomIn", {ch(K::Right, M), ch(K::PageUp)});
  add("playlist.zoomOut", {ch(K::Left, M), ch(K::PageDown)});
  add("playlist.marker", {ch(K::Apostrophe, A)});
  add("playlist.deselect", {ch(K::D, S)});
  add("piano.deselect", {ch(K::D, S)});
  add("piano.quickQuantize", {ch(K::Q)});
  add("piano.zoomIn", {ch(K::Right, M), ch(K::PageUp)});
  add("piano.zoomOut", {ch(K::Left, M), ch(K::PageDown)});
  add("piano.transposeUp", {ch(K::Up), ch(K::Up, A)});
  add("piano.transposeDown", {ch(K::Down), ch(K::Down, A)});
  add("piano.octaveUp", {ch(K::Up, S), ch(K::Up, A | S)});
  add("piano.octaveDown", {ch(K::Down, S), ch(K::Down, A | S)});
  return r;
}

// K-112 Tracker: the function keys work as in trackers: F1 to F8 pick the typing keyboard's
// octave and do nothing else. The window commands that had F1, F2 and F4 to F8 move to Ctrl+Alt
// keys; F9 to F12 go (they jump rows in the pattern; cp25) (Ctrl+Alt+X closes every screen). The set adds
// no key that deletes, clears or rewrites anything: every chord it adds opens, shows or picks.
Rows trackerRows() {
  using K = Key;
  Rows r;
  const char* const octaves[8] = {"typing.octave1", "typing.octave2", "typing.octave3", "typing.octave4",
                                  "typing.octave5", "typing.octave6", "typing.octave7", "typing.octave8"};
  const Key fkeys[8] = {K::F1, K::F2, K::F3, K::F4, K::F5, K::F6, K::F7, K::F8};
  for (int i = 0; i < 8; ++i) r.push_back({octaves[i], {ch(fkeys[i])}});
  auto add = [&](const char* id, std::vector<Chord> c) { r.push_back({id, std::move(c)}); };
  add("help.shortcuts", {ch(K::Slash, C)});
  add("wave.warpMode", {ch(K::W, A)});  // K-248: plain W types a note here
  // cp25: Ctrl+Alt+F2 (Studio's F2 with Ctrl+Alt): Shift+Enter plays the pattern from the cursor's row.
  add("edit.rename", {ch(K::F2, C | A)});
  add("pattern.new", {ch(K::N, C | A)});
  add("pattern.clone", {ch(K::U, C | A)});  // TRK-1118: Duplicate pattern (U: dUplicate; Ctrl+Shift+C is the pattern's block copy here)
  // The screens in the header's order, Ctrl+Alt with the digits (0 is the tenth), minus for Help.
  add("view.patternEditor", {ch(K::D1, C | A), ch(K::P, C | A)});  // FT2-MAP K-69: P as the classic Pattern screen key
  add("view.playlist", {ch(K::D2, C | A)});
  add("view.pianoRoll", {ch(K::D3, C | A)});
  add("view.channelRack", {ch(K::D4, C | A)});
  add("view.mixer", {ch(K::D5, C | A)});  // cp25: no F9 (it jumps rows in the pattern; T-26)
  add("view.stage", {ch(K::G, C | A)});  // the Stage page, beside Mix (G: staGe; free in this set)
  add("view.pads", {ch(K::D6, C | A)});
  add("view.channelSettings", {ch(K::D7, C | A), ch(K::I, C | A)});  // K-66: I, the instrument
  add("view.audioEditor", {ch(K::D8, C | A)});
  add("view.files", {ch(K::D9, C | A), ch(K::D, C | A)});  // K-63: D, the disk screen
  add("view.settings", {ch(K::D0, C | A), ch(K::C, C | A)});  // K-62: C, the config screen (cp25: no F10, a pattern jump)
  add("help.trackerKeys", {ch(K::Minus, C | A), ch(K::H, C | A)});  // K-65: H, help
  add("view.headerBand", {ch(K::Z, C | A)});  // K-73: Z, the extended pattern editor
  // K-60, K-70, K-72: the scope box's tools with the classic letters (A advanced edit, R trim, T transpose).
  add("view.transposeBox", {ch(K::T, C | A)});
  add("view.advancedEdit", {ch(K::A, C | A)});
  add("view.cleanUp", {ch(K::R, C | A)});
  add("view.midiBox", {ch(K::J, C | A)});      // cp13: the MIDI box (the classic I.E.Ext)
  add("view.sampleTools", {ch(K::K, C | A)});  // cp13: the Sample tools box (the classic S.E.Ext)
  // cp17: E, the Sample editor (K-316 5.4: free in this set; the Live set's Ctrl+Alt+E is its own).
  add("view.sampleEditor", {ch(K::E, C | A)});
  // cp17 (user): the sideways zoom: comma narrower, period wider (the < and > keys), slash every
  // track in view. Free in this set (Ctrl+Alt+minus is Help, so not minus and equals).
  add("view.zoomTracksOut", {ch(K::Comma, C | A)});
  add("view.zoomTracksIn", {ch(K::Period, C | A)});
  add("view.zoomTracksFit", {ch(K::Slash, C | A)});
  // N-05, N-06: X, back to the main screen (every screen closes, the pattern page in front), in place of
  // F12; F11 (Project info) goes too: in the pattern the function keys jump rows, so the menus do
  // not show keys the pattern holds back. Project info stays in the File menu.
  add("view.closeAll", {ch(K::X, C | A)});
  // R8-02: Ctrl+Shift+Up / Down step the instrument (the classic Shift+Up / Down) wherever the keys
  // are: the playlist's Move pattern up / down keeps its menu items but has no key here.
  add("pattern.moveUp", {});
  add("pattern.moveDown", {});
  add("file.projectInfo", {});
  add("view.pluginPicker", {ch(K::F, C | A)});  // K-69: the picker moves to F (find), P is the Pattern screen
  add("view.browser", {ch(K::B, C | A)});
  // cp25 (key map check, tests/test_key_sets.cpp): no key here that the Tracker's own keys
  // (core/TrackerKeys) take, so the menus never show a key that does something else in the
  // Tracker. Each command keeps its menu item.
  add("transport.songMode", {});   // L: a note key (the Tracker plays the song or the pattern by its own keys)
  add("view.backToPlaylist", {});  // Esc: edit mode in the pattern, closes the editors and screens
  add("pattern.findFirstEmpty", {});  // Shift+F4: copies the track
  add("pattern.findNextEmpty", {});   // Ctrl+F4: copies the pattern
  add("pattern.next", {});            // keypad + and -: the Song page's entry pattern (Ctrl+Left / Right step patterns)
  add("pattern.previous", {});
  add("edit.history", {});  // Alt+Z: the Sample editor's zoom out (Ctrl+Alt+Z is the header strip)
  const char* const workspaces[9] = {"view.workspace1", "view.workspace2", "view.workspace3", "view.workspace4", "view.workspace5",
                                     "view.workspace6", "view.workspace7", "view.workspace8", "view.workspace9"};
  // K-256: Ctrl+1 .. 9 is the edit step here, Ctrl+Alt+digit the screens and Alt+digit the macros; the
  // workspaces take Ctrl+Alt+Shift+1 .. 9, free in this set.
  const Key digits[9] = {K::D1, K::D2, K::D3, K::D4, K::D5, K::D6, K::D7, K::D8, K::D9};
  for (int i = 0; i < 9; ++i) add(workspaces[i], {ch(digits[i], C | A | S)});
  return r;
}

std::map<std::string, std::vector<Chord>> buildProfile(Profile p, bool sep) {
  std::map<std::string, std::vector<Chord>> out;
  const Rows rows = p == Profile::Live ? liveRows() : p == Profile::Tracker ? trackerRows() : logicRows(sep);
  std::map<std::string, std::vector<Chord>> own;
  for (const auto& [id, chords] : rows) own[id] = chords;
  // Chords the set gives to its own rows, per context.
  std::map<Context, std::vector<Chord>> claimed;
  for (const auto& [id, chords] : own)
    if (const Command* c = findCommand(id))
      for (const Chord& x : chords) claimed[c->context].push_back(x);
  auto taken = [&](Context ctx, const Chord& x) {
    const auto& v = claimed[ctx];
    return std::find(v.begin(), v.end(), x) != v.end();
  };
  for (const Command& c : commands()) {
    if (auto it = own.find(c.id); it != own.end()) {
      out[c.id] = it->second;
      continue;
    }
    std::vector<Chord> keep;
    for (Chord x : c.defaults) {
      if (sep && (x.mods & kCtrl)) x.mods = static_cast<uint8_t>((x.mods & ~kCtrl) | kCmd);  // Studio's Ctrl is Cmd on macOS
      // A chord the set uses elsewhere in this context, or globally, leaves this command.
      if (taken(c.context, x) || (c.context != Context::Global && taken(Context::Global, x))) continue;
      keep.push_back(x);
    }
    out[c.id] = std::move(keep);
  }
  return out;
}

}  // namespace

std::vector<SystemMenuKey> systemMenuKeys(const Keymap& km) {
  const uint8_t cmd = km.separateCmd() ? kCmd : kCtrl;
  const SystemMenuKey all[] = {{"hide", Chord{Key::H, cmd}}, {"hideOthers", Chord{Key::H, static_cast<uint8_t>(cmd | kAlt)}}};
  std::vector<SystemMenuKey> out;
  for (const SystemMenuKey& k : all) {
    bool used = false;
    for (const Command& c : commands())
      for (const Chord& x : km.bindings(c.id)) used = used || x == k.chord;
    if (!used) out.push_back(k);
  }
  return out;
}

const std::map<std::string, std::vector<Chord>>& profileDefaults(Profile p, bool separateCmd) {
  static const std::map<std::string, std::vector<Chord>> studio = [] {
    std::map<std::string, std::vector<Chord>> m;
    for (const Command& c : commands()) m[c.id] = c.defaults;
    return m;
  }();
  static const auto perf = buildProfile(Profile::Live, false);
  static const auto tracks = buildProfile(Profile::Logic, false);
  static const auto tracksMac = buildProfile(Profile::Logic, true);
  static const auto tracker = buildProfile(Profile::Tracker, false);
  switch (p) {
    case Profile::Tracker: return tracker;
    case Profile::Live: return perf;
    case Profile::Logic: return separateCmd ? tracksMac : tracks;
    default: return studio;
  }
}

Keymap& keymap() {
  static Keymap k;
  return k;
}

const std::vector<MouseModifier>& mouseModifiers() {
  static const std::vector<MouseModifier> v = {
      {"piano.laneSelectDrag", Context::PianoRoll, N_("Event lane: drag selects notes"), kCtrl},
      {"piano.laneDefaultDrag", Context::PianoRoll, N_("Event lane: drag sets the default value"), kAlt},
      // Q-8: the reference moves an effect with the plain wheel over its slot (no modifier).
      {"mixer.slotWheelMove", Context::Mixer, N_("Effect slot: mouse wheel moves the effect up / down"), 0},
      // J-83: while the mixer scrolls (thin layout, or Settings > Interface "scroll") the plain
      // wheel scrolls it; with this held the wheel adjusts the control under the pointer.
      // J-84: the same modifier adjusts channel rack knobs, step velocities and keyboard-lane notes while the rack scrolls.
      {"mixer.wheelAdjust", Context::Mixer, N_("Mixer / channel rack control: mouse wheel adjusts it while the pane scrolls"), kCtrl},
  };
  return v;
}

uint8_t mouseModifier(const std::string& id) {
  for (const auto& m : mouseModifiers())
    if (id == m.id) return m.mods;
  return 0;
}

}  // namespace gl::keys
