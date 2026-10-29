// Gridline — "platter" pack: the deck's model as text (grid, cues, loop,
// controller set-up, takes), the take point codec, the deck controls, the
// controller profiles and the MIDI decoder. Pure logic: the decoder runs on the
// audio thread (no allocation), the rest on the UI thread.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "platter.h"

namespace gl::packs::platter {

namespace fs = std::filesystem;

// ================================================================== small values
double loopBeats(int index) {
  static const double k[kLoopSizes] = {0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32};
  return k[std::clamp(index, 0, kLoopSizes - 1)];
}
const char* loopLabel(int index) {
  static const char* const k[kLoopSizes] = {"1/8", "1/4", "1/2", "1", "2", "4", "8", "16", "32"};
  return k[std::clamp(index, 0, kLoopSizes - 1)];
}
double pitchRangeOf(int range) {
  static const double k[RangeCount] = {0.04, 0.08, 0.16, 0.5};
  return k[std::clamp(range, 0, RangeCount - 1)];
}

// EQ knob law: the upper half is 0..+6 dB, the lower half falls to -26 dB and the
// last 2 % is the kill (the knob's bottom).
double eqDbOf(double n) {
  n = std::clamp(n, 0.0, 1.0);
  if (n >= 0.5) return (n - 0.5) * 2.0 * 6.0;
  if (n <= 0.02) return kEqKillDb;
  const double u = (0.5 - n) / 0.48;  // 0 at the centre .. 1 near the bottom
  return std::max(kEqKillDb, -24.0 * u * u * (3.0 - 2.0 * u) - 2.0 * u);
}
double eqNormOf(double db) {
  if (db >= 0) return 0.5 + std::min(db, 6.0) / 12.0;
  if (db <= kEqKillDb + 1e-9) return 0.0;
  double lo = 0.02, hi = 0.5;  // eqDbOf is monotonic: bisection
  for (int i = 0; i < 50; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (eqDbOf(mid) < db) lo = mid;
    else hi = mid;
  }
  return 0.5 * (lo + hi);
}

// ================================================================== parameters / controls
int deckOfParam(int p, int& d) {
  d = -1;
  if (p >= PDeckB && p < PDeckB + DCount) {
    d = p - PDeckB;
    return 1;
  }
  if (p >= 0 && p < PXfader) {
    d = p;
    return 0;
  }
  if (p == PXfSide) {
    d = DXfSide;
    return 0;
  }
  if (p == PLoopSize) {
    d = DLoopSize;
    return 0;
  }
  if (p == PJogA || p == PJogB) {
    d = DJog;
    return p == PJogA ? 0 : 1;
  }
  if (perfParam(p)) {
    d = DPos + (p - PPosA) / 2;
    return (p - PPosA) % 2;
  }
  return -1;
}

// ------------------------------------------------------------------ command keys (J-159)
int cmdKeyPitch(int deck, int index) {
  if (index >= KeyClear1) return (deck == 0 ? kClearKeyFirstA : kClearKeyFirstB) + (index - KeyClear1);
  return (deck == 0 ? kCmdKeyFirstA : kCmdKeyFirstB) + index;
}

int cmdKeyOf(int pitch, int& index) {
  for (int d = 0; d < kDecks; ++d) {
    const int k = pitch - (d == 0 ? kCmdKeyFirstA : kCmdKeyFirstB);
    if (k >= 0 && k < KeyClear1) {
      index = k;
      return d;
    }
    const int c = pitch - (d == 0 ? kClearKeyFirstA : kClearKeyFirstB);
    if (c >= 0 && c < kHotCues) {
      index = KeyClear1 + c;
      return d;
    }
  }
  index = -1;
  return -1;
}

int cmdKeyControl(int index) {
  if (index >= KeyHot1 && index < KeyHot1 + kHotCues) return CtlHot1 + (index - KeyHot1);
  if (index >= KeyClear1 && index < KeyClear1 + kHotCues) return CtlHotClear1 + (index - KeyClear1);
  switch (index) {
    case KeyPlay: return CtlPlay;
    case KeyCue: return CtlCue;
    case KeyLoopIn: return CtlLoopIn;
    case KeyLoopOut: return CtlLoopOut;
    case KeyLoopAuto: return CtlLoopAuto;
    case KeyLoopHalve: return CtlLoopHalve;
    case KeyLoopDouble: return CtlLoopDouble;
    case KeyJumpBack: return CtlJumpBack;
    case KeyJumpFwd: return CtlJumpFwd;
    case KeyJumpStart: return CtlJumpStart;
    case KeyCensor: return CtlCensor;
    case KeyBendDown: return CtlBendDown;
    case KeyBendUp: return CtlBendUp;
    case KeyTouch: return CtlTouch;
    default: return CtlNone;
  }
}

int controlCmdKey(int control) {
  if (control == CtlNone) return -1;
  for (int i = 0; i < kCmdKeys; ++i)
    if (cmdKeyControl(i) == control) return i;
  return -1;
}

std::string keyName(int pitch) {
  char b[48];
  int idx = -1;
  const int d = cmdKeyOf(pitch, idx);
  const char deck = d == 1 ? 'B' : 'A';
  if (d >= 0) {
    if (idx >= KeyHot1 && idx < KeyHot1 + kHotCues) {
      std::snprintf(b, sizeof b, "%c Hot cue %d", deck, idx - KeyHot1 + 1);
      return b;
    }
    if (idx >= KeyClear1) {
      std::snprintf(b, sizeof b, "%c Clear hot cue %d", deck, idx - KeyClear1 + 1);
      return b;
    }
    static const char* const kNames[KeyClear1] = {"Play", "Cue", "", "", "", "", "", "", "", "", "Loop in", "Loop out", "Auto loop", "Loop / 2",
                                                  "Loop x 2", "Jump back", "Jump forward", "Back to start", "Reverse", "Bend -", "Bend +",
                                                  "Wheel touch", "Mouse hand"};
    std::snprintf(b, sizeof b, "%c %s", deck, kNames[idx]);
    return b;
  }
  if (pitch == kStartKey) return "Performance start";
  if (pitch >= kTakeKeyFirst && pitch < kTakeKeyFirst + kTakeKeys) {
    std::snprintf(b, sizeof b, "Take key %d", pitch - kTakeKeyFirst + 1);
    return b;
  }
  for (int k = 0; k < kDecks; ++k) {
    const int first = k == 0 ? kHotKeyFirst : kHotKeyFirstB;
    if (pitch >= first && pitch < first + kHotCues) {
      std::snprintf(b, sizeof b, "%c Hot key %d", k == 0 ? 'A' : 'B', pitch - first + 1);
      return b;
    }
  }
  return {};
}

const ControlInfo& controlInfo(int c) {
  static const ControlInfo k[CtlCount] = {
      {"none", "None", TypeButton, "", -1, false},
      {"play", "Play / pause", TypeButton, "Transport", -1, false},
      {"cue", "Cue", TypeButton, "Transport", -1, false},
      {"sync", "Sync", TypeButton, "Transport", DSync, false},
      {"keylock", "Key lock", TypeButton, "Modes", DKeyLock, false},
      {"shift", "Shift", TypeButton, "Transport", -1, false},
      {"touch", "Jog touch", TypeButton, "Jog", -1, false},
      {"jog", "Jog platter", TypeRelative, "Jog", -1, false},
      {"rim", "Jog side (nudge)", TypeRelative, "Jog", -1, false},
      {"search", "Jog search", TypeRelative, "Jog", -1, false},
      {"pitch", "Pitch fader", TypeAbsolute, "Mixer", DPitch, false},
      {"volume", "Channel fader", TypeAbsolute, "Mixer", DVolume, false},
      {"xfader", "Crossfader", TypeAbsolute, "Mixer", PXfader, true},
      {"eqhigh", "EQ high", TypeAbsolute, "Mixer", DEqHigh, false},
      {"eqmid", "EQ mid", TypeAbsolute, "Mixer", DEqMid, false},
      {"eqlow", "EQ low", TypeAbsolute, "Mixer", DEqLow, false},
      {"filter", "Filter", TypeAbsolute, "Mixer", DFilter, false},
      {"trim", "Trim", TypeAbsolute, "Mixer", DTrim, false},
      {"hot1", "Hot cue 1", TypeButton, "Pads", -1, false},
      {"hot2", "Hot cue 2", TypeButton, "Pads", -1, false},
      {"hot3", "Hot cue 3", TypeButton, "Pads", -1, false},
      {"hot4", "Hot cue 4", TypeButton, "Pads", -1, false},
      {"hot5", "Hot cue 5", TypeButton, "Pads", -1, false},
      {"hot6", "Hot cue 6", TypeButton, "Pads", -1, false},
      {"hot7", "Hot cue 7", TypeButton, "Pads", -1, false},
      {"hot8", "Hot cue 8", TypeButton, "Pads", -1, false},
      {"hotclear1", "Clear hot cue 1", TypeButton, "Pads", -1, false},
      {"hotclear2", "Clear hot cue 2", TypeButton, "Pads", -1, false},
      {"hotclear3", "Clear hot cue 3", TypeButton, "Pads", -1, false},
      {"hotclear4", "Clear hot cue 4", TypeButton, "Pads", -1, false},
      {"hotclear5", "Clear hot cue 5", TypeButton, "Pads", -1, false},
      {"hotclear6", "Clear hot cue 6", TypeButton, "Pads", -1, false},
      {"hotclear7", "Clear hot cue 7", TypeButton, "Pads", -1, false},
      {"hotclear8", "Clear hot cue 8", TypeButton, "Pads", -1, false},
      {"loopin", "Loop in", TypeButton, "Loop", -1, false},
      {"loopout", "Loop out", TypeButton, "Loop", -1, false},
      {"loopauto", "Auto loop", TypeButton, "Loop", -1, false},
      {"loophalve", "Loop / 2", TypeButton, "Loop", -1, false},
      {"loopdouble", "Loop x 2", TypeButton, "Loop", -1, false},
      {"jumpback", "Beat jump back", TypeButton, "Loop", -1, false},
      {"jumpfwd", "Beat jump forward", TypeButton, "Loop", -1, false},
      {"slip", "Slip", TypeButton, "Modes", DSlip, false},
      {"vinyl", "Vinyl mode", TypeButton, "Modes", DVinyl, false},
      {"quantize", "Quantize", TypeButton, "Modes", DQuantize, false},
      {"censor", "Reverse (held)", TypeButton, "Transport", -1, false},
      {"jumpstart", "Back to start", TypeButton, "Transport", -1, false},
      {"benddown", "Pitch bend -", TypeButton, "Transport", -1, false},
      {"bendup", "Pitch bend +", TypeButton, "Transport", -1, false},
      {"loaded", "Track loaded", TypeOutput, "Transport", -1, false},
      {"master", "Master level", TypeAbsolute, "Mixer", PMaster, true},
  };
  return k[c >= 0 && c < CtlCount ? c : 0];
}

int controlById(const std::string& id) {
  for (int c = 1; c < CtlCount; ++c)
    if (id == controlInfo(c).id) return c;
  return CtlNone;
}

int controlParam(int c, int deck) {
  const ControlInfo& ci = controlInfo(c);
  if (ci.param < 0) return -1;
  return ci.mixer ? ci.param : deckParam(deck & 1, ci.param);
}

bool Binding::operator==(const Binding& o) const {
  return control == o.control && kind == o.kind && channel == o.channel && num == o.num && lsb == o.lsb && enc == o.enc && invert == o.invert &&
         layer == o.layer && deck == o.deck;
}
bool Led::operator==(const Led& o) const {
  return control == o.control && status == o.status && num == o.num && on == o.on && off == o.off && deck == o.deck;
}
bool Mapping::operator==(const Mapping& o) const {
  return profile == o.profile && side == o.side && device == o.device && ticksPerRev == o.ticksPerRev && feedback == o.feedback &&
         bindings == o.bindings && leds == o.leds && init == o.init;
}

// ================================================================== profiles
const std::vector<Profile>& profiles() {
  static const std::vector<Profile> v = {
      {"ddj200", "Pioneer DDJ-200", "DDJ-200; DDJ200", true, "Two decks, eight pads per deck, 128 jog ticks per turn."},
      {"inpulse200", "Hercules Inpulse 200", "Inpulse 200; DJControl Inpulse 200", true,
       "Two decks, four pads per deck, two-band EQ, 248 jog ticks per turn."},
      {"mixtrackprofx", "Numark Mixtrack Pro FX", "Mixtrack Pro FX", true, "Two decks, eight pads per deck, 1024 jog ticks per turn."},
      {"generic", "Any MIDI controller", "", false, "Nothing is mapped: teach each control with MIDI learn."},
  };
  return v;
}

const Profile* findProfile(const std::string& id) {
  for (const auto& p : profiles())
    if (id == p.id) return &p;
  return nullptr;
}

JogFilter jogFilterOf(const std::string& profile) {
  JogFilter f;  // DDJ-200, Inpulse 200, learned: 1/8 and 1/256 (soft, no ringing)
  if (profile == "mixtrackprofx") f.alpha = 1.0, f.beta = 1.0 / 32;  // a fine wheel: a plain 32 ms smoothing of its tick rate
  return f;
}

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void bind(Mapping& m, int control, uint8_t kind, int channel, int num, int lsb = 0, uint8_t enc = EncAbs, bool invert = false) {
  Binding b;
  b.control = static_cast<uint8_t>(control);
  b.kind = kind;
  b.channel = static_cast<int8_t>(channel);
  b.num = static_cast<uint8_t>(num);
  b.lsb = static_cast<uint8_t>(lsb);
  b.enc = enc;
  b.invert = invert;
  m.bindings.push_back(b);
}
void led(Mapping& m, int control, int status, int num, int on = 0x7F, int off = 0x00) {
  Led l;
  l.control = static_cast<uint8_t>(control);
  l.status = static_cast<uint8_t>(status);
  l.num = static_cast<uint8_t>(num);
  l.on = static_cast<uint8_t>(on);
  l.off = static_cast<uint8_t>(off);
  m.leds.push_back(l);
}

// Pioneer DDJ-200 (Pioneer's public MIDI message list). Deck n on MIDI channel
// n + 1; mixer CCs on channel 7; pads on 0x97 + 2n (shifted 0x98 + 2n).
Mapping ddj200(int n) {
  Mapping m;
  m.ticksPerRev = 128;
  bind(m, CtlPlay, MkNote, n, 0x0B);
  bind(m, CtlCensor, MkNote, n, 0x47);
  bind(m, CtlCue, MkNote, n, 0x0C);
  bind(m, CtlJumpStart, MkNote, n, 0x48);
  bind(m, CtlSync, MkNote, n, 0x58);
  bind(m, CtlKeyLock, MkNote, n, 0x60);
  bind(m, CtlShift, MkNote, n, 0x3F);
  bind(m, CtlTouch, MkNote, n, 0x36);
  bind(m, CtlTouch, MkNote, n, 0x67);
  bind(m, CtlJogPlatter, MkCC, n, 0x22, 0, EncRelOffset);
  bind(m, CtlJogPlatter, MkCC, n, 0x23, 0, EncRelOffset);
  bind(m, CtlJogSearch, MkCC, n, 0x29, 0, EncRelOffset);
  bind(m, CtlJogRim, MkCC, n, 0x21, 0, EncRelOffset);
  bind(m, CtlPitch, MkCC14, n, 0x00, 0x20);
  bind(m, CtlEqHigh, MkCC14, n, 0x07, 0x27);
  bind(m, CtlEqMid, MkCC14, n, 0x0B, 0x2B);
  bind(m, CtlEqLow, MkCC14, n, 0x0F, 0x2F);
  bind(m, CtlVolume, MkCC14, n, 0x13, 0x33);
  bind(m, CtlFilter, MkCC14, 6, 0x17 + n, 0x37 + n);
  bind(m, CtlXfader, MkCC14, 6, 0x1F, 0x3F);
  bind(m, CtlSlip, MkNote, n, 0x54);
  bind(m, CtlVinyl, MkNote, n, 0x68);
  for (int k = 0; k < kHotCues; ++k) {
    bind(m, CtlHot1 + k, MkNote, 7 + 2 * n, k);
    bind(m, CtlHotClear1 + k, MkNote, 8 + 2 * n, k);
  }
  const int st = 0x90 | n;
  led(m, CtlPlay, st, 0x0B);
  led(m, CtlPlay, st, 0x47);
  led(m, CtlCue, st, 0x0C);
  led(m, CtlCue, st, 0x48);
  led(m, CtlSync, st, 0x58);
  led(m, CtlKeyLock, st, 0x60);
  led(m, CtlSlip, st, 0x54);
  led(m, CtlVinyl, st, 0x68);
  for (int k = 0; k < kHotCues; ++k) {
    led(m, CtlHot1 + k, 0x97 + 2 * n, k);
    led(m, CtlHot1 + k, 0x98 + 2 * n, k);
  }
  led(m, CtlLoaded, 0x9F, n);
  m.init.push_back({0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x02, 0x0A, 0x00, 0x03, 0x01, 0xF7});  // report the knob positions
  return m;
}

// Hercules DJControl Inpulse 200 (as the public Mixxx mapping documents it): deck
// A on MIDI channel 2, deck B on 3, shifted controls 3 channels higher; pads on
// channels 7 / 8.
Mapping inpulse200(int n) {
  Mapping m;
  m.ticksPerRev = 248;
  const int c = 1 + n, sc = 4 + n, pc = 6 + n;
  bind(m, CtlPlay, MkNote, c, 0x07);
  bind(m, CtlCensor, MkNote, sc, 0x07);
  bind(m, CtlCue, MkNote, c, 0x06);
  bind(m, CtlJumpStart, MkNote, sc, 0x06);
  bind(m, CtlSync, MkNote, c, 0x05);
  bind(m, CtlKeyLock, MkNote, sc, 0x05);
  bind(m, CtlVinyl, MkNote, c, 0x03);
  bind(m, CtlSlip, MkNote, c, 0x0C);
  bind(m, CtlLoopIn, MkNote, c, 0x09);
  bind(m, CtlLoopOut, MkNote, c, 0x0A);
  bind(m, CtlLoopHalve, MkNote, sc, 0x09);
  bind(m, CtlLoopDouble, MkNote, sc, 0x0A);
  bind(m, CtlTouch, MkNote, c, 0x08);
  bind(m, CtlTouch, MkNote, sc, 0x08);
  bind(m, CtlJogPlatter, MkCC, c, 0x0A, 0, EncRelTwos);
  bind(m, CtlJogSearch, MkCC, sc, 0x0A, 0, EncRelTwos);
  bind(m, CtlJogRim, MkCC, c, 0x09, 0, EncRelTwos);
  bind(m, CtlPitch, MkCC14, c, 0x08, 0x28, EncAbs, true);
  bind(m, CtlVolume, MkCC, c, 0x00);
  bind(m, CtlFilter, MkCC, c, 0x01);
  bind(m, CtlEqLow, MkCC, c, 0x02);
  bind(m, CtlEqHigh, MkCC, c, 0x04);
  bind(m, CtlTrim, MkCC, c, 0x05);
  bind(m, CtlXfader, MkCC, 0, 0x00);
  for (int k = 0; k < 4; ++k) {
    bind(m, CtlHot1 + k, MkNote, pc, k);
    bind(m, CtlHotClear1 + k, MkNote, pc, 0x08 + k);
  }
  const int st = 0x90 | c;
  led(m, CtlPlay, st, 0x07);
  led(m, CtlCue, st, 0x06);
  led(m, CtlSync, st, 0x05);
  led(m, CtlVinyl, st, 0x03);
  led(m, CtlSlip, st, 0x0C);
  led(m, CtlLoopAuto, st, 0x09);
  for (int k = 0; k < 4; ++k) led(m, CtlHot1 + k, 0x90 | pc, k, 0x7E, 0x00);
  m.init.push_back({0xB0, 0x7F, 0x7F});  // report the knob positions
  return m;
}

// Numark Mixtrack Pro FX (as the public Mixxx mapping documents it): deck n on
// MIDI channel n + 1 (button releases are note offs), pads on 0x94 + n,
// crossfader on channel 16.
Mapping mixtrackProFx(int n) {
  Mapping m;
  m.ticksPerRev = 1024;
  bind(m, CtlPlay, MkNote, n, 0x00);
  bind(m, CtlCensor, MkNote, n, 0x04);
  bind(m, CtlCue, MkNote, n, 0x01);
  bind(m, CtlJumpStart, MkNote, n, 0x05);
  bind(m, CtlSync, MkNote, n, 0x02);
  bind(m, CtlKeyLock, MkNote, n, 0x03);
  bind(m, CtlShift, MkNote, n, 0x20);
  bind(m, CtlVinyl, MkNote, n, 0x07);
  bind(m, CtlSlip, MkNote, n, 0x1B);
  bind(m, CtlBendDown, MkNote, n, 0x0C);
  bind(m, CtlBendUp, MkNote, n, 0x0B);
  bind(m, CtlTouch, MkNote, n, 0x06);
  bind(m, CtlJogPlatter, MkCC, n, 0x06, 0, EncRelTwos);
  bind(m, CtlPitch, MkCC14, n, 0x09, 0x29);
  bind(m, CtlVolume, MkCC, n, 0x1C);
  bind(m, CtlEqHigh, MkCC, n, 0x17);
  bind(m, CtlEqMid, MkCC, n, 0x18);
  bind(m, CtlEqLow, MkCC, n, 0x19);
  bind(m, CtlFilter, MkCC, n, 0x1A);
  bind(m, CtlTrim, MkCC, n, 0x16);
  bind(m, CtlXfader, MkCC, 15, 0x08);
  bind(m, CtlLoopAuto, MkNote, 4 + n, 0x40);
  bind(m, CtlLoopHalve, MkNote, 4 + n, 0x34);
  bind(m, CtlLoopDouble, MkNote, 4 + n, 0x35);
  for (int k = 0; k < kHotCues; ++k) {
    bind(m, CtlHot1 + k, MkNote, 4 + n, 0x14 + k);
    bind(m, CtlHotClear1 + k, MkNote, 4 + n, 0x1C + k);
  }
  const int st = 0x90 | n;
  led(m, CtlPlay, st, 0x00, 0x7F, 0x01);
  led(m, CtlCue, st, 0x01, 0x7F, 0x01);
  led(m, CtlSync, st, 0x02, 0x7F, 0x01);
  led(m, CtlVinyl, st, 0x07, 0x7F, 0x01);
  led(m, CtlSlip, st, 0x1B, 0x7F, 0x01);
  led(m, CtlLoopAuto, 0x94 + n, 0x40, 0x7F, 0x01);
  for (int k = 0; k < kHotCues; ++k) led(m, CtlHot1 + k, 0x94 + n, 0x14 + k, 0x7F, 0x01);
  m.init.push_back({0xF0, 0x00, 0x20, 0x7F, 0x03, 0x01, 0xF7});  // report the control positions
  return m;
}

}  // namespace

