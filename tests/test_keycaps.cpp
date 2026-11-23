// Gridline: "My keys say" (GitHub #79). A shortcut is shown by the printed key that runs it on the
// computer's layout; no shortcut moves. Listed layouts stand in for the computer's (no real system
// layout needed). Checks: the default names every chord of every shortcut set as before, byte for
// byte, whatever the computer's layout; Dvorak typed on keys printed QWERTY names Stop all sound
// Ctrl+J and Copy Ctrl+I (and the rest of the issue's table); the / key on Dvorak (the interface's
// own rule leaves it without a key; punctuation then goes by what it types, on that layout only);
// every shortcut key has a key on every listed layout; a key that no key gives says so; the
// setting's ids and the move from the typing keyboard's old layout choice.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "../src/core/KeyCaps.h"
#include "../src/core/Keymap.h"
#include "../src/core/MidiRecord.h"
#include "../src/core/Settings.h"
#include "Check.h"

using namespace gl;
using keys::Chord;
using keys::Key;

namespace {

constexpr TypingLayout kListed[] = {TypingLayout::Standard, TypingLayout::Qwertz, TypingLayout::Azerty, TypingLayout::Dvorak, TypingLayout::Colemak};

Chord ctrl(Key k) { return Chord{k, keys::kCtrl}; }

// Every chord of every command in every shortcut set (both Cmd forms).
template <class F>
bool everyChord(F f) {
  bool ok = true;
  for (int p = 0; p < static_cast<int>(keys::Profile::Count); ++p)
    for (bool cmd : {false, true})
      for (const auto& [id, chords] : keys::profileDefaults(static_cast<keys::Profile>(p), cmd))
        for (const Chord& c : chords)
          if (!f(c)) {
            std::printf("# %s %s\n", id.c_str(), keys::format(c).c_str());
            ok = false;
          }
  return ok;
}

// A made-up computer layout: US, but the / key types q (so two keys type q and none types /).
int noSlash(int position) { return position == 56 ? 'q' : TypingKeyboard::usChar(position); }

std::string loadMyKeys(const std::string& dir, const std::string& valuesJson) {
  const std::string path = dir + "/settings.json";
  std::ofstream(path) << "{\"version\": 1, \"values\": {" << valuesJson << "}}";
  Settings s;
  s.load(path);
  return s.getString("keys.myKeys");
}

}  // namespace

