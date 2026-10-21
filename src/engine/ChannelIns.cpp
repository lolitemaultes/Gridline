// Gridline — channel instrument envelopes (J-33, J-72). See ChannelIns.h.
//
// Classic timing keeps the reference's single-precision arithmetic and its
// evaluation order (measured sample by sample against the reference running
// locally); keep both when editing. Floating-point contraction is off here
// so the order holds on targets with fused multiply-add.
#include "ChannelIns.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace gl {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kLn1001 = 6.9087547793152204;    // time knob law
constexpr double kTension = 8.7557375393064696;   // tension law
constexpr float kInv128 = 0.0078125f, kInv256 = 0.00390625f;
constexpr float kTiny = 5.96046448e-08f;            // 2^-24
constexpr float kMaxStep = 0.00100000005f;          // gain / coefficient slope limit per sample
constexpr int kCtl = 8;                             // smooth timing: control period (samples)
using Ext = InsExt;

// Lowest 2^-24 test on the bit pattern (the reference compares magnitudes as integers).
bool belowTiny(float v) {
  uint32_t b, t;
  const float a = std::fabs(v);
  std::memcpy(&b, &a, 4);
  std::memcpy(&t, &kTiny, 4);
  return b < t;
}
float flushTiny(float v) { return belowTiny(v) ? 0.0f : v; }
double flushTiny(double v) { return std::fabs(v) < static_cast<double>(kTiny) ? 0.0 : v; }

// The LFO tables (16384 steps per cycle).
float lfoTable(int shape, uint32_t phase) {
  const int i = static_cast<int>(phase >> 18);
  switch (shape) {
    case 1:  // triangle, in phase with the sine
      if (i < 4096) return static_cast<float>(i / 4096.0);
      if (i < 12288) return static_cast<float>(1.0 - (i - 4096) / 4096.0);
      return static_cast<float>(-1.0 + (i - 12288) / 4096.0);
    case 2: return i < 8192 ? 1.0f : -1.0f;
    default: return static_cast<float>(std::sin(kTwoPi * i / 16384.0));
  }
}
// Smooth timing: the same shapes on a continuous phase (cycles).
double lfoShape(int shape, double ph) {
  switch (shape) {
    case 1: return ph < 0.25 ? 4.0 * ph : ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0;
    case 2: return ph < 0.5 ? 1.0 : -1.0;
    default: return std::sin(kTwoPi * ph);
  }
}

// Time knob → u (0 .. 1000) as the reference computes it.
double knobU(int v) {
  if (v < 101) return 0.0;
  const float xf = static_cast<float>(static_cast<double>(v) * (1.0 / 65536.0));
  return static_cast<double>(static_cast<float>(std::exp(static_cast<double>(xf) * kLn1001) - 1.0));
}

}  // namespace

double insKnobTime(int v, bool beats) { return knobU(v) * (beats ? 0.128 : 0.064); }

bool insSupported(const std::string& pluginType) {
  return pluginType == "gl.synth" || pluginType == "gl.fmduo" || pluginType == "gl.pixeltone";
}

bool InsTarget::operator==(const InsTarget& o) const {
  return flags == o.flags && on == o.on && predelay == o.predelay && attack == o.attack && hold == o.hold && decay == o.decay &&
         sustain == o.sustain && release == o.release && amount == o.amount && lfoDelay == o.lfoDelay &&
         lfoAttack == o.lfoAttack && lfoAmount == o.lfoAmount && lfoSpeed == o.lfoSpeed && lfoShape == o.lfoShape &&
         tensionA == o.tensionA && tensionD == o.tensionD && tensionR == o.tensionR;
}

InsSettings::InsSettings() {
  t[InsVol].flags = 4;
  t[InsVol].tensionR = -101;
}

bool InsSettings::operator==(const InsSettings& o) const {
  for (int k = 0; k < InsTargets; ++k)
    if (!(t[k] == o.t[k])) return false;
  for (int k = 0; k < 2; ++k)
    for (int j = 0; j < 4; ++j)
      if (track[k][j] != o.track[k][j]) return false;
  return cutoff == o.cutoff && reso == o.reso && type == o.type && timing == o.timing;
}

bool InsSettings::tracking() const {
  for (int k = 0; k < 2; ++k)
    if (track[k][1] != 0 || track[k][2] != 0 || track[k][3] != 0) return true;
  return false;
}

bool InsSettings::active() const {
  if (cutoff < 256 || reso > 0 || tracking()) return true;
  for (const auto& x : t)
    if ((x.on && (x.amount != 0 || &x == &t[InsVol])) || x.lfoAmount != 0) return true;
  return false;
}

