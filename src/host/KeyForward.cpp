// Gridline — forwarded plugin keys (see KeyForward.h).
#include "KeyForward.h"

namespace gl::host {

int hidUsageOfMacKey(uint16_t code) {
  // macOS virtual key codes (ANSI positions) -> USB HID usages.
  switch (code) {
    case 0x00: return 4;    // A
    case 0x0B: return 5;    // B
    case 0x08: return 6;    // C
    case 0x02: return 7;    // D
    case 0x0E: return 8;    // E
    case 0x03: return 9;    // F
    case 0x05: return 10;   // G
    case 0x04: return 11;   // H
    case 0x22: return 12;   // I
    case 0x26: return 13;   // J
    case 0x28: return 14;   // K
    case 0x25: return 15;   // L
    case 0x2E: return 16;   // M
    case 0x2D: return 17;   // N
    case 0x1F: return 18;   // O
    case 0x23: return 19;   // P
    case 0x0C: return 20;   // Q
    case 0x0F: return 21;   // R
    case 0x01: return 22;   // S
    case 0x11: return 23;   // T
    case 0x20: return 24;   // U
    case 0x09: return 25;   // V
    case 0x0D: return 26;   // W
    case 0x07: return 27;   // X
    case 0x10: return 28;   // Y
    case 0x06: return 29;   // Z
    case 0x12: return 30;   // 1
    case 0x13: return 31;   // 2
    case 0x14: return 32;   // 3
    case 0x15: return 33;   // 4
    case 0x17: return 34;   // 5
    case 0x16: return 35;   // 6
    case 0x1A: return 36;   // 7
    case 0x1C: return 37;   // 8
    case 0x19: return 38;   // 9
    case 0x1D: return 39;   // 0
    case 0x24: return 40;   // Return
    case 0x35: return 41;   // Escape
    case 0x33: return 42;   // Backspace (Delete)
    case 0x30: return 43;   // Tab
    case 0x31: return 44;   // Space
    case 0x1B: return 45;   // -
    case 0x18: return 46;   // =
    case 0x21: return 47;   // [
    case 0x1E: return 48;   // ]
    case 0x2A: return 49;   // backslash
    case 0x29: return 51;   // ;
    case 0x27: return 52;   // '
    case 0x32: return 53;   // `
    case 0x2B: return 54;   // ,
    case 0x2F: return 55;   // .
    case 0x2C: return 56;   // /
    case 0x7A: return 58;   // F1
    case 0x78: return 59;   // F2
    case 0x63: return 60;   // F3
    case 0x76: return 61;   // F4
    case 0x60: return 62;   // F5
    case 0x61: return 63;   // F6
    case 0x62: return 64;   // F7
    case 0x64: return 65;   // F8
    case 0x65: return 66;   // F9
    case 0x6D: return 67;   // F10
    case 0x67: return 68;   // F11
    case 0x6F: return 69;   // F12
    case 0x72: return 73;   // Help = Insert on PC keyboards
    case 0x73: return 74;   // Home
    case 0x74: return 75;   // Page up
    case 0x75: return 76;   // Forward delete
    case 0x77: return 77;   // End
    case 0x79: return 78;   // Page down
    case 0x7C: return 79;   // Right
    case 0x7B: return 80;   // Left
    case 0x7D: return 81;   // Down
    case 0x7E: return 82;   // Up
    case 0x47: return 83;   // Keypad clear (num lock)
    case 0x4B: return 84;   // Keypad /
    case 0x43: return 85;   // Keypad *
    case 0x4E: return 86;   // Keypad -
    case 0x45: return 87;   // Keypad +
    case 0x4C: return 88;   // Keypad enter
    case 0x53: return 89;   // Keypad 1
    case 0x54: return 90;   // Keypad 2
    case 0x55: return 91;   // Keypad 3
    case 0x56: return 92;   // Keypad 4
    case 0x57: return 93;   // Keypad 5
    case 0x58: return 94;   // Keypad 6
    case 0x59: return 95;   // Keypad 7
    case 0x5B: return 96;   // Keypad 8
    case 0x5C: return 97;   // Keypad 9
    case 0x52: return 98;   // Keypad 0
    case 0x41: return 99;   // Keypad .
    case 0x0A: return 100;  // ISO section (non-US backslash)
    case 0x51: return 103;  // Keypad =
    case 0x69: return 104;  // F13
    case 0x6B: return 105;  // F14
    case 0x71: return 106;  // F15
    case 0x6A: return 107;  // F16
    default: return 0;
  }
}

keys::Key keyOfHidUsage(int u) {
  using K = keys::Key;
  if (u >= 4 && u <= 29) return static_cast<K>(static_cast<int>(K::A) + (u - 4));
  if (u >= 30 && u <= 38) return static_cast<K>(static_cast<int>(K::D1) + (u - 30));
  if (u == 39) return K::D0;
  if (u >= 58 && u <= 69) return static_cast<K>(static_cast<int>(K::F1) + (u - 58));
  if (u >= 89 && u <= 97) return static_cast<K>(static_cast<int>(K::Num1) + (u - 89));
  switch (u) {
    case 40: return K::Enter;
    case 41: return K::Escape;
    case 42: return K::Backspace;
    case 43: return K::Tab;
    case 44: return K::Space;
    case 45: return K::Minus;
    case 46: return K::Equal;
    case 47: return K::LeftBracket;
    case 48: return K::RightBracket;
    case 49: return K::Backslash;
    case 51: return K::Semicolon;
    case 52: return K::Apostrophe;
    case 53: return K::Grave;
    case 54: return K::Comma;
    case 55: return K::Period;
    case 56: return K::Slash;
    case 73: return K::Insert;
    case 74: return K::Home;
    case 75: return K::PageUp;
    case 76: return K::Delete;
    case 77: return K::End;
    case 78: return K::PageDown;
    case 79: return K::Right;
    case 80: return K::Left;
    case 81: return K::Down;
    case 82: return K::Up;
    case 84: return K::NumDivide;
    case 85: return K::NumMultiply;
    case 86: return K::NumSubtract;
    case 87: return K::NumAdd;
    case 88: return K::NumEnter;
    case 98: return K::Num0;
    default: return K::None;
  }
}

bool vstKeyOf(int u, uint32_t mods, VstKey& k) {
  k = VstKey{};
  const bool shift = (mods & kKeyShift) != 0;
  if (u >= 4 && u <= 29) k.character = static_cast<char16_t>((shift ? 'A' : 'a') + (u - 4));
  else if (u >= 30 && u <= 38) k.character = static_cast<char16_t>('1' + (u - 30));
  else if (u == 39) k.character = u'0';
  else if (u >= 58 && u <= 69) k.virtualKey = static_cast<int16_t>(40 + (u - 58));  // F1..F12
  else if (u >= 89 && u <= 97) k.virtualKey = static_cast<int16_t>(25 + (u - 89));  // keypad 1..9
  else {
    static const struct {
      int usage;
      char16_t ch;
      int16_t vk;
    } table[] = {
        {40, 0, 4},   {41, 0, 6},  {42, 0, 1},  {43, 0, 2},  {44, u' ', 7}, {45, u'-', 0}, {46, u'=', 0}, {47, u'[', 0}, {48, u']', 0},
        {49, u'\\', 0}, {51, u';', 0}, {52, u'\'', 0}, {53, u'`', 0}, {54, u',', 0}, {55, u'.', 0}, {56, u'/', 0},
        {73, 0, 21},  {74, 0, 10}, {75, 0, 15}, {76, 0, 22}, {77, 0, 9},  {78, 0, 16},  {79, 0, 13},  {80, 0, 11},  {81, 0, 14},
        {82, 0, 12},  {84, 0, 39}, {85, 0, 34}, {86, 0, 37}, {87, 0, 35}, {88, 0, 19},  {98, 0, 24},  {99, 0, 38},
    };
    for (const auto& t : table)
      if (t.usage == u) {
        k.character = t.ch;
        k.virtualKey = t.vk;
      }
  }
  if (!k.character && !k.virtualKey) return false;
  if (mods & kKeyShift) k.modifiers |= 1;
  if (mods & kKeyAlt) k.modifiers |= 2;
#if defined(__APPLE__)
  if (mods & kKeyCmd) k.modifiers |= 4;
  if (mods & kKeyCtrl) k.modifiers |= 8;
#else
  if (mods & kKeyCtrl) k.modifiers |= 4;
  if (mods & kKeyCmd) k.modifiers |= 8;
#endif
  return true;
}

keys::Chord chordOf(const KeyEvent& e) {
  keys::Chord c;
  c.key = keyOfHidUsage(hidUsageOfMacKey(e.code));
#if defined(__APPLE__)
  if (e.mods & kKeyCmd) c.mods |= keys::kCtrl;
#else
  if (e.mods & kKeyCtrl) c.mods |= keys::kCtrl;
#endif
  if (e.mods & kKeyShift) c.mods |= keys::kShift;
  if (e.mods & kKeyAlt) c.mods |= keys::kAlt;
  return c;
}

uint16_t macKeyOfHidUsage(int usage) {
  if (usage <= 0) return 0xFFFF;
  for (uint16_t code = 0; code < 128; ++code)
    if (hidUsageOfMacKey(code) == usage) return code;
  return 0xFFFF;
}

int hidUsageOfWinVk(int vk) {
  if (vk >= 'A' && vk <= 'Z') return 4 + (vk - 'A');
  if (vk >= '1' && vk <= '9') return 30 + (vk - '1');
  if (vk == '0') return 39;
  if (vk >= 0x70 && vk <= 0x7B) return 58 + (vk - 0x70);  // VK_F1..VK_F12
  switch (vk) {
    case 0x0D: return 40;  // VK_RETURN
    case 0x1B: return 41;  // VK_ESCAPE
    case 0x08: return 42;  // VK_BACK
    case 0x09: return 43;  // VK_TAB
    case 0x20: return 44;  // VK_SPACE
    case 0xBD: return 45;  // VK_OEM_MINUS
    case 0xBB: return 46;  // VK_OEM_PLUS (the = key)
    case 0xDB: return 47;  // VK_OEM_4 ([)
    case 0xDD: return 48;  // VK_OEM_6 (])
    case 0xDC: return 49;  // VK_OEM_5 (backslash)
    case 0xBA: return 51;  // VK_OEM_1 (;)
    case 0xDE: return 52;  // VK_OEM_7 (')
    case 0xC0: return 53;  // VK_OEM_3 (`)
    case 0xBC: return 54;  // VK_OEM_COMMA
    case 0xBE: return 55;  // VK_OEM_PERIOD
    case 0xBF: return 56;  // VK_OEM_2 (/)
    case 0x2D: return 73;  // VK_INSERT
    case 0x24: return 74;  // VK_HOME
    case 0x21: return 75;  // VK_PRIOR
    case 0x2E: return 76;  // VK_DELETE
    case 0x23: return 77;  // VK_END
    case 0x22: return 78;  // VK_NEXT
    case 0x27: return 79;  // VK_RIGHT
    case 0x25: return 80;  // VK_LEFT
    case 0x28: return 81;  // VK_DOWN
    case 0x26: return 82;  // VK_UP
    default: return 0;
  }
}

}  // namespace gl::host