int main() {
  // 1. The default: the computer's layout names the keys, as before.
  expect(keycaps::caps() == TypingLayout::Computer, "default: Same as my computer");
  expect(everyChord([](const Chord& c) { return keycaps::format(c) == keys::format(c); }), "default: every chord of every set as before (US computer)");
  for (TypingLayout sys : kListed) {
    keycaps::simulateSystem(sys);
    const std::string what = std::string("default: every chord as before on a ") + typingLayoutName(sys) + " computer";
    expect(everyChord([](const Chord& c) { return keycaps::format(c) == keys::format(c); }), what.c_str());
  }
  keycaps::simulateSystem(TypingLayout::Computer);

  // 2. Every shortcut key has a key on every listed layout; the punctuation rule changes only Dvorak.
  for (TypingLayout sys : kListed) {
    keycaps::simulateSystem(sys);
    bool all = true;
    for (int k = static_cast<int>(Key::A); k <= static_cast<int>(Key::D9); ++k) all = all && keycaps::positionOf(static_cast<Key>(k)) > 0;
    for (int k = static_cast<int>(Key::Minus); k <= static_cast<int>(Key::Grave); ++k) all = all && keycaps::positionOf(static_cast<Key>(k)) > 0;
    expect(all, (std::string("every letter, digit and punctuation key has a key: ") + typingLayoutName(sys)).c_str());
    const bool dvorak = sys == TypingLayout::Dvorak;
    expect(keycaps::punctuationByTyped() == dvorak, (std::string("punctuation by what it types only on Dvorak: ") + typingLayoutName(sys)).c_str());
    bool same = true;
    for (int p = 4; p <= 56; ++p) same = same && (dvorak || keycaps::remapPosition(p) == 0);
    expect(same, (std::string("no key event changes: ") + typingLayoutName(sys)).c_str());
  }
  keycaps::simulateSystem(TypingLayout::Computer);
  expect(keycaps::positionOf(Key::Slash) == 56 && keycaps::positionOf(Key::H) == 11 && keycaps::positionOf(Key::F5) == -1, "US: / in its place, H in its");

  // 3. Dvorak: the interface's own rule has no / key (the key that types / sits in the [ place, the
  // key in the / place types z). Punctuation then goes by what it types, so / is the key printed [.
  keycaps::simulateSystem(TypingLayout::Dvorak);
  expect(keycaps::positionOf(Key::Slash) == 47 && keycaps::remapPosition(47) == 56, "Dvorak: / is the key that types it (the [ place)");
  expect(keycaps::positionOf(Key::LeftBracket) == 45 && keycaps::remapPosition(45) == 47, "Dvorak: [ is the key that types it (the - place)");
  expect(keycaps::positionOf(Key::Minus) == 52 && keycaps::positionOf(Key::Apostrophe) == 20, "Dvorak: - and ' are the keys that type them");
  expect(keycaps::positionOf(Key::Z) == 56 && keycaps::remapPosition(56) == 0 && keycaps::remapPosition(11) == 0, "Dvorak: letters as before");

  // 4. Dvorak typed on keys printed QWERTY: the issue's table.
  keycaps::setCaps(TypingLayout::Standard);
  expect(keycaps::format(ctrl(Key::H)) == "Ctrl+J", "Dvorak + QWERTY keys: Stop all sound (Ctrl+H) shows Ctrl+J");
  expect(keycaps::format(ctrl(Key::C)) == "Ctrl+I", "Dvorak + QWERTY keys: Copy (Ctrl+C) shows Ctrl+I");
  expect(keycaps::format(ctrl(Key::V)) == "Ctrl+." && keycaps::format(ctrl(Key::X)) == "Ctrl+B" && keycaps::format(ctrl(Key::Z)) == "Ctrl+/",
         "Dvorak + QWERTY keys: Paste, Cut, Undo show the keys printed . B /");
  expect(keycaps::format(ctrl(Key::Slash)) == "Ctrl+[" && keycaps::format(Chord{Key::LeftBracket, 0}) == "-", "Dvorak + QWERTY keys: / on the key printed [, [ on -");
  expect(keycaps::format(Chord{Key::F5, keys::kShift}) == "Shift+F5" && keycaps::format(Chord{Key::NumAdd, 0}) == "Num +", "function and keypad keys keep their names");
  const keys::Command* stop = keys::findCommand("tools.stopSound");
  expect(stop && !stop->defaults.empty() && stop->defaults.front() == ctrl(Key::H), "Stop all sound stays on Ctrl+H (no shortcut moves)");
  expect(keycaps::formatText("Ctrl+X") == "Ctrl+B" && keycaps::formatText("Ctrl+wheel") == "Ctrl+wheel" && keycaps::formatText("Alt+drag") == "Alt+drag",
         "fixed texts: a chord is renamed, other text is not");
  // The keys say what the computer types: every name as the computer's.
  keycaps::setCaps(TypingLayout::Dvorak);
  expect(everyChord([](const Chord& c) { return keycaps::format(c) == keys::format(c); }), "Dvorak keys on a Dvorak computer: every chord as before");
  keycaps::setCaps(TypingLayout::Computer);
  expect(keycaps::format(ctrl(Key::H)) == "Ctrl+H" && keycaps::format(ctrl(Key::Slash)) == "Ctrl+/", "Dvorak computer, default: as before");

  // 5. QWERTY keys on the other computers; the computer's own keys on QWERTY.
  keycaps::simulateSystem(TypingLayout::Azerty);
  keycaps::setCaps(TypingLayout::Standard);
  expect(keycaps::format(ctrl(Key::Z)) == "Ctrl+W" && keycaps::format(ctrl(Key::A)) == "Ctrl+Q" && keycaps::format(ctrl(Key::M)) == "Ctrl+;",
         "AZERTY computer + QWERTY keys: Z on W, A on Q, M on ;");
  keycaps::setCaps(TypingLayout::Azerty);
  expect(keycaps::format(ctrl(Key::Z)) == "Ctrl+Z" && keycaps::format(ctrl(Key::D1)) == "Ctrl+1" && keycaps::format(ctrl(Key::Slash)) == "Ctrl+!",
         "AZERTY keys on an AZERTY computer: letters and digits as typed, / on the key printed !");
  keycaps::simulateSystem(TypingLayout::Qwertz);
  keycaps::setCaps(TypingLayout::Qwertz);
  expect(keycaps::format(ctrl(Key::Y)) == "Ctrl+Y" && keycaps::format(ctrl(Key::Minus)) == "Ctrl+\xC3\x9F", "QWERTZ keys on a QWERTZ computer: Y, - on the key printed ß");
  keycaps::simulateSystem(TypingLayout::Standard);
  keycaps::setCaps(TypingLayout::Qwertz);
  expect(keycaps::format(ctrl(Key::Z)) == "Ctrl+Y" && keycaps::format(Chord{Key::Semicolon, 0}) == "\xC3\x96", "US computer + QWERTZ keys: Z on Y, ; on Ö");
  keycaps::setCaps(TypingLayout::Colemak);
  expect(keycaps::format(ctrl(Key::S)) == "Ctrl+R" && keycaps::format(ctrl(Key::Semicolon)) == "Ctrl+O", "US computer + Colemak keys: S on R, ; on O");
  keycaps::setCaps(TypingLayout::Dvorak);
  expect(keycaps::format(ctrl(Key::H)) == "Ctrl+D" && keycaps::format(ctrl(Key::Z)) == "Ctrl+;" && keycaps::keyName(Key::Slash) == "Z",
         "US computer + Dvorak keys: H on D, Z on ;, / on Z");

  // 6. A key no key gives says so (whatever the keys say), and does not count as a place.
  keycaps::simulateSystem(TypingLayout::Computer);
  keycaps::setSystem(noSlash);
  keycaps::setCaps(TypingLayout::Computer);
  expect(keycaps::positionOf(Key::Slash) == 0 && !keycaps::punctuationByTyped(), "no key types / or sits for it: none");
  const std::string noKey = keycaps::format(ctrl(Key::Slash));
  expect(noKey == "Ctrl+/ (no key)", ("a shortcut no key runs says so: " + noKey).c_str());
  keycaps::setCaps(TypingLayout::Standard);
  expect(keycaps::format(ctrl(Key::Slash)) == "Ctrl+/ (no key)" && keycaps::format(ctrl(Key::Q)) == "Ctrl+Q", "with QWERTY keys too");
  keycaps::setSystem(nullptr);
  keycaps::setCaps(TypingLayout::Computer);

  // 7. The setting's ids, and the move from the typing keyboard's layout choice.
  bool ids = true;
  for (TypingLayout l : keycaps::kCapsOrder) ids = ids && keycaps::capsFromId(keycaps::capsId(l)) == l;
  expect(ids && keycaps::capsFromId("") == TypingLayout::Computer && keycaps::capsFromId("bogus") == TypingLayout::Computer &&
             keycaps::capsFromId("standard") == TypingLayout::Standard && std::string(keycaps::capsId(TypingLayout::Standard)) == "qwerty",
         "ids: computer qwerty qwertz azerty dvorak colemak; unknown is computer");
  expect(std::string(keycaps::capsName(TypingLayout::Computer)) == "Same as my computer" && keycaps::kCapsOrder[0] == TypingLayout::Computer,
         "names: Same as my computer first");
  expect(Settings().getString("keys.myKeys") == "computer", "a fresh config: Same as my computer");
  const std::string dir = (std::filesystem::temp_directory_path() / "gridline_test_keycaps").string();
  std::filesystem::create_directories(dir);
  expect(loadMyKeys(dir, "\"ui.scale\": 1.0") == "computer", "an old file without the choice: computer");
  expect(loadMyKeys(dir, "\"keys.typingLayout\": \"standard\"") == "computer", "old Standard (QWERTY): computer (no shortcut renamed)");
  bool carried = true;
  for (const char* id : {"qwertz", "azerty", "dvorak", "colemak", "computer"}) {
    const std::string got = loadMyKeys(dir, std::string("\"keys.typingLayout\": \"") + id + "\"");
    if (got != id) std::printf("# %s became %s\n", id, got.c_str());
    carried = carried && got == id;
  }
  expect(carried, "old QWERTZ, AZERTY, Dvorak, Colemak and Follow the computer carry over");
  expect(loadMyKeys(dir, "\"keys.typingLayout\": \"dvorak\", \"keys.myKeys\": \"qwerty\"") == "qwerty", "a saved choice wins over the old one");
  // The note keys keep their names for a layout chosen before: the same table names them.
  bool notes = true;
  for (TypingLayout l : kListed)
    for (const char* k = "zsxdcvgbhnjm,l.;/q2w3er5t6y7ui9o0p[]-=\\'"; *k; ++k) {
      const int p = TypingKeyboard::positionOfKey(*k);
      notes = notes && TypingKeyboard::layoutChar(l, p) != 0;
    }
  expect(notes, "every note key has a name on every listed layout");
  std::filesystem::remove_all(dir);
  return finish();
}
