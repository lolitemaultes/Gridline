// Gridline — K-214: Mackie Control (MCU) / HUI surfaces (see MackieControl.h).
#include "MackieControl.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "Params.h"

namespace gl::mackie {

namespace {

double normGet(const Song& s, ParamId pid) {
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  return d && v ? d->toNormalized(*v) : 0.0;
}
void normSet(Document& doc, ParamId pid, double v) {
  auto d = describeParam(doc.song(), pid);
  if (!d || !std::isfinite(v)) return;
  doc.setParam(pid, d->fromNormalized(std::clamp(v, 0.0, 1.0)), true);
}

}  // namespace

Surface::Surface(script::ControllerHost& host, Options o, Out out) : host_(host), opt_(o), out_(std::move(out)) {
  opt_.extenders = std::clamp(opt_.extenders, 0, 3);
  shown_.assign(static_cast<size_t>(strips()), Shown{});
  touched_.assign(static_cast<size_t>(strips()), false);
}

std::string Surface::stripName(const std::string& name, size_t width) {
  std::string o;
  for (unsigned char c : name)
    if (c >= 0x20 && c < 0x7F) o += static_cast<char>(c);  // the displays show ASCII only
  if (o.size() > width) {  // drop spaces, then lower-case vowels after the first letter, then cut
    std::string t;
    for (char c : o)
      if (c != ' ') t += c;
    o = t;
    for (size_t i = o.size(); o.size() > width && i-- > 1;)
      if (std::string("aeiou").find(o[i]) != std::string::npos) o.erase(i, 1);
    o.resize(std::min(o.size(), width));
  }
  o.resize(width, ' ');
  return o;
}

int Surface::mixerOf(int strip) const {
  const auto& mx = host_.doc.song().mixer;
  if (opt_.singleStrip) {
    const int sel = host_.selectedMixer();
    return strip == 0 && sel >= 0 && static_cast<size_t>(sel) < mx.size() ? sel : -1;
  }
  const int m = 1 + bank_ + strip;
  return strip >= 0 && strip < strips() && static_cast<size_t>(m) < mx.size() ? m : -1;
}

double Surface::faderNorm(int mixer) const {
  const auto& mx = host_.doc.song().mixer;
  return mixer >= 0 && static_cast<size_t>(mixer) < mx.size() ? normGet(host_.doc.song(), mixerParam(mx[static_cast<size_t>(mixer)].id, MixerParam::Volume)) : 0.0;
}
double Surface::panNorm(int mixer) const {
  const auto& mx = host_.doc.song().mixer;
  return mixer >= 0 && static_cast<size_t>(mixer) < mx.size() ? normGet(host_.doc.song(), mixerParam(mx[static_cast<size_t>(mixer)].id, MixerParam::Pan)) : 0.5;
}
std::string Surface::levelText(int mixer) const {
  const auto& mx = host_.doc.song().mixer;
  if (mixer < 0 || static_cast<size_t>(mixer) >= mx.size()) return "";
  const double v = mx[static_cast<size_t>(mixer)].volume;
  if (v <= 1e-5) return "-inf";
  char b[16];
  std::snprintf(b, sizeof b, "%+.1f", 20.0 * std::log10(v / 0.8));  // 0.8 = 0 dB on Gridline's faders
  return b;
}

void Surface::setFader(int strip, double norm) {
  const int m = mixerOf(strip);
  if (m < 0) return;
  normSet(host_.doc, mixerParam(host_.doc.song().mixer[static_cast<size_t>(m)].id, MixerParam::Volume), norm);
  shown_[static_cast<size_t>(strip)].fader = -1;  // echo the stored value when the fader is let go
}

void Surface::turn(int strip, int steps) {
  const int m = mixerOf(strip);
  if (m < 0 || steps == 0) return;
  normSet(host_.doc, mixerParam(host_.doc.song().mixer[static_cast<size_t>(m)].id, MixerParam::Pan), panNorm(m) + steps * 0.01);
}

void Surface::button(int strip, int what) {
  const int m = mixerOf(strip);
  if (m < 0) return;
  const MixerTrack& t = host_.doc.song().mixer[static_cast<size_t>(m)];
  switch (what) {
    case 0: host_.setArmed(m, !host_.armed(m)); break;
    case 1: {
      const bool on = !t.solo;
      host_.doc.editMixerTrack(t.id, on ? "solo track" : "unsolo track", [on](MixerTrack& x) { x.solo = on; });
      break;
    }
    case 2: host_.doc.setParam(mixerParam(t.id, MixerParam::Mute), t.muted ? 0.0 : 1.0, false); break;
    case 3: host_.selectMixer(m); break;
    case 4: normSet(host_.doc, mixerParam(t.id, MixerParam::Pan), 0.5); break;
    default: break;
  }
}

void Surface::moveBank(int by) {
  const int inserts = std::max(0, static_cast<int>(host_.doc.song().mixer.size()) - 1);
  if (opt_.singleStrip) {  // one strip: step the selection
    host_.selectMixer(std::clamp(host_.selectedMixer() + (by > 0 ? 1 : -1), 0, inserts));
    return;
  }
  const int nb = std::clamp(bank_ + by, 0, std::max(0, inserts - strips()));
  if (nb == bank_) return;
  bank_ = nb;
  for (auto& s : shown_) s = Shown{};
  host_.hint("Mixer inserts " + std::to_string(bank_ + 1) + " to " + std::to_string(std::min(inserts, bank_ + strips())));
}

void Surface::transport(int key, bool down) {
  if (!down) return;
  const int ppq = host_.doc.song().ppq;
  switch (key) {
    case 0: host_.action(RemoteAction::PlayPause); break;
    case 1: host_.action(RemoteAction::Stop); break;
    case 2: host_.action(RemoteAction::Record); break;
    case 3: host_.setPosition(std::max<Tick>(0, host_.position() - static_cast<Tick>(4 * ppq))); break;  // a bar back
    case 4: host_.setPosition(host_.position() + static_cast<Tick>(4 * ppq)); break;
    case 5: host_.action(RemoteAction::Metronome); break;
    case 6: host_.action(RemoteAction::Undo); break;
    case 7: host_.action(RemoteAction::SongMode); break;  // cycle: pattern / song loop
    default: break;
  }
}

bool Surface::onMidi(int unit, const MidiMessage& m) {
  return opt_.protocol == Protocol::Hui ? onHui(m) : onMcu(unit, m);
}

bool Surface::onMcu(int unit, const MidiMessage& m) {
  unit = std::clamp(unit, 0, units() - 1);
  const int base = 8 * unit;
  const int kind = m.kind();
  if (kind == 0xE0) {  // fader: 14-bit pitch bend, channel = strip (8 = master on the main unit)
    const double v = ((m.d2 << 7) | m.d1) / 16383.0;
    const int ch = m.channel();
    if (ch == 8 && unit == 0) {
      const auto& mx = host_.doc.song().mixer;
      if (!mx.empty()) normSet(host_.doc, mixerParam(mx[0].id, MixerParam::Volume), v);
      masterFader_ = -1;
      return true;
    }
    if (ch < 8 && base + ch < strips()) setFader(base + ch, v);
    return true;
  }
  if (kind == 0xB0) {
    if (m.d1 >= CVPot && m.d1 < CVPot + 8) {  // encoder: bit 6 = counter-clockwise, low bits = steps
      const int steps = m.d2 & 0x3F;
      turn(base + m.d1 - CVPot, (m.d2 & 0x40) ? -steps : steps);
      return true;
    }
    if (m.d1 == CJog) {
      const int steps = m.d2 & 0x3F;
      const Tick beat = host_.doc.song().ppq;
      const Tick to = host_.position() + ((m.d2 & 0x40) ? -steps : steps) * beat;
      host_.setPosition(std::max<Tick>(0, to));
      return true;
    }
    return false;
  }
  if (kind != 0x90 && kind != 0x80) return false;
  const bool down = m.noteOn();
  const int n = m.d1;
  if (n >= NFaderTouch && n < NFaderTouch + 8) {
    if (base + n - NFaderTouch < strips()) {
      touched_[static_cast<size_t>(base + n - NFaderTouch)] = down;
      if (!down) shown_[static_cast<size_t>(base + n - NFaderTouch)].fader = -1;
    }
    return true;
  }
  if (n == NMasterTouch) {
    masterTouched_ = down;
    if (!down) masterFader_ = -1;
    return true;
  }
  if (n < 0x28) {  // strip buttons: arm, solo, mute, select, v-pot push (8 each)
    if (down && base + (n & 7) < strips()) button(base + (n & 7), n >> 3);
    return true;
  }
  if (!down) return n >= NBankLeft;
  switch (n) {
    case NBankLeft: moveBank(-strips()); return true;
    case NBankRight: moveBank(strips()); return true;
    case NChannelLeft: moveBank(-1); return true;
    case NChannelRight: moveBank(1); return true;
    case NPlay: transport(0, true); return true;
    case NStop: transport(1, true); return true;
    case NRecord: transport(2, true); return true;
    case NRewind: transport(3, true); return true;
    case NForward: transport(4, true); return true;
    case NClick: transport(5, true); return true;
    case NUndo: transport(6, true); return true;
    case NCycle: transport(7, true); return true;
    default: return false;
  }
}

// HUI: switches arrive as a zone (CC 0x0F) then a port (CC 0x2F, bit 6 = pressed); faders as
// CC 0x00..0x07 (high 7 bits) + 0x20..0x27 (low bits); encoders CC 0x40..0x47 (bit 6 = clockwise);
// the ping (note 0 velocity 0 from the host) is answered with velocity 0x7F.
bool Surface::onHui(const MidiMessage& m) {
  const int kind = m.kind();
  if (kind == 0x90 && m.d1 == 0 && m.d2 == 0x7F) return true;  // the surface answering our ping
  if (kind != 0xB0) return false;
  const int cc = m.d1, v = m.d2;
  if (cc < 0x08) {
    huiFaderMsb_[static_cast<size_t>(cc)] = v;
    return true;
  }
  if (cc >= 0x20 && cc < 0x28) {
    const int s = cc - 0x20;
    if (s < strips()) setFader(s, ((huiFaderMsb_[static_cast<size_t>(s)] << 7) | v) / 16383.0);
    return true;
  }
  if (cc >= 0x40 && cc < 0x48) {
    const int steps = v & 0x3F;
    turn(cc - 0x40, (v & 0x40) ? steps : -steps);
    return true;
  }
  if (cc == 0x0F) {
    huiZone_ = v;
    return true;
  }
  if (cc == 0x2F && huiZone_ >= 0) {
    const bool down = (v & 0x40) != 0;
    const int port = v & 0x0F;
    if (huiZone_ < 8) {  // a strip: 0 fader touch, 1 select, 2 mute, 3 solo, 7 rec arm
      const int s = huiZone_;
      if (s >= strips()) return true;
      if (port == 0) {
        touched_[static_cast<size_t>(s)] = down;
        if (!down) shown_[static_cast<size_t>(s)].fader = -1;
      } else if (down) {
        if (port == 1) button(s, 3);
        if (port == 2) button(s, 2);
        if (port == 3) button(s, 1);
        if (port == 7) button(s, 0);
      }
      return true;
    }
    if (huiZone_ == 0x0A && down) {  // channel / bank shift
      if (port == 0) moveBank(-1);
      if (port == 1) moveBank(-strips());
      if (port == 2) moveBank(1);
      if (port == 3) moveBank(strips());
      return true;
    }
    if (huiZone_ == 0x0E) {  // transport: 1 rewind, 2 forward, 3 stop, 4 play, 5 record
      static const int key[6] = {-1, 3, 4, 1, 0, 2};
      if (port < 6 && key[port] >= 0) transport(key[port], down);
      return true;
    }
    return true;
  }
  return false;
}

void Surface::sendText(int unit, int offset, const std::string& text) {
  std::vector<uint8_t> b = {0xF0, 0x00, 0x00, 0x66, static_cast<uint8_t>(unit == 0 ? 0x14 : 0x15), 0x12, static_cast<uint8_t>(offset)};
  for (char c : text) b.push_back(static_cast<uint8_t>(c) & 0x7F);
  b.push_back(0xF7);
  out_(unit, b);
}

void Surface::sendTimecode() {
  // Bars (3), beats (2), sub-beats (2), ticks (3): ten digits, sent right to left.
  const Song& s = host_.doc.song();
  const Tick ppq = std::max<Tick>(1, s.ppq);
  const Tick pos = std::max<Tick>(0, host_.position());
  const long long bar = pos / (4 * ppq) + 1, beat = (pos / ppq) % 4 + 1, sub = (pos * 4 / ppq) % 4 + 1, tick = pos % (ppq / 4 > 0 ? ppq / 4 : 1);
  char t[16];
  std::snprintf(t, sizeof t, "%3lld%2lld%2lld%3lld", bar % 1000, beat, sub, tick % 1000);
  for (int i = 0; i < 10; ++i) {
    const char c = t[9 - i];
    if (timecodeSent_ && timecode_[static_cast<size_t>(i)] == c) continue;
    timecode_[static_cast<size_t>(i)] = c;
    // 7-segment code: digits as their ASCII code, a space as 0x20; a dot after bar / beat / sub-beat.
    uint8_t code = static_cast<uint8_t>(c == ' ' ? 0x20 : (c >= '0' && c <= '9' ? 0x30 + (c - '0') : 0x20));
    if (i == 3 || i == 5 || i == 7) code |= 0x40;
    send(0, {0xB0, static_cast<uint8_t>(CTimecode + i), code});
  }
  timecodeSent_ = true;
}

void Surface::refresh() {
  for (auto& s : shown_) s = Shown{};
  masterFader_ = transportLeds_ = -1;
  timecodeSent_ = false;
}

void Surface::idle(double now, const std::vector<float>& levels) {
  const auto& mx = host_.doc.song().mixer;
  const bool hui = opt_.protocol == Protocol::Hui;
  if (hui && now - lastPing_ >= 1.0) {  // HUI surfaces go offline without the host's ping
    lastPing_ = now;
    send(0, {0x90, 0x00, 0x00});
  }
  for (int s = 0; s < strips(); ++s) {
    Shown& sh = shown_[static_cast<size_t>(s)];
    const int m = mixerOf(s), unit = s / 8, ch = s % 8;
    // Fader (not while a hand is on it).
    const int f = m >= 0 ? static_cast<int>(std::lround(faderNorm(m) * 16383.0)) : 0;
    if (!touched_[static_cast<size_t>(s)] && f != sh.fader) {
      sh.fader = f;
      if (hui) {
        send(0, {0xB0, static_cast<uint8_t>(ch), static_cast<uint8_t>(f >> 7)});
        send(0, {0xB0, static_cast<uint8_t>(0x20 + ch), static_cast<uint8_t>(f & 0x7F)});
      } else {
        send(unit, {static_cast<uint8_t>(0xE0 | ch), static_cast<uint8_t>(f & 0x7F), static_cast<uint8_t>(f >> 7)});
      }
    }
    // Pan ring: boost/cut mode, 11 positions, 6 = centre.
    const int ring = m >= 0 ? 1 + static_cast<int>(std::lround(panNorm(m) * 10.0)) : 0;
    if (ring != sh.ring) {
      sh.ring = ring;
      if (hui) send(0, {0xB0, static_cast<uint8_t>(0x10 + ch), static_cast<uint8_t>(ring == 0 ? 0 : ring | 0x40)});
      else send(unit, {0xB0, static_cast<uint8_t>(CRing + ch), static_cast<uint8_t>(ring == 0 ? 0 : 0x10 | ring)});
    }
    // Strip LEDs: rec arm, solo, mute, select.
    int leds = 0;
    if (m >= 0) {
      const MixerTrack& t = mx[static_cast<size_t>(m)];
      leds = (host_.armed(m) ? 1 : 0) | (t.solo ? 2 : 0) | (t.muted ? 4 : 0) | (host_.selectedMixer() == m ? 8 : 0);
    }
    if (leds != sh.leds) {
      for (int b = 0; b < 4; ++b) {
        if (sh.leds >= 0 && ((leds ^ sh.leds) & (1 << b)) == 0) continue;
        const bool on = (leds >> b) & 1;
        if (hui) {
          static const int port[4] = {7, 3, 2, 1};
          send(0, {0xB0, 0x0C, static_cast<uint8_t>(ch)});
          send(0, {0xB0, 0x2C, static_cast<uint8_t>(port[b] | (on ? 0x40 : 0))});
        } else {
          static const uint8_t base[4] = {NRecArm, NSolo, NMute, NSelect};
          send(unit, {0x90, static_cast<uint8_t>(base[b] + ch), static_cast<uint8_t>(on ? 0x7F : 0)});
        }
      }
      sh.leds = leds;
    }
    // Displays: the name on top, the level below (MCU: 7 characters a strip; HUI: 4, names only).
    const std::string top = m >= 0 ? stripName(mx[static_cast<size_t>(m)].name.empty() ? (m == 0 ? std::string("Master") : "Ins " + std::to_string(m)) : mx[static_cast<size_t>(m)].name, hui ? 4 : 6)
                                   : std::string(hui ? 4 : 6, ' ');
    const std::string bottom = hui ? std::string() : stripName(levelText(m), 6);
    if (top != sh.top) {
      sh.top = top;
      if (hui) {
        std::vector<uint8_t> b = {0xF0, 0x00, 0x00, 0x66, 0x05, 0x00, 0x10, static_cast<uint8_t>(ch)};
        for (char c : top) b.push_back(static_cast<uint8_t>(c));
        b.push_back(0xF7);
        out_(0, b);
      } else {
        sendText(unit, ch * 7, top + " ");
      }
    }
    if (!hui && bottom != sh.bottom) {
      sh.bottom = bottom;
      sendText(unit, 56 + ch * 7, bottom + " ");
    }
    // Meters: MCU channel pressure (strip << 4 | 0..12); HUI poly pressure per side.
    if (m >= 0 && static_cast<size_t>(m) < levels.size()) {
      const double db = levels[static_cast<size_t>(m)] > 1e-6f ? 20.0 * std::log10(levels[static_cast<size_t>(m)]) : -100.0;
      const int lv = std::clamp(static_cast<int>(std::lround((db + 60.0) / 5.0)), 0, 12);
      if (lv > 0) {
        if (hui) {
          send(0, {0xA0, static_cast<uint8_t>(ch), static_cast<uint8_t>(lv)});
          send(0, {0xA0, static_cast<uint8_t>(ch), static_cast<uint8_t>(0x10 | lv)});
        } else {
          send(unit, {0xD0, static_cast<uint8_t>((ch << 4) | lv)});
        }
      }
    }
  }
  if (!hui && !opt_.singleStrip && !mx.empty() && !masterTouched_) {  // master fader (main unit, channel 9)
    const int f = static_cast<int>(std::lround(faderNorm(0) * 16383.0));
    if (f != masterFader_) {
      masterFader_ = f;
      send(0, {0xE8, static_cast<uint8_t>(f & 0x7F), static_cast<uint8_t>(f >> 7)});
    }
  }
  // Transport LEDs: play, stop, record.
  const int tl = (host_.playing() ? 1 : 0) | (host_.playing() ? 0 : 2) | (host_.recording() ? 4 : 0);
  if (tl != transportLeds_) {
    for (int b = 0; b < 3; ++b) {
      if (transportLeds_ >= 0 && ((tl ^ transportLeds_) & (1 << b)) == 0) continue;
      const bool on = (tl >> b) & 1;
      if (hui) {
        static const int port[3] = {4, 3, 5};
        send(0, {0xB0, 0x0C, 0x0E});
        send(0, {0xB0, 0x2C, static_cast<uint8_t>(port[b] | (on ? 0x40 : 0))});
      } else {
        static const uint8_t note[3] = {NPlay, NStop, NRecord};
        send(0, {0x90, note[b], static_cast<uint8_t>(on ? 0x7F : 0)});
      }
    }
    transportLeds_ = tl;
  }
  if (!hui) sendTimecode();
}

}  // namespace gl::mackie