namespace {

// One side of a profile, every binding on deck A (false for "generic" / unknown ids).
bool oneSide(const std::string& id, int side, Mapping& m) {
  if (id == "ddj200") m = ddj200(side);
  else if (id == "inpulse200") m = inpulse200(side);
  else if (id == "mixtrackprofx") m = mixtrackProFx(side);
  else return false;
  return true;
}

}  // namespace

Mapping profileMapping(const std::string& id, int side) {
  side = std::clamp(side, 0, kSideBoth);
  Mapping m;
  if (side == kSideBoth) {
    // the whole controller: the left side drives deck A, the right side deck B, the
    // mixer controls they share (the crossfader) and the set-up messages once
    Mapping right;
    if (oneSide(id, 0, m) && oneSide(id, 1, right)) {
      for (Binding b : right.bindings) {
        if (controlInfo(b.control).mixer) {
          bool dup = false;
          for (const Binding& x : m.bindings) dup = dup || (x.control == b.control && x.kind == b.kind && x.channel == b.channel && x.num == b.num);
          if (dup) continue;
        }
        b.deck = 1;
        m.bindings.push_back(b);
      }
      for (Led l : right.leds) {
        l.deck = 1;
        m.leds.push_back(l);
      }
      for (const auto& msg : right.init)
        if (std::find(m.init.begin(), m.init.end(), msg) == m.init.end()) m.init.push_back(msg);
    } else {
      m = Mapping{};
      m.ticksPerRev = 128;
    }
  } else if (!oneSide(id, side, m)) {
    m.ticksPerRev = 128;
  }
  m.profile = id;
  m.side = side;
  return m;
}