std::string formatIns(const InsSettings& s) {
  std::ostringstream o;
  for (int k = 0; k < InsTargets; ++k) {
    const InsTarget& x = s.t[k];
    o << "T " << x.flags << ' ' << (x.on ? 1 : 0) << ' ' << x.predelay << ' ' << x.attack << ' ' << x.hold << ' ' << x.decay << ' '
      << x.sustain << ' ' << x.release << ' ' << x.amount << ' ' << x.lfoDelay << ' ' << x.lfoAttack << ' ' << x.lfoAmount << ' '
      << x.lfoSpeed << ' ' << x.lfoShape << ' ' << x.tensionA << ' ' << x.tensionD << ' ' << x.tensionR << '\n';
  }
  o << "F " << s.cutoff << ' ' << s.reso << ' ' << s.type << '\n';
  if (s.track[0][0] != 100 || s.track[1][0] != 60 || s.tracking()) {
    o << 'K';
    for (const auto& r : s.track)
      for (int v : r) o << ' ' << v;
    o << '\n';
  }
  if (s.timing != InsTiming::Classic) o << "M " << static_cast<int>(s.timing) << '\n';
  return o.str();
}

bool parseIns(const std::string& text, InsSettings& s) {
  s = InsSettings{};
  if (text.empty()) return false;
  std::istringstream in(text);
  std::string line;
  int k = 0;
  bool ok = false;
  while (std::getline(in, line)) {
    std::istringstream l(line);
    std::string tag;
    l >> tag;
    if (tag == "T" && k < InsTargets) {
      InsTarget& x = s.t[k++];
      int on = 0;
      l >> x.flags >> on >> x.predelay >> x.attack >> x.hold >> x.decay >> x.sustain >> x.release >> x.amount >> x.lfoDelay >>
          x.lfoAttack >> x.lfoAmount >> x.lfoSpeed >> x.lfoShape >> x.tensionA >> x.tensionD >> x.tensionR;
      x.on = on != 0;
      x.lfoShape = std::clamp(x.lfoShape, 0, 2);
      x.sustain = std::clamp(x.sustain, 0, 128);
      ok = true;
    } else if (tag == "F") {
      l >> s.cutoff >> s.reso >> s.type;
      s.type = std::clamp(s.type, 0, 7);
      ok = true;
    } else if (tag == "K") {
      for (auto& r : s.track)
        for (int& v : r) l >> v;
    } else if (tag == "M") {
      int m = 1;
      l >> m;
      s.timing = m == 0 ? InsTiming::Smooth : InsTiming::Classic;
    }
  }
  return ok;
}

// ------------------------------------------------------------------ runtime

namespace {

// Ticks of a time knob (reference law; `scale` = ticks per u).
int knobTicks(int v, double scale) {
  if (v < 101) return 0;
  return static_cast<int>(static_cast<int64_t>(std::nearbyint(knobU(v) * scale)));
}

}  // namespace

void InsVoice::Ramp::begin(float target, int len) {
  // Ramp from the current value towards `target` over `len` samples; a slope
  // above the limit stops short of the target (continued in the next tick).
  const float start = cur;
  float st = target - start;
  if (st != 0.0f) {
    st = st / static_cast<float>(len);
    if (kMaxStep < std::fabs(st)) {
      st = std::copysign(kMaxStep, st);
      target = static_cast<float>(len) * st + start;
      if (belowTiny(target)) {
        target = 0.0f;
        st = -start / static_cast<float>(len);
      }
    }
  }
  step = st;
  even = start + 0.0f;
  odd = start + st;
  tail = 0;
  cur = target;
}

float InsVoice::Ramp::at(int i, int len) {
  // Two interleaved lanes advanced by two steps (blocks of 8 samples), then
  // the remainder one step at a time from the even lane.
  const int blocked = (len >> 3) << 3;
  const float two = step + step;
  if (i < blocked) {
    if ((i & 1) == 0) {
      const float g = even;
      even = even + two;
      return g;
    }
    const float g = odd;
    odd = odd + two;
    return g;
  }
  if (i == blocked) tail = even;
  const float g = tail;
  tail = tail + step;
  return g;
}

