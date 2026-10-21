// Gridline tests — J-66: keys typed into a hosted plugin's interface that it
// does not use reach Gridline's global shortcuts (Space plays / stops); a
// text field of the interface and an interface that uses the key keep it.
// macOS: real AppKit key events sent to a plugin-like view inside Gridline's
// plugin container, in a window that is never shown.
#include <string>
#include <vector>
#include "Check.h"
#include "core/Keymap.h"
#include "engine/AudioEngine.h"
#include "host/KeyForward.h"
#include "host/sandbox/Sandbox.h"
#include "host/ui/NativeView.h"

using namespace gl;
namespace native = gl::host::native;

namespace {

constexpr uint16_t kSpace = 0x31, kS = 0x01, kZ = 0x06;

bool isCommand(const keys::Chord& c, const char* id) {
  for (const keys::Command* cmd : keys::keymap().lookup(keys::Context::Global, c))
    if (std::string(cmd->id) == id) return true;
  return false;
}

// What Gridline's main window does with a forwarded key (handleGlobalShortcuts):
// the transport toggles on "transport.play".
[[maybe_unused]] void applyToTransport(const host::KeyEvent& k, AudioEngine& eng) {
  if (!k.down || k.repeat) return;
  if (isCommand(host::chordOf(k), "transport.play")) eng.playing() ? eng.stop() : eng.play();
}

[[maybe_unused]] std::vector<host::KeyEvent> drain(void* container) {
  std::vector<host::KeyEvent> v;
  host::KeyEvent k;
  while (native::popKey(container, k)) v.push_back(k);
  return v;
}

void testMapping() {
  expect(host::hidUsageOfMacKey(kSpace) == 44, "mac Space -> HID usage 44 (SDL_SCANCODE_SPACE)");
  expect(host::hidUsageOfMacKey(kS) == 22 && host::hidUsageOfMacKey(kZ) == 29, "mac S / Z -> HID usages");
  expect(host::hidUsageOfMacKey(0x7A) == 58 && host::hidUsageOfMacKey(0x6F) == 69, "mac F1 / F12 -> HID usages");
  expect(host::hidUsageOfMacKey(0xFF) == 0, "unknown mac key -> 0");
  host::KeyEvent space;
  space.code = kSpace;
  expect(host::chordOf(space).key == keys::Key::Space && host::chordOf(space).mods == 0, "Space chord");
  expect(isCommand(host::chordOf(space), "transport.play"), "forwarded Space is the play / stop command");
  host::KeyEvent save;
  save.code = kS;
#if defined(__APPLE__)
  save.mods = host::kKeyCmd;  // Cmd is Gridline's Ctrl on macOS
#else
  save.mods = host::kKeyCtrl;
#endif
  expect(isCommand(host::chordOf(save), "file.save"), "forwarded Cmd/Ctrl+S is save");
  host::VstKey vk;
  expect(host::vstKeyOf(44, 0, vk) && vk.character == u' ' && vk.virtualKey == 7, "VST key: Space");
  expect(host::vstKeyOf(58, host::kKeyShift, vk) && vk.virtualKey == 40 && vk.modifiers == 1, "VST key: Shift+F1");
  expect(host::vstKeyOf(4, host::kKeyShift, vk) && vk.character == u'A', "VST key: Shift+A");
}

#if defined(__APPLE__)
void testNative() {
  void* content = host::offscreenEditorParent(400, 300);
  expect(content != nullptr, "offscreen window");
  if (!content) return;
  native::Window w = native::windowOfView(content);
  void* c = native::createContainer(w, host::layout::Rect{0, 0, 400, 300});
  expect(c != nullptr, "plugin container");
  if (!c) return;
  AudioEngine eng(44100);

  // 1. The interface has the keyboard and passes Space on: the transport toggles.
  expect(native::devTestKeyView(c, 0) != nullptr, "interface view takes the keyboard");
  expect(native::keyFocus(c) == native::KeyFocus::Plugin, "focus: plugin");
  native::devSendKey(c, kSpace, true, 0, " ");
  native::devSendKey(c, kSpace, false, 0, " ");
  std::vector<host::KeyEvent> got = drain(c);
  expect(got.size() == 2 && got[0].down && !got[1].down && got[0].code == kSpace, "Space down / up forwarded");
  const bool before = eng.playing();
  for (const auto& k : got) applyToTransport(k, eng);
  expect(eng.playing() != before, "transport toggled by Space typed into the interface");
  for (const auto& k : got) applyToTransport(k, eng);
  // Modifiers travel with the key.
  native::devSendKey(c, kS, true, host::kKeyCmd, "s");
  native::devSendKey(c, kS, false, host::kKeyCmd, "s");
  got = drain(c);
  expect(got.size() == 2 && got[0].mods == host::kKeyCmd && isCommand(host::chordOf(got[0]), "file.save"), "Cmd+S forwarded as save");

  // 2. A text field of the interface keeps every key.
  expect(native::devTestKeyView(c, 2) != nullptr, "text field");
  expect(native::keyFocus(c) == native::KeyFocus::PluginText, "focus: plugin text field");
  const bool before2 = eng.playing();
  native::devSendKey(c, kSpace, true, 0, " ");
  native::devSendKey(c, kSpace, false, 0, " ");
  got = drain(c);
  for (const auto& k : got) applyToTransport(k, eng);
  expect(got.empty(), "nothing forwarded while typing in the interface's text field");
  expect(eng.playing() == before2, "transport unchanged by Space typed into a text field");

  // 3. An interface that uses the key (its own keyboard notes) keeps it.
  native::devTestKeyView(c, 1);
  native::devSendKey(c, kZ, true, 0, "z");
  native::devSendKey(c, kZ, false, 0, "z");
  expect(drain(c).empty(), "a key the interface uses is not forwarded");

  // 4. A forwarded key whose key-up the interface swallows is released when
  //    the window loses the keyboard (no stuck key in Gridline).
  native::devTestKeyView(c, 0);
  native::devSendKey(c, kZ, true, 0, "z");
  native::devTestKeyView(c, 1);
  native::devSendKey(c, kZ, false, 0, "z");
  got = drain(c);
  expect(got.size() == 1 && got[0].down, "key-down forwarded, key-up swallowed");
  native::saveKeyFocus(c);
  got = drain(c);
  expect(got.size() == 1 && !got[0].down && got[0].code == kZ, "held key released when the window loses the keyboard");

  // 5. Focus comes back to the interface's view after the window regains the keyboard.
  void* v = native::devTestKeyView(c, 0);
  native::saveKeyFocus(c);
  native::devTestKeyView(c, 1);  // something else took it meanwhile...
  native::restoreKeyFocus(c);    // ...but inside the interface: left alone
  expect(native::keyFocus(c) == native::KeyFocus::Plugin, "focus stays in the interface");
  (void)v;

  native::destroyContainer(w, c);
  host::KeyEvent k;
  expect(!native::popKey(c, k), "no keys after the container is gone");
  host::closeOffscreenEditorParent();
}
#endif

}  // namespace

int main() {
  testMapping();
#if defined(__APPLE__)
  testNative();
#endif
  return finish();
}