std::string profileForDevice(const std::string& deviceName) {
  const std::string d = lower(deviceName);
  for (const auto& p : profiles()) {
    std::string match = p.match;
    size_t s = 0;
    while (s < match.size()) {
      size_t e = match.find("; ", s);
      if (e == std::string::npos) e = match.size();
      const std::string w = lower(match.substr(s, e - s));
      if (!w.empty() && d.find(w) != std::string::npos) return p.id;
      s = e + 2;
    }
  }
  return "";
}

std::string bindingText(const Binding& b) {
  char buf[96];
  const char* ch = b.channel < 0 ? "any" : nullptr;
  char chb[8];
  if (!ch) {
    std::snprintf(chb, sizeof chb, "%d", b.channel + 1);
    ch = chb;
  }
  if (b.kind == MkNote) {
    std::snprintf(buf, sizeof buf, "Note %02X  ch %s", b.num, ch);
  } else if (b.kind == MkCC14) {
    std::snprintf(buf, sizeof buf, "CC %02X+%02X  ch %s", b.num, b.lsb, ch);
  } else {
    const char* enc = b.enc == EncRelOffset ? "  rel" : b.enc == EncRelTwos ? "  rel2" : b.enc == EncRelSign ? "  rel+-" : "";
    std::snprintf(buf, sizeof buf, "CC %02X  ch %s%s", b.num, ch, enc);
  }
  std::string s = buf;
  if (b.invert) s += "  inv";
  if (b.layer == LayerShift) s += "  shift";
  return s;
}