void InsVoice::derive() {
  const int ppq4 = ppq_ >> 2;
  const double base = ((static_cast<double>(ppq4) / 48.0) * 24576.0) / 1000.0;  // ticks per u, tempo-synced
  const double bpmF = static_cast<double>(static_cast<float>(bpm_));
  const double free = (base * bpmF) / 120.0;
  const double secPerBeat = 60.0 / bpm_;
  for (int k = 0; k < InsTargets; ++k) {
    const InsTarget& t = s_->t[k];
    Derived& d = d_[k];
    d = Derived{};
    d.on = t.on;
    d.global = (t.flags & 0x20) != 0;
    const double es = (t.flags & 1) ? base : free, ls = (t.flags & 2) ? base : free;
    d.predelayT = knobTicks(t.predelay, es);
    d.attackInc = static_cast<float>(1.0 / static_cast<double>(static_cast<int64_t>(knobTicks(t.attack, es)) + 1));
    d.sustain = static_cast<float>(t.sustain) * kInv128;
    d.holdT = knobTicks(t.hold, es);
    d.decayInc = static_cast<float>(-1.0 / static_cast<double>(static_cast<int64_t>(knobTicks(t.decay, es)) + 1));
    d.releaseInc = static_cast<float>(-1.0 / static_cast<double>(static_cast<int64_t>(knobTicks(t.release, es)) + 1));
    d.amt = static_cast<float>(t.amount) * kInv128;
    d.amtRaw = static_cast<float>(t.amount);
    const int tens[3] = {t.tensionA, t.tensionD, t.tensionR};
    for (int c = 0; c < 3; ++c) {
      Curve& cv = d.cv[c];
      const int v = tens[c];
      const float sign = v == 0 ? 0.0f : (v < 0 ? -1.0f : 1.0f);
      const float a = std::fabs(static_cast<float>(v) * kInv128);
      const double e = std::exp(static_cast<double>(a) * kTension);
      cv.c = (static_cast<float>(e - 1.0) * sign) * 0.100000001f;
      if (cv.c != 0.0f) {
        const float ac = std::fabs(cv.c);
        cv.k = 1.0f / ac;
        cv.L = static_cast<float>(std::log(static_cast<double>(ac + 1.0f)));
      }
    }
    d.lfoDelayT = knobTicks(t.lfoDelay, ls);
    d.lfoAttackInc = static_cast<float>(1.0 / static_cast<double>(static_cast<int64_t>(knobTicks(t.lfoAttack, ls)) + 1));
    const int period = std::max(3, knobTicks(t.lfoSpeed, ls));
    d.lfoInc = static_cast<uint32_t>(4294967296ull / static_cast<uint64_t>(period));
    d.lfoAmt = static_cast<float>(t.lfoAmount) * kInv128;
    d.lfoAmtRaw = static_cast<float>(t.lfoAmount);
    d.shape = std::clamp(t.lfoShape, 0, 2);
    // Smooth timing: the same knob laws in seconds.
    auto sec = [&](int v, bool sync) { return insKnobTime(v, sync) * (sync ? secPerBeat : 1.0); };
    const bool eSync = t.flags & 1, lSync = t.flags & 2;
    d.predelayS = sec(t.predelay, eSync), d.attackS = sec(t.attack, eSync), d.holdS = sec(t.hold, eSync);
    d.decayS = sec(t.decay, eSync), d.releaseS = sec(t.release, eSync);
    d.lfoDelayS = sec(t.lfoDelay, lSync), d.lfoAttackS = sec(t.lfoAttack, lSync);
    d.lfoHz = 1.0 / std::max(sec(t.lfoSpeed, lSync), 3.0 * 60.0 / (bpm_ * ppq_));
  }
}

namespace {

float curveFwd(float c, float k, float L, float x) {
  if (c == 0.0f) return x;
  const double xd = static_cast<double>(x);
  if (c > 0.0f) return static_cast<float>(1.0 - static_cast<double>(k) * (std::exp(static_cast<double>(L) * (1.0 - xd)) - 1.0));
  return static_cast<float>(static_cast<double>(k) * (std::exp(static_cast<double>(L) * xd) - 1.0));
}
float curveInv(float c, float L, float y) {
  if (c == 0.0f) return y;
  const double ac = static_cast<double>(std::fabs(c));
  if (c > 0.0f) return static_cast<float>(1.0 - std::log(ac * (1.0 - static_cast<double>(y)) + 1.0) / static_cast<double>(L));
  return static_cast<float>(std::log(ac * static_cast<double>(y) + 1.0) / static_cast<double>(L));
}
// Smooth timing: the same curves in double precision.
double curveFwdD(float c, double x) {
  if (c == 0.0f) return x;
  const double a = std::fabs(static_cast<double>(c)), L = std::log1p(a);
  return c > 0.0f ? 1.0 - (std::exp(L * (1.0 - x)) - 1.0) / a : (std::exp(L * x) - 1.0) / a;
}
double curveInvD(float c, double y) {
  if (c == 0.0f) return y;
  const double a = std::fabs(static_cast<double>(c)), L = std::log1p(a);
  return c > 0.0f ? 1.0 - std::log(a * (1.0 - y) + 1.0) / L : std::log(a * y + 1.0) / L;
}

}  // namespace

double insCurve(int tension, double x) {
  if (tension == 0) return x;
  const double a = (std::exp(std::abs(tension) / 128.0 * kTension) - 1.0) * 0.1;
  return curveFwdD(static_cast<float>(tension < 0 ? -a : a), x);
}

