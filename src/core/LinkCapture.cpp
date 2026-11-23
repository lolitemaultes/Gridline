#include "LinkCapture.h"
#include <algorithm>
#include <cmath>

namespace gl {

// ---------------------------------------------------------------- decoder ----

void MidiControlDecoder::feed(const MidiMessage& m, std::vector<ControlEvent>& out) {
  const int k = m.kind();
  if (k < 0x80 || k > 0xE0) return;
  Chan& c = ch_[(m.port + 1) * 16 + m.channel()];
  ControlEvent e;
  e.channel = m.channel();
  e.port = m.port;
  const int prev = c.lastCc;
  c.lastCc = k == 0xB0 ? m.d1 : -1;
  switch (k) {
    case 0x80:
    case 0x90:
      e.source = RemoteSource::Note, e.number = m.d1, e.value = m.d2, e.noteOff = m.noteOff();
      if (e.noteOff) e.value = 0;
      out.push_back(e);
      return;
    case 0xA0: e.source = RemoteSource::KeyPressure, e.number = m.d1, e.value = m.d2; out.push_back(e); return;
    case 0xC0: e.source = RemoteSource::Program, e.number = m.d1, e.value = 127; out.push_back(e); return;
    case 0xD0: e.source = RemoteSource::ChannelPressure, e.value = m.d1; out.push_back(e); return;
    case 0xE0:
      e.source = RemoteSource::PitchBend, e.value = (m.d2 & 0x7F) << 7 | (m.d1 & 0x7F), e.hiRes = true;
      out.push_back(e);
      return;
    default: break;
  }
  const int n = m.d1 & 0x7F, v = m.d2 & 0x7F;
  // NRPN (99 MSB, 98 LSB) and RPN (101, 100) select a parameter; switching kind forgets the other half.
  if (n == 99 || n == 98 || n == 101 || n == 100) {
    const bool rpn = n == 101 || n == 100;
    if (rpn != c.rpn) c.paramMsb = c.paramLsb = -1;
    c.rpn = rpn;
    (n == 99 || n == 101 ? c.paramMsb : c.paramLsb) = v;
    c.fine = false;
    if (rpn && c.paramMsb == 127 && c.paramLsb == 127) c.paramMsb = c.paramLsb = -1;  // the null RPN
    return;
  }
  if ((n == 6 || n == 38 || n == 96 || n == 97) && c.paramMsb >= 0 && c.paramLsb >= 0) {
    e.source = c.rpn ? RemoteSource::Rpn : RemoteSource::Nrpn;
    e.number = c.paramMsb * 128 + c.paramLsb;
    if (n == 6) {
      c.dataMsb = v;
      e.hiRes = c.fine;
      e.value = c.fine ? v << 7 : v;
    } else if (n == 38) {
      c.fine = true;
      e.hiRes = true;
      e.value = c.dataMsb << 7 | v;
    } else {
      e.delta = n == 96 ? 1 : -1;
    }
    out.push_back(e);
    return;
  }
  e.source = RemoteSource::CC;
  if (n < 32) {
    c.msb[static_cast<size_t>(n)] = static_cast<int16_t>(v);
    e.number = n;
    e.hiRes = c.paired[static_cast<size_t>(n)];
    e.value = e.hiRes ? v << 7 : v;
  } else if (n < 64 && prev == n - 32) {  // the fine part right after its coarse part: one 14-bit control
    c.paired[static_cast<size_t>(n - 32)] = true;
    e.number = n - 32;
    e.hiRes = true;
    e.value = c.msb[static_cast<size_t>(n - 32)] << 7 | v;
  } else {
    e.number = n;
    e.value = v;
  }
  out.push_back(e);
}

bool linkUsesDecoder(const ControllerLink& l) {
  return l.source == RemoteSource::Nrpn || l.source == RemoteSource::Rpn || (l.source == RemoteSource::CC && l.hiRes);
}

bool remoteMatchesEvent(const ControllerLink& l, const ControlEvent& e) {
  if (!linkUsesDecoder(l)) return false;
  if (l.port >= 0 && e.port >= 0 && l.port != e.port) return false;
  if (l.midiChannel >= 0 && l.midiChannel != e.channel) return false;
  return e.source == l.source && e.number == l.cc;
}

double remoteEventInput(const ControllerLink& l, const ControlEvent& e) {
  (void)l;
  return std::clamp(e.normalized(), 0.0, 1.0);
}

// ---------------------------------------------------------------- capture ----

namespace {
// A move is at least two steps of a 7-bit control from where the control rested: a fader or knob
// that jitters by one step, and a 14-bit control's fine part, never count.
constexpr double kMove = 1.5 / 127.0;
}  // namespace

LinkCapture::Key LinkCapture::keyOf(const std::string& device, const ControlEvent& e) {
  return {device, e.channel, static_cast<int>(e.source), e.number};
}

void LinkCapture::start(double now, Options o) {
  o_ = o;
  listening_ = true;
  start_ = now;
  ctl_.clear();
  captured_ = Captured{};
  capturedKey_ = Key{};
  pending_ = false;
  lone_ = false;
  prev_ = Captured{};
  prevKey_ = Key{};
  ignored_ = "";
}

void LinkCapture::take(const ControlEvent& e, const std::string& device, bool surfacePort, double now) {
  const Key k = keyOf(device, e);
  const bool same = hasCapture() && k == capturedKey_;
  const int n = captured_.captures;
  if (!same) {
    captured_ = Captured{};
    captured_.source = e.source;
    captured_.number = e.number;
    captured_.channel = e.channel;
    captured_.port = e.port;
    captured_.device = device;
    captured_.surface = surfacePort;
  }
  captured_.captures = same ? n : n + 1;
  captured_.hiRes = captured_.hiRes || (e.hiRes && e.source == RemoteSource::CC);
  captured_.value = e.delta ? std::clamp(captured_.value + e.delta / 128.0, 0.0, 1.0) : e.normalized();
  captured_.valueTime = now;
  capturedKey_ = k;
  lone_ = false;
  if (pending_ && keyOf(pendingDevice_, pendingEv_) != k) pending_ = false;
}

bool LinkCapture::feed(const ControlEvent& e, const std::string& device, bool surfacePort, double now) {
  if (!listening_) return false;
  ignored_ = "";
  const Key k = keyOf(device, e);
  const bool isCaptured = hasCapture() && k == capturedKey_;
  const bool settling = now < start_ + o_.settle;
  // The keys: notes and their pressure only when the user wants a note or a button.
  if ((e.source == RemoteSource::Note || e.source == RemoteSource::KeyPressure) && !o_.notes) {
    ignored_ = "keys";
    return false;
  }
  if (settling) ignored_ = "settling";
  // Notes and program changes are presses: the press counts, a release only moves the meter.
  if (e.source == RemoteSource::Note || e.source == RemoteSource::Program) {
    if (e.noteOff) {
      if (isCaptured) captured_.value = 0, captured_.valueTime = now;
      return false;
    }
    if (settling) return false;
    const bool fresh = !isCaptured;
    take(e, device, surfacePort, now);
    return fresh;
  }
  // NRPN / RPN increment and decrement buttons: a step is a move.
  if (e.delta != 0) {
    if (settling) return false;
    const bool fresh = !isCaptured;
    take(e, device, surfacePort, now);
    return fresh;
  }
  const double v = e.normalized();
  Ctl& c = ctl_[k];
  const bool first = c.anchor < 0;
  if (first) {
    // Where it rests before anyone touched it: a pitch wheel at its centre, pressure at nothing.
    // (Seen while settling: where it is now.)
    const double rest = settling ? v : e.source == RemoteSource::PitchBend ? 0.5 : e.source == RemoteSource::ChannelPressure ? 0.0 : v;
    c.anchor = static_cast<int>(std::lround(rest * 16383.0));
  }
  const double anchor = c.anchor / 16383.0, last = c.last < 0 ? -1.0 : c.last / 16383.0;
  const bool repeat = last >= 0 && std::fabs(v - last) < 1e-9;
  const double gap = now - c.lastTime;
  if (repeat) {
    // The same value again and again at a steady pace: the device sends it on its own.
    if (c.repeats >= 1 && c.lastGap > 0 && std::fabs(gap - c.lastGap) <= std::max(0.02, 0.3 * c.lastGap)) c.stream = true;
    ++c.repeats;
  } else {
    c.repeats = 0;
  }
  c.lastGap = c.last < 0 ? 0 : gap;
  c.last = static_cast<int>(std::lround(v * 16383.0));
  c.lastTime = now;
  if (isCaptured) {  // the captured control moves the meter
    captured_.value = v, captured_.valueTime = now;
    c.anchor = c.last;
    if (c.stream && lone_) {  // it was a device sending on its own, not a press: back to what was captured before
      captured_ = prev_;
      capturedKey_ = prevKey_;
      lone_ = false;
    }
    return false;
  }
  if (pending_ && keyOf(pendingDevice_, pendingEv_) == k) pending_ = false;  // more than one message: not a lone press
  if (settling) return false;
  if (repeat) {
    ignored_ = "repeat";
    return false;
  }
  if (std::fabs(v - anchor) >= kMove) {
    take(e, device, surfacePort, now);
    c.anchor = c.last;
    return true;
  }
  if (first && e.source == RemoteSource::CC && !c.stream) {  // perhaps a button that sends one message
    pending_ = true;
    pendingEv_ = e;
    pendingDevice_ = device;
    pendingSurface_ = surfacePort;
    pendingTime_ = now;
    ignored_ = "waiting";
    return false;
  }
  ignored_ = "jitter";
  return false;
}

bool LinkCapture::tick(double now) {
  if (!listening_ || !pending_ || now - pendingTime_ < o_.lonePress) return false;
  pending_ = false;
  auto it = ctl_.find(keyOf(pendingDevice_, pendingEv_));
  if (it != ctl_.end() && it->second.stream) return false;
  prev_ = captured_;
  prevKey_ = capturedKey_;
  take(pendingEv_, pendingDevice_, pendingSurface_, now);
  lone_ = true;
  return true;
}

void LinkCapture::apply(ControllerLink& l) const {
  if (!hasCapture()) return;
  l.source = captured_.source;
  l.cc = captured_.source == RemoteSource::PitchBend || captured_.source == RemoteSource::ChannelPressure ? l.cc : captured_.number;
  l.midiChannel = captured_.channel;
  l.port = captured_.port;
  l.hiRes = captured_.hiRes && captured_.source == RemoteSource::CC;
}

}  // namespace gl