Binding learnBinding(int control, uint8_t status, uint8_t d1, uint8_t d2, bool relative, int deck) {
  Binding b;
  b.control = static_cast<uint8_t>(control);
  b.deck = static_cast<uint8_t>(controlInfo(control).mixer ? 0 : deck & 1);
  b.channel = static_cast<int8_t>(status & 0x0F);
  b.num = d1 & 0x7F;
  const int kind = status & 0xF0;
  if (kind == 0x90 || kind == 0x80) {
    b.kind = MkNote;
    return b;
  }
  b.kind = MkCC;
  if (relative) {
    // a first move either side of 64 (offset binary) or near 0 / 127 (two's complement)
    b.enc = (d2 >= 0x38 && d2 <= 0x48) ? EncRelOffset : EncRelTwos;
  } else if (controlInfo(control).type == TypeAbsolute && d1 < 32) {
    b.kind = MkCC14;  // a 7 bit controller sends no LSB: the MSB alone still gives the value
    b.lsb = static_cast<uint8_t>(d1 + 32);
  }
  return b;
}

// ================================================================== decoder
void Decoder::reset() {
  msb_.fill(0);
  lsb_.fill(0);
  shift_[0] = shift_[1] = false;
}

int Decoder::feed(const Mapping& m, uint8_t status, uint8_t d1, uint8_t d2, InEvent* out, int max) {
  const int kind = status & 0xF0, ch = status & 0x0F;
  const bool note = kind == 0x90 || kind == 0x80;
  if (!note && kind != 0xB0) return 0;
  const bool down = kind == 0x90 && d2 > 0;
  int n = 0;
  const size_t count = std::min(m.bindings.size(), static_cast<size_t>(kMaxBindings));
  // Shift first: the layer of the other controls follows it.
  for (size_t i = 0; i < count; ++i) {
    const Binding& b = m.bindings[i];
    if (b.control != CtlShift || b.kind != MkNote || !note || b.num != d1 || (b.channel >= 0 && b.channel != ch)) continue;
    shift_[b.deck & 1] = down;
  }
  for (size_t i = 0; i < count && n < max; ++i) {
    const Binding& b = m.bindings[i];
    if (b.channel >= 0 && b.channel != ch) continue;
    const ControlInfo& ci = controlInfo(b.control);
    const int dk = ci.mixer ? 0 : (b.deck & 1);
    if (b.layer == LayerPlain && shift_[dk] && b.control != CtlShift) continue;
    if (b.layer == LayerShift && !shift_[dk]) continue;
    const ControlType t = ci.type;
    InEvent e;
    e.control = b.control;
    e.deck = static_cast<uint8_t>(dk);
    e.type = static_cast<uint8_t>(t);
    if (b.kind == MkNote) {
      if (!note || b.num != d1) continue;
      e.down = down;
      e.value = down ? 1.f : 0.f;
      if (t == TypeRelative) e.ticks = down ? (b.invert ? -1 : 1) : 0;
      if (t == TypeAbsolute) e.value = b.invert ? 1.f - e.value : e.value;
    } else if (b.kind == MkCC) {
      if (kind != 0xB0 || b.num != d1) continue;
      if (t == TypeRelative) {
        int v = d2;
        if (b.enc == EncRelTwos) v = d2 < 64 ? d2 : d2 - 128;
        else if (b.enc == EncRelSign) v = (d2 & 64) ? -(d2 & 63) : (d2 & 63);
        else v = d2 - 64;
        e.ticks = b.invert ? -v : v;
        if (e.ticks == 0) continue;
      } else {
        e.down = d2 >= 64;
        const float u = static_cast<float>(d2) / 127.0f;
        e.value = b.invert ? 1.0f - u : u;
      }
    } else {  // 14 bit
      if (kind != 0xB0 || (b.num != d1 && b.lsb != d1)) continue;
      if (b.num == d1) msb_[i] = d2 & 0x7F;
      else lsb_[i] = d2 & 0x7F;
      if (b.num == d1 && b.lsb != d1) {
        // a new MSB: the LSB that follows completes it; until then the MSB alone
        lsb_[i] = 0;
      }
      const float u = static_cast<float>((msb_[i] << 7) | lsb_[i]) / 16383.0f;
      e.value = b.invert ? 1.0f - u : u;
      e.down = u >= 0.5f;
      if (t == TypeRelative) continue;
    }
    out[n++] = e;
  }
  return n;
}

// ================================================================== mapping text
namespace {

std::string hexBytes(const std::vector<uint8_t>& b) {
  std::string s;
  char x[4];
  for (uint8_t v : b) {
    std::snprintf(x, sizeof x, "%02X", v);
    s += x;
  }
  return s;
}
bool fromHex(const std::string& s, std::vector<uint8_t>& out) {
  out.clear();
  if (s.size() % 2) return false;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    unsigned v = 0;
    if (std::sscanf(s.c_str() + i, "%2x", &v) != 1) return false;
    out.push_back(static_cast<uint8_t>(v));
  }
  return true;
}

void mappingLines(std::ostringstream& o, const Mapping& m) {
  char b[160];
  std::snprintf(b, sizeof b, "map %s %d %.3f %d\n", m.profile.empty() ? "-" : m.profile.c_str(), m.side, m.ticksPerRev, m.feedback ? 1 : 0);
  o << b;
  o << "dev " << m.device << "\n";
  for (const auto& x : m.bindings) {
    std::snprintf(b, sizeof b, "bind %s %d %d %d %d %d %d %d %d\n", controlInfo(x.control).id, x.kind, x.channel, x.num, x.lsb, x.enc, x.invert ? 1 : 0,
                  x.layer, x.deck);
    o << b;
  }
  for (const auto& l : m.leds) {
    std::snprintf(b, sizeof b, "led %s %d %d %d %d %d\n", controlInfo(l.control).id, l.status, l.num, l.on, l.off, l.deck);
    o << b;
  }
  for (const auto& i : m.init) o << "init " << hexBytes(i) << "\n";
}