void InsVoice::start(const InsSettings* s, double sampleRate, double bpm, int ppq, double modX, double modY, double songTick) {
  s_ = s;
  if (!s) return;
  classic_ = s->classic();
  sr_ = sampleRate > 0 ? sampleRate : 44100.0;
  bpm_ = bpm > 0 ? bpm : 120.0;
  ppq_ = ppq > 0 ? ppq : 96;
  spt_ = sr_ * 60.0 / (bpm_ * ppq_);
  modX_ = modX, modY_ = modY;
  host_ = false, notePan_ = 0, noteVol_ = 1, trackPan_ = 0;
  derive();
  first_ = true;
  chunkLen_ = chunkPos_ = 0;
  gL_ = Ramp{}, gR_ = Ramp{};
  panOfs_ = 0, volGain_ = 1, modXv_ = modYv_ = 0, pitchCents_ = 0;
  // Place the voice on the song's tick grid.
  const double st = std::max(0.0, songTick);
  pos_ = static_cast<int64_t>(std::floor(st * spt_ + 0.5));
  tickIdx_ = static_cast<int64_t>(std::floor(st));
  while (static_cast<int64_t>(std::floor(static_cast<double>(tickIdx_) * spt_ + 0.5)) <= pos_) ++tickIdx_;
  for (int k = 0; k < InsTargets; ++k) {
    const InsTarget& t = s->t[k];
    const Derived& d = d_[k];
    Env& e = st_[k];
    e = Env{};
    if (d.predelayT < 1 || !t.on) {
      e.stage = kAttack;
      const bool full = !((((t.flags & 4) == 0) || t.on) && (!t.on || t.attack != 100));
      if (full) {
        e.x = 1.0f;
        e.stage = d.holdT < 1 ? kDecay : kHold;
      }
    } else {
      e.stage = kPredelay;
    }
    e.y = e.x;
    e.lfoDelayLeft = d.lfoDelayT;
    if (!classic_) {  // smooth timing: stage clock in seconds
      if (!t.on) {
        e.stage = kDone;
        e.lx = e.ly = (t.flags & 4) ? 1.0 : 0.0;
      } else if (d.predelayS > 0) {
        e.stage = kPredelay;
      } else if (d.attackS <= 0 || t.attack == 100) {
        e.stage = d.holdS > 0 ? kHold : kDecay;
        e.lx = e.ly = 1.0;
      } else {
        e.stage = kAttack;
      }
    }
    e.lfoPh = 0;
    if (d.global) {  // song-locked phase
      const double cycles = (st * 60.0 / (bpm_ * ppq_)) * d.lfoHz;
      e.lfoPh = cycles - std::floor(cycles);
    }
  }
  // Filter.
  static const int kMode[8] = {0, 1, 2, 3, 4, 1, 6, 6};
  mode_ = kMode[std::clamp(s->type, 0, 7)];
  doubled_ = s->type == 5 || s->type == 7;
  filterOn_ = mode_ > 0;
  fg_ = 1, fgStep_ = 0, fgTarget_ = 1, fd_ = 0;
  fv_[0] = fv_[1] = fy_[0] = fy_[1] = 0;
  std::memset(bx_, 0, sizeof bx_);
  std::memset(by_, 0, sizeof by_);
  std::memset(svfLow_, 0, sizeof svfLow_);
  std::memset(svfBand_, 0, sizeof svfBand_);
  b0_ = 1, b1_ = b2_ = a1_ = a2_ = 0, lpGain_ = 1;
  svfF_ = 0, svfQ_ = 1;
  ctl_ = 0;
  smoothFirst_ = true;
}

void InsVoice::setNote(double key, float velocity) {
  if (!s_ || !s_->tracking()) return;
  // The reference host adds the channel's tracking to the note's levels:
  // velocity term = note volume − volume of the middle velocity (volume law
  // (11^x − 1)/10 with x = velocity / 100 for the note, middle / 128 for the
  // middle), key term = cents from C5 + (middle − 60) · 100; each scaled by
  // amount / 128 (velocity) or amount / 307200 (key, i.e. per 2 octaves).
  const float v = static_cast<float>(std::nearbyint(static_cast<double>(velocity) * 128.0)) * 0.00999999978f;
  const float vol = v == 1.0f ? 1.0f : static_cast<float>(static_cast<double>(0.100000001f) * (std::exp(static_cast<double>(v * 2.39789534f)) - 1.0));
  const float m = static_cast<float>(s_->track[0][0]) * kInv128;
  const float volMid = m == 1.0f ? 1.0f : static_cast<float>(static_cast<double>(0.100000001f) * (std::exp(static_cast<double>(m * 2.39789534f)) - 1.0));
  const float velTerm = vol + -volMid;
  const float keyTerm = static_cast<float>((key - 60.0) * 100.0) + static_cast<float>((s_->track[1][0] - 60) * 100);
  float add[3];
  for (int j = 0; j < 3; ++j)
    add[j] = static_cast<float>(s_->track[1][j + 1]) * 3.25520841e-06f * keyTerm + static_cast<float>(s_->track[0][j + 1]) * kInv128 * velTerm;
  trackPan_ = add[0];
  notePan_ = add[0] + notePan_;
  modX_ = static_cast<double>(add[1] + static_cast<float>(modX_));
  modY_ = static_cast<double>(add[2] + static_cast<float>(modY_));
}

