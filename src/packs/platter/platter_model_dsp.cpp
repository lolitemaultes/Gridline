// Gridline — "platter" pack: the deck's model as text (grid, cues, loop,
// controller set-up, takes), the take point codec, the deck controls, the
// controller profiles and the MIDI decoder. Pure logic: the decoder runs on the
// audio thread (no allocation), the rest on the UI thread.
#include <algorithm>
#include <cmath>
#include <cstdio>
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

// ================================================================== controls
const ControlInfo& controlInfo(int c) {
  static const ControlInfo k[CtlCount] = {
      {"none", "None", TypeButton, "", -1},
      {"play", "Play / pause", TypeButton, "Transport", -1},
      {"cue", "Cue", TypeButton, "Transport", -1},
      {"sync", "Sync", TypeButton, "Transport", PSync},
      {"keylock", "Key lock", TypeButton, "Modes", PKeyLock},
      {"shift", "Shift", TypeButton, "Transport", -1},
      {"touch", "Jog touch", TypeButton, "Jog", -1},
      {"jog", "Jog platter", TypeRelative, "Jog", -1},
      {"rim", "Jog side (nudge)", TypeRelative, "Jog", -1},
      {"search", "Jog search", TypeRelative, "Jog", -1},
      {"pitch", "Pitch fader", TypeAbsolute, "Mixer", PPitch},
      {"volume", "Channel fader", TypeAbsolute, "Mixer", PVolume},
      {"xfader", "Crossfader", TypeAbsolute, "Mixer", PXfader},
      {"eqhigh", "EQ high", TypeAbsolute, "Mixer", PEqHigh},
      {"eqmid", "EQ mid", TypeAbsolute, "Mixer", PEqMid},
      {"eqlow", "EQ low", TypeAbsolute, "Mixer", PEqLow},
      {"filter", "Filter", TypeAbsolute, "Mixer", PFilter},
      {"trim", "Trim", TypeAbsolute, "Mixer", PTrim},
      {"hot1", "Hot cue 1", TypeButton, "Pads", -1},
      {"hot2", "Hot cue 2", TypeButton, "Pads", -1},
      {"hot3", "Hot cue 3", TypeButton, "Pads", -1},
      {"hot4", "Hot cue 4", TypeButton, "Pads", -1},
      {"hot5", "Hot cue 5", TypeButton, "Pads", -1},
      {"hot6", "Hot cue 6", TypeButton, "Pads", -1},
      {"hot7", "Hot cue 7", TypeButton, "Pads", -1},
      {"hot8", "Hot cue 8", TypeButton, "Pads", -1},
      {"hotclear1", "Clear hot cue 1", TypeButton, "Pads", -1},
      {"hotclear2", "Clear hot cue 2", TypeButton, "Pads", -1},
      {"hotclear3", "Clear hot cue 3", TypeButton, "Pads", -1},
      {"hotclear4", "Clear hot cue 4", TypeButton, "Pads", -1},
      {"hotclear5", "Clear hot cue 5", TypeButton, "Pads", -1},
      {"hotclear6", "Clear hot cue 6", TypeButton, "Pads", -1},
      {"hotclear7", "Clear hot cue 7", TypeButton, "Pads", -1},
      {"hotclear8", "Clear hot cue 8", TypeButton, "Pads", -1},
      {"loopin", "Loop in", TypeButton, "Loop", -1},
      {"loopout", "Loop out", TypeButton, "Loop", -1},
      {"loopauto", "Auto loop", TypeButton, "Loop", -1},
      {"loophalve", "Loop / 2", TypeButton, "Loop", -1},
      {"loopdouble", "Loop x 2", TypeButton, "Loop", -1},
      {"jumpback", "Beat jump back", TypeButton, "Loop", -1},
      {"jumpfwd", "Beat jump forward", TypeButton, "Loop", -1},
      {"slip", "Slip", TypeButton, "Modes", PSlip},
      {"vinyl", "Vinyl mode", TypeButton, "Modes", PVinyl},
      {"quantize", "Quantize", TypeButton, "Modes", PQuantize},
      {"censor", "Reverse (held)", TypeButton, "Transport", -1},
      {"jumpstart", "Back to start", TypeButton, "Transport", -1},
      {"benddown", "Pitch bend -", TypeButton, "Transport", -1},
      {"bendup", "Pitch bend +", TypeButton, "Transport", -1},
      {"loaded", "Track loaded", TypeOutput, "Transport", -1},
  };
  return k[c >= 0 && c < CtlCount ? c : 0];
}

int controlById(const std::string& id) {
  for (int c = 1; c < CtlCount; ++c)
    if (id == controlInfo(c).id) return c;
  return CtlNone;
}

bool Binding::operator==(const Binding& o) const {
  return control == o.control && kind == o.kind && channel == o.channel && num == o.num && lsb == o.lsb && enc == o.enc && invert == o.invert &&
         layer == o.layer;
}
bool Led::operator==(const Led& o) const { return control == o.control && status == o.status && num == o.num && on == o.on && off == o.off; }
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