// One mapping line into m; false when the line is not a mapping line.
bool mappingLine(const std::string& key, std::istringstream& ls, const std::string& line, Mapping& m) {
  if (key == "map") {
    std::string prof;
    int fb = 1;
    ls >> prof >> m.side >> m.ticksPerRev >> fb;
    m.profile = prof == "-" ? "" : prof;
    m.side = std::clamp(m.side, 0, kSideBoth);
    if (!(m.ticksPerRev >= 8 && m.ticksPerRev <= 100000)) m.ticksPerRev = 128;
    m.feedback = fb != 0;
    return true;
  }
  if (key == "dev") {
    m.device = line.size() > 4 ? line.substr(4) : "";
    return true;
  }
  if (key == "bind") {
    std::string id;
    int kind = 0, chn = -1, num = 0, lsb = 0, enc = 0, inv = 0, layer = 0, deck = 0;
    ls >> id >> kind >> chn >> num >> lsb >> enc >> inv >> layer;
    if (!(ls >> deck)) deck = 0;  // (J-150 lines have no deck: deck A)
    const int c = controlById(id);
    if (c == CtlNone || m.bindings.size() >= static_cast<size_t>(kMaxBindings)) return true;
    Binding x;
    x.control = static_cast<uint8_t>(c);
    x.kind = static_cast<uint8_t>(std::clamp(kind, 0, 2));
    x.channel = static_cast<int8_t>(std::clamp(chn, -1, 15));
    x.num = static_cast<uint8_t>(std::clamp(num, 0, 127));
    x.lsb = static_cast<uint8_t>(std::clamp(lsb, 0, 127));
    x.enc = static_cast<uint8_t>(std::clamp(enc, 0, 3));
    x.invert = inv != 0;
    x.layer = static_cast<uint8_t>(std::clamp(layer, 0, 2));
    x.deck = static_cast<uint8_t>(controlInfo(c).mixer ? 0 : std::clamp(deck, 0, 1));
    m.bindings.push_back(x);
    return true;
  }
  if (key == "led") {
    std::string id;
    int st = 0x90, num = 0, on = 0x7F, off = 0, deck = 0;
    ls >> id >> st >> num >> on >> off;
    if (!(ls >> deck)) deck = 0;
    const int c = controlById(id);
    if (c == CtlNone) return true;
    Led l;
    l.control = static_cast<uint8_t>(c);
    l.status = static_cast<uint8_t>(std::clamp(st, 0x80, 0xEF));
    l.num = static_cast<uint8_t>(std::clamp(num, 0, 127));
    l.on = static_cast<uint8_t>(std::clamp(on, 0, 127));
    l.off = static_cast<uint8_t>(std::clamp(off, 0, 127));
    l.deck = static_cast<uint8_t>(std::clamp(deck, 0, 1));
    m.leds.push_back(l);
    return true;
  }
  if (key == "init") {
    std::string hex;
    ls >> hex;
    std::vector<uint8_t> v;
    if (fromHex(hex, v) && !v.empty() && v.size() <= 256) m.init.push_back(v);
    return true;
  }
  return false;
}

}  // namespace

std::string mappingToText(const Mapping& m) {
  std::ostringstream o;
  o << "platter-mapping 2\n";
  mappingLines(o, m);
  return o.str();
}

bool mappingFromText(const std::string& text, Mapping& m) {
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("platter-mapping", 0) != 0) return false;
  m = Mapping{};
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string key;
    ls >> key;
    mappingLine(key, ls, line, m);
  }
  return true;
}

bool saveMappingPreset(const std::string& dir, const std::string& name, const Mapping& m) {
  std::string clean;
  for (char c : name)
    if (c != '/' && c != '\\' && c != ':' && c != '\n' && c != '\r') clean += c;
  while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
  if (clean.empty()) return false;
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::ofstream f(fs::path(dir) / (clean + ".gpmap"), std::ios::binary);
  if (!f) return false;
  f << mappingToText(m);
  return static_cast<bool>(f);
}

bool loadMappingPreset(const std::string& path, Mapping& m) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  return mappingFromText(ss.str(), m);
}

std::vector<std::string> mappingPresetFiles(const std::string& dir) {
  std::vector<std::string> v;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().extension() == ".gpmap") v.push_back(it->path().string());
  std::sort(v.begin(), v.end());
  return v;
}

// ================================================================== take points
bool TakePoint::operator==(const TakePoint& o) const { return pos == o.pos && from == o.from && gain == o.gain && flags == o.flags; }
bool TakeStream::operator==(const TakeStream& o) const {
  return on == o.on && file == o.file && startPos == o.startPos && startRate == o.startRate && startGain == o.startGain && pts == o.pts;
}
bool Take::operator==(const Take& o) const {
  return key == o.key && name == o.name && sampleRate == o.sampleRate && step == o.step && tempo == o.tempo && ppq == o.ppq &&
         startTick == o.startTick && lengthTicks == o.lengthTicks && s == o.s && moves == o.moves;
}
size_t Take::steps() const {
  size_t n = 0;
  for (const auto& x : s)
    if (x.on) n = std::max(n, x.pts.size());
  return n;
}

const std::array<uint8_t, kMoveParams>& moveParams() {
  static const std::array<uint8_t, kMoveParams> k = [] {
    std::array<uint8_t, kMoveParams> v{};
    int i = 0;
    for (int d = 0; d < kDecks; ++d)
      for (int x : {DTrim, DEqHigh, DEqMid, DEqLow, DFilter, DFilterRes}) v[static_cast<size_t>(i++)] = static_cast<uint8_t>(deckParam(d, x));
    v[static_cast<size_t>(i)] = PMaster;
    return v;
  }();
  return k;
}

namespace {

constexpr double kPosQ = 256.0;    // position resolution: 1/256 frame
constexpr double kGainQ = 4096.0;  // gain resolution

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64(const std::vector<uint8_t>& d) {
  std::string s;
  s.reserve((d.size() + 2) / 3 * 4);
  for (size_t i = 0; i < d.size(); i += 3) {
    const uint32_t v = (static_cast<uint32_t>(d[i]) << 16) | (i + 1 < d.size() ? static_cast<uint32_t>(d[i + 1]) << 8 : 0) |
                       (i + 2 < d.size() ? d[i + 2] : 0);
    s += kB64[(v >> 18) & 63];
    s += kB64[(v >> 12) & 63];
    s += i + 1 < d.size() ? kB64[(v >> 6) & 63] : '=';
    s += i + 2 < d.size() ? kB64[v & 63] : '=';
  }
  return s;
}
bool unb64(const std::string& s, std::vector<uint8_t>& d) {
  d.clear();
  int val[256];
  std::fill(val, val + 256, -1);
  for (int i = 0; i < 64; ++i) val[static_cast<unsigned char>(kB64[i])] = i;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=') break;
    const int v = val[static_cast<unsigned char>(c)];
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      d.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
    }
  }
  return true;
}
void putVar(std::vector<uint8_t>& o, uint64_t v) {
  while (v >= 0x80) {
    o.push_back(static_cast<uint8_t>(v | 0x80));
    v >>= 7;
  }
  o.push_back(static_cast<uint8_t>(v));
}
bool getVar(const std::vector<uint8_t>& d, size_t& i, uint64_t& v) {
  v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (i >= d.size()) return false;
    const uint8_t b = d[i++];
    v |= static_cast<uint64_t>(b & 0x7F) << shift;
    if (!(b & 0x80)) return true;
  }
  return false;
}
uint64_t zig(int64_t v) { return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63); }
int64_t unzig(uint64_t v) { return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1); }

}  // namespace