void InsVoice::setHostGains(float notePan, float noteVolume) {
  host_ = true;
  notePan_ = notePan + trackPan_;
  noteVol_ = noteVolume;
}

void InsVoice::release() {
  if (!s_) return;
  for (int k = 1; k < InsTargets; ++k) {  // the panning envelope never releases
    Env& e = st_[k];
    if (e.stage >= 7) continue;
    if (classic_) {
      e.x = curveInv(d_[k].cv[2].c, d_[k].cv[2].L, e.y);
    } else {
      e.lx = curveInvD(d_[k].cv[2].c, e.ly);
      e.t = 0;
    }
    e.counter = 0;
    e.stage = kRelease;
  }
}

bool InsVoice::done() const { return s_ && s_->t[InsVol].on && st_[InsVol].stage == kDone; }

int InsVoice::pitchHold() const {
  if (!s_) return 1 << 30;
  if (classic_) return std::max(1, chunkLen_ - chunkPos_);
  return std::max(1, kCtl - ctl_);
}

// ------------------------------------------------------------------ classic timing

void InsVoice::tickClassic() {
  for (int k = 0; k < InsTargets; ++k) {
    const Derived& d = d_[k];
    Env& e = st_[k];
    if (d.on) {
      switch (e.stage) {
        case kPredelay:
          if (d.predelayT <= ++e.counter) e.stage = kAttack;
          break;
        case kAttack: {
          const float nx = e.x + d.attackInc;
          e.x = nx;
          if (1.0f <= nx) {
            e.x = 1.0f;
            if (d.holdT < 1) {
              e.stage = kDecay;
            } else {
              e.counter = 0;
              e.stage = kHold;
            }
          }
          e.y = curveFwd(d.cv[0].c, d.cv[0].k, d.cv[0].L, e.x);
          break;
        }
        case kHold:
          if (d.holdT <= ++e.counter) e.stage = kDecay;
          break;
        case kDecay: {
          e.x = e.x + d.decayInc;
          const float y = curveFwd(d.cv[1].c, d.cv[1].k, d.cv[1].L, e.x);
          e.y = y;
          if (y <= d.sustain) {
            e.y = d.sustain;
            e.x = curveInv(d.cv[1].c, d.cv[1].L, d.sustain);
            e.counter = 0;
            e.stage = kSustain;
          }
          break;
        }
        case kRelease:
          e.x = e.x + d.releaseInc;
          e.y = curveFwd(d.cv[2].c, d.cv[2].k, d.cv[2].L, e.x);
          break;
        default: break;
      }
      if (e.stage > kHold && e.y < kTiny) e.y = 0.0f, e.stage = kDone;
    }
    if (e.lfoDelayLeft == 0) {
      if (d.global)
        e.lfoPhase = static_cast<uint32_t>((static_cast<uint64_t>(tickIdx_ - 1) * d.lfoInc) & 0xFFFFFFFFull);
      else
        e.lfoPhase += d.lfoInc;
      float out = lfoTable(d.shape, e.lfoPhase);
      if (e.lfoRamp != 1.0f) {
        const float r = e.lfoRamp + d.lfoAttackInc;
        e.lfoRamp = 1.0f < r ? 1.0f : r;
        out = out * e.lfoRamp;
      }
      e.lfoOut = out;
    } else {
      --e.lfoDelayLeft;
    }
  }
  const Derived* d = d_;
  panOfs_ = (st_[InsPan].lfoOut * d[InsPan].lfoAmt) * 2.0f;
  float g = (st_[InsVol].lfoOut * d[InsVol].lfoAmt + 1.0f) * st_[InsVol].y;
  if (g < 0.0f) g = 0.0f;
  volGain_ = g;
  modXv_ = st_[InsModX].lfoOut * d[InsModX].lfoAmt + st_[InsModX].y * d[InsModX].amt;
  modYv_ = st_[InsModY].lfoOut * d[InsModY].lfoAmt + st_[InsModY].y * d[InsModY].amt;
  pitchCents_ = static_cast<double>(d[InsPitch].lfoAmtRaw * st_[InsPitch].lfoOut + d[InsPitch].amtRaw * st_[InsPitch].y);
}

float InsVoice::law(float pan, float vol, bool left) const {
  const float p = std::clamp(pan, -1.0f, 1.0f);
  const long double a = (1.0L + static_cast<long double>(p)) * static_cast<long double>(0.78539816339744828);
  return static_cast<float>((left ? std::cos(a) : std::sin(a)) * static_cast<long double>(vol));
}

