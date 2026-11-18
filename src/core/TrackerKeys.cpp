// Gridline - K-106 cp25: the Tracker's own keys as data (see TrackerKeys.h).
#include "TrackerKeys.h"
#include "I18n.h"

namespace gl::trackerkeys {

using i18n::N_;

namespace {

using V = std::vector<std::string>;

// "Alt+" before each key.
V with(const char* mod, const V& keys) {
  V out;
  for (const std::string& k : keys) out.push_back(std::string(mod) + k);
  return out;
}
V join(V a, const V& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

const V kLowNotes = {"Z", "S", "X", "D", "C", "V", "G", "B", "H", "N", "J", "M", ",", "L", ".", ";", "/"};
const V kHighNotes = {"Q", "2", "W", "3", "E", "R", "5", "T", "6", "Y", "7", "U", "I", "9", "O", "0", "P", "[", "=", "]"};
const V kDigits = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
const V kHex = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D", "E", "F"};
const V kJumps = {"F9", "F10", "F11", "F12"};

std::vector<Row> build() {
  const Area P = Area::Pattern, S = Area::Song, E = Area::Sample, I = Area::Instrument;
  std::vector<Row> r = {
      // ---- the pattern editor ----
      {P, "pattern.notes", kLowNotes, "Z S X D C ... /", N_("Notes in the octave shown (the row above: the black keys)"), true},
      {P, "pattern.notesUp", kHighNotes, "Q 2 W 3 E ... P", N_("Notes an octave up"), true},
      {P, "pattern.octave", {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8"}, "F1 ... F8", N_("The octave"), false, "typing.octave#"},
      {P, "pattern.octaveStep", {"Num /", "Num *"}, "Keypad / *", N_("The octave one lower, one higher")},
      {P, "pattern.noteOff", {"CapsLock", "<>", "1"}, "Caps Lock, < >, 1", N_("A note off (the note above ends here)"), true},
      {P, "pattern.editMode", {"Esc"}, "Esc", N_("Edit mode on or off (off: the keys only play; a marked block: unmarks it)")},
      {P, "pattern.digits", kHex, "0 ... 9, A ... F", N_("A digit: written, then down by the edit step (effects: after the last digit)"), true},
      {P, "pattern.effects", {"G", "P", "T", "X", "Y", "R"}, "G P T X Y R", N_("An effect letter, in the effect field (the Effect letters subject)"), true},
      {P, "pattern.stepKey", {"`", "Shift+`"}, "`  Shift+`", N_("Edit step one more, one fewer")},
      {P, "pattern.step", join(with("Ctrl+", kDigits), {"Ctrl+-", "Ctrl+="}), "Ctrl+0 ... 9, Ctrl+- =", N_("Edit step (minus and equals: one fewer, one more)")},
      {P, "pattern.rows", {"Up", "Down", "PgUp", "PgDn"}, "Up Down PgUp PgDn", N_("Rows (16 at a time with the page keys)")},
      {P, "pattern.ends", {"Home", "End"}, "Home End", N_("The first, the last row")},
      {P, "pattern.fields", {"Left", "Right"}, "Left Right", N_("The fields of a cell")},
      {P, "pattern.tracks", {"Tab", "Shift+Tab"}, "Tab  Shift+Tab", N_("The next, the previous track")},
      {P, "pattern.trackJump", with("Alt+", {"Q", "W", "E", "R", "T", "Y", "U", "I", "A", "S", "D", "F", "G", "H", "J", "K"}), "Alt+Q ... I, Alt+A ... K",
       N_("The first eight tracks, the next eight")},
      {P, "pattern.instrument", {"Ctrl+Shift+Up", "Ctrl+Shift+Down"}, "Ctrl+Shift+Up Down", N_("The previous, the next instrument")},
      {P, "pattern.jump", join(kJumps, join(with("Shift+", kJumps), with("Ctrl+", kJumps))), "F9 ... F12",
       N_("Jump rows (Shift: keep the cursor's row there, Ctrl: play the pattern from it)")},
      {P, "pattern.mark",
       {"Shift+Up", "Shift+Down", "Shift+Left", "Shift+Right", "Shift+PgUp", "Shift+PgDn", "Shift+Home", "Shift+End", "Alt+Up", "Alt+Down", "Alt+Left",
        "Alt+Right"},
       "Shift or Alt+arrows", N_("Mark a block (Shift: the page keys, Home and End too)")},
      {P, "pattern.markTrack", {"Alt+C"}, "Alt+C", N_("Mark the track")},
      {P, "pattern.block", {"Ctrl+C", "Ctrl+X", "Ctrl+V", "Alt+F4", "Alt+F3", "Alt+F5"}, "Ctrl+C X V, Alt+F4 F3 F5",
       N_("Copy, cut, paste the block (masks apply; cut, paste: edit mode)")},
      {P, "pattern.trackClip", {"Shift+F3", "Shift+F4", "Shift+F5"}, "Shift+F3 F4 F5", N_("Cut, copy, paste the track (own clipboard; cut, paste: edit mode)")},
      {P, "pattern.patternClip", {"Ctrl+F3", "Ctrl+F4", "Ctrl+F5"}, "Ctrl+F3 F4 F5", N_("Cut, copy, paste the pattern (own clipboard; cut, paste: edit mode)")},
      {P, "pattern.clearField", {"Del"}, "Delete", N_("Clears the field (the block when one is marked)")},
      {P, "pattern.clearCell", {"Shift+Del"}, "Shift+Delete", N_("Clears the cell")},
      {P, "pattern.clearVolFx", {"Ctrl+Del", "Alt+Del"}, "Ctrl+Delete  Alt+Delete", N_("Clears volume and effect, the effect")},
      {P, "pattern.masterVolume", {"Ctrl+Num +", "Ctrl+Num -"}, "Ctrl+keypad + -", N_("The master volume up, down")},
      {P, "pattern.keypadInstrument", {"Num 1", "Num 2", "Num 3", "Num 4", "Num 5", "Num 6", "Num 7", "Num 8", "Num Enter"}, "Keypad 1 ... 8, Enter",
       N_("Instruments 1 to 8 of the keypad's group, the next group")},
      {P, "pattern.macro", with("Alt+", kDigits), "Alt+1 ... 0", N_("Writes macro 1 to 10 into the volume or effect field at the cursor")},
      {P, "pattern.macroStore", with("Alt+Shift+", kDigits), "Shift+Alt+1 ... 0", N_("Stores that field as macro 1 to 10 (volumes and effects keep a bank each)")},
      {P, "pattern.insertRow", {"Ins", "Shift+Ins"}, "Insert", N_("A row in (Shift: in every track)")},
      {P, "pattern.deleteRow", {"Backspace", "Shift+Backspace"}, "Backspace", N_("The row above out (Shift: in every track)")},
      {P, "pattern.transposeBlock", {"Ctrl+Up", "Ctrl+Down"}, "Ctrl+Up Down", N_("The block a semitone up, down (Gridline's own)")},
      {P, "pattern.transposeTrack", {"Shift+F1", "Shift+F2", "Shift+F7", "Shift+F8", "Ctrl+F7", "Ctrl+F8"}, "Shift+F1 F2",
       N_("The track a semitone down, up (Shift or Ctrl+F7 F8 the same)")},
      {P, "pattern.transposePattern", {"Ctrl+F1", "Ctrl+F2"}, "Ctrl+F1 F2", N_("The pattern a semitone down, up")},
      {P, "pattern.transposeMarked", {"Alt+F1", "Alt+F2", "Alt+F7", "Alt+F8"}, "Alt+F1 F2", N_("The block a semitone down, up (Alt+F7 F8 the same)")},
      {P, "pattern.patterns", {"Ctrl+Left", "Ctrl+Right"}, "Ctrl+Left Right", N_("The previous, the next pattern (past the last: a new one)")},
      {P, "pattern.positions", {"Ctrl+Shift+Left", "Ctrl+Shift+Right"}, "Ctrl+Shift+Left Right", N_("The previous, the next song position")},
      {P, "pattern.playStop", {"Space"}, "Space", N_("Play, stop"), false, "transport.play"},
      {P, "pattern.playSong", {"Enter", "Right Ctrl"}, "Enter, Right Ctrl", N_("Play the song from the current order entry")},
      {P, "pattern.playPattern", {"Ctrl+Enter", "Right Alt", "Shift+Enter"}, "Ctrl+Enter, Right Alt",
       N_("Play the pattern (Shift+Enter: from the cursor's row)")},
      {P, "pattern.record", {"Right Shift"}, "Right Shift (alone)", N_("Record the pattern: the keys write on the row playing")},
      // ---- the Song page (its order list has the keys) ----
      {S, "song.entries", {"Up", "Down"}, "Up Down", N_("The previous, the next entry (the pattern editor shows its pattern)")},
      {S, "song.mark", {"Shift+Up", "Shift+Down"}, "Shift+Up Down", N_("Mark entries (Esc unmarks them)")},
      {S, "song.unmark", {"Esc"}, "Esc", N_("Unmark the entries")},
      {S, "song.move", {"Ctrl+Up", "Ctrl+Down"}, "Ctrl+Up Down", N_("Move the entry up, down")},
      {S, "song.ends", {"Home", "End"}, "Home End", N_("The first, the last entry")},
      {S, "song.pattern", {"Left", "Right", "-", "=", "Num -", "Num +"}, "Left Right, - =", N_("The entry's pattern number down, up (keypad - + too)")},
      {S, "song.insert", {"Ins"}, "Insert", N_("A new entry after this one, with its pattern")},
      {S, "song.delete", {"Del"}, "Delete", N_("Removes the entry")},
      {S, "song.play", {"Enter", "Num Enter"}, "Enter", N_("Play the song from the entry")},
      {S, "song.loop", {"Ctrl+L"}, "Ctrl+L", N_("Loop the marked entries (or this one); again: the whole song")},
      // ---- the Sample editor ----
      {E, "sample.close", {"Esc", "Ctrl+W"}, "Esc, Ctrl+W", N_("Close the editor")},
      {E, "sample.selectAll", {"Ctrl+A", "Alt+A"}, "Ctrl+A", N_("Mark the whole sample")},
      {E, "sample.clipboard", {"Ctrl+X", "Ctrl+C", "Ctrl+V", "Alt+X", "Alt+C", "Alt+V"}, "Ctrl+X C V", N_("Cut, copy, paste the range (Alt the same)")},
      {E, "sample.crop", {"Alt+R"}, "Alt+R", N_("Crop to the range")},
      {E, "sample.zoom", {"Alt+S", "Alt+Z"}, "Alt+S  Alt+Z", N_("Zoom to the range, zoom out")},
      {E, "sample.play", {"Alt+W", "Alt+Q"}, "Alt+W  Alt+Q", N_("Play the sample, stop")},
      {E, "sample.slot", {"Alt+Up", "Alt+Down"}, "Alt+Up Down", N_("The previous, the next sample of the instrument")},
      {E, "sample.delete", {"Del"}, "Delete", N_("Delete the range")},
      {E, "sample.scroll", {"Left", "Right", "Shift+Left", "Shift+Right"}, "Left Right", N_("Scroll the view (Shift: a whole view)")},
      {E, "sample.notes", join(kLowNotes, kHighNotes), "Z ... /, Q ... ]", N_("Play the instrument (sets the play note)"), true},
      // ---- the Instrument editor ----
      {I, "instrument.close", {"Esc", "Ctrl+W"}, "Esc, Ctrl+W", N_("Close the editor")},
      {I, "instrument.slot", {"Alt+Up", "Alt+Down"}, "Alt+Up Down", N_("The previous, the next sample of the instrument")},
      {I, "instrument.envelope", {"Tab"}, "Tab", N_("The other envelope (volume, panning)")},
      {I, "instrument.point", {"Left", "Right"}, "Left Right", N_("The previous, the next point")},
      {I, "instrument.pointTick", {"Shift+Left", "Shift+Right"}, "Shift+Left Right", N_("Move the point earlier, later")},
      {I, "instrument.pointLevel", {"Up", "Down", "Shift+Up", "Shift+Down"}, "Up Down", N_("The point's level up, down (Shift: 8 at a time)")},
      {I, "instrument.add", {"Ins", "Ctrl+Enter"}, "Insert, Ctrl+Enter", N_("Add a point after this one")},
      {I, "instrument.remove", {"Del", "Backspace"}, "Delete, Backspace", N_("Delete the point")},
      {I, "instrument.notes", join(kLowNotes, kHighNotes), "Z ... /, Q ... ]", N_("Play the instrument"), true},
  };
  return r;
}

}  // namespace

const char* areaId(Area a) {
  switch (a) {
    case Area::Pattern: return "pattern";
    case Area::Song: return "song";
    case Area::Sample: return "sample";
    case Area::Instrument: return "instrument";
    default: return "";
  }
}

const std::vector<Row>& rows() {
  static const std::vector<Row> r = build();
  return r;
}

const std::vector<ScreenKey>& screenKeys() {
  static const std::vector<ScreenKey> k = {
      {"view.patternEditor", N_("The Pattern page")},
      {"view.playlist", N_("The Song page")},
      {"view.pianoRoll", N_("The Roll page")},
      {"view.channelRack", N_("The Steps page")},
      {"view.mixer", N_("The Mix page")},
      {"view.pads", N_("The Pads page")},
      {"view.channelSettings", N_("Channel settings: the instrument's editor under the header")},
      {"view.audioEditor", N_("The audio editor")},
      {"view.files", N_("The Files screen")},
      {"view.settings", N_("The Settings screen")},
      {"help.trackerKeys", N_("This help")},
      {"view.closeAll", N_("Back to the main screen (every screen closes, the Pattern page in front)")},
      {"pattern.new", N_("A new pattern (Ins puts it in the song)")},
      {"pattern.clone", N_("Duplicate pattern: a copy of the pattern after it, shown (the Pattern menu also puts one in the song)")},
      {"view.headerBand", N_("The header as one strip, the pattern taller")},
      {"view.transposeBox", N_("The Transpose box")},
      {"view.advancedEdit", N_("The Advanced edit box")},
      {"view.cleanUp", N_("The Clean up box")},
      {"view.midiBox", N_("The MIDI box: the instrument's MIDI out and the MIDI input")},
      {"view.sampleTools", N_("The Sample tools box: range edits on the instrument's sample")},
      {"view.sampleEditor", N_("The Sample editor: the instrument's samples under the pattern")},
      {"view.zoomTracksOut", N_("Narrower tracks, more in view (the Song page: its lanes or timeline)")},
      {"view.zoomTracksIn", N_("Wider tracks, fewer in view")},
      {"view.zoomTracksFit", N_("Every track in view (Ctrl+wheel zooms too)")},
      {"view.browser", N_("The browser")},
      {"view.pluginPicker", N_("The plugin picker")},
      {"view.commandPalette", N_("The command palette: find and run any command, window or plugin by name")},
      {"edit.rename", N_("Rename the pattern, instrument or mixer track")},
      {"help.shortcuts", N_("Every shortcut")},
  };
  return k;
}

const std::vector<std::string>& sameJob() {
  static const std::vector<std::string> s = {};
  return s;
}

}  // namespace gl::trackerkeys
