// Gridline — "plasma" pack: the arpeggiator (20 shapes plus a step pattern,
// transposition ranges, repeats, gate, chance, velocity decay, retrigger
// options, latch / thru) and the clip player (looped note clips triggered and
// transposed from the keyboard, playback modes, automation lanes).
#include <algorithm>
#include <cmath>
#include "plasma_dsp.h"

namespace gl::packs::plasma {

namespace {
double rateBeats(int div, bool trip, bool dot, double offset) {
  const int d = std::clamp(div + static_cast<int>(std::lround(offset * 8)), 0, static_cast<int>(divisionNames().size()) - 1);
  double b = divisionBeats(d);
  if (trip) b *= 2.0 / 3.0;
  if (dot) b *= 1.5;
  return std::max(1.0 / 64, b);
}
int snapToScale(int key, const std::vector<int>& scale, int root) {
  if (root <= 0 || scale.empty() || scale.size() >= 12) return key;
  const int r = root - 1;
  const int rel = ((key - r) % 12 + 12) % 12;
  int best = scale[0], bestD = 99;
  for (int s : scale) {
    const int d = std::abs(s - rel);
    if (d < bestD) bestD = d, best = s;
  }
  return key - rel + best;
}
}  // namespace

// ------------------------------------------------------------------ arpeggiator
Arp::Arp() {
  sorted_.reserve(128);
  cycle_.reserve(4096);
  played_.reserve(128);
  reset();
}

void Arp::reset() {
  heldCount_ = 0;
  latched_ = latchArmed_ = false;
  pos_ = step_ = repeat_ = transStep_ = 0;
  phase_ = 0;
  gateLeft_ = -1;
  soundingN_ = 0;
  veloCur_ = 1;
  first_ = true;
  heldKey_.fill(0);
}

void Arp::noteOn(int key, float vel) {
  key = std::clamp(key, 0, 127);
  if (latched_ && heldCount_ == 0) {  // a new chord replaces the latched one
    heldKey_.fill(0);
    latched_ = false;
  }
  if (!heldKey_[static_cast<size_t>(key)]) {
    heldKey_[static_cast<size_t>(key)] = 1;
    ++heldCount_;
  }
  heldVel_[static_cast<size_t>(key)] = vel;
  int maxOrder = 0;
  for (int k = 0; k < 128; ++k)
    if (heldKey_[static_cast<size_t>(k)]) maxOrder = std::max(maxOrder, order_[static_cast<size_t>(k)]);
  order_[static_cast<size_t>(key)] = maxOrder + 1;
  if (heldCount_ == 1) first_ = true;
  newNote_ = true;
}

void Arp::noteOff(int key) {
  key = std::clamp(key, 0, 127);
  if (!heldKey_[static_cast<size_t>(key)]) return;
  --heldCount_;
  if (latchArmed_) {
    if (heldCount_ == 0) latched_ = true;  // keep sounding
    return;
  }
  heldKey_[static_cast<size_t>(key)] = 0;
}

namespace {
// The global SWING DIVISION choice (1/4, 1/8, 1/16, 1/32) in beats.
double swingDivBeats(int choice) { return 1.0 / static_cast<double>(1 << std::clamp(choice, 0, 3)); }
}  // namespace

void Arp::allOff() {
  heldKey_.fill(0);
  heldCount_ = 0;
  latched_ = false;
}

void Arp::run(const ArpSlot& s, const float* off, int n, double sr, double tempo, double beat, bool playing, double swing,
              int swingDiv, const std::vector<int>& scale, int key, SeqOut& out) {
  latchArmed_ = s.latch;
  if (!s.latch && latched_) {
    latched_ = false;
    for (int k = 0; k < 128; ++k)
      if (heldKey_[static_cast<size_t>(k)] && heldCount_ == 0) heldKey_[static_cast<size_t>(k)] = 0;
  }
  // gather held notes (sorted low..high, and in played order)
  sorted_.clear();
  for (int k = 0; k < 128; ++k)
    if (heldKey_[static_cast<size_t>(k)]) sorted_.push_back(k);
  const int count = static_cast<int>(sorted_.size());
  auto stopAll = [&](int at) {
    for (int i = 0; i < soundingN_; ++i) out.push({sounding_[i], 0, false, at});
    soundingN_ = 0;
  };
  if (count == 0) {
    stopAll(0);
    first_ = true;
    phase_ = 0;
    return;
  }
  played_.clear();
  {
    // played order: indices into sorted_ ordered by order_
    std::array<int, 128> idx{};
    int m = 0;
    for (int i = 0; i < count; ++i) idx[static_cast<size_t>(m++)] = i;
    std::sort(idx.begin(), idx.begin() + m, [&](int a, int b) {
      return order_[static_cast<size_t>(sorted_[static_cast<size_t>(a)])] < order_[static_cast<size_t>(sorted_[static_cast<size_t>(b)])];
    });
    for (int i = 0; i < m; ++i) played_.push_back(idx[static_cast<size_t>(i)]);
  }
  const double stepBeats = s.bpm ? rateBeats(s.rate, s.trip, s.dot, off[0]) : 0;
  const double hz = std::max(0.05, static_cast<double>(s.hz) * std::pow(2.0, off[0] * 3.0));
  const double stepsPerSample = s.bpm ? tempo / 60.0 / sr / stepBeats : hz / sr;
  const double beatsPerSample = tempo / 60.0 / sr;
  const int range = std::clamp(s.range + static_cast<int>(std::lround(off[2] * 4)), 0, 8);
  const int shift = s.shift + static_cast<int>(std::lround(off[1] * 24));
  const int offset = s.offset + static_cast<int>(std::lround(off[3] * 8));
  const int repeats = std::max(1, s.repeats + static_cast<int>(std::lround(off[4] * 8)));
  const double gate = std::clamp(static_cast<double>(s.gate) * std::pow(2.0, off[5] * 2.0), 0.01, 4.0);
  const float chance = std::clamp(s.chance + off[6], 0.f, 1.f);
  const double retrigBeats = rateBeats(s.retrigDiv, false, false, off[7]);
  const float vDecay = std::clamp(s.veloDecay + off[8] * 0.5f, 0.f, 1.f), vTarget = std::clamp(s.veloTarget + off[9] * 0.5f, 0.f, 1.f);
  const int transpose = static_cast<int>(std::lround(off[10] * 24));
  const int wrapPitch = s.wrapPitch + static_cast<int>(std::lround(off[11] * 24));
  const int cyclesPerStep = std::clamp(s.rangeWrap + 1 + static_cast<int>(std::lround(off[12] * 3)), 1, 8);
  const bool phantom = off[13] > 0.5f;
  // swing (0.5 = straight, 1 = the second division of each pair starts a third
  // of a division late), on rates at or below the swing division. Each pair of
  // swing divisions is warped piecewise linearly: its midpoint moves late, its
  // ends stay put, so finer steps follow smoothly and never overtake each other.
  const double sw = std::clamp((swing - 0.5) * 2.0, 0.0, 1.0) / 3.0;
  const bool swingOn = sw > 0 && s.bpm && stepBeats <= swingDivBeats(swingDiv) * 1.001;
  const double div = swingOn ? swingDivBeats(swingDiv) / stepBeats : 1.0;  // steps per swing division
  auto swungAt = [&](double g) {  // grid position (steps) -> swung position (steps)
    if (!swingOn) return g;
    const double pair = std::floor(g / (2.0 * div) + 1e-9) * 2.0 * div, u = g - pair, mid = div * (1.0 + sw);
    return pair + (u < div ? u * mid / div : mid + (u - div) * (2.0 * div - mid) / div);
  };
  // Synced to a running song (retrigger on launch off): the step grid is the
  // song's (phase_ counts song steps); otherwise it starts at the trigger.
  const bool songGrid = s.bpm && playing && !s.retrigLaunch;
  auto restart = [&]() {
    pos_ = 0, repeat_ = 0, transStep_ = 0, transDir_ = 1, cycles_ = 0, step_ = 0;
    cycle_.clear();
    if (s.veloRetrig) veloCur_ = 1;
  };
  // J-138: phase_ = position on the step grid (steps), next_ = grid index of the next step.
  auto join = [&]() {  // on the song grid: the next step is the first grid step from here
    phase_ = beat / stepBeats;
    next_ = std::ceil(phase_ - 1e-6);
  };
  if (first_) {
    restart();
    veloCur_ = 1;
    if (songGrid) join();
    else phase_ = 0, next_ = 0;  // fire immediately; the grid starts here
    retrigAcc_ = 0;
    first_ = false;
  } else if (newNote_ && (s.retrigNote || (s.retrigFirst && heldCount_ == 1))) {
    restart();
    phase_ = 0, next_ = 0;
  } else if (songGrid && std::fabs(beat / stepBeats - phase_) > 0.5) {
    join();  // the song jumped (loop, seek) or the rate changed: back onto its grid
  }
  newNote_ = false;
  for (int i = 0; i < n; ++i) {
    if (s.retrigRate) {
      retrigAcc_ += beatsPerSample;
      if (retrigAcc_ >= retrigBeats) {
        retrigAcc_ -= retrigBeats;
        restart();
        phase_ = 0, next_ = 0;
      }
    }
    if (gateLeft_ > 0) {
      gateLeft_ -= stepsPerSample;
      if (gateLeft_ <= 0 && s.shape != ArpChord) stopAll(i);
    }
    const bool due = phase_ >= swungAt(next_) - 1e-9;
    phase_ += stepsPerSample;
    if (!due) continue;
    next_ += 1;
    // next step
    if (repeat_ <= 0) {
      if (cycle_.empty() || pos_ >= static_cast<int>(cycle_.size())) {
        if (!cycle_.empty() && ++cycles_ >= cyclesPerStep) {
          cycles_ = 0;
          // transposition steps: Up, Down, Up/Down, Random
          if (range > 0) {
            switch (s.rangeShape) {
              case 1: transStep_ = transStep_ <= 0 ? range : transStep_ - 1; break;
              case 2:
                if (transStep_ + transDir_ > range || transStep_ + transDir_ < 0) transDir_ = -transDir_;
                transStep_ += transDir_;
                break;
              case 3: transStep_ = static_cast<int>(rngNext(rng_) % static_cast<uint32_t>(range + 1)); break;
              default: transStep_ = transStep_ >= range ? 0 : transStep_ + 1; break;
            }
          } else {
            transStep_ = 0;
          }
        }
        arpOrder(s.shape == ArpPattern ? ArpUp : s.shape, count, played_, rng_, cycle_);
        pos_ = 0;
      }
      repeat_ = repeats;
      chanceHold_ = chance >= 1.f || rngUni(rng_) < chance;  // "pre": one roll for the repeated group
    }
    --repeat_;
    stopAll(i);
    const bool fire = s.chancePre ? chanceHold_ : (chance >= 1.f || rngUni(rng_) < chance);
    int idx = cycle_.empty() ? 0 : cycle_[static_cast<size_t>((pos_ + offset % std::max(1, static_cast<int>(cycle_.size())) + static_cast<int>(cycle_.size())) % static_cast<int>(cycle_.size()))];
    if (repeat_ <= 0) ++pos_;
    ++step_;
    if (!fire) continue;
    float vel = 0.8f;
    if (s.velo) {
      veloCur_ = veloCur_ + (vTarget - veloCur_) * vDecay;
      vel = std::clamp(veloCur_, 0.02f, 1.f);
    }
    const int tshift = shift == 0 ? 12 : shift;
    auto emit = [&](int k, float v) {
      int kk = k + transStep_ * tshift + transpose;
      kk = snapToScale(kk, scale, key);
      if (kk < 0 || kk > 127 || soundingN_ >= 16) return;
      out.push({kk, v, true, i});
      sounding_[soundingN_++] = kk;
    };
    if (s.shape == ArpPattern && !s.steps.empty()) {
      // J-201: steps sit on a grid of 0.25 pattern time per arp step (the editor's columns, and the
      // source format's pattern clips: measured, a gap of 0.5 plays as two steps of the rate);
      // columns without a step are rests. The pattern repeats after patternBeats (at least
      // the last step).
      int cols = std::max(1, static_cast<int>(std::lround(s.patternBeats / 0.25f)));
      for (const auto& q : s.steps) cols = std::max(cols, static_cast<int>(std::lround(q.time / 0.25f)) + 1);
      const int col = (step_ - 1) % cols;
      const ArpSlot::Step* hit = nullptr;
      for (const auto& q : s.steps)
        if (static_cast<int>(std::lround(q.time / 0.25f)) == col) {
          hit = &q;
          break;
        }
      if (!hit) continue;  // a rest
      const auto& st = *hit;
      if (s.stepMode == 3 && !st.accent) continue;  // accents only
      const int row = st.row;
      const int wraps = row >= 0 ? row / count : -((-row + count - 1) / count);
      const int base = sorted_[static_cast<size_t>(((row % count) + count) % count)];
      int k = base;
      if (wraps != 0) {
        // rows past the held notes: octave, custom interval, repeat, or skip (phantom plays them as octaves)
        switch (s.wrap) {
          case 1: k = base + wraps * wrapPitch; break;
          case 2: k = base; break;
          case 3:
            if (!phantom) continue;
            k = base + 12 * wraps;
            break;
          default: k = base + 12 * wraps; break;
        }
      }
      emit(k, std::clamp(st.vel * (st.accent ? 1.25f : 1.f), 0.02f, 1.f));
      // a step's length in steps (0.25 pattern time = one step), times the gate
      double g = static_cast<double>(st.len) / 0.25 / std::max(1e-3, static_cast<double>(s.patTime)) * gate;
      if (s.stepMode == 1) g = std::max(g, 1.0);      // tie: hold to the next step
      else if (s.stepMode == 2) g *= 0.5;             // short
      gateLeft_ = std::max(0.02, g);
    } else if (idx < 0) {  // chord
      for (int k : sorted_) emit(k, s.velo ? vel : heldVel_[static_cast<size_t>(k)]);
      gateLeft_ = gate;
    } else {
      const int k = sorted_[static_cast<size_t>(std::clamp(idx, 0, count - 1))];
      emit(k, s.velo ? vel : heldVel_[static_cast<size_t>(k)]);
      gateLeft_ = gate;
    }
  }
}

// ------------------------------------------------------------------ clip player
void ClipPlayer::reset() {
  running_ = false;
  finishing_ = false;
  waitBeats_ = -1;
  held_ = 0;
  pos_ = 0;
  dir_ = 1;
  on_.fill(0);
}

void ClipPlayer::noteOn(int key, float vel) {
  ++held_;
  trigKey_ = key;
  trigVel_ = vel;
  if (!running_) {
    running_ = true;
    finishing_ = false;
    waitBeats_ = -1;
    pos_ = -1;  // start on the next run
  }
}

void ClipPlayer::noteOff(int) {
  if (held_ > 0) --held_;
}

void ClipPlayer::run(const ClipSlot& c, const float* off, int n, double sr, double tempo, double beat, bool playing, double swing,
                     int swingDiv, SeqOut& out, float* laneValues, int laneCount) {
  // swing: notes on odd positions of the swing division start late
  const double sw = std::clamp((swing - 0.5) * 2.0, 0.0, 1.0) / 3.0;
  const double swDiv = swingDivBeats(swingDiv);
  auto swung = [&](double t) {
    if (sw <= 0) return t;
    const double q = t / swDiv, r = std::round(q);
    if (std::fabs(q - r) < 1e-3 && (static_cast<long long>(r) & 1)) return t + sw * swDiv;
    return t;
  };
  auto stopAll = [&](int at) {
    for (int k = 0; k < 128; ++k)
      if (on_[static_cast<size_t>(k)]) {
        out.push({k, 0, false, at});
        on_[static_cast<size_t>(k)] = 0;
      }
  };
  if (running_ && held_ <= 0) {
    if (c.noteGate) {  // note gate: the clip stops with the keys
      running_ = false;
      stopAll(0);
    } else {
      finishing_ = true;  // otherwise it plays to the end of the clip
    }
  }
  if (held_ > 0) finishing_ = false;
  if (!running_ || c.notes.empty()) return;
  const double len = std::max(0.25, static_cast<double>(c.lengthBeats));
  double rate = c.rate * std::pow(2.0, std::round(off[1] * 3.0));
  if (c.trip) rate *= 1.5;
  if (c.dot) rate /= 1.5;
  const double beatsPerSample = (c.bpm ? tempo / 60.0 : 2.0) / sr * rate;
  const int trans = c.transpose + static_cast<int>(std::lround(off[0] * 24)) + (c.span == 0 ? 0 : trigKey_ - 60);
  const double startOff = c.startOffset + off[2] * len;
  if (pos_ < 0 && waitBeats_ < 0 && playing && c.launchQuant >= 0) {
    // launch quantize: start on the next multiple of the launch division
    const double q = divisionBeats(std::clamp(c.launchQuant, 0, static_cast<int>(divisionNames().size()) - 1));
    const double next = std::ceil(beat / q - 1e-9) * q;
    waitBeats_ = std::max(0.0, next - beat);
  }
  if (pos_ < 0 && waitBeats_ > 0) {
    waitBeats_ -= tempo / 60.0 / sr * n;
    if (waitBeats_ > 0) return;
  }
  if (pos_ < 0) {
    waitBeats_ = -1;
    pos_ = c.retrig || !playing ? std::fmod(std::max(0.0, startOff), len) : std::fmod(beat * rate + startOff, len);
    dir_ = c.mode == 1 ? -1 : 1;
    if (c.mode == 1) pos_ = len - pos_;
  }
  const double blockBeats = beatsPerSample * n;
  const double a = pos_, b = pos_ + blockBeats * dir_;
  auto inWindow = [&](double t, double lo, double hi) { return lo <= hi ? (t >= lo && t < hi) : (t > hi && t <= lo); };
  // note-ons and note-offs in clip positions [lo, hi), the window starting `base` samples into the block
  auto scan = [&](double lo, double hi, double base) {
    for (const auto& nt : c.notes) {
      if (nt.mute) continue;
      const double s0 = swung(nt.start), e0 = s0 + nt.len;
      const int k = std::clamp(nt.key + trans, 0, 127);
      if (inWindow(s0, lo, hi)) {
        const int at = std::clamp(static_cast<int>(base + std::fabs(s0 - lo) / beatsPerSample), 0, n - 1);
        if (on_[static_cast<size_t>(k)]) out.push({k, 0, false, at});
        out.push({k, std::clamp(nt.vel * (c.veloTrig ? trigVel_ / 0.8f : 1.f), 0.02f, 1.f), true, at});
        on_[static_cast<size_t>(k)] = 1;
      }
      if (on_[static_cast<size_t>(k)] && inWindow(e0, lo, hi)) {
        const int at = std::clamp(static_cast<int>(base + std::fabs(e0 - lo) / beatsPerSample), 0, n - 1);
        out.push({k, 0, false, at});
        on_[static_cast<size_t>(k)] = 0;
      }
    }
  };
  scan(a, b, 0);
  // automation lanes
  for (int l = 0; l < std::min(laneCount, static_cast<int>(c.lanes.size())); ++l) {
    const auto& pts = c.lanes[static_cast<size_t>(l)].pts;
    float v = -1;
    if (!pts.empty()) {
      v = pts.front().second;
      for (size_t i = 1; i < pts.size(); ++i)
        if (pos_ <= pts[i].first) {
          const double w = pts[i].first - pts[i - 1].first;
          const double t = w > 1e-9 ? (pos_ - pts[i - 1].first) / w : 1.0;
          v = static_cast<float>(pts[i - 1].second + (pts[i].second - pts[i - 1].second) * std::clamp(t, 0.0, 1.0));
          break;
        } else {
          v = pts[i].second;
        }
    }
    laneValues[l] = v;
  }
  pos_ = b;
  if (c.mode == 8) {  // static: loops the first slice (mode time)
    const double slice = std::clamp(static_cast<double>(c.modeTime), 0.0625, len);
    if (pos_ >= slice) {
      stopAll(n - 1);
      pos_ = std::fmod(pos_, slice);
    }
  }
  // loop handling per mode
  if (pos_ >= len || pos_ < 0) {
    // J-138: a plain forward loop wraps inside the block: notes end at the clip
    // end and the next pass's first notes play in the rest of the block (they
    // used to be skipped whenever the loop did not end on a block boundary).
    const bool wrapFwd = c.mode == 0 && dir_ > 0 && !finishing_ && pos_ >= len && pos_ - len < len;
    const double wrapAt = std::clamp((len - a) / beatsPerSample, 0.0, static_cast<double>(n - 1));
    stopAll(wrapFwd ? static_cast<int>(wrapAt) : n - 1);
    if (wrapFwd) {
      pos_ -= len;
      scan(0.0, pos_, wrapAt);
      return;
    }
    if (finishing_) {
      running_ = false, finishing_ = false;
      return;
    }
    switch (c.mode) {
      case 1: pos_ += len; break;                                  // reverse
      case 2: dir_ = -dir_; pos_ = dir_ < 0 ? len - 1e-6 : 0; break;  // pendulum
      case 3:
      case 4:
      case 5:
      case 6: {  // random slices / random start / random end
        const double slice = std::max(0.0625, static_cast<double>(c.modeTime));
        const int slices = std::max(1, static_cast<int>(len / slice));
        int s = static_cast<int>(rngNext(rng_) % static_cast<uint32_t>(slices));
        pos_ = s * slice;
        break;
      }
      case 7: running_ = false; break;                              // one-shot
      default: pos_ = std::fmod(pos_, len); if (pos_ < 0) pos_ += len; break;
    }
  }
}

}  // namespace gl::packs::plasma