void InsVoice::gainTargets(float& gl, float& gr) const {
  if (host_) {
    const float pan = notePan_ + panOfs_, vol = noteVol_ * volGain_;
    gl = law(pan, vol, true);
    gr = law(pan, vol, false);
    return;
  }
  const float p = std::clamp(panOfs_ + trackPan_, -1.0f, 1.0f);
  gl = volGain_ * (p > 0.0f ? 1.0f - p : 1.0f);
  gr = volGain_ * (p < 0.0f ? 1.0f + p : 1.0f);
}

void InsVoice::filterCoefs(double xd, double rd) {
  const float x = static_cast<float>(xd), r = static_cast<float>(rd);
  const float F = static_cast<float>(44100.0 / sr_);
  if (mode_ == 0) {
    float g;
    if (0.0f < x) {
      if (x < 1.0f) {
        const double e = std::exp(static_cast<double>(x * 6.39859486f));
        g = static_cast<float>(static_cast<double>(0.00166666671f) * (e - 1.0)) * F;
        if (1.0f <= g) g = 1.0f;
      } else {
        g = 1.0f;
      }
    } else {
      g = 0.0f;
    }
    fgTarget_ = g;
    if (0.0f < r) {
      const float rr = 1.0f < r ? 1.0f : r;
      const double e = std::exp(static_cast<double>((1.0f - rr * 0.625f) * 6.39859486f));
      fd_ = 1.0f - static_cast<float>(static_cast<double>(0.00166666671f) * (e - 1.0));
    } else {
      fd_ = 0.0f;
    }
    if (fgTarget_ != 1.0f || fd_ != 0.0f) filterOn_ = true;
  } else if (mode_ == 6) {
    float f;
    if (0.0f < x) {
      f = (static_cast<float>(std::exp(static_cast<double>(x) * 3.2580965380214821) - 1.0) * 0.0399999991f) * F;
      if (1.0f <= f) f = 1.0f;
    } else {
      f = 0.0f;
    }
    svfF_ = f;
    svfQ_ = 0.0f < r ? (r < 1.0f ? 1.0f - r * 0.800000012f : 0.200000003f) : 1.0f;
  } else {
    const float lo = static_cast<float>(static_cast<double>(F) * 0.02);
    const double e = std::exp(static_cast<double>(x * 3.93182564f));
    float w = static_cast<float>(static_cast<double>(F) * (static_cast<double>(0.0595999993f) * (e - 1.0) + 0.02));
    if (w < lo) w = lo;
    else if (3.0f < w) w = 3.0f;
    float q;
    if (0.0f < r) {
      const float rr = 1.0f < r ? 1.0f : r;
      q = static_cast<float>(static_cast<double>(0.400000006f) * (std::exp(static_cast<double>(rr * 2.45739102f)) - 1.0) + 0.1);
    } else {
      q = 0.100000001f;
    }
    const float sn = std::sin(w), cs = std::cos(w);
    const double alpha = static_cast<double>(sn / (2.0f * q)), c = static_cast<double>(cs);
    const double dd = 1.0 / (1.0 + alpha);
    a1_ = static_cast<float>(-2.0 * c * dd);
    a2_ = static_cast<float>((1.0 - alpha) * dd);
    switch (mode_) {
      case 1:
        b1_ = static_cast<float>((1.0 - c) * dd);
        b0_ = b1_ * 0.5f;
        b2_ = b0_;
        break;
      case 2:
        b1_ = 0.0f;
        b0_ = static_cast<float>(dd * alpha);
        b2_ = static_cast<float>(-alpha * dd);
        break;
      case 3: {
        const float t = static_cast<float>(-(1.0 + c) * dd);
        b1_ = t;
        b0_ = -t * 0.5f;
        b2_ = b0_;
        break;
      }
      default:
        b1_ = a1_;
        b0_ = static_cast<float>(dd);
        b2_ = static_cast<float>(dd);
        break;
    }
    lpGain_ = doubled_ ? static_cast<float>(1.0 - (3.2 - static_cast<double>(w)) * static_cast<double>(q) * 0.046874999999999993) : 1.0f;
  }
}

