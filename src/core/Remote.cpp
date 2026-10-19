#include "Remote.h"
#include <algorithm>
#include <cmath>

namespace gl {

namespace {
double clamp01(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
}  // namespace

bool remoteMatches(const ControllerLink& l, const MidiMessage& m) {
  if (l.port >= 0 && m.port >= 0 && l.port != m.port) return false;
  if (l.midiChannel >= 0 && l.midiChannel != m.channel()) return false;
  const int k = m.kind();
  switch (l.source) {
    case RemoteSource::CC: return k == 0xB0 && m.d1 == l.cc;
    case RemoteSource::Note: return (k == 0x90 || k == 0x80) && m.d1 == l.cc;
    case RemoteSource::PitchBend: return k == 0xE0;
    case RemoteSource::ChannelPressure: return k == 0xD0;
    case RemoteSource::KeyPressure: return k == 0xA0 && m.d1 == l.cc;
    case RemoteSource::Program: return k == 0xC0 && m.d1 == l.cc;
    default: return false;
  }
}

double remoteInput(const ControllerLink& l, const MidiMessage& m) {
  switch (l.source) {
    case RemoteSource::CC: return m.d2 / 127.0;
    case RemoteSource::Note: return m.noteOn() ? m.d2 / 127.0 : 0.0;
    case RemoteSource::PitchBend: return ((m.d2 & 0x7F) << 7 | (m.d1 & 0x7F)) / 16383.0;
    case RemoteSource::ChannelPressure: return m.d1 / 127.0;
    case RemoteSource::KeyPressure: return m.d2 / 127.0;
    case RemoteSource::Program: return 1.0;
    default: return 0.0;
  }
}

bool remoteButtonDown(const ControllerLink& l, const MidiMessage& m) {
  switch (l.source) {
    case RemoteSource::Note: return m.noteOn();
    case RemoteSource::CC: return m.d2 >= 64;
    case RemoteSource::Program: return true;
    default: return remoteInput(l, m) >= 0.5;
  }
}

int decodeRelative(RemoteEncoder e, int v) {
  v &= 0x7F;
  switch (e) {
    case RemoteEncoder::TwosComplement: return v < 64 ? v : v - 128;
    case RemoteEncoder::BinaryOffset: return v - 64;
    case RemoteEncoder::SignBit: return (v & 0x40) ? -(v & 0x3F) : (v & 0x3F);
    default: return 0;
  }
}

int encodeRelative(RemoteEncoder e, int d) {
  d = std::clamp(d, -63, 63);
  switch (e) {
    case RemoteEncoder::TwosComplement: return d >= 0 ? d : 128 + d;
    case RemoteEncoder::BinaryOffset: return 64 + d;
    case RemoteEncoder::SignBit: return d >= 0 ? d : (0x40 | -d);
    default: return 0;
  }
}

double remoteCurve(const ControllerLink& l, double u, const Expr* formula) {
  u = clamp01(u);
  if (l.invert) u = 1.0 - u;
  if (l.curve == ModCurve::Formula) {
    if (!formula || formula->empty()) return u;
    double v = formula->eval(&u);
    return std::isfinite(v) ? clamp01(v) : 0.0;
  }
  return applyModCurve(l.curve, u);
}

bool compileRemoteFormula(const std::string& text, Expr& out, std::string* err) {
  return out.compile(text.empty() ? "input" : text, {"input"}, err);
}

const std::vector<RemotePreset>& remotePresets() {
  static const std::vector<RemotePreset> p = {
      {"Absolute", "Linear", ModCurve::Linear, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Absolute", "Inverted", ModCurve::Inverted, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Absolute", "Fast rise", ModCurve::Log, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Absolute", "Slow rise", ModCurve::InvLog, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Absolute", "On / off at half way", ModCurve::Switch, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Absolute", "8 steps", ModCurve::Steps, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Absolute", "Lower half only", ModCurve::FirstHalf, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Absolute", "Upper half only", ModCurve::LastHalf, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Absolute", "Centre dead zone", ModCurve::DeadZone, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Absolute", "Centre at value 64", ModCurve::Formula,
       "input*127 <= 64 ? input*127/128 : 0.5 + (input*127 - 64)/126", RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Absolute", "Semitone steps (24)", ModCurve::Formula, "round(input*24)/24", RemoteEncoder::Absolute, 1,
       RemoteButton::Value},
      {"Absolute", "Squared", ModCurve::Formula, "input^2", RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Absolute", "Smooth S", ModCurve::Formula, "smoothstep(0, 1, input)", RemoteEncoder::Absolute, 1,
       RemoteButton::Value},
      {"Relative", "Encoder: two's complement", ModCurve::Linear, nullptr, RemoteEncoder::TwosComplement, 1,
       RemoteButton::Value},
      {"Relative", "Encoder: offset 64", ModCurve::Linear, nullptr, RemoteEncoder::BinaryOffset, 1, RemoteButton::Value},
      {"Relative", "Encoder: sign bit", ModCurve::Linear, nullptr, RemoteEncoder::SignBit, 1, RemoteButton::Value},
      {"Relative", "Encoder, fine (1/4)", ModCurve::Linear, nullptr, RemoteEncoder::TwosComplement, 0.25f,
       RemoteButton::Value},
      {"Relative", "Encoder, coarse (x4)", ModCurve::Linear, nullptr, RemoteEncoder::TwosComplement, 4,
       RemoteButton::Value},
      {"Buttons", "Velocity sets value", ModCurve::Linear, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Value},
      {"Buttons", "Hold (momentary)", ModCurve::Linear, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Momentary},
      {"Buttons", "Toggle on press", ModCurve::Linear, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Toggle},
      {"Buttons", "Step up on press", ModCurve::Linear, nullptr, RemoteEncoder::Absolute, 1, RemoteButton::Increment},
  };
  return p;
}

void applyRemotePreset(ControllerLink& l, const RemotePreset& p) {
  l.curve = p.curve;
  l.formula = p.formula ? p.formula : "";
  l.encoder = p.encoder;
  l.sensitivity = p.sensitivity;
  l.button = p.button;
  l.invert = false;
}

const char* remoteSourceName(RemoteSource s) {
  static const char* n[] = {"Controller (CC)", "Note / pad", "Pitch bend", "Channel pressure", "Key pressure",
                            "Program change"};
  int i = static_cast<int>(s);
  return i >= 0 && i < static_cast<int>(RemoteSource::Count) ? n[i] : "?";
}
const char* remoteTakeoverName(RemoteTakeover t) {
  static const char* n[] = {"Jump", "Pick up", "Scale"};
  int i = static_cast<int>(t);
  return i >= 0 && i < static_cast<int>(RemoteTakeover::Count) ? n[i] : "?";
}
const char* remoteEncoderName(RemoteEncoder e) {
  static const char* n[] = {"Absolute", "Relative: two's complement", "Relative: offset 64", "Relative: sign bit"};
  int i = static_cast<int>(e);
  return i >= 0 && i < static_cast<int>(RemoteEncoder::Count) ? n[i] : "?";
}
const char* remoteButtonName(RemoteButton b) {
  static const char* n[] = {"Value", "Hold", "Toggle", "Step up"};
  int i = static_cast<int>(b);
  return i >= 0 && i < static_cast<int>(RemoteButton::Count) ? n[i] : "?";
}
const char* remoteActionName(RemoteAction a) {
  static const char* n[] = {"(parameter)", "Play / pause", "Stop", "Record", "Pattern / song mode", "Metronome",
                            "Count-in", "Step record", "Loop record", "Next pattern", "Previous pattern", "Undo",
                            "Redo", "Tap tempo"};
  int i = static_cast<int>(a);
  return i >= 0 && i < static_cast<int>(RemoteAction::Count) ? n[i] : "?";
}

std::string midiNoteName(int note) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  note = std::clamp(note, 0, 127);
  return std::string(names[note % 12]) + std::to_string(note / 12);
}

std::string remoteSourceLabel(const ControllerLink& l) {
  std::string s;
  switch (l.source) {
    case RemoteSource::CC: s = "CC " + std::to_string(l.cc); break;
    case RemoteSource::Note: s = "Note " + midiNoteName(l.cc); break;
    case RemoteSource::PitchBend: s = "Pitch bend"; break;
    case RemoteSource::ChannelPressure: s = "Ch. pressure"; break;
    case RemoteSource::KeyPressure: s = "Key pressure " + midiNoteName(l.cc); break;
    case RemoteSource::Program: s = "Program " + std::to_string(l.cc + 1); break;
    default: s = "?";
  }
  s += l.midiChannel >= 0 ? " · ch " + std::to_string(l.midiChannel + 1) : " · any ch";
  if (l.port >= 0) s += " · port " + std::to_string(l.port);
  return s;
}

// ---------------------------------------------------------------- mapper ----

uint64_t RemoteMapper::key(const ControllerLink& l) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (i * 8)) & 0xFF;
      h *= 1099511628211ull;
    }
  };
  mix(l.action != RemoteAction::None ? 0 : l.owner);
  mix(l.action != RemoteAction::None ? 0 : l.index);
  mix(static_cast<uint64_t>(l.action));
  mix(static_cast<uint64_t>(l.source));
  mix(static_cast<uint64_t>(l.cc));
  mix(static_cast<uint64_t>(l.midiChannel + 1));
  mix(static_cast<uint64_t>(l.port + 1));
  return h;
}