// Stream: tokens 0 = a run of n points that repeat the last motion (same
// speed step, gain and flags), 1 = a point: zigzag(second difference of the
// position), zigzag(gain difference), flags, and for jumps zigzag(from - last).
std::string encodePoints(const std::vector<TakePoint>& pts) {
  std::vector<uint8_t> o;
  int64_t lastQ = 0, lastV = 0, lastG = 0;
  uint8_t lastF = 0;
  bool first = true;
  uint64_t run = 0;
  auto flush = [&] {
    if (!run) return;
    o.push_back(0);
    putVar(o, run);
    run = 0;
  };
  for (const auto& p : pts) {
    const int64_t q = std::llround(p.pos * kPosQ);
    const int64_t g = std::llround(static_cast<double>(p.gain) * kGainQ);
    const bool jump = (p.flags & TfJump) != 0;
    const int64_t base = jump ? std::llround(p.from * kPosQ) : lastQ;
    const int64_t v = q - base;
    if (!first && !jump && v == lastV && g == lastG && p.flags == lastF) {
      ++run;
      lastQ = q;
      continue;
    }
    flush();
    o.push_back(1);
    putVar(o, zig(v - lastV));
    putVar(o, zig(g - lastG));
    o.push_back(p.flags);
    if (jump) putVar(o, zig(base - lastQ));
    lastQ = q, lastV = v, lastG = g, lastF = p.flags;
    first = false;
  }
  flush();
  return b64(o);
}

bool decodePoints(const std::string& text, std::vector<TakePoint>& pts) {
  pts.clear();
  std::vector<uint8_t> d;
  if (!unb64(text, d)) return false;
  int64_t lastQ = 0, lastV = 0, lastG = 0;
  uint8_t lastF = 0;
  size_t i = 0;
  while (i < d.size()) {
    const uint8_t tok = d[i++];
    if (tok == 0) {
      uint64_t n = 0;
      if (!getVar(d, i, n) || n > 50000000ull) return false;
      for (uint64_t k = 0; k < n; ++k) {
        lastQ += lastV;
        TakePoint p;
        p.pos = static_cast<double>(lastQ) / kPosQ;
        p.from = 0;
        p.gain = static_cast<float>(static_cast<double>(lastG) / kGainQ);
        p.flags = lastF;
        pts.push_back(p);
      }
    } else if (tok == 1) {
      uint64_t dv = 0, dg = 0;
      if (!getVar(d, i, dv) || !getVar(d, i, dg) || i >= d.size()) return false;
      const uint8_t f = d[i++];
      int64_t base = lastQ;
      if (f & TfJump) {
        uint64_t df = 0;
        if (!getVar(d, i, df)) return false;
        base = lastQ + unzig(df);
      }
      lastV += unzig(dv);
      lastG += unzig(dg);
      lastQ = base + lastV;
      lastF = f;
      TakePoint p;
      p.pos = static_cast<double>(lastQ) / kPosQ;
      p.from = (f & TfJump) ? static_cast<double>(base) / kPosQ : 0;
      p.gain = static_cast<float>(static_cast<double>(lastG) / kGainQ);
      p.flags = f;
      pts.push_back(p);
    } else {
      return false;
    }
  }
  return true;
}

// Moves: per move varint(step delta), the parameter id, the value's 32 bits
// (little endian; replay needs the exact float), base64.
std::string encodeMoves(const std::vector<TakeMove>& mv) {
  std::vector<uint8_t> o;
  uint32_t last = 0;
  for (const auto& m : mv) {
    putVar(o, m.step >= last ? m.step - last : 0);
    last = std::max(last, m.step);
    o.push_back(m.param);
    uint32_t bits = 0;
    std::memcpy(&bits, &m.value, 4);
    for (int k = 0; k < 4; ++k) o.push_back(static_cast<uint8_t>(bits >> (8 * k)));
  }
  return b64(o);
}

bool decodeMoves(const std::string& text, std::vector<TakeMove>& mv) {
  mv.clear();
  std::vector<uint8_t> d;
  if (!unb64(text, d)) return false;
  size_t i = 0;
  uint64_t step = 0;
  while (i < d.size()) {
    uint64_t dt = 0;
    if (!getVar(d, i, dt) || i + 5 > d.size()) return false;
    step += dt;
    if (step > 0xFFFFFFFFull) return false;
    TakeMove m;
    m.step = static_cast<uint32_t>(step);
    m.param = d[i++];
    uint32_t bits = 0;
    for (int k = 0; k < 4; ++k) bits |= static_cast<uint32_t>(d[i++]) << (8 * k);
    std::memcpy(&m.value, &bits, 4);
    if (m.param >= PCount || !std::isfinite(m.value)) return false;
    mv.push_back(m);
  }
  return true;
}

int freeTakeKey(const std::vector<Take>& takes) {
  for (int k = kTakeKeyFirst; k < kTakeKeyFirst + kTakeKeys; ++k) {
    bool used = false;
    for (const auto& t : takes) used = used || t.key == k;
    if (!used) return k;
  }
  return -1;
}

// ================================================================== model
bool DeckModel::operator==(const DeckModel& o) const {
  return file == o.file && srcFrames == o.srcFrames && srcRate == o.srcRate && analysed == o.analysed && grid.beats == o.grid.beats &&
         grid.bpm == o.grid.bpm && grid.beatsPerBar == o.grid.beatsPerBar && grid.bar1 == o.grid.bar1 && grid.key == o.grid.key &&
         grid.minor == o.grid.minor && cue == o.cue && hot == o.hot && loopIn == o.loopIn && loopOut == o.loopOut;
}
bool Model::operator==(const Model& o) const {
  return files == o.files && deck == o.deck && layout == o.layout && asked == o.asked && armed == o.armed && recordAs == o.recordAs &&
         map == o.map && takes == o.takes && starts == o.starts;
}
std::string Model::trackOf(int d) const {
  if (d < 0 || d >= kDecks) return {};
  const int f = deck[static_cast<size_t>(d)].file;
  return f >= 0 && f < static_cast<int>(files.size()) ? files[static_cast<size_t>(f)] : std::string{};
}

std::string pathOf(const std::string& data) {
  const size_t nl = data.find('\n');
  return nl == std::string::npos ? data : data.substr(0, nl);
}