void InsVoice::beginChunk() {
  // A tick boundary (or the note start): chunk length to the next tick.
  const int64_t next = static_cast<int64_t>(std::floor(static_cast<double>(tickIdx_) * spt_ + 0.5));
  chunkLen_ = static_cast<int>(std::max<int64_t>(1, next - pos_));
  chunkPos_ = 0;
  tickClassic();
  ++tickIdx_;
  float tl, tr;
  gainTargets(tl, tr);
  if (first_) {
    gL_.cur = gR_.cur = 0.0f;
    if (st_[InsVol].stage > kAttack) gL_.cur = tl, gR_.cur = tr;  // starts at full level: no fade-in
  }
  const float x = (static_cast<float>(s_->cutoff) * kInv256 + static_cast<float>(modX_)) + modXv_;
  const float r = (static_cast<float>(s_->reso) * kInv256 + static_cast<float>(modY_)) + modYv_;
  const float prevG = fgTarget_;
  filterCoefs(x, r);
  if (mode_ == 0 && filterOn_) {  // the coefficient ramps across the tick like the gains
    Ramp fr;
    fr.cur = first_ ? fgTarget_ : prevG;
    const float start = fr.cur;
    fr.begin(fgTarget_, chunkLen_);
    fg_ = start, fgStep_ = fr.step, fgTarget_ = fr.cur;
    fgAcc_ = static_cast<Ext>(start);
  }
  if (mode_ > 0 && mode_ < 6 && doubled_) tl *= lpGain_, tr *= lpGain_;
  gL_.begin(tl, chunkLen_);
  gR_.begin(tr, chunkLen_);
  first_ = false;
}

void InsVoice::filterSample(float& l, float& r, InsExt g) {
  float io[2] = {l, r};
  if (mode_ == 0) {
    if (filterOn_)
      for (int c = 0; c < 2; ++c) {
        fv_[c] = fv_[c] + (static_cast<Ext>(io[c]) - fy_[c]) * static_cast<Ext>(g);
        fy_[c] = fy_[c] + fv_[c];
        fv_[c] = fv_[c] * static_cast<Ext>(fd_);
        io[c] = static_cast<float>(fy_[c]);
      }
  } else if (mode_ == 6) {
    // The state runs at extended precision through a tick and is stored as
    // single precision at its end (see processWith).
    for (int st = 0; st < (doubled_ ? 2 : 1); ++st) {
      const Ext f = svfF_, q = st == 0 ? svfQ_ : (1.0f - svfQ_) * 0.25f + svfQ_;
      for (int c = 0; c < 2; ++c) {
        Ext& lo = svfLow_[st][c];
        Ext& bd = svfBand_[st][c];
        lo = bd * f + lo;
        bd = (static_cast<Ext>(io[c]) - (bd * q + lo)) * f + bd;
        io[c] = static_cast<float>(lo);
      }
    }
  } else {
    // Single-precision state and coefficients, the sum at extended precision.
    for (int st = 0; st < (doubled_ ? 2 : 1); ++st)
      for (int c = 0; c < 2; ++c) {
        float* x = bx_[st][c];
        float* y = by_[st][c];
        const float in = io[c];
        Ext acc = static_cast<Ext>(x[0]) * b1_ + static_cast<Ext>(x[1]) * b2_;
        acc = acc - static_cast<Ext>(y[0]) * a1_;
        acc = acc - static_cast<Ext>(y[1]) * a2_;
        acc = acc + static_cast<Ext>(in) * b0_;
        const float out = static_cast<float>(acc);
        x[1] = x[0], x[0] = in, y[1] = y[0], y[0] = out;
        io[c] = out;
      }
  }
  l = io[0], r = io[1];
}

// ------------------------------------------------------------------ smooth timing

