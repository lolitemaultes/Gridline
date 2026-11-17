// Gridline - K-106 cp25: the Tracker's own keys as data.
//
// The keys the Tracker's pages and editors handle themselves (not through the shortcut set): the
// pattern editor, the Song page's order list, the Sample editor and the Instrument editor. The
// Tracker help lists them from here (its "Pattern keys", "Song page keys" and "Editor keys"
// subjects), and tests/k112/key_sets.tsv holds them (rows "tracker.<area>"), so a key added to a
// handler without a line here, or a line here with no golden row, fails k112_keys. The test also
// checks that no key here takes a chord the Tracker shortcut set gives a global command (the menus
// would show a key that does something else here), apart from the few listed in sameJob().
//
// Chords use the shortcut set's spelling (keys::format: "Ctrl+Shift+Up", "Num 1"); keys the set
// cannot bind have their own names ("CapsLock", "<>", "Right Ctrl", "Right Alt", "Right Shift").
// A row with a command (`command`) is a shortcut-set key the help lists among these (Space, F1 to
// F8; "#" stands for the chord's place, 1 to 8): the test checks the set really binds it so.
#pragma once
#include <string>
#include <vector>

namespace gl::trackerkeys {

enum class Area { Pattern, Song, Sample, Instrument, Count };
const char* areaId(Area a);  // "pattern", "song", "sample", "instrument"

struct Row {
  Area area;
  const char* id;                   // stable: "pattern.noteOff"
  std::vector<std::string> chords;  // every chord the row covers
  const char* keys;                 // the help's key column ("Shift+F3 F4 F5")
  const char* what;                 // the help's text (translatable)
  bool typed = false;               // typed into the field at the cursor (notes, digits, letters)
  const char* command = nullptr;    // a shortcut-set command these keys belong to
};

const std::vector<Row>& rows();

// The screens and their keys as the help's "Screen keys" subject lists them: shortcut-set commands
// with the Tracker's own names. Every command the Tracker set gives a key to is here or in rows().
struct ScreenKey {
  const char* id;
  const char* what;
};
const std::vector<ScreenKey>& screenKeys();

// A local chord that is also a global command's key in the Tracker set, on purpose: both do the same
// job (Ctrl+C copies, here the block). "<chord>|<command id>".
const std::vector<std::string>& sameJob();

}  // namespace gl::trackerkeys