Mapping profileMapping(const std::string& id, int side) {
  side = std::clamp(side, 0, 1);
  Mapping m;
  if (id == "ddj200") m = ddj200(side);
  else if (id == "inpulse200") m = inpulse200(side);
  else if (id == "mixtrackprofx") m = mixtrackProFx(side);
  else m.ticksPerRev = 128;
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

Binding learnBinding(int control, uint8_t status, uint8_t d1, uint8_t d2, bool relative) {
  Binding b;
  b.control = static_cast<uint8_t>(control);
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
  shift_ = false;
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
    shift_ = down;
  }
  for (size_t i = 0; i < count && n < max; ++i) {
    const Binding& b = m.bindings[i];
    if (b.channel >= 0 && b.channel != ch) continue;
    if (b.layer == LayerPlain && shift_ && b.control != CtlShift) continue;
    if (b.layer == LayerShift && !shift_) continue;
    const ControlType t = controlInfo(b.control).type;
    InEvent e;
    e.control = b.control;
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
    std::snprintf(b, sizeof b, "bind %s %d %d %d %d %d %d %d\n", controlInfo(x.control).id, x.kind, x.channel, x.num, x.lsb, x.enc, x.invert ? 1 : 0,
                  x.layer);
    o << b;
  }
  for (const auto& l : m.leds) {
    std::snprintf(b, sizeof b, "led %s %d %d %d %d\n", controlInfo(l.control).id, l.status, l.num, l.on, l.off);
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
    m.side = std::clamp(m.side, 0, 1);
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
    int kind = 0, chn = -1, num = 0, lsb = 0, enc = 0, inv = 0, layer = 0;
    ls >> id >> kind >> chn >> num >> lsb >> enc >> inv >> layer;
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
    m.bindings.push_back(x);
    return true;
  }
  if (key == "led") {
    std::string id;
    int st = 0x90, num = 0, on = 0x7F, off = 0;
    ls >> id >> st >> num >> on >> off;
    const int c = controlById(id);
    if (c == CtlNone) return true;
    Led l;
    l.control = static_cast<uint8_t>(c);
    l.status = static_cast<uint8_t>(std::clamp(st, 0x80, 0xEF));
    l.num = static_cast<uint8_t>(std::clamp(num, 0, 127));
    l.on = static_cast<uint8_t>(std::clamp(on, 0, 127));
    l.off = static_cast<uint8_t>(std::clamp(off, 0, 127));
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
  o << "platter-mapping 1\n";
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
bool Take::operator==(const Take& o) const {
  return key == o.key && name == o.name && sampleRate == o.sampleRate && step == o.step && tempo == o.tempo && ppq == o.ppq &&
         startTick == o.startTick && lengthTicks == o.lengthTicks && startPos == o.startPos && startRate == o.startRate &&
         startGain == o.startGain && pts == o.pts;
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

int freeTakeKey(const std::vector<Take>& takes) {
  for (int k = kTakeKeyFirst; k < kTakeKeyFirst + kTakeKeys; ++k) {
    bool used = false;
    for (const auto& t : takes) used = used || t.key == k;
    if (!used) return k;
  }
  return -1;
}

// ================================================================== model
bool Model::operator==(const Model& o) const {
  return srcFrames == o.srcFrames && srcRate == o.srcRate && analysed == o.analysed && grid.beats == o.grid.beats && grid.bpm == o.grid.bpm &&
         grid.beatsPerBar == o.grid.beatsPerBar && grid.bar1 == o.grid.bar1 && cue == o.cue && hot == o.hot && loopIn == o.loopIn &&
         loopOut == o.loopOut && asked == o.asked && armed == o.armed && map == o.map && takes == o.takes;
}

std::string pathOf(const std::string& data) {
  const size_t nl = data.find('\n');
  return nl == std::string::npos ? data : data.substr(0, nl);
}

std::string modelToText(const Model& m) {
  std::ostringstream o;
  char b[256];
  o << "platter 1\n";
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
  std::snprintf(b, sizeof b, "cue %.4f\n", m.cue);
  o << b;
  o << "hot";
  for (double h : m.hot) {
    std::snprintf(b, sizeof b, " %.4f", h);
    o << b;
  }
  o << "\n";
  std::snprintf(b, sizeof b, "loop %.4f %.4f\nflags %d %d\n", m.loopIn, m.loopOut, m.asked ? 1 : 0, m.armed ? 1 : 0);
  o << b;
  mappingLines(o, m.map);
  for (const auto& t : m.takes) {
    std::snprintf(b, sizeof b, "take %d %.3f %d %.6f %d %lld %lld %.6f %.9f %.6f %zu ", t.key, t.sampleRate, t.step, t.tempo, t.ppq,
                  static_cast<long long>(t.startTick), static_cast<long long>(t.lengthTicks), t.startPos, t.startRate,
                  static_cast<double>(t.startGain), t.pts.size());
    o << b << encodePoints(t.pts) << "\n";
    o << "tname " << t.key << " " << t.name << "\n";
  }
  return o.str();
}

bool modelFromText(const std::string& text, Model& m) {
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("platter ", 0) != 0) return false;
  m = Model{};
  bool any = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string key;
    ls >> key;
    if (key == "src") {
      long long f = 0;
      int an = 0;
      ls >> f >> m.srcRate >> an;
      m.srcFrames = std::max(0LL, f);
      m.analysed = an != 0;
      m.grid.frames = m.srcFrames;
      m.grid.sampleRate = m.srcRate;
      any = true;
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
    } else if (key == "cue") {
      ls >> m.cue;
    } else if (key == "hot") {
      for (auto& h : m.hot)
        if (!(ls >> h)) h = -1;
    } else if (key == "loop") {
      ls >> m.loopIn >> m.loopOut;
      if (!(m.loopOut > m.loopIn)) m.loopIn = m.loopOut = -1;
    } else if (key == "flags") {
      int a = 0, r = 1;
      ls >> a >> r;
      m.asked = a != 0;
      m.armed = r != 0;
    } else if (key == "take") {
      Take t;
      long long st = 0, len = 0;
      float sg = 1;
      size_t n = 0;
      std::string data;
      ls >> t.key >> t.sampleRate >> t.step >> t.tempo >> t.ppq >> st >> len >> t.startPos >> t.startRate >> sg >> n >> data;
      t.startTick = st;
      t.lengthTicks = std::max(0LL, len);
      t.startGain = sg;
      if (t.key < kTakeKeyFirst || t.key >= kTakeKeyFirst + kTakeKeys || !(t.sampleRate > 1000) || t.step < 1 || t.step > 4096 ||
          !(t.tempo > 1))
        continue;
      if (!decodePoints(data, t.pts) || t.pts.size() != n) continue;
      bool dup = false;
      for (const auto& o : m.takes) dup = dup || o.key == t.key;
      if (!dup) m.takes.push_back(std::move(t));
    } else if (key == "tname") {
      int k = 0;
      ls >> k;
      std::string rest;
      std::getline(ls, rest);
      if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
      for (auto& t : m.takes)
        if (t.key == k) t.name = rest;
    } else {
      mappingLine(key, ls, line, m.map);
    }
  }
  return any;
}

bool parseData(const std::string& data, std::string& path, Model& m) {
  const size_t nl = data.find('\n');
  path = nl == std::string::npos ? data : data.substr(0, nl);
  if (nl == std::string::npos) return false;
  return modelFromText(data.substr(nl + 1), m);
}

std::string formatData(const std::string& path, const Model& m) { return path + "\n" + modelToText(m); }

Model defaultModel(const Sample& s, const seltmann::Analysis& a, bool analysed) {
  Model m;
  m.srcFrames = static_cast<int64_t>(s.frames());
  m.srcRate = s.sampleRate;
  m.analysed = analysed;
  m.grid.sampleRate = s.sampleRate;
  m.grid.frames = m.srcFrames;
  m.grid.bpm = a.bpm > 1 ? a.bpm : 120;
  m.grid.beats = a.beats;
  m.grid.beatsPerBar = std::clamp(a.beatsPerBar, 1, 16);
  m.grid.bar1 = std::clamp(a.bar1, 0, std::max(0, static_cast<int>(a.beats.size()) - 1));
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

Model carryOver(const Model& keep, const Model& fresh) {
  Model m = fresh;
  m.asked = keep.asked;
  m.armed = keep.armed;
  m.map = keep.map;
  return m;  // (takes belong to the file they were played on)
}

double beatAt(const Model& m, double frame) { return seltmann::beatAt(m.grid, frame); }
double frameAt(const Model& m, double beat) { return seltmann::frameAt(m.grid, beat); }
double nearestBeatFrame(const Model& m, double frame) { return frameAt(m, std::round(beatAt(m, frame))); }
double bpmOf(const Model& m) { return m.grid.bpm > 1 ? m.grid.bpm : 120.0; }

bool TakeBuilder::feed(const CaptureEvent& e) {
  switch (e.kind) {
    case CaptureEvent::Start:
      take = Take{};
      take.sampleRate = e.sampleRate;
      take.step = kStep;
      take.tempo = e.tempo;
      take.ppq = e.ppq;
      take.startTick = static_cast<Tick>(std::llround(e.tick));
      take.startPos = e.pt.pos;
      take.startRate = e.startRate;
      take.startGain = e.gain;
      take.pts.reserve(4096);
      active = true;
      lost = false;
      return false;
    case CaptureEvent::Point:
      if (active) take.pts.push_back(e.pt);
      return false;
    case CaptureEvent::Lost:  // an end after dropped points: no take
      lost = true;
      active = false;
      return false;
    case CaptureEvent::End:
      if (!active) return false;
      active = false;
      take.lengthTicks = std::max<Tick>(1, static_cast<Tick>(std::llround(e.tick)) - take.startTick);
      return !take.pts.empty() && !lost;
  }
  return false;
}

}  // namespace gl::packs::platter