void InsVoice::controlSmooth() {
  const double dt = kCtl / sr_;
  for (int k = 0; k < InsTargets; ++k) {
    const Derived& d = d_[k];
    const InsTarget& t = s_->t[k];
    Env& e = st_[k];
    if (t.on) {
      e.t += dt;
      switch (e.stage) {
        case kPredelay:
          if (e.t >= d.predelayS) e.stage = kAttack, e.t = 0;
          break;
        case kAttack:
          e.lx = d.attackS > 0 ? e.lx + dt / d.attackS : 1.0;
          if (e.lx >= 1.0) e.lx = 1.0, e.t = 0, e.stage = d.holdS > 0 ? kHold : kDecay;
          e.ly = curveFwdD(d.cv[0].c, e.lx);
          break;
        case kHold:
          if (e.t >= d.holdS) e.stage = kDecay, e.t = 0;
          break;
        case kDecay: {
          const double sus = d.sustain;
          e.lx = d.decayS > 0 ? e.lx - dt / d.decayS : -1.0;
          e.ly = curveFwdD(d.cv[1].c, std::max(0.0, e.lx));
          if (e.ly <= sus) e.ly = sus, e.lx = curveInvD(d.cv[1].c, sus), e.stage = kSustain;
          break;
        }
        case kRelease:
          e.lx = d.releaseS > 0 ? e.lx - dt / d.releaseS : -1.0;
          e.ly = e.lx <= 0 ? 0.0 : curveFwdD(d.cv[2].c, e.lx);
          break;
        default: break;
      }
      if (e.stage > kHold && e.stage != kDone && e.ly < 1e-6) e.ly = 0, e.stage = kDone;
    }
    // LFO.
    e.lfoT += dt;
    if (e.lfoT < d.lfoDelayS) {
      e.lfoV = 0;
    } else {
      const double ramp = d.lfoAttackS > 0 ? std::min(1.0, (e.lfoT - d.lfoDelayS) / d.lfoAttackS) : 1.0;
      e.lfoPh += d.lfoHz * dt;
      e.lfoPh -= std::floor(e.lfoPh);
      e.lfoV = lfoShape(d.shape, e.lfoPh) * ramp;
    }
  }
  const Derived* d = d_;
  const double lfoV[InsTargets] = {st_[0].lfoV, st_[1].lfoV, st_[2].lfoV, st_[3].lfoV, st_[4].lfoV};
  const double gain = std::max(0.0, (1.0 + lfoV[InsVol] * d[InsVol].lfoAmt) * st_[InsVol].ly);
  const double pan = std::clamp(2.0 * lfoV[InsPan] * d[InsPan].lfoAmt, -1.0, 1.0);
  tgtX_ = s_->cutoff / 256.0 + modX_ + st_[InsModX].ly * d[InsModX].amt + lfoV[InsModX] * d[InsModX].lfoAmt;
  tgtY_ = s_->reso / 256.0 + modY_ + st_[InsModY].ly * d[InsModY].amt + lfoV[InsModY] * d[InsModY].lfoAmt;
  const double pitch = d[InsPitch].amtRaw * st_[InsPitch].ly + d[InsPitch].lfoAmtRaw * lfoV[InsPitch];
  // One-pole smoothing (≈ 2 ms) removes the steps of pulse LFOs and instant stages.
  const double a = smoothFirst_ ? 1.0 : 1.0 - std::exp(-dt / 0.002);
  sGain_ += (gain - sGain_) * a;
  sPan_ += (pan - sPan_) * a;
  sX_ += (tgtX_ - sX_) * a;
  sY_ += (tgtY_ - sY_) * a;
  sPitch_ += (pitch - sPitch_) * a;
  pitchCents_ = sPitch_;
  panOfs_ = static_cast<float>(sPan_);
  volGain_ = static_cast<float>(sGain_);
  float tl, tr;
  gainTargets(tl, tr);
  filterCoefs(sX_, sY_);
  if (mode_ > 0 && mode_ < 6 && doubled_) tl *= lpGain_, tr *= lpGain_;
  if (smoothFirst_) {
    gL_.cur = tl, gR_.cur = tr;
    fg_ = fgTarget_;
  }
  // Per-sample linear interpolation to the new values over the control period.
  gL_.even = gL_.cur, gL_.step = (tl - gL_.cur) / kCtl, gL_.cur = tl;
  gR_.even = gR_.cur, gR_.step = (tr - gR_.cur) / kCtl, gR_.cur = tr;
  fgStep_ = (fgTarget_ - fg_) / kCtl;
  smoothFirst_ = false;
}

// ------------------------------------------------------------------ per sample

void InsVoice::sync() {
  if (!s_) return;
  if (classic_) {
    if (first_ || chunkPos_ >= chunkLen_) beginChunk();
  } else if (smoothFirst_ || ctl_ >= kCtl) {
    controlSmooth();
    ctl_ = 0;
  }
}

void InsVoice::processWith(float& l, float& r, float pre) {
  if (!s_) return;
  sync();
  if (classic_) {
    const int i = chunkPos_++;
    ++pos_;
    filterSample(l, r, fgAcc_);
    fgAcc_ += static_cast<Ext>(fgStep_);
    if (chunkPos_ >= chunkLen_) {  // end of the tick: flush tiny filter states
      for (int c = 0; c < 2; ++c)  // stored as double, tiny values flushed
        fv_[c] = flushTiny(static_cast<double>(fv_[c])), fy_[c] = flushTiny(static_cast<double>(fy_[c]));
      for (auto& a : bx_)
        for (auto& b : a)
          for (float& v : b) v = flushTiny(v);
      for (auto& a : by_)
        for (auto& b : a)
          for (float& v : b) v = flushTiny(v);
      for (auto& a : svfLow_)
        for (auto& v : a) v = flushTiny(static_cast<float>(v));
      for (auto& a : svfBand_)
        for (auto& v : a) v = flushTiny(static_cast<float>(v));
      fg_ = fgTarget_, fgStep_ = 0;
    }
    if (pre != 1.0f) l *= pre, r *= pre;
    const float gl = gL_.at(i, chunkLen_), gr = gR_.at(i, chunkLen_);
    l = l * gl;
    r = r * gr;
    return;
  }
  const int i = ctl_++;
  filterSample(l, r, static_cast<double>(fg_) + static_cast<double>(fgStep_) * (i + 1));
  if (ctl_ >= kCtl) fg_ = fgTarget_;
  if (pre != 1.0f) l *= pre, r *= pre;
  const float gl = gL_.even + gL_.step * static_cast<float>(i + 1), gr = gR_.even + gR_.step * static_cast<float>(i + 1);
  l *= gl, r *= gr;
}

}  // namespace gl
