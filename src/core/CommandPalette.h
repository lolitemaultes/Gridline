// Gridline - K-93 command palette: the rows and how a query ranks them (no UI here).
//
// The app builds the rows from what is already there (app/CommandPaletteUi.cpp): the menus
// as drawn, the central keymap, the windows, the settings pages and the plugin list. This
// file only knows rows, the fuzzy match and the recent list, so the ranking is testable.
//
// Matching: every query word must match the row (its label, English label, path or
// shortcut). A word scores best as the whole label, then as the label's start, the start of
// one of its words, the initials of its words, anywhere in it, and last as letters in order
// with gaps. A word that matches none of these still matches a word of the text within a
// small edit distance (a typo: one letter wrong, missing, extra or two swapped).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gl::cmdpal {

enum class Kind : uint8_t { Command, Window, Setting, Plugin, Count };
const char* kindName(Kind k);  // English, translatable: "Commands", "Windows", "Settings", "Plugins"

struct Entry {
  std::string id;        // stable over runs and languages: "cmd:file.save", "menu:Options/Song mode", "win:Mixer", ...
  Kind kind = Kind::Command;
  std::string label;     // as shown (translated)
  std::string labelEn;   // English (empty: same as label)
  std::string path;      // where it lives, as shown: "Options > Recording options" (empty: top level)
  std::string pathEn;    // English path (empty: same as path)
  std::string alias;     // more words it is found by (the keymap's description, a settings group), shown nowhere
  std::string aliasEn;
  std::string shortcut;  // "Ctrl+S", empty when unbound
  std::string hint;      // what it does; for a disabled row the reason, when the menu gives one
  std::string commandId; // keymap command behind the row ("" when none)
  bool enabled = true;
  bool checkable = false, checked = false;  // on / off items show their state
  int order = 0;         // the order the rows were found in (menus top to bottom): the tie breaker
};

// Score of one query word in `text` (both compared case-insensitively), < 0 when it does not
// match. `hits` (optional) gets the byte offsets of the matched characters in `text`.
int wordScore(const std::string& word, const std::string& text, std::vector<int>* hits = nullptr);

// Score of the whole query for a row (< 0: no match). `labelHits` gets the matched byte
// offsets in the row's shown label (for highlighting).
int entryScore(const Entry& e, const std::string& query, std::vector<int>* labelHits = nullptr);

struct Ranked {
  int index = 0;            // into the entries
  int score = 0;
  bool recent = false;      // one of the recent rows (the empty query lists them first)
  std::vector<int> hits;    // matched offsets in the label
};

// The rows for `query` in the order to show them. `kind` < 0: every kind, else only rows of
// that Kind. An empty query lists the recent rows first (newest first), then every other row
// in its order. A query lists matches best first; recent rows and enabled rows win ties.
std::vector<Ranked> rank(const std::vector<Entry>& entries, const std::string& query, const std::vector<std::string>& recent, int kind = -1);

// ---- recent rows (a settings value: one id per line, newest first) ----
constexpr size_t kRecentMax = 8;
std::vector<std::string> parseRecent(const std::string& text);
std::string serializeRecent(const std::vector<std::string>& ids);
// Moves `id` to the front (added when new), keeping at most `cap` ids.
void pushRecent(std::vector<std::string>& ids, const std::string& id, size_t cap = kRecentMax);

// Helpers (exposed for tests).
std::string lower(const std::string& s);  // ASCII letters only; UTF-8 bytes are kept
int editDistance(const std::string& a, const std::string& b, int limit);  // adjacent swaps count 1; > limit returns limit + 1
std::string stripId(const std::string& label);  // "Save##x" -> "Save"

}  // namespace gl::cmdpal
