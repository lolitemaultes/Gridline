#include "MidiSync.h"
#include <algorithm>

namespace gl {

const char* syncOutName(SyncOut s) {
  static const char* n[] = {"Off", "MIDI clock", "MTC 24 fps", "MTC 25 fps", "MTC 29.97 fps (drop)", "MTC 30 fps"};
  int i = static_cast<int>(s);
  return i >= 0 && i < static_cast<int>(SyncOut::Count) ? n[i] : "?";
}

double mtcFrameRate(SyncOut s) {
  switch (s) {
    case SyncOut::Mtc24: return 24.0;
    case SyncOut::Mtc25: return 25.0;
    case SyncOut::Mtc2997Drop: return 30000.0 / 1001.0;
    default: return 30.0;
  }
}
int mtcNominalFps(SyncOut s) { return s == SyncOut::Mtc24 ? 24 : s == SyncOut::Mtc25 ? 25 : 30; }
int mtcRateCode(SyncOut s) { return s == SyncOut::Mtc24 ? 0 : s == SyncOut::Mtc25 ? 1 : s == SyncOut::Mtc2997Drop ? 2 : 3; }

Timecode mtcTimecode(SyncOut s, int64_t frame) {
  if (frame < 0) frame = 0;
  const int fps = mtcNominalFps(s);
  if (s == SyncOut::Mtc2997Drop) {
    // Drop-frame labels skip frame numbers 0 and 1 at the start of every
    // minute except each tenth minute.
    const int64_t per10 = 17982, perMin = 1798;
    int64_t d = frame / per10, m = frame % per10;
    frame += 18 * d + (m > 1 ? 2 * ((m - 2) / perMin) : 0);
  }
  Timecode t;
  t.f = static_cast<int>(frame % fps);
  int64_t secs = frame / fps;
  t.s = static_cast<int>(secs % 60);
  t.m = static_cast<int>((secs / 60) % 60);
  t.h = static_cast<int>((secs / 3600) % 24);
  return t;
}

void MidiClockOut::emit(int64_t at, uint8_t a, int size, uint8_t b, uint8_t c) {
  if (!out) return;
  MidiOutEvent e;
  e.sample = at;
  e.size = static_cast<uint8_t>(size);
  e.data[0] = a;
  e.data[1] = b;
  e.data[2] = c;
  if (!out->push(e)) ++dropped_;
}

void MidiClockOut::start(double tick, double seconds, int64_t at) {
  (void)at;
  if (mode_ == SyncOut::Off) return;
  running_ = true;
  pendingStart_ = true;
  startTick_ = tick;
  startSec_ = seconds;
  nextTick_ = tick;
}

void MidiClockOut::stop(int64_t at) {
  if (!running_) return;
  running_ = false;
  pendingStart_ = false;
  if (mode_ == SyncOut::Clock) emit(at, 0xFC);
}

void MidiClockOut::announce(double tick, double sec, int64_t at, bool fresh) {
  if (mode_ == SyncOut::Clock) {
    if (fresh && tick < 1e-9) {
      emit(at, 0xFA);  // start from the top
      return;
    }
    // Song position pointer counts 16th notes (6 clocks each).
    int spp = static_cast<int>(std::floor(tick / (ppq_ / 4.0) + 1e-9));
    spp = std::clamp(spp, 0, 16383);
    if (!fresh) emit(at, 0xFC);
    emit(at, 0xF2, 3, static_cast<uint8_t>(spp & 0x7F), static_cast<uint8_t>((spp >> 7) & 0x7F));
    emit(at, 0xFB);
  } else {
    fullFrame(sec, at);
  }
}

void MidiClockOut::quarterFrame(int64_t at, int64_t j) {
  const int piece = static_cast<int>(j % 8);
  const Timecode t = mtcTimecode(mode_, (j - piece) / 4);
  int nib = 0;
  switch (piece) {
    case 0: nib = t.f & 0xF; break;
    case 1: nib = (t.f >> 4) & 0x1; break;
    case 2: nib = t.s & 0xF; break;
    case 3: nib = (t.s >> 4) & 0x3; break;
    case 4: nib = t.m & 0xF; break;
    case 5: nib = (t.m >> 4) & 0x3; break;
    case 6: nib = t.h & 0xF; break;
    default: nib = ((t.h >> 4) & 0x1) | (mtcRateCode(mode_) << 1); break;
  }
  emit(at, 0xF1, 2, static_cast<uint8_t>((piece << 4) | nib));
}

void MidiClockOut::fullFrame(double sec, int64_t at) {
  if (!out) return;
  const Timecode t = mtcTimecode(mode_, static_cast<int64_t>(std::floor(std::max(0.0, sec) * mtcFrameRate(mode_) + 1e-9)));
  MidiOutEvent e;
  e.sample = at;
  e.size = 10;
  const uint8_t d[10] = {0xF0, 0x7F, 0x7F, 0x01, 0x01, static_cast<uint8_t>(t.h | (mtcRateCode(mode_) << 5)),
                         static_cast<uint8_t>(t.m), static_cast<uint8_t>(t.s), static_cast<uint8_t>(t.f), 0xF7};
  std::memcpy(e.data, d, 10);
  if (!out->push(e)) ++dropped_;
}

// ---------------------------------------------------------------- follower ----

void MidiClockFollower::reset() {
  count_ = head_ = 0;
  bpm_ = 0;
  running_ = false;
  clocks_ = 0;
  cmds_.clear();
}

void MidiClockFollower::message(uint8_t status, uint8_t d1, uint8_t d2, double t) {
  switch (status) {
    case 0xF8: {
      if (count_ > 0) {
        double last = times_[(head_ + kWindow) % (kWindow + 1)];
        if (t - last > 0.5 || t < last) count_ = 0;  // a gap: the clock stopped
      }
      times_[head_] = t;
      head_ = (head_ + 1) % (kWindow + 1);
      count_ = std::min(count_ + 1, kWindow + 1);
      if (count_ >= 7) {
        double newest = times_[(head_ + kWindow) % (kWindow + 1)];
        double oldest = times_[(head_ + kWindow + 1 - count_) % (kWindow + 1)];
        double interval = (newest - oldest) / (count_ - 1);
        if (interval > 1e-6) bpm_ = 60.0 / (interval * 24.0);
      }
      if (running_) ++clocks_;
      break;
    }
    case 0xFA:
      running_ = true;
      clocks_ = 0;
      cmds_.push_back({Cmd::Start, 0});
      break;
    case 0xFB:
      running_ = true;
      cmds_.push_back({Cmd::Continue, 0});
      break;
    case 0xFC:
      running_ = false;
      cmds_.push_back({Cmd::Stop, 0});
      break;
    case 0xF2: {
      double s = static_cast<double>((d1 & 0x7F) | ((d2 & 0x7F) << 7));
      clocks_ = static_cast<int64_t>(s) * 6;
      cmds_.push_back({Cmd::Locate, s});
      break;
    }
    default: break;
  }
  if (cmds_.size() > 256) cmds_.erase(cmds_.begin(), cmds_.begin() + 128);
}

std::vector<MidiClockFollower::Cmd> MidiClockFollower::takeCommands() {
  std::vector<Cmd> out;
  out.swap(cmds_);
  return out;
}

}  // namespace gl
