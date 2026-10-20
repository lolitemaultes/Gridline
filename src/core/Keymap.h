// Gridline - command / shortcut registry (ST-0x, "manage shortcuts").
//
// Every keyboard command of the app is one entry here: a stable id, the
// context it acts in (the whole app, or one editor window), an English label
// and its default chords (FL Studio's default map). The user's changes are
// stored as overrides only, as a small JSON object in the settings
// ("keys.bindings"), so new defaults reach users who never touched a key.
//
// Contexts: a command bound in an editor context wins over a global command
// with the same chord while that editor has focus (the global one is
// "shadowed" there). Two commands of the same context on one chord are a
// conflict; the shortcut editor refuses to create one silently.
//
// The core knows keys by its own names (Key); the app maps them to ImGui keys
// (app/Keys.cpp). Nothing here depends on the UI library.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gl::keys {

enum class Key : uint8_t {
  None,
  A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
  D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
  Space, Enter, Escape, Tab, Backspace, Insert, Delete, Home, End, PageUp, PageDown,
  Up, Down, Left, Right,
  NumAdd, NumSubtract, NumMultiply, NumDivide, NumEnter,
  Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
  Minus, Equal, Comma, Period, Slash, Semicolon, Apostrophe, LeftBracket, RightBracket, Backslash, Grave,
  Count
};

const char* keyName(Key k);       // "S", "F4", "PgUp", "Num +", ...
Key keyFromName(const std::string& name);  // case-insensitive; Key::None if unknown

enum Mod : uint8_t { kCtrl = 1, kShift = 2, kAlt = 4 };

struct Chord {
  Key key = Key::None;
  uint8_t mods = 0;
  bool valid() const { return key != Key::None; }
  bool operator==(const Chord& o) const { return key == o.key && mods == o.mods; }
  bool operator!=(const Chord& o) const { return !(*this == o); }
  bool operator<(const Chord& o) const { return key != o.key ? key < o.key : mods < o.mods; }
};

// "Ctrl+Alt+Shift+S" (modifier order Ctrl, Alt, Shift). Empty for an invalid chord.
std::string format(const Chord& c);
// Accepts any modifier order and case ("shift+ctrl+s", "Ctrl+Num +"). Invalid -> Key::None.
Chord parse(const std::string& text);

enum class Context : uint8_t { Global, Playlist, PianoRoll, ChannelRack, Mixer, Automation, Count };
const char* contextName(Context c);  // English, for the shortcut editor ("Piano roll", ...)

struct Command {
  const char* id;        // stable, persisted: "file.save", "piano.toolDraw"
  Context context;
  const char* label;     // English description (translatable)
  std::vector<Chord> defaults;
  bool repeat = false;   // fires again while the key is held (arrows, moves)
};

// The default command table (FL's default shortcuts), in display order.
const std::vector<Command>& commands();
const Command* findCommand(const std::string& id);

class Keymap {
 public:
  // Current chords of a command (overrides, else defaults). Empty for unknown ids.
  const std::vector<Chord>& bindings(const std::string& id) const;
  // Replaces the chords of one command (empty = unbound). False for unknown ids.
  bool set(const std::string& id, std::vector<Chord> chords);
  void reset(const std::string& id);
  void resetAll();
  bool customized(const std::string& id) const;
  bool anyCustomized() const { return !overrides_.empty(); }

  // Commands of exactly this context bound to the chord.
  std::vector<const Command*> lookup(Context ctx, const Chord& c) const;
  // Commands that would clash with binding `chord` to `id`: same context, other id.
  std::vector<const Command*> conflicts(const std::string& id, const Chord& chord) const;
  // Commands that overlap across the global / editor boundary (informational:
  // the editor binding wins while that editor has focus).
  std::vector<const Command*> shadows(const std::string& id, const Chord& chord) const;
  // Every same-context clash in the whole map, as "idA|idB|chord" strings.
  std::vector<std::string> allConflicts() const;
  // Binds `chord` to `id` (replaceAll: as its only chord, else added to the
  // existing ones) and removes it from every conflicting command of the same
  // context. Returns the ids that lost the chord.
  std::vector<std::string> assign(const std::string& id, const Chord& chord, bool replaceAll = true);

  // Overrides only, as a JSON object {"id": ["Ctrl+S", ...], ...}.
  std::string serialize() const;
  // Missing / empty text = defaults. Unknown ids and unparsable chords are dropped.
  bool load(const std::string& json, std::string* err = nullptr);

  // Increases on every change (the app caches menu labels per version).
  uint32_t version() const { return version_; }

 private:
  std::map<std::string, std::vector<Chord>> overrides_;
  uint32_t version_ = 1;
};

// The app's keymap.
Keymap& keymap();

}  // namespace gl::keys
