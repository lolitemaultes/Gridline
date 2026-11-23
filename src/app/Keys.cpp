// Gridline - keyboard commands in the UI (see Keys.h).
#include "Keys.h"
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/KeyCaps.h"

namespace gl::keys {

namespace {

Context gFocus = Context::Global;
bool gCapturing = false;
int gEscClaimFrame = -10;  // K-124: frame in which something used Esc
bool gEscBusyAtStart = false;
int gCaptureFrame = -10;  // capture lapses when the shortcut editor stops refreshing it (window closed)
int gReleaseFrame = -10;  // frame a capture ended in
std::string gInjected;    // K-93: a command the command palette runs (as if its key was pressed)
int gInjectFrame = -10;

bool injectedNow(const char* id) { return gInjectFrame == ImGui::GetFrameCount() && gInjected == id; }

bool gTyping = false;  // K-175
bool gTypingARow = false;  // R09-10
bool gEditorKeys = false;  // K-106
// R09-10: the A-row typing keyboard (Smooth, Clips) uses only its own keys, as the reference's: the
// notes (A W S E D F T G Y H U J K O L P ; ') and Z X (octave), C V (velocity). Every other key
// (R records, Space plays, B, I, X ...) still runs its command.
bool aRowKey(Key k) {
  switch (k) {
    case Key::A: case Key::W: case Key::S: case Key::E: case Key::D: case Key::F: case Key::T: case Key::G: case Key::Y:
    case Key::H: case Key::U: case Key::J: case Key::K: case Key::O: case Key::L: case Key::P: case Key::Semicolon:
    case Key::Apostrophe: case Key::Z: case Key::X: case Key::C: case Key::V:
      return true;
    default:
      return false;
  }
}
// The typing keyboard plays this chord: its command does not run.
bool typingTakes(const char* id, const Chord& c) {
  return (gTyping && playsOnTypingKeyboard(c) && (!gTypingARow || aRowKey(c.key)) && std::strcmp(id, "options.typingKeyboard") != 0) ||
         editorTakes(c);
}

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
  const uint8_t rest = static_cast<uint8_t>((io.KeyShift ? kShift : 0) | (io.KeyAlt ? kAlt : 0));
  if (keymap().separateCmd()) {
    // K-79 (tracks shortcut set, macOS): ImGui reports Command as KeyCtrl and Control as KeySuper there.
    return static_cast<uint8_t>(rest | (io.KeyCtrl ? kCmd : 0) | (io.KeySuper ? kCtrl : 0));
  }
  return static_cast<uint8_t>(((io.KeyCtrl || io.KeySuper) ? kCtrl : 0) | rest);
}

// Mouse gestures keep Cmd and Ctrl as one modifier in every shortcut set.
uint8_t mouseMods() {
  const ImGuiIO& io = ImGui::GetIO();
  return static_cast<uint8_t>(((io.KeyCtrl || io.KeySuper) ? kCtrl : 0) | (io.KeyShift ? kShift : 0) | (io.KeyAlt ? kAlt : 0));
}

struct LabelCache {
  uint32_t version = 0, caps = 0;
  std::unordered_map<std::string, std::string> text;
  std::unordered_map<std::string, std::string> mod;  // modLabel()
};
LabelCache& labelCache() {
  static LabelCache c;
  if (c.version != keymap().version() || c.caps != keycaps::version()) {  // GitHub #79: and when the keys' names change
    c.text.clear();
    c.mod.clear();
    c.version = keymap().version();
    c.caps = keycaps::version();
  }
  return c;
}

// GitHub #79: fixed chord texts as the keys say (a pointer that stays valid while the names do).
const char* cachedText(const char* text, const std::string& want) {
  if (want == text) return text;
  static std::unordered_set<std::string> cache;  // (by the text shown: elements never move)
  static uint32_t caps = 0;
  if (caps != keycaps::version()) cache.clear(), caps = keycaps::version();
  return cache.insert(want).first->c_str();
}

bool foldCmd(const std::string& s) {
#ifdef __APPLE__
  return keymap().profile() == Profile::Live && !keymap().separateCmd() && s.compare(0, 5, "Ctrl+") == 0;
#else
  (void)s;
  return false;
#endif
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

void setTypingKeyboard(bool on, bool aRow) { gTyping = on, gTypingARow = aRow; }
bool typingKeyPlays(Key k) { return !gTypingARow || aRowKey(k); }
bool typingKeyboardTakes(ImGuiKey k) {
  const Key key = fromImGui(k);
  return gTyping && key != Key::None && playsOnTypingKeyboard(Chord{key, 0}) && typingKeyPlays(key);
}
void setEditorKeys(bool on) { gEditorKeys = on; }
bool gSampleEditorKeys = false;  // K-106 cp18
void setSampleEditorKeys(bool on) { gSampleEditorKeys = on; }

bool editorTakes(const Chord& c) {
  if (gSampleEditorKeys) {
    const bool ctrl = (c.mods & (kCtrl | kCmd)) != 0, alt = (c.mods & kAlt) != 0;
    if (ctrl && alt) return false;
    if (ctrl) return c.key == Key::A || c.key == Key::X || c.key == Key::C || c.key == Key::V || c.key == Key::W;
    if (alt)
      return c.key == Key::A || c.key == Key::X || c.key == Key::C || c.key == Key::V || c.key == Key::R || c.key == Key::S || c.key == Key::Z ||
             c.key == Key::W || c.key == Key::Q || c.key == Key::Up || c.key == Key::Down;
    return c.key != Key::Space && !(c.key >= Key::F1 && c.key <= Key::F12);
  }
  if (!gEditorKeys) return false;
  const bool ctrl = (c.mods & (kCtrl | kCmd)) != 0, alt = (c.mods & kAlt) != 0;
  if (ctrl && alt) return false;
  // The Tracker key set's pattern keys (trackerview::trackerTakes): their global commands (Find
  // empty on Shift/Ctrl+F4, the windows on F9 to F12, ...) do not run while the editor has the keys.
  if (keymap().profile() == Profile::Tracker) {
    const bool shift = (c.mods & kShift) != 0, mod = shift || ctrl || alt;
    if (c.key >= Key::F9 && c.key <= Key::F12 && !alt) return true;  // (Alt+F12 still closes the plugin windows)
    if ((c.key == Key::F1 || c.key == Key::F2 || c.key == Key::F7 || c.key == Key::F8) && mod) return true;
    if ((c.key == Key::F3 || c.key == Key::F4 || c.key == Key::F5) && mod) return true;
    if (c.key == Key::Grave && !ctrl && !alt) return true;
    if (alt && (c.key == Key::Q || c.key == Key::W || c.key == Key::E || c.key == Key::R || c.key == Key::T || c.key == Key::Y || c.key == Key::U ||
                c.key == Key::I || c.key == Key::A || c.key == Key::S || c.key == Key::D || c.key == Key::F || c.key == Key::G || c.key == Key::H ||
                c.key == Key::J || c.key == Key::K || c.key == Key::C || c.key == Key::Delete || c.key == Key::Up || c.key == Key::Down || c.key == Key::Left ||
                c.key == Key::Right))
      return true;
    if (ctrl && (c.key == Key::Left || c.key == Key::Right || c.key == Key::Enter || (c.key == Key::Delete && !shift))) return true;  // (Ctrl+Shift+Del: delete pattern)
  }
  if (ctrl)  // copy, cut, paste, transpose, edit step
    return c.key == Key::C || c.key == Key::X || c.key == Key::V || c.key == Key::Up || c.key == Key::Down || c.key == Key::Minus ||
           c.key == Key::Equal || (c.key >= Key::D0 && c.key <= Key::D9);
  if (alt) return c.key == Key::F3 || c.key == Key::F4 || c.key == Key::F5;  // block cut, copy, paste
  // Plain and Shift: every key but Space and the function keys (play, the octave, the windows).
  return c.key != Key::Space && !(c.key >= Key::F1 && c.key <= Key::F12);
}
void setFocusContext(Context c) { gFocus = c; }
Context focusContext() { return gFocus; }

bool pressed(const char* id) {
  if (injectedNow(id)) return true;
  if (capturing() || ImGui::GetIO().WantTextInput) return false;
  const Command* cmd = findCommand(id);
  if (!cmd) return false;
  const uint8_t mods = modsNow();
  const Keymap& km = keymap();
  for (const Chord& c : km.bindings(id)) {
    if (c.mods != mods || typingTakes(id, c) || !ImGui::IsKeyPressed(toImGui(c.key), cmd->repeat)) continue;
    if (cmd->context == Context::Global && gFocus != Context::Global && !km.lookup(gFocus, c).empty()) continue;  // the editor wins
    return true;
  }
  return false;
}

bool pressedOverText(const char* id) {
  if (pressed(id)) return true;
  if (capturing() || !ImGui::GetIO().WantTextInput) return false;
  const Command* cmd = findCommand(id);
  if (!cmd) return false;
  const uint8_t mods = modsNow();
  for (const Chord& c : keymap().bindings(id))
    if (c.mods == mods && (c.mods & (kCtrl | kCmd)) && ImGui::IsKeyPressed(toImGui(c.key), cmd->repeat)) return true;
  return false;
}

bool pressedPastEditor(const char* id) {
  if (injectedNow(id)) return true;
  if (capturing() || ImGui::GetIO().WantTextInput) return false;
  const Command* cmd = findCommand(id);
  if (!cmd) return false;
  const uint8_t mods = modsNow();
  const Keymap& km = keymap();
  for (const Chord& c : km.bindings(id)) {
    if (c.mods != mods || typingTakes(id, c) || !ImGui::IsKeyPressed(toImGui(c.key), cmd->repeat)) continue;
    bool editorTakes = false;
    if (cmd->context == Context::Global && gFocus != Context::Global)
      for (const Command* e : km.lookup(gFocus, c)) {
        const std::string eid = e->id;
        if (eid != "playlist.cancelDrag" && eid != "piano.cancelDrag" && eid != "auto.cancel") editorTakes = true;
      }
    if (!editorTakes) return true;
  }
  return false;
}

void escapeFrameBegin() {
  const ImGuiIO& io = ImGui::GetIO();
  gEscBusyAtStart = io.WantTextInput || capturing() || ImGui::IsAnyMouseDown() || ImGui::GetActiveID() != 0 ||
                    ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
                    ImGui::GetDragDropPayload() != nullptr;
  // K-297: Esc closes the open menu or popup with its submenus, as a click outside it does. Keyboard
  // navigation is off, so ImGui never did: a menu stayed open, and the next click on another menu
  // title only closed it (K-301 R02-07). Not a modal dialog (it answers Esc itself) nor what it
  // opened over, not while a field or another item holds the key. The frame counts as busy, so no
  // command also takes it.
  ImGuiContext& g = *GImGui;
  if (g.OpenPopupStack.Size > 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !io.WantTextInput && !capturing() &&
      ImGui::GetActiveID() == 0) {
    int level = g.OpenPopupStack.Size;
    while (level > 0 && g.OpenPopupStack[level - 1].Window && !(g.OpenPopupStack[level - 1].Window->Flags & ImGuiWindowFlags_Modal)) --level;
    if (level < g.OpenPopupStack.Size) ImGui::ClosePopupToLevel(level, true);
  }
}

bool escapePressed(bool repeat) {
  if (!ImGui::IsKeyPressed(ImGuiKey_Escape, repeat)) return false;
  claimEscape();
  return true;
}

void claimEscape() { gEscClaimFrame = ImGui::GetFrameCount(); }

bool escapeBusy() { return gEscBusyAtStart || gEscClaimFrame == ImGui::GetFrameCount(); }

bool defaultPressed() {
  const ImGuiContext& ctx = *GImGui;
  const ImGuiIO& io = ctx.IO;
  if (capturing() || io.KeyCtrl || io.KeyShift || io.KeyAlt || io.KeySuper) return false;
  if (!ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) return false;
  if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return false;
  if (ctx.ActiveId && ctx.InputTextState.ID == ctx.ActiveId && (ctx.InputTextState.Flags & ImGuiInputTextFlags_Multiline)) return false;
  if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return false;  // (a combo's list or a menu over the dialog takes it)
  return true;
}

bool pressedHeld(const char* id) {
  if (injectedNow(id)) return true;
  if (capturing() || ImGui::GetIO().WantTextInput) return false;
  const Command* cmd = findCommand(id);
  if (!cmd) return false;
  for (const Chord& c : keymap().bindings(id))
    if (!typingTakes(id, Chord{c.key, 0}) && ImGui::IsKeyPressed(toImGui(c.key), cmd->repeat)) return true;
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
  return want != 0 && mouseMods() == want;
}

bool modifierExact(const char* id) { return mouseMods() == mouseModifier(id); }

bool modifierIncludes(const char* id) {
  const uint8_t want = mouseModifier(id);
  return want != 0 && (mouseMods() & want) == want;
}

std::string shown(const Chord& c) {
  std::string s = keycaps::format(c);
  if (foldCmd(s)) s.replace(0, 4, "Cmd");
  return s;
}

const char* shownText(const char* text) {
  if (!text) return text;
  std::string s = keycaps::formatText(text);
  if (foldCmd(s)) s.replace(0, 4, "Cmd");
  return cachedText(text, s);
}

std::string named(const Chord& c) { return keycaps::format(c); }

const char* namedText(const char* text) { return text ? cachedText(text, keycaps::formatText(text)) : text; }

const char* label(const char* id) {
  LabelCache& lc = labelCache();
  auto it = lc.text.find(id);
  if (it == lc.text.end()) {
    const auto& b = keymap().bindings(id);
    it = lc.text.emplace(id, b.empty() ? std::string() : shown(b.front())).first;
  }
  return it->second.empty() ? nullptr : it->second.c_str();
}

const char* modLabel(const char* id) {
  LabelCache& lc = labelCache();
  auto it = lc.mod.find(id);
  if (it == lc.mod.end()) {
    const auto& b = keymap().bindings(id);
    std::string t;
    for (const Chord& c : b)
      if (t.empty() && c.mods != 0) t = keycaps::format(c);
    if (t.empty() && !b.empty()) t = keycaps::format(b.front());
    it = lc.mod.emplace(id, t).first;
  }
  return it->second.empty() ? nullptr : it->second.c_str();
}

const char* commandForLabel(const char* text) {
  if (!text) return nullptr;
  const LabelCache& lc = labelCache();
  for (const auto& [id, t] : lc.text)
    if (t.c_str() == text) return findCommand(id) ? findCommand(id)->id : nullptr;
  for (const auto& [id, t] : lc.mod)
    if (t.c_str() == text) return findCommand(id) ? findCommand(id)->id : nullptr;
  return nullptr;
}

void inject(const char* id, int frame) {
  gInjected = id ? id : "";
  gInjectFrame = frame;
}

std::string labels(const char* id) {
  std::string s;
  for (const Chord& c : keymap().bindings(id)) s += (s.empty() ? "" : " / ") + shown(c);
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
