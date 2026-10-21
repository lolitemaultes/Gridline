// Gridline — keys typed into a hosted plugin's own interface that the plugin
// did not use (J-66). Like FL Studio, Gridline's global shortcuts (Space
// play / stop, save, undo, record, window keys …) keep working while a
// plugin's interface has the keyboard: a key the interface consumes (typing
// in one of its text fields, its own computer-keyboard notes) stays with the
// plugin; every other key is handed to Gridline.
//
// macOS: a plugin view that does not handle a key passes it up the responder
// chain to the container view Gridline attached it to; the container queues it
// (host/ui/NativeViewMac.mm). Key codes are macOS virtual key codes; they are
// turned into USB HID usages (= SDL scancodes) here, without AppKit or SDL.
#pragma once
#include <cstdint>
#include "../core/Keymap.h"

namespace gl::host {

enum KeyMods : uint32_t { kKeyShift = 1, kKeyCtrl = 2, kKeyAlt = 4, kKeyCmd = 8 };

struct KeyEvent {
  bool down = true;
  bool repeat = false;
  uint16_t code = 0;  // macOS virtual key code (kVK_*)
  uint32_t mods = 0;  // KeyMods
};

// USB HID keyboard usage (the value of SDL_Scancode) of a macOS virtual key
// code; 0 when unknown.
int hidUsageOfMacKey(uint16_t code);
// Gridline's key of a HID usage (Key::None when Gridline has no name for it).
keys::Key keyOfHidUsage(int usage);
// The chord a forwarded key makes in Gridline's keymap (Cmd is Gridline's
// Ctrl on macOS, as in the rest of the interface).
keys::Chord chordOf(const KeyEvent& e);

// A key in the terms of the VST key APIs (VST3 IPlugView::onKeyDown, VST2
// effEditKeyDown share them): the character for printable keys, else a
// virtual key code (VST3 VirtualKeyCodes = VST2 VstVirtualKey); modifiers as
// kShiftKey 1, kAlternateKey 2, kCommandKey 4 (Ctrl off macOS), kControlKey 8.
struct VstKey {
  char16_t character = 0;
  int16_t virtualKey = 0;
  int16_t modifiers = 0;
};
// False when the key has neither (the plugin is not asked).
bool vstKeyOf(int usage, uint32_t mods, VstKey& out);

}  // namespace gl::host