namespace {

void deckLines(std::ostringstream& o, const DeckModel& m) {
  char b[256];
  std::snprintf(b, sizeof b, "src %lld %.3f %d\n", static_cast<long long>(m.srcFrames), m.srcRate, m.analysed ? 1 : 0);
  o << b;
  const auto& g = m.grid;
  // a regular grid is stored as its first beat and period, an analysed one beat by beat
  bool regular = g.beats.size() >= 2;
  const double period = regular ? g.beats[1] - g.beats[0] : 0;
  for (size_t i = 2; regular && i < g.beats.size(); ++i) regular = std::fabs(g.beats[i] - g.beats[i - 1] - period) < 1e-6;
  std::snprintf(b, sizeof b, "grid %.6f %d %d", g.bpm, g.beatsPerBar, g.bar1);
  o << b;
  if (regular) {
    std::snprintf(b, sizeof b, " reg %.6f %.6f %zu\n", g.beats[0], period, g.beats.size());
    o << b;
  } else {
    o << " list " << g.beats.size();
    for (double x : g.beats) {
      std::snprintf(b, sizeof b, " %.3f", x);
      o << b;
    }
    o << "\n";
  }
  std::snprintf(b, sizeof b, "key %d %d\n", g.key, g.minor ? 1 : 0);
  o << b;
  std::snprintf(b, sizeof b, "cue %.4f\n", m.cue);
  o << b;
  o << "hot";
  for (double h : m.hot) {
    std::snprintf(b, sizeof b, " %.4f", h);
    o << b;
  }
  o << "\n";
  std::snprintf(b, sizeof b, "loop %.4f %.4f\n", m.loopIn, m.loopOut);
  o << b;
}

// One deck line into m; false when the line is not a deck line.
bool deckLine(const std::string& key, std::istringstream& ls, DeckModel& m) {
  if (key == "src") {
    long long f = 0;
    int an = 0;
    ls >> f >> m.srcRate >> an;
    m.srcFrames = std::max(0LL, f);
    if (!(m.srcRate >= 0 && m.srcRate < 1e7)) m.srcRate = 0;
    m.analysed = an != 0;
    m.grid.frames = m.srcFrames;
    m.grid.sampleRate = m.srcRate;
  } else if (key == "grid") {
    std::string kind;
    ls >> m.grid.bpm >> m.grid.beatsPerBar >> m.grid.bar1 >> kind;
    m.grid.beatsPerBar = std::clamp(m.grid.beatsPerBar, 1, 16);
    m.grid.beats.clear();
    if (kind == "reg") {
      double first = 0, period = 0;
      size_t n = 0;
      ls >> first >> period >> n;
      if (period > 1 && n < 2000000)
        for (size_t i = 0; i < n; ++i) m.grid.beats.push_back(first + period * static_cast<double>(i));
    } else if (kind == "list") {
      size_t n = 0;
      ls >> n;
      for (size_t i = 0; i < n && i < 2000000; ++i) {
        double x = 0;
        if (!(ls >> x)) break;
        m.grid.beats.push_back(x);
      }
      std::sort(m.grid.beats.begin(), m.grid.beats.end());
    }
    if (!(m.grid.bpm > 1 && m.grid.bpm < 1000)) m.grid.bpm = 120;
    m.grid.bar1 = std::clamp(m.grid.bar1, 0, std::max(0, static_cast<int>(m.grid.beats.size()) - 1));
  } else if (key == "key") {
    int k = -1, mi = 0;
    ls >> k >> mi;
    m.grid.key = std::clamp(k, -1, 11);
    m.grid.minor = mi != 0;
  } else if (key == "cue") {
    ls >> m.cue;
  } else if (key == "hot") {
    for (auto& h : m.hot)
      if (!(ls >> h)) h = -1;
  } else if (key == "loop") {
    ls >> m.loopIn >> m.loopOut;
    if (!(m.loopOut > m.loopIn)) m.loopIn = m.loopOut = -1;
  } else {
    return false;
  }
  return true;
}

bool takeHeaderOk(const Take& t) {
  return t.key >= kTakeKeyFirst && t.key < kTakeKeyFirst + kTakeKeys && t.sampleRate > 1000 && t.sampleRate < 1e7 && t.step >= 1 &&
         t.step <= 4096 && t.tempo > 1 && t.tempo < 10000;
}

Take* takeOf(Model& m, int key) {
  for (auto& t : m.takes)
    if (t.key == key) return &t;
  return nullptr;
}

}  // namespace

std::string modelToText(const Model& m) {
  std::ostringstream o;
  char b[256];
  o << "platter 2\n";
  std::snprintf(b, sizeof b, "layout %d\nflags %d %d\n", m.layout, m.asked ? 1 : 0, m.armed ? 1 : 0);
  o << b;
  if (m.recordAs != RecAutomation) o << "rec take\n";  // J-159 (missing: automation)
  for (const auto& f : m.files) o << "file " << f << "\n";
  for (int d = 0; d < kDecks; ++d) {
    const DeckModel& dm = m.deck[static_cast<size_t>(d)];
    o << "deck " << d << " " << dm.file << "\n";
    deckLines(o, dm);
  }
  mappingLines(o, m.map);
  for (const auto& t : m.takes) {
    std::snprintf(b, sizeof b, "take2 %d %.3f %d %.6f %d %lld %lld\n", t.key, t.sampleRate, t.step, t.tempo, t.ppq,
                  static_cast<long long>(t.startTick), static_cast<long long>(t.lengthTicks));
    o << b;
    for (int d = 0; d < kDecks; ++d) {
      const TakeStream& x = t.s[static_cast<size_t>(d)];
      if (!x.on) continue;
      std::snprintf(b, sizeof b, "tstream %d %d %d %.6f %.9f %.6f %zu ", t.key, d, x.file, x.startPos, x.startRate,
                    static_cast<double>(x.startGain), x.pts.size());
      o << b << encodePoints(x.pts) << "\n";
    }
    if (!t.moves.empty()) o << "tmoves " << t.key << " " << t.moves.size() << " " << encodeMoves(t.moves) << "\n";
    o << "tname " << t.key << " " << t.name << "\n";
  }
  // J-159: recorded performances' start states (exact: each value's bits in hex)
  for (const auto& ps : m.starts) {
    o << "pstart " << ps.id << " " << ps.v.size() << " ";
    for (double x : ps.v) {
      uint64_t u = 0;
      std::memcpy(&u, &x, sizeof u);
      std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(u));
      o << b;
    }
    o << "\n";
  }
  return o.str();
}

bool modelFromText(const std::string& text, Model& m) {
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("platter ", 0) != 0) return false;
  const int version = std::atoi(line.c_str() + 8);
  m = Model{};
  bool any = false;
  int cur = 0;  // deck the src / grid / cue lines belong to
  if (version < 2) m.deck[0].file = 0;  // J-150: one deck on the first line's track
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string key;
    ls >> key;
    if (key == "layout") {
      int l = LayoutTwo;
      ls >> l;
      m.layout = l == LayoutOne ? LayoutOne : LayoutTwo;
    } else if (key == "file") {
      if (m.files.size() < 256) m.files.push_back(line.size() > 5 ? line.substr(5) : std::string{});
    } else if (key == "deck") {
      int d = 0, f = -1;
      ls >> d >> f;
      cur = std::clamp(d, 0, kDecks - 1);
      m.deck[static_cast<size_t>(cur)].file = f;
    } else if (deckLine(key, ls, m.deck[static_cast<size_t>(cur)])) {
      if (key == "src") any = true;
    } else if (key == "flags") {
      int a = 0, r = 1;
      ls >> a >> r;
      m.asked = a != 0;
      m.armed = r != 0;
    } else if (key == "rec") {
      std::string v;
      ls >> v;
      m.recordAs = v == "take" ? RecTake : RecAutomation;
    } else if (key == "take") {  // J-150: deck A's stream on the first file
      Take t;
      long long st = 0, len = 0;
      float sg = 1;
      size_t n = 0;
      std::string data;
      TakeStream& x = t.s[0];
      ls >> t.key >> t.sampleRate >> t.step >> t.tempo >> t.ppq >> st >> len >> x.startPos >> x.startRate >> sg >> n >> data;
      t.startTick = st;
      t.lengthTicks = std::max(0LL, len);
      x.startGain = sg;
      x.on = true;
      x.file = 0;
      if (!takeHeaderOk(t) || takeOf(m, t.key)) continue;
      if (!decodePoints(data, x.pts) || x.pts.size() != n) continue;
      m.takes.push_back(std::move(t));
    } else if (key == "pstart") {  // J-159
      PerfStart ps;
      size_t n = 0;
      std::string hex;
      ls >> ps.id >> n >> hex;
      if (n > 4096 || hex.size() != n * 16) continue;
      ps.v.resize(n);
      bool ok = true;
      for (size_t i = 0; i < n && ok; ++i) {
        uint64_t u = 0;
        for (size_t c = 0; c < 16; ++c) {
          const char h = hex[i * 16 + c];
          const int dgt = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : -1;
          if (dgt < 0) ok = false;
          u = (u << 4) | static_cast<uint64_t>(dgt < 0 ? 0 : dgt);
        }
        std::memcpy(&ps.v[i], &u, sizeof u);
      }
      if (ok) {
        m.starts.erase(std::remove_if(m.starts.begin(), m.starts.end(), [&](const PerfStart& q) { return q.id == ps.id; }), m.starts.end());
        m.starts.push_back(std::move(ps));
      }
    } else if (key == "take2") {
      Take t;
      long long st = 0, len = 0;
      ls >> t.key >> t.sampleRate >> t.step >> t.tempo >> t.ppq >> st >> len;
      t.startTick = st;
      t.lengthTicks = std::max(0LL, len);
      if (!takeHeaderOk(t) || takeOf(m, t.key)) continue;
      m.takes.push_back(std::move(t));
    } else if (key == "tstream") {
      int k = 0, d = 0, f = -1;
      float sg = 1;
      size_t n = 0;
      std::string data;
      TakeStream x;
      ls >> k >> d >> f >> x.startPos >> x.startRate >> sg >> n >> data;
      Take* t = takeOf(m, k);
      if (!t || d < 0 || d >= kDecks || !decodePoints(data, x.pts) || x.pts.size() != n) continue;
      x.on = true;
      x.file = f;
      x.startGain = sg;
      t->s[static_cast<size_t>(d)] = std::move(x);
    } else if (key == "tmoves") {
      int k = 0;
      size_t n = 0;
      std::string data;
      ls >> k >> n >> data;
      Take* t = takeOf(m, k);
      std::vector<TakeMove> mv;
      if (t && decodeMoves(data, mv) && mv.size() == n) t->moves = std::move(mv);
    } else if (key == "tname") {
      int k = 0;
      ls >> k;
      std::string rest;
      std::getline(ls, rest);
      if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
      if (Take* t = takeOf(m, k)) t->name = rest;
    } else {
      mappingLine(key, ls, line, m.map);
    }
  }
  // takes without a stream are dropped; indices out of the file list are empty
  m.takes.erase(std::remove_if(m.takes.begin(), m.takes.end(), [](const Take& t) { return t.steps() == 0; }), m.takes.end());
  if (version >= 2) {
    const int nf = static_cast<int>(m.files.size());
    for (auto& d : m.deck)
      if (d.file >= nf) d.file = -1;
    for (auto& t : m.takes)
      for (auto& x : t.s)
        if (x.file >= nf) x.file = -1;
  }
  return any || !m.files.empty();
}

