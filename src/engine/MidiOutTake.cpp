#include "MidiOutTake.h"
#include <algorithm>
#include <cmath>
#include "NoteFxBurn.h"

namespace gl {

void MidiOutTake::begin(AudioEngine& e, ObjectId channel, ObjectId likePattern, Tick loop, Tick origin) {
  cancel(e);
  notes_.clear();
  notes_.reserve(kMaxNotes);
  open_.fill(Open{});
  channel_ = channel;
  like_ = likePattern;
  loop_ = std::max<Tick>(0, loop);
  origin_ = std::max<Tick>(0, origin);
  last_ = 0;
  lastPass_ = 0;
  // What was captured before (a recording stopped earlier) is not this one's.
  while (e.drainCapturedMidi(buf_.data(), static_cast<int>(buf_.size())) > 0) {
  }
  active_ = channel != kNoId;
  e.captureMidiOf(active_ ? channel : kNoId);
}

void MidiOutTake::cancel(AudioEngine& e) {
  if (active_ && e.midiCaptureChannel() == channel_) e.captureMidiOf(kNoId);
  active_ = false;
}

int MidiOutTake::held() const {
  int n = 0;
  for (const Open& o : open_) n += o.on ? 1 : 0;
  return n;
}

Tick MidiOutTake::place(double tick) const {
  if (loop_ > 0) {
    // (rounded first: a tick a hair before the loop end is the next pass's start)
    return static_cast<Tick>(std::llround(std::max(0.0, tick))) % loop_;
  }
  return std::max<Tick>(0, static_cast<Tick>(std::llround(tick)) - origin_);
}

void MidiOutTake::close(int slot, double at, uint32_t pass) {
  Open& o = open_[static_cast<size_t>(slot)];
  if (!o.on) return;
  o.on = false;
  const Tick s = place(o.start);
  Tick end = place(at);
  if (loop_ > 0 && (pass != o.pass || end < s)) end = loop_;  // held over the loop end: cut there
  if (end <= s) end = s + 1;
  if (notes_.size() >= kMaxNotes) return;  // (no growth while recording)
  Note n(s, end - s, slot % 128, o.velocity);
  n.color = slot / 128;  // the MIDI channel
  notes_.push_back(n);
}

void MidiOutTake::message(const AudioEngine::CapturedMidi& m) {
  const int st = m.s & 0xF0, mch = m.s & 0x0F;
  last_ = m.tick;
  lastPass_ = m.pass;
  if (st != 0x90 && st != 0x80) return;
  const int slot = mch * 128 + (m.a & 0x7F);
  if (st == 0x90 && m.b > 0) {
    close(slot, m.tick, m.pass);  // a repeated key ends the note before it
    open_[static_cast<size_t>(slot)] = {m.tick, m.pass, std::clamp(static_cast<int>(m.b), 1, 127), true};
  } else {
    close(slot, m.tick, m.pass);
  }
}

void MidiOutTake::poll(AudioEngine& e) {
  if (!active_) return;
  int n = 0;
  while ((n = e.drainCapturedMidi(buf_.data(), static_cast<int>(buf_.size()))) > 0)
    for (int k = 0; k < n; ++k) message(buf_[static_cast<size_t>(k)]);
}

ObjectId MidiOutTake::end(Document& d, AudioEngine& e, ObjectId target) {
  if (!active_) return kNoId;
  poll(e);
  cancel(e);
  for (int slot = 0; slot < static_cast<int>(open_.size()); ++slot) close(slot, last_, lastPass_);  // still held: ends at the last message
  // Loop passes play the same notes again: each one is kept once.
  std::vector<Note> out = notes_;
  std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  // A pass after the first may land a tick or two off (the instrument's own clock against the
  // transport's): within kSameTicks the note is the one already kept.
  std::vector<Note> kept;
  kept.reserve(out.size());
  for (const Note& n : out) {
    bool same = false;
    for (size_t k = kept.size(); k-- > 0 && !same;) {
      if (n.start - kept[k].start > kSameTicks) break;
      same = kept[k].pitch == n.pitch && kept[k].color == n.color;
    }
    if (!same) kept.push_back(n);
  }
  out.swap(kept);
  notes_.clear();
  return writeMidiOutputPattern(d, like_, channel_, target, out, "record MIDI output");
}

}  // namespace gl
