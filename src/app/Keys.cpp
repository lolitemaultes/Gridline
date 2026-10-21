// Gridline - keyboard commands in the UI (see Keys.h).
#include "Keys.h"
#include <unordered_map>

namespace gl::keys {

namespace {

Context gFocus = Context::Global;
bool gCapturing = false;
int gCaptureFrame = -10;  // capture lapses when the shortcut editor stops refreshing it (window closed)
int gReleaseFrame = -10;  // frame a capture ended in

const ImGuiKey kMap[] = {
    ImGuiKey_None,
    ImGuiKey_A, ImGuiKey_B, ImGuiKey_C, ImGuiKey_D, ImGuiKey_E, ImGuiKey_F, ImGuiKey_G, ImGuiKey_H, ImGuiKey_I, ImGuiKey_J,
    ImGuiKey_K, ImGuiKey_L, ImGuiKey_M, ImGuiKey_N, ImGuiKey_O, ImGuiKey_P, ImGuiKey_Q, ImGuiKey_R, ImGuiKey_S, ImGuiKey_T,
    ImGuiKey_U, ImGuiKey_V, ImGuiKey_W, ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z,
    ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9,
    ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F5, ImGuiKey_F6, ImGuiKey_F7, ImGuiKey_F8, ImGuiKey_F9,
    ImGuiKey_F10, ImGuiKey_F11, ImGuiKey_F12,
    ImGuiKey_Space, ImGuiKey_Enter, ImGuiKey_Escape, ImGuiKey_Tab, ImGuiKey_Backspace, ImGuiKey_Insert, ImGuiKey_Delete,
    ImGuiKey_Home, ImGuiKey_End, ImGuiKey_PageUp, ImGuiKey_PageDown,
    ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
    ImGuiKey_KeypadAdd, ImGuiKey_KeypadSubtract, ImGuiKey_KeypadMultiply, ImGuiKey_KeypadDivide, ImGuiKey_KeypadEnter,
    ImGuiKey_Keypad0, ImGuiKey_Keypad1, ImGuiKey_Keypad2, ImGuiKey_Keypad3, ImGuiKey_Keypad4, ImGuiKey_Keypad5,
    ImGuiKey_Keypad6, ImGuiKey_Keypad7, ImGuiKey_Keypad8, ImGuiKey_Keypad9,
    ImGuiKey_Minus, ImGuiKey_Equal, ImGuiKey_Comma, ImGuiKey_Period, ImGuiKey_Slash, ImGuiKey_Semicolon, ImGuiKey_Apostrophe,
    ImGuiKey_LeftBracket, ImGuiKey_RightBracket, ImGuiKey_Backslash, ImGuiKey_GraveAccent,
};
static_assert(sizeof(kMap) / sizeof(kMap[0]) == static_cast<size_t>(Key::Count), "ImGui key table");

uint8_t modsNow() {
  const ImGuiIO& io = ImGui::GetIO();
  return static_cast<uint8_t>(((io.KeyCtrl || io.KeySuper) ? kCtrl : 0) | (io.KeyShift ? kShift : 0) | (io.KeyAlt ? kAlt : 0));
}

struct LabelCache {
  uint32_t version = 0;
  std::unordered_map<std::string, std::string> text;
};
LabelCache& labelCache() {
  static LabelCache c;
  if (c.version != keymap().version()) {
    c.text.clear();
    c.version = keymap().version();
  }
  return c;
}

}  // namespace

ImGuiKey toImGui(Key k) {
  size_t i = static_cast<size_t>(k);
  return i < static_cast<size_t>(Key::Count) ? kMap[i] : ImGuiKey_None;
}

Key fromImGui(ImGuiKey k) {
  for (size_t i = 1; i < static_cast<size_t>(Key::Count); ++i)
    if (kMap[i] == k) return static_cast<Key>(i);
  return Key::None;
}

void setFocusContext(Context c) { gFocus = c; }
Context focusContext() { return gFocus; }

bool pressed(const char* id) {
  if (capturing() || ImGui::GetIO().WantTextInput) return false;
  const Command* cmd = findCommand(id);
  if (!cmd) return false;
  const uint8_t mods = modsNow();
  const Keymap& km = keymap();
  for (const Chord& c : km.bindings(id)) {
    if (c.mods != mods || !ImGui::IsKeyPressed(toImGui(c.key), cmd->repeat)) continue;
    if (cmd->context == Context::Global && gFocus != Context::Global && !km.lookup(gFocus, c).empty()) continue;  // the editor wins
    return true;
  }
  return false;
}

bool pressedHeld(const char* id) {
  if (capturing() || ImGui::GetIO().WantTextInput) return false;
  const Command* cmd = findCommand(id);
  if (!cmd) return false;
  for (const Chord& c : keymap().bindings(id))
    if (ImGui::IsKeyPressed(toImGui(c.key), cmd->repeat)) return true;
  return false;
}

bool keyDown(const char* id) {
  if (capturing() || ImGui::GetIO().WantTextInput || !findCommand(id)) return false;
  for (const Chord& c : keymap().bindings(id))
    if (ImGui::IsKeyDown(toImGui(c.key))) return true;
  return false;
}

bool modifierHeld(const char* id) {
  const uint8_t want = mouseModifier(id);
  return want != 0 && modsNow() == want;
}

bool modifierExact(const char* id) { return modsNow() == mouseModifier(id); }

bool modifierIncludes(const char* id) {
  const uint8_t want = mouseModifier(id);
  return want != 0 && (modsNow() & want) == want;
}

const char* label(const char* id) {
  LabelCache& lc = labelCache();
  auto it = lc.text.find(id);
  if (it == lc.text.end()) {
    const auto& b = keymap().bindings(id);
    it = lc.text.emplace(id, b.empty() ? std::string() : format(b.front())).first;
  }
  return it->second.empty() ? nullptr : it->second.c_str();
}

std::string labels(const char* id) {
  std::string s;
  for (const Chord& c : keymap().bindings(id)) s += (s.empty() ? "" : " / ") + format(c);
  return s;
}

void setCapturing(bool on) {
  // Ending a capture keeps the shortcuts quiet for the rest of this frame: the
  // captured key must not also run its old command.
  if (gCapturing && !on) gReleaseFrame = ImGui::GetFrameCount();
  gCapturing = on;
  gCaptureFrame = ImGui::GetFrameCount();
}
bool capturing() {
  const int f = ImGui::GetFrameCount();
  return (gCapturing && f - gCaptureFrame <= 1) || f == gReleaseFrame;
}

bool captureChord(Chord* out) {
  for (size_t i = 1; i < static_cast<size_t>(Key::Count); ++i)
    if (ImGui::IsKeyPressed(kMap[i], false)) {
      *out = Chord{static_cast<Key>(i), modsNow()};
      return true;
    }
  return false;
}

}  // namespace gl::keys