int fileIndex(Model& m, const std::string& path) {
  for (size_t i = 0; i < m.files.size(); ++i)
    if (m.files[i] == path) return static_cast<int>(i);
  m.files.push_back(path);
  return static_cast<int>(m.files.size()) - 1;
}

void pruneFiles(Model& m) {
  std::vector<int> used(m.files.size(), 0);
  auto mark = [&](int f) {
    if (f >= 0 && f < static_cast<int>(used.size())) used[static_cast<size_t>(f)] = 1;
  };
  for (const auto& d : m.deck) mark(d.file);
  for (const auto& t : m.takes)
    for (const auto& x : t.s)
      if (x.on) mark(x.file);
  std::vector<int> to(m.files.size(), -1);
  std::vector<std::string> keep;
  for (size_t i = 0; i < m.files.size(); ++i)
    if (used[i]) {
      to[i] = static_cast<int>(keep.size());
      keep.push_back(m.files[i]);
    }
  auto remap = [&](int f) { return f >= 0 && f < static_cast<int>(to.size()) ? to[static_cast<size_t>(f)] : -1; };
  for (auto& d : m.deck) d.file = remap(d.file);
  for (auto& t : m.takes)
    for (auto& x : t.s) x.file = remap(x.file);
  m.files = std::move(keep);
}

bool parseData(const std::string& data, std::string& path, Model& m) {
  const size_t nl = data.find('\n');
  path = nl == std::string::npos ? data : data.substr(0, nl);
  const bool has = nl != std::string::npos && modelFromText(data.substr(nl + 1), m);
  if (!has) m = Model{};
  if (m.files.empty() && !path.empty()) m.files.push_back(path);  // J-150 state / bare path
  if (m.trackOf(0) != path) {
    // the first line changed (a drop on the channel, a relink): deck A's new track,
    // its grid and cues come with the next analysis
    DeckModel fresh;
    if (!path.empty()) fresh.file = fileIndex(m, path);
    m.deck[0] = fresh;
    pruneFiles(m);
  }
  return has;
}

std::string formatData(const std::string& path, const Model& m) { return path + "\n" + modelToText(m); }

DeckModel deckModel(const Sample& s, const seltmann::Analysis& a, bool analysed) {
  DeckModel m;
  m.srcFrames = static_cast<int64_t>(s.frames());
  m.srcRate = s.sampleRate;
  m.analysed = analysed;
  m.grid.sampleRate = s.sampleRate;
  m.grid.frames = m.srcFrames;
  m.grid.bpm = a.bpm > 1 ? a.bpm : 120;
  m.grid.beats = a.beats;
  m.grid.beatsPerBar = std::clamp(a.beatsPerBar, 1, 16);
  m.grid.bar1 = std::clamp(a.bar1, 0, std::max(0, static_cast<int>(a.beats.size()) - 1));
  m.grid.key = analysed ? a.key : -1;
  m.grid.minor = analysed && a.minor;
  if (m.grid.beats.empty()) m.grid.beats.push_back(0);
  // the cue sits on the first downbeat of the audible part
  double cue = m.grid.beats[static_cast<size_t>(m.grid.bar1)];
  if (a.contentStart > 0 && cue < static_cast<double>(a.contentStart) - 0.25 * seltmann::beatFrames(m.grid)) {
    for (size_t i = static_cast<size_t>(m.grid.bar1); i < m.grid.beats.size(); i += static_cast<size_t>(m.grid.beatsPerBar))
      if (m.grid.beats[i] >= static_cast<double>(a.contentStart) - 0.25 * seltmann::beatFrames(m.grid)) {
        cue = m.grid.beats[i];
        break;
      }
  }
  m.cue = std::clamp(cue, 0.0, static_cast<double>(std::max<int64_t>(0, m.srcFrames - 1)));
  return m;
}

void loadTrack(Model& m, int d, const std::string& path, const DeckModel& fresh) {
  if (d < 0 || d >= kDecks) return;
  DeckModel x = fresh;
  x.file = path.empty() ? -1 : fileIndex(m, path);
  m.deck[static_cast<size_t>(d)] = std::move(x);
  pruneFiles(m);
}

double beatAt(const DeckModel& m, double frame) { return seltmann::beatAt(m.grid, frame); }
double frameAt(const DeckModel& m, double beat) { return seltmann::frameAt(m.grid, beat); }
double nearestBeatFrame(const DeckModel& m, double frame) { return frameAt(m, std::round(beatAt(m, frame))); }
double bpmOf(const DeckModel& m) { return m.grid.bpm > 1 ? m.grid.bpm : 120.0; }

bool TakeBuilder::feed(const CaptureEvent& e) {
  switch (e.kind) {
    case CaptureEvent::Start:
      take = Take{};
      take.sampleRate = e.sampleRate;
      take.step = kStep;
      take.tempo = e.tempo;
      take.ppq = e.ppq;
      take.startTick = static_cast<Tick>(std::llround(e.tick));
      for (int d = 0; d < kDecks; ++d) {
        if (!(e.streams & (1u << d))) continue;
        TakeStream& x = take.s[static_cast<size_t>(d)];
        x.on = true;
        x.file = e.file[d];
        x.startPos = e.pt[d].pos;
        x.startRate = e.startRate[d];
        x.startGain = e.gain[d];
        x.pts.reserve(4096);
      }
      active = true;
      lost = false;
      return false;
    case CaptureEvent::Point:
      if (active)
        for (int d = 0; d < kDecks; ++d)
          if (take.s[static_cast<size_t>(d)].on) take.s[static_cast<size_t>(d)].pts.push_back(e.pt[d]);
      return false;
    case CaptureEvent::Move:  // (a take keeps the mixer values its streams do not hold)
      if (active)
        for (uint8_t p : moveParams())
          if (p == e.param) take.moves.push_back(TakeMove{e.step, e.param, static_cast<float>(e.value)});
      return false;
    case CaptureEvent::Button:
    case CaptureEvent::Snap:
      return false;
    case CaptureEvent::Lost:  // an end after dropped points: no take
      lost = true;
      active = false;
      return false;
    case CaptureEvent::End:
      if (!active) return false;
      active = false;
      take.lengthTicks = std::max<Tick>(1, static_cast<Tick>(std::llround(e.tick)) - take.startTick);
      return take.steps() > 0 && !lost;
  }
  return false;
}

}  // namespace gl::packs::platter
