// Gridline - "My keys say" (GitHub #79): what is printed on the person's keys, and the names of keys
// and shortcuts that follow from it. No shortcut moves: a shortcut is shown by the printed key that
// runs it on the computer's current layout.
//
// How a key press reaches the shortcuts: the interface names a key by what it types for letters,
// digits and , . ; and by where it sits on a US keyboard for the other punctuation (- = [ ] \ ' / `).
// On a layout where that leaves one of these with no key at all (US Dvorak has no / key that way),
// the punctuation keys go by what they type instead, as the letters do, so every shortcut keeps a key.
// On US QWERTY, QWERTZ, AZERTY, Colemak and every layout without such a gap nothing changes.
//
// Positions are USB usages (SDL's scan codes): 4..29 the letter keys, 30..39 the digit row, 45..56
// the punctuation keys, named after where they sit on US QWERTY.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include "Keymap.h"
#include "MidiRecord.h"

namespace gl::keycaps {

// The computer's layout: what a position types without Shift (a code point, 0 = nothing known). The
// app passes SDL's keymap; without one (tests, plugin builds) the computer types US QWERTY.
void setSystem(int (*typedAt)(int position));
// Tests: a listed layout's characters stand in for the computer's (digits stay digits, as SDL keeps
// them). TypingLayout::Computer goes back to the real one.
void simulateSystem(TypingLayout l);
TypingLayout simulatedSystem();  // Computer when none
// What the computer's layout types at `position`.
int systemChar(int position);
// Reads the computer's layout again (once a frame; cheap): a change bumps version().
void refresh();

// What the person's keys say. Computer ("Same as my computer", the default): every name as before.
void setCaps(TypingLayout l);
TypingLayout caps();
// The setting's values: "computer", "qwerty", "qwertz", "azerty", "dvorak", "colemak" (anything else:
// computer; "standard", the typing keyboard's old name for QWERTY, reads as qwerty).
const char* capsId(TypingLayout l);
TypingLayout capsFromId(const std::string& id);
// The choice's name ("Same as my computer", "QWERTY", ...), untranslated (N_).
const char* capsName(TypingLayout l);
// The order the choices are listed in (Computer first).
constexpr TypingLayout kCapsOrder[] = {TypingLayout::Computer, TypingLayout::Standard, TypingLayout::Qwertz,
                                       TypingLayout::Azerty,   TypingLayout::Dvorak,   TypingLayout::Colemak};

// Bumps when the choice or the computer's layout changes (labels cached by the app follow it).
uint32_t version();

// The key a press at `position` gives the shortcuts, Key::None for none (or not a letter, digit or
// punctuation position).
keys::Key keyAt(int position);
// The position that gives `k`: 0 when no key does, -1 for keys that are not letters, digits or
// punctuation (function keys, arrows, the keypad...: always there, never renamed).
int positionOf(keys::Key k);
// True when the punctuation keys go by what they type (the computer's layout had a gap).
bool punctuationByTyped();
// For key events: the US position whose punctuation key a press at `position` gives when that
// differs from what the interface would make of it (the app tells the interface that one instead),
// else 0.
int remapPosition(int position);

// A key's name as the keys say ("J" for H on Dvorak with QWERTY keys); keys::keyName(k) for
// Computer and for keys that are never renamed. A key no key gives keeps its own name.
std::string keyName(keys::Key k);
// keys::format(c) with keyName(); a chord no key can run says so: "Ctrl+/ (no key)".
std::string format(const keys::Chord& c);
// The same for text written as a chord ("Ctrl+X"); other text comes back unchanged.
std::string formatText(const std::string& text);

// ---- Keys named inside a sentence (tooltips, hints, status lines, help text) ----
// A sentence is translated whole with a %s where each key goes, and the keys go in by name:
//   withKeys(N_("Metronome (%s)"), {cmdKey("options.metronome", "Ctrl+M")})
// At "Same as my computer" with no shortcut changed the sentence reads exactly as it was written.

// A command's key in a sentence. `fixed` is the chord the sentence was written for ("Ctrl+M", or
// "Cmd+L" in a Clips sentence: Cmd and Ctrl count as one here). It stays, named as the keys say, while
// the command has that chord in the shortcut set in use (with the person's changes); else the
// command's first chord there, spelt with Cmd where `fixed` is or where the menus show Cmd (Clips on a
// Mac). A command the set gives no key keeps `fixed`; one the person took every key from: "no key".
std::string cmdKey(const char* id, const char* fixed);
// A key no command runs (an editor's own key: "Alt+Q" in the Tracker's Sample editor), named as the
// keys say. Function keys, arrows and the like are never renamed.
std::string fixedKey(const char* fixed);
// The translated `text` with each %s in turn replaced by the next of `keyTexts`.
std::string fill(const char* text, std::initializer_list<std::string> keyTexts);
// The same as a pointer that stays valid for the whole run (for widgets that take const char*).
const char* withKeys(const char* text, std::initializer_list<std::string> keyTexts);
// A key list as the Tracker help writes it ("Ctrl+X C V", "G P T X Y R I", "0 ... 9, A ... F"): each
// chord and each single key named as the keys say; `byPlace`: the keys play notes by where they sit
// (named after what the chosen keys print at that place, as the typing keyboard names them; at
// "Same as my computer" as written).
std::string keyList(const std::string& text, bool byPlace);
// The name of the typing key `key` (lower-case US character) sits where, as the keys say.
std::string placeName(int key);

}  // namespace gl::keycaps
