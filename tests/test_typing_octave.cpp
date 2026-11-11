// Gridline: the typing keyboard keeps its octave (GitHub issue 42). Physical key events by position:
// only a fresh press of an octave key moves the octave, one step; repeats, a key-down reported twice
// and keys reported again as the focus comes back do nothing; focus loss lets every key go; every
// layout plays by position; the keys it plays run no other shortcut while it is on.
#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "../src/core/Keymap.h"
#include "../src/core/MidiRecord.h"
#include "Check.h"

using namespace gl;

namespace {
// US positions (USB usage = SDL scan code).
constexpr int kZ = 29, kQ = 20, kLeftBracket = 47, kRightBracket = 48, kMinus = 45, kEqual = 46;
constexpr int kPadMinus = 86, kPadPlus = 87, kA = 4, kLeftCtrl = 224;

struct Rng {
  uint32_t s;
  uint32_t next(uint32_t n) {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) % n;
  }
};
}  // namespace

int main() {
  // ---------------------------------------------------------------- repeat
  {
    TypingKeyboard k;
    k.press(kRightBracket);
    for (int i = 0; i < 30; ++i) k.press(kRightBracket, true);  // the system's key repeat
    expect(k.octave == 5, "repeat: a held ] with thirty repeats is one step");
    for (int i = 0; i < 5; ++i) k.press(kRightBracket);  // the same key-down again, unmarked
    expect(k.octave == 5, "repeat: key-downs of a key already down are no presses");
    k.release(kRightBracket);
    k.press(kRightBracket);
    k.release(kRightBracket);
    expect(k.octave == 6 && k.octaveSteps() == 2, "repeat: a new press after the release steps again");
    k.press(kPadMinus);
    k.press(kPadMinus, true);
    k.release(kPadMinus);
    expect(k.octave == 5, "repeat: keypad - steps down once");
    k.press(kZ);
    expect(k.press(kZ, true).empty() && k.press(kZ).empty() && k.heldNotes() == std::vector<int>{60},
           "repeat: a held note does not start again");
    expect(k.release(kZ) == std::vector<int>{60} && k.release(kZ).empty(), "repeat: the note ends once");
    expect(k.press(kA).empty() && !k.isDown(kA), "repeat: keys that play nothing are not tracked");
  }

  // ---------------------------------------------------------------- focus loss and focus coming back
  {
    TypingKeyboard k;
    k.press(kZ);
    k.press(kLeftBracket);
    expect(k.octave == 3 && k.heldNotes().size() == 1, "focus: a note and [ held");
    expect(k.releaseAllKeys() == std::vector<int>{48} && !k.anyHeld(), "focus lost: the held note ends");
    expect(!k.isDown(kLeftBracket) && !k.isDown(kZ), "focus lost: no key counts as held");
    k.press(kLeftBracket);  // its key-up went to the other application: the next press is still a press
    expect(k.octave == 2, "focus lost: the octave key works on the next press");
    k.release(kLeftBracket);
    // Coming back: the system reports keys still held as key-downs.
    k.press(kRightBracket);
    k.release(kRightBracket);  // the system's release as the focus leaves
    k.focusGained();
    k.press(kRightBracket);  // reported down again as the focus comes back
    k.press(kQ);
    k.settle();
    expect(k.octave == 3 && !k.anyHeld(), "focus back: keys reported held again neither step nor play");
    expect(k.isDown(kRightBracket) && k.isDown(kQ), "focus back: they count as down until released");
    k.press(kRightBracket);
    expect(k.octave == 3, "focus back: no step before their release");
    k.release(kRightBracket);
    k.release(kQ);
    k.press(kRightBracket);
    expect(k.octave == 4, "focus back: the next real press steps");
  }

  // ---------------------------------------------------------------- per layout
  {
    // Every layout's keys sit where the US ones do: what a position types never changes what it does.
    for (int l = 0; l < static_cast<int>(TypingLayout::Count); ++l) {
      const TypingLayout layout = static_cast<TypingLayout>(l);
      TypingKeyboard k;
      bool notesOk = true, controlsOk = true;
      for (int position = 1; position < 256; ++position) {
        const int key = TypingKeyboard::keyAtPosition(position);
        if (key == 0) continue;
        const int octave = k.octave, velocity = k.velocity;
        const std::vector<int> on = k.press(position);
        k.release(position);
        const int off = TypingKeyboard::keyOffset(key);
        if (off >= 0) notesOk = notesOk && on == std::vector<int>{12 * octave + off};
        const bool octaveKey = key == '[' || key == ']';
        if (octaveKey) controlsOk = controlsOk && k.octave == octave + (key == ']' ? 1 : -1) && k.velocity == velocity;
        else controlsOk = controlsOk && k.octave == octave;
        k.octave = 4;
        k.velocity = 100;
        k.chordMode = false;
      }
      const std::string id = typingLayoutId(layout);
      expect(notesOk, ("layout " + id + ": every note key plays by its position").c_str());
      expect(controlsOk, ("layout " + id + ": only the [ ] positions and keypad - + move the octave").c_str());
    }
    // Layouts that type [ or ] on the velocity positions (US Dvorak types [ ] where US has - =).
    expect(TypingKeyboard::layoutChar(TypingLayout::Dvorak, kMinus) == '[' && TypingKeyboard::keyAtPosition(kMinus) == '-',
           "layout dvorak: the position typing [ is a velocity key");
    TypingKeyboard k;
    k.press(kMinus);
    k.release(kMinus);
    k.press(kEqual);
    k.release(kEqual);
    expect(k.octave == 4 && k.velocity == 100, "layout dvorak: [ and ] typed there change the velocity, not the octave");
    // AZERTY types A at the Q position, Q at the A position: the Q position plays, the A position does not.
    expect(TypingKeyboard::layoutChar(TypingLayout::Azerty, kQ) == 'a' && k.press(kQ) == std::vector<int>{60} && k.press(kA).empty(),
           "layout azerty: the Q position (typing A) plays C5, the A position (typing Q) plays nothing");
    k.release(kQ);
    // QWERTZ types Z at the Y position and Y at the Z position.
    expect(TypingKeyboard::layoutChar(TypingLayout::Qwertz, kZ) == 'y' && k.press(kZ) == std::vector<int>{48},
           "layout qwertz: the Z position (typing Y) plays C4");
    k.release(kZ);
    expect(TypingKeyboard::keyAtPosition(kPadMinus) == '[' && TypingKeyboard::keyAtPosition(kPadPlus) == ']',
           "keypad - + are octave keys on every layout");
  }

  // ---------------------------------------------------------------- each key once
  {
    using keys::Chord;
    using keys::Key;
    expect(keys::playsOnTypingKeyboard(Chord{Key::NumAdd, 0}) && keys::playsOnTypingKeyboard(Chord{Key::NumSubtract, 0}),
           "once: keypad + - belong to the typing keyboard (not the pattern too)");
    expect(keys::playsOnTypingKeyboard(Chord{Key::Minus, 0}) && keys::playsOnTypingKeyboard(Chord{Key::Equal, 0}),
           "once: - = belong to the typing keyboard (not the zoom too)");
    expect(keys::playsOnTypingKeyboard(Chord{Key::Q, 0}) && keys::playsOnTypingKeyboard(Chord{Key::Q, keys::kShift}),
           "once: Q and Shift+Q play notes");
    expect(!keys::playsOnTypingKeyboard(Chord{Key::Q, keys::kCtrl}) && !keys::playsOnTypingKeyboard(Chord{Key::S, keys::kCmd}) &&
               !keys::playsOnTypingKeyboard(Chord{Key::LeftBracket, keys::kAlt}),
           "once: Ctrl, Cmd and Alt chords stay shortcuts");
    expect(!keys::playsOnTypingKeyboard(Chord{Key::Space, 0}) && !keys::playsOnTypingKeyboard(Chord{Key::F5, 0}) &&
               !keys::playsOnTypingKeyboard(Chord{Key::NumEnter, 0}) && !keys::playsOnTypingKeyboard(Chord{Key::Up, keys::kShift}),
           "once: Space, F keys, keypad Enter and arrows stay shortcuts");
  }

  // ---------------------------------------------------------------- a long random session
  {
    // Thousands of notes, chords and held keys; octave keys held with repeats, their key-downs
    // reported twice; keys reported again as the focus comes back; focus lost; modifier keys.
    // The octave must only move on fresh octave presses, one step each.
    TypingKeyboard k;
    Rng rng{42};
    const int noteKeys[] = {29, 22, 27, 7, 6, 25, 10, 5, 11, 17, 13, 16, 54, 15, 55, 51, 56,
                            20, 31, 26, 32, 8, 21, 34, 23, 35, 28, 36, 24, 12, 38, 18, 39, 19};
    std::set<int> held;
    int want = 4, presses = 0, wrongNow = 0, maxNotes = 0, notes = 0;
    auto octavePress = [&](int position, int dir) {
      const bool fresh = !k.isDown(position);
      k.press(position);
      if (fresh) {
        ++presses;
        want = std::clamp(want + dir, 0, 9);
      }
      const uint32_t reps = rng.next(8);
      for (uint32_t i = 0; i < reps; ++i) k.press(position, true);
      if (rng.next(3) == 0) k.press(position);  // a copy
      if (rng.next(4) == 0) {                   // the focus moves away and back while it is held
        k.release(position);
        k.focusGained();
        k.press(position);
        k.settle();
      }
      k.release(position);
    };
    for (int i = 0; i < 20000; ++i) {
      const uint32_t r = rng.next(100);
      if (r < 50) {
        const int p = noteKeys[rng.next(sizeof noteKeys / sizeof noteKeys[0])];
        if (held.count(p)) {
          k.release(p);
          held.erase(p);
        } else {
          notes += static_cast<int>(k.press(p).size());
          held.insert(p);
        }
      } else if (r < 60) {
        for (int p : held) k.release(p);
        held.clear();
      } else if (r < 68) {
        const int pos[] = {kLeftBracket, kRightBracket, kPadMinus, kPadPlus};
        const int which = static_cast<int>(rng.next(4));
        octavePress(pos[which], which % 2 ? 1 : -1);
      } else if (r < 72) {
        k.press(rng.next(2) ? kMinus : kEqual);
        k.release(kMinus);
        k.release(kEqual);
      } else if (r < 76) {  // focus moves inside Gridline: held keys released, then reported down again
        for (int p : held) k.release(p);
        k.focusGained();
        for (int p : held) k.press(p);
        k.settle();
      } else if (r < 78) {  // focus leaves Gridline
        k.releaseAllKeys();
        held.clear();
      } else if (r < 82) {  // modifier keys are not typing keys
        k.press(kLeftCtrl);
        k.release(kLeftCtrl);
      }
      if (k.octave != want) ++wrongNow;
      maxNotes = std::max(maxNotes, static_cast<int>(k.heldNotes().size()));
    }
    for (int p : held) k.release(p);
    std::printf("# session: %d notes started, %d octave presses, %d steps\n", notes, presses, k.octaveSteps());
    expect(notes > 4000 && presses > 1000, "session: thousands of notes and octave presses");
    expect(wrongNow == 0, "session: the octave never moved except on octave presses");
    expect(!k.anyHeld(), "session: no note left held once every key is up");
    expect(maxNotes <= 34, "session: never more notes held than keys");
  }
  return finish();
}