const Expr* RemoteMapper::formula(const std::string& text) {
  auto it = exprs_.find(text);
  if (it == exprs_.end()) {
    if (exprs_.size() > 256) exprs_.clear();
    Expr e;
    compileRemoteFormula(text, e);
    it = exprs_.emplace(text, std::move(e)).first;
  }
  return &it->second;
}

bool RemoteMapper::trigger(const ControllerLink& l, const MidiMessage& m) {
  State& s = st_[key(l)];
  bool down = remoteButtonDown(l, m);
  bool rising = down && (!s.down || (l.source == RemoteSource::Note && m.noteOn()) || l.source == RemoteSource::Program);
  s.down = down;
  return rising;
}

bool RemoteMapper::waiting(const ControllerLink& l) const {
  if (l.takeover != RemoteTakeover::Pickup) return false;
  auto it = st_.find(key(l));
  return it != st_.end() && !it->second.picked && it->second.lastIn >= 0;
}

void RemoteMapper::forget(const ControllerLink& l) { st_.erase(key(l)); }

bool RemoteMapper::process(const ControllerLink& l, const MidiMessage& m, double cur, double* out, double step) {
  State& s = st_[key(l)];
  const double lo = std::min(l.min, l.max), hi = std::max(l.min, l.max);
  // Someone else moved the parameter since this link last wrote it: pick it up again.
  if (s.lastOut >= 0 && std::fabs(cur - s.lastOut) > 1e-4) s.picked = false;
  auto write = [&](double v) {
    v = std::clamp(v, 0.0, 1.0);
    s.lastOut = v;
    *out = v;
    return true;
  };
  const Expr* f = l.curve == ModCurve::Formula ? formula(l.formula) : nullptr;

  // Relative encoders: each detent moves 1/128 of the range (× sensitivity).
  if (l.encoder != RemoteEncoder::Absolute && l.source == RemoteSource::CC) {
    int d = decodeRelative(l.encoder, m.d2);
    if (d == 0) return false;
    double delta = d * static_cast<double>(l.sensitivity) / 128.0 * (hi - lo);
    if (l.invert != (l.max < l.min)) delta = -delta;
    s.picked = true;
    return write(std::clamp(cur + delta, lo, hi));
  }

  // Notes, program changes and CC buttons.
  const bool buttonish = l.source == RemoteSource::Note || l.source == RemoteSource::Program ||
                         (l.source == RemoteSource::CC && l.button != RemoteButton::Value);
  if (buttonish) {
    bool down = remoteButtonDown(l, m);
    bool rising = down && (!s.down || (l.source == RemoteSource::Note && m.noteOn()) || l.source == RemoteSource::Program);
    s.down = down;
    switch (l.button) {
      case RemoteButton::Value:
        if (!down) return false;  // note-offs leave the value
        return write(remoteRange(l, remoteCurve(l, remoteInput(l, m), f)));
      case RemoteButton::Momentary:
        if (down && !rising) return false;
        return write(remoteRange(l, remoteCurve(l, down ? 1.0 : 0.0, f)));
      case RemoteButton::Toggle:
        if (!rising) return false;
        s.toggled = !s.toggled;
        return write(remoteRange(l, remoteCurve(l, s.toggled ? 1.0 : 0.0, f)));
      case RemoteButton::Increment: {
        if (!rising) return false;
        double st = step > 0 ? step : 0.125;
        double v = cur + st;
        if (v > hi + 1e-9) v = lo;
        return write(std::clamp(v, lo, hi));
      }
      default: return false;
    }
  }

  // Absolute controls.
  const double n = remoteRange(l, remoteCurve(l, remoteInput(l, m), f));
  switch (l.takeover) {
    case RemoteTakeover::Pickup: {
      if (!s.picked) {
        bool near = std::fabs(n - cur) <= kPickupTolerance;
        bool crossed = s.lastIn >= 0 && (s.lastIn - cur) * (n - cur) <= 0;
        s.lastIn = n;
        if (!near && !crossed) return false;
        s.picked = true;
      }
      s.lastIn = n;
      return write(n);
    }
    case RemoteTakeover::Scale: {
      const double c = std::clamp(cur, lo, hi);
      if (s.lastIn < 0 || std::fabs(n - c) <= kPickupTolerance) {
        bool first = s.lastIn < 0;
        s.lastIn = n;
        if (first && std::fabs(n - c) > kPickupTolerance) return false;  // no direction yet
        return write(n);
      }
      const double prev = s.lastIn;
      s.lastIn = n;
      double v;
      if (n > prev)
        v = hi - prev > 1e-9 ? c + (hi - c) * (n - prev) / (hi - prev) : hi;
      else if (n < prev)
        v = prev - lo > 1e-9 ? c - (c - lo) * (prev - n) / (prev - lo) : lo;
      else
        return false;
      return write(v);
    }
    default:
      s.lastIn = n;
      s.picked = true;
      return write(n);
  }
}

// ---------------------------------------------------------------- velocity ----

const char* velocityCurveName(VelocityCurve c) {
  static const char* n[] = {"Linear", "Soft (lighter touch)", "Hard (heavier touch)", "Fixed"};
  int i = static_cast<int>(c);
  return i >= 0 && i < static_cast<int>(VelocityCurve::Count) ? n[i] : "?";
}

int applyVelocityCurve(VelocityCurve c, int v, int fixed) {
  if (v <= 0) return 0;
  double x = std::min(127, v) / 127.0;
  switch (c) {
    case VelocityCurve::Soft: x = std::pow(x, 0.6); break;
    case VelocityCurve::Hard: x = std::pow(x, 1.7); break;
    case VelocityCurve::Fixed: return std::clamp(fixed, 1, 127);
    default: break;
  }
  return std::clamp(static_cast<int>(std::lround(x * 127.0)), 1, 127);
}

}  // namespace gl
