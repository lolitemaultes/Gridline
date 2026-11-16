// Gridline - K-199: controller rules (see Gamepad.h).
#include "Gamepad.h"
#include <algorithm>
#include <cstring>

namespace gl::gamepad {

namespace {
struct Names {
  const char* sdl;
  const char* label;
};
constexpr Names kNames[static_cast<int>(Button::Count)] = {
    {"a", "A"},         {"b", "B"},         {"x", "X"},           {"y", "Y"},
    {"back", "View"},   {"guide", "Steam"}, {"start", "Menu"},    {"leftstick", "L3"},
    {"rightstick", "R3"}, {"leftshoulder", "L1"}, {"rightshoulder", "R1"},
    {"dpup", "Up"},     {"dpdown", "Down"}, {"dpleft", "Left"},   {"dpright", "Right"},
};
}  // namespace

Button buttonFromName(const std::string& name) {
  for (int i = 0; i < static_cast<int>(Button::Count); ++i)
    if (name == kNames[i].sdl) return static_cast<Button>(i);
  return Button::Count;
}
const char* buttonName(Button b) { return b < Button::Count ? kNames[static_cast<int>(b)].sdl : ""; }
const char* buttonLabel(Button b) { return b < Button::Count ? kNames[static_cast<int>(b)].label : ""; }

bool gameMode(const std::function<const char*(const char*)>& env) {
  auto set = [&](const char* k) {
    const char* v = env(k);
    return v && *v;
  };
  auto is = [&](const char* k, const char* want) {
    const char* v = env(k);
    return v && std::strcmp(v, want) == 0;
  };
  return is("SteamDeck", "1") || is("XDG_CURRENT_DESKTOP", "gamescope") || set("GAMESCOPE_WAYLAND_DISPLAY") ||
         is("SteamGamepadUI", "1") || is("SteamTenfoot", "1");
}

Mode modeFromSetting(const std::string& s) {
  if (s == "on") return Mode::On;
  if (s == "off") return Mode::Off;
  return Mode::Auto;
}
bool listening(Mode m, bool gameModeSession, bool controllerUsed) {
  return m == Mode::On || (m == Mode::Auto && (gameModeSession || controllerUsed));
}

const std::vector<Binding>& defaultBindings() {
  static const std::vector<Binding> v = {
      {Button::A, false, "transport.play"},
      {Button::A, true, "transport.record"},
      {Button::B, false, "pad.close"},
      {Button::B, true, "transport.toStart"},
      {Button::X, false, "edit.undo"},
      {Button::X, true, "edit.redo"},
      {Button::Y, false, "pad.toggle"},
      {Button::Y, true, "file.save"},
      {Button::Left, false, "pad.prevView"},
      {Button::Right, false, "pad.nextView"},
      {Button::Left, true, "pattern.previous"},
      {Button::Right, true, "pattern.next"},
      {Button::Up, false, "pad.prevChannel"},
      {Button::Down, false, "pad.nextChannel"},
      {Button::Up, true, "transport.rewindBar"},
      {Button::Down, true, "transport.forwardBar"},
      {Button::R1, false, "pad.notes"},
      {Button::Start, false, "pad.toggle"},
      {Button::Back, false, "view.commandPalette"},
      {Button::LeftStick, false, "options.typingKeyboard"},
      {Button::RightStick, false, "pad.osk"},
  };
  return v;
}

std::string commandFor(Button b, bool l1Held) {
  const Binding* plain = nullptr;
  for (const Binding& x : defaultBindings()) {
    if (x.button != b) continue;
    if (x.withL1 == l1Held) return x.command;
    if (!x.withL1) plain = &x;
  }
  return plain ? plain->command : "";
}

namespace {
struct PadCmd {
  const char* id;
  const char* label;
};
constexpr PadCmd kPadCmds[] = {
    {"pad.toggle", "Command pad"},          {"pad.close", "Close the pad or a dialog"},
    {"pad.nextView", "Next window"},        {"pad.prevView", "Previous window"},
    {"pad.prevChannel", "Previous channel"}, {"pad.nextChannel", "Next channel"},
    {"pad.notes", "Note entry on or off"},  {"pad.osk", "On-screen keyboard"},
};
}  // namespace

bool isPadCommand(const std::string& id) { return padCommandLabel(id) != nullptr; }
const char* padCommandLabel(const std::string& id) {
  for (const PadCmd& c : kPadCmds)
    if (id == c.id) return c.label;
  return nullptr;
}

bool playsNote(Button b) {
  switch (b) {
    case Button::Left: case Button::Down: case Button::Right: case Button::Up:
    case Button::X: case Button::A: case Button::B: case Button::Y: return true;
    default: return false;
  }
}

int noteFor(Button b, bool l1Held, int octave, int root, const std::vector<int>& scale) {
  static const Button order[8] = {Button::Left, Button::Down, Button::Right, Button::Up, Button::X, Button::A, Button::B, Button::Y};
  static const std::vector<int> major = {0, 2, 4, 5, 7, 9, 11};
  const std::vector<int>& sc = scale.size() >= 2 ? scale : major;
  int degree = -1;
  for (int i = 0; i < 8; ++i)
    if (order[i] == b) degree = i;
  if (degree < 0) return -1;
  const int n = static_cast<int>(sc.size());
  const int semis = sc[static_cast<size_t>(degree % n)] + 12 * (degree / n);
  const int pitch = 12 * std::clamp(octave, kMinOctave, kMaxOctave) + (((root % 12) + 12) % 12) + semis + (l1Held ? 1 : 0);
  return std::clamp(pitch, 0, 127);
}

std::vector<BindingRow> bindingRows(const std::function<const char*(const std::string&)>& commandLabel) {
  auto name = [&](const std::string& id) -> std::string {
    if (id.empty()) return "";
    if (const char* p = padCommandLabel(id)) return p;
    const char* l = commandLabel(id);
    return l ? l : id;
  };
  static const char* const degrees[8] = {"Note 1", "Note 2", "Note 3", "Note 4", "Note 5", "Note 6", "Note 7", "Note 8 (octave)"};
  static const Button order[8] = {Button::Left, Button::Down, Button::Right, Button::Up, Button::X, Button::A, Button::B, Button::Y};
  std::vector<BindingRow> out;
  for (int i = 0; i < static_cast<int>(Button::Count); ++i) {
    const Button b = static_cast<Button>(i);
    if (b == Button::Guide) continue;  // Steam's own button
    BindingRow r{b, "", "", ""};
    for (const Binding& x : defaultBindings())
      if (x.button == b) (x.withL1 ? r.withL1 : r.alone) = name(x.command);
    for (int k = 0; k < 8; ++k)
      if (order[k] == b) r.notes = degrees[k];
    if (b == Button::L1) r.alone = "Second command (hold)", r.notes = "Sharp (hold)";
    if (b == Button::R1) r.notes = r.alone;
    if (r.alone.empty() && r.withL1.empty() && r.notes.empty()) continue;
    out.push_back(r);
  }
  return out;
}

const std::array<Tile, kPadTiles>& padTiles() {
  static const std::array<Tile, kPadTiles> t = {{
      {"Play / stop", "transport.play"},
      {"Record", "transport.record"},
      {"To start", "transport.toStart"},
      {"Undo", "edit.undo"},
      {"Redo", "edit.redo"},
      {"Save", "file.save"},
      {"Playlist", "view.playlist"},
      {"Piano roll", "view.pianoRoll"},
      {"Channel rack", "view.channelRack"},
      {"Mixer", "view.mixer"},
      {"Browser", "view.browser"},
      {"Note entry", "pad.notes"},
      {"Typing keyboard", "options.typingKeyboard"},
      {"Command palette", "view.commandPalette"},
      {"On-screen keyboard", "pad.osk"},
  }};
  return t;
}

int padMove(int tile, int dx, int dy) {
  tile = std::clamp(tile, 0, kPadTiles - 1);
  int col = tile % kPadCols, row = tile / kPadCols;
  col = ((col + dx) % kPadCols + kPadCols) % kPadCols;
  row = std::clamp(row + dy, 0, kPadRows - 1);
  return row * kPadCols + col;
}

const std::vector<const char*>& viewCycle() {
  static const std::vector<const char*> v = {"view.playlist", "view.channelRack", "view.pianoRoll", "view.mixer", "view.browser"};
  return v;
}
int nextView(int current, int dir) {
  const int n = static_cast<int>(viewCycle().size());
  if (current < 0 || current >= n) return dir < 0 ? n - 1 : 0;
  return ((current + dir) % n + n) % n;
}

}  // namespace gl::gamepad
