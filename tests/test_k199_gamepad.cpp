// Gridline - K-199: controller rules (core/Gamepad.h): Game mode detection, the default bindings
// (each one a real keymap command or a pad command), the L1 layer and the command pad's moves.
#include <cstring>
#include <map>
#include <set>
#include <string>
#include "Check.h"
#include "core/Gamepad.h"
#include "core/Keymap.h"

using namespace gl;
using namespace gl::gamepad;

namespace {
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }
bool padCommand(const std::string& c) { return c.rfind("pad.", 0) == 0; }
bool realCommand(const std::string& c) {
  if (padCommand(c)) return isPadCommand(c) && padCommandLabel(c) != nullptr;
  return keys::findCommand(c) != nullptr;
}
bool detect(const std::map<std::string, std::string>& m) {
  return gameMode([&](const char* k) -> const char* {
    auto it = m.find(k);
    return it == m.end() ? nullptr : it->second.c_str();
  });
}
}  // namespace

int main() {
  // ---- detection
  expect(!detect({}), "no Game mode in a plain desktop session");
  expect(detect({{"SteamDeck", "1"}}), "SteamDeck=1 is Game mode");
  expect(!detect({{"SteamDeck", "0"}}), "SteamDeck=0 is not");
  expect(detect({{"XDG_CURRENT_DESKTOP", "gamescope"}}), "a gamescope desktop is Game mode");
  expect(!detect({{"XDG_CURRENT_DESKTOP", "KDE"}}), "a KDE desktop is not");
  expect(detect({{"GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0"}}), "a gamescope display is Game mode");
  expect(detect({{"SteamGamepadUI", "1"}}), "Steam's controller interface is Game mode");
  expect(!detect({{"GAMESCOPE_WAYLAND_DISPLAY", ""}}), "an empty variable does not count");

  // ---- the setting
  expect(modeFromSetting("") == Mode::Auto && modeFromSetting("auto") == Mode::Auto, "input.gamepad defaults to automatic");
  expect(modeFromSetting("on") == Mode::On && modeFromSetting("off") == Mode::Off, "on and off");
  expect(!listening(Mode::Auto, false, false), "automatic: not listening on a desktop without a pad press");
  expect(listening(Mode::Auto, true, false), "automatic: listening in Game mode");
  expect(listening(Mode::Auto, false, true), "automatic: listening once a controller is used");
  expect(!listening(Mode::Off, true, true), "off never listens");
  expect(listening(Mode::On, false, false), "on always listens");

  // ---- button names
  for (int i = 0; i < static_cast<int>(Button::Count); ++i) {
    const Button b = static_cast<Button>(i);
    expect(buttonFromName(buttonName(b)) == b, std::string("button name round trip: ") + buttonName(b));
    expect(std::strlen(buttonLabel(b)) > 0, std::string("button label: ") + buttonName(b));
  }
  expect(buttonFromName("nonsense") == Button::Count, "an unknown name is no button");

  // ---- bindings: every command exists, no button+layer is bound twice
  std::set<std::pair<int, bool>> seen;
  for (const Binding& b : defaultBindings()) {
    expect(realCommand(b.command), std::string("binding runs a real command: ") + b.command);
    expect(seen.insert({static_cast<int>(b.button), b.withL1}).second, std::string("bound once: ") + buttonName(b.button));
  }
  expect(commandFor(Button::A, false) == "transport.play", "A plays");
  expect(commandFor(Button::A, true) == "transport.record", "L1+A records");
  expect(commandFor(Button::X, false) == "edit.undo" && commandFor(Button::X, true) == "edit.redo", "X undoes, L1+X redoes");
  expect(commandFor(Button::Y, true) == "file.save", "L1+Y saves");
  expect(commandFor(Button::Start, false) == "pad.toggle" && commandFor(Button::Y, false) == "pad.toggle", "Menu and Y open the pad");
  expect(commandFor(Button::Start, true) == "pad.toggle", "a button without an L1 binding keeps its own with L1 held");
  expect(commandFor(Button::Back, false) == "view.commandPalette", "View opens the command palette");
  expect(commandFor(Button::Up, false) == "pad.prevChannel" && commandFor(Button::Down, false) == "pad.nextChannel",
         "D-pad up and down pick the channel outside the pad");
  expect(commandFor(Button::Left, false) == "pad.prevView" && commandFor(Button::Right, false) == "pad.nextView", "D-pad left and right step the windows");
  expect(commandFor(Button::R1, false) == "pad.notes", "R1 turns note entry on and off");
  expect(!isPadCommand("pad.nonsense") && !isPadCommand("transport.play"), "only the pad's own ids are pad commands");
  expect(commandFor(Button::L1, false).empty(), "L1 itself is the layer");

  // ---- the pad
  std::set<std::string> cmds;
  for (const Tile& t : padTiles()) {
    expect(realCommand(t.command), std::string("tile runs a real command: ") + t.label);
    cmds.insert(t.command);
  }
  for (const char* need : {"transport.play", "transport.record", "edit.undo", "edit.redo", "file.save", "view.playlist", "view.pianoRoll",
                           "view.channelRack", "view.mixer", "options.typingKeyboard", "view.commandPalette", "pad.osk"})
    expect(cmds.count(need) == 1, std::string("the pad offers ") + need);
  expect(padMove(0, 1, 0) == 1, "right moves one tile");
  expect(padMove(4, 1, 0) == 0, "right wraps within the row");
  expect(padMove(0, -1, 0) == 4, "left wraps within the row");
  expect(padMove(5, -1, 0) == 9, "left wraps on the second row too");
  expect(padMove(0, 0, -1) == 0, "up stops at the first row");
  expect(padMove(12, 0, 1) == 12, "down stops at the last row");
  expect(padMove(2, 0, 1) == 7 && padMove(7, 0, 1) == 12, "down moves a row");
  expect(padMove(99, 0, 0) == kPadTiles - 1 && padMove(-3, 0, 0) == 0, "a tile out of range is clamped");

  // ---- the note layer
  {
    const Button order[8] = {Button::Left, Button::Down, Button::Right, Button::Up, Button::X, Button::A, Button::B, Button::Y};
    const int major[8] = {0, 2, 4, 5, 7, 9, 11, 12};
    bool ok = true;
    for (int i = 0; i < 8; ++i) ok = ok && playsNote(order[i]) && noteFor(order[i], false, kDefaultOctave) == 60 + major[i];
    expect(ok, "the eight note buttons play C major from middle C (60)");
    expect(noteFor(Button::A, true, kDefaultOctave) == 70, "L1 raises a note a semitone");
    expect(noteFor(Button::Left, false, 6) == 72 && noteFor(Button::Left, false, 4) == 48, "the octave moves every note by 12");
    expect(noteFor(Button::Left, false, 99) == 12 * kMaxOctave && noteFor(Button::Left, false, -5) == 12 * kMinOctave, "the octave is clamped");
    expect(noteFor(Button::Right, false, kDefaultOctave, 2, {0, 2, 3, 5, 7, 8, 10}) == 65, "a scale and root: D minor's third is F");
    for (Button b : {Button::Start, Button::Back, Button::L1, Button::R1, Button::LeftStick, Button::RightStick})
      expect(!playsNote(b) && noteFor(b, false, kDefaultOctave) < 0, std::string("keeps its job in the note layer: ") + buttonName(b));
  }

  // ---- the list on the Shortcuts page
  {
    const auto rows = bindingRows([](const std::string& id) -> const char* {
      const keys::Command* c = keys::findCommand(id);
      return c ? c->label : nullptr;
    });
    std::set<int> seenButtons;
    bool named = true;
    int notes = 0;
    for (const auto& r : rows) {
      seenButtons.insert(static_cast<int>(r.button));
      for (const Binding& b : defaultBindings()) named = named && r.alone != b.command && r.withL1 != b.command;  // names, not ids
      notes += r.notes.size() >= 6 && r.notes.rfind("Note ", 0) == 0 && r.notes[5] >= '1' && r.notes[5] <= '8' ? 1 : 0;
    }
    expect(seenButtons.size() == rows.size(), "one row per button");
    expect(named, "the list shows command names, not ids");
    expect(notes == 8, "the list shows the eight note buttons");
    bool hasA = false;
    for (const auto& r : rows)
      if (r.button == Button::A) hasA = !r.alone.empty() && !r.withL1.empty() && r.notes == "Note 6";
    expect(hasA, "A: play, record, note 6");
  }

  // ---- window cycling
  const int n = static_cast<int>(viewCycle().size());
  for (const char* v : viewCycle()) expect(keys::findCommand(v) != nullptr, std::string("cycled window is a command: ") + v);
  expect(nextView(-1, 1) == 0 && nextView(-1, -1) == n - 1, "from no window: the first (or last)");
  expect(nextView(n - 1, 1) == 0 && nextView(0, -1) == n - 1, "cycling wraps");
  return finish();
}
