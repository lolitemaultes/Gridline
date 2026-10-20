// Gridline — channel instrument envelopes (J-33). See ChannelIns.h.
#include "ChannelIns.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace gl {

namespace {

constexpr double kTwoPi = 6.283185307179586;

// Tension → curve constant.
double curveC(int t) {
  if (t == 0) return 0.0;
  const double c = (std::exp(std::abs(t) / 128.0 * 8.75573753930647) - 1.0) * 0.1;
  return t < 0 ? -c : c;
}
double shape(double c, double x) {
  if (c == 0.0) return x;
  const double a = std::fabs(c), L = std::log1p(a), k = 1.0 / a;
  return c > 0 ? 1.0 - k * (std::exp(L * (1.0 - x)) - 1.0) : k * (std::exp(L * x) - 1.0);
}
double inverse(double c, double y) {
  if (c == 0.0) return y;
  const double a = std::fabs(c), L = std::log1p(a);
  return c > 0 ? 1.0 - std::log(a * (1.0 - y) + 1.0) / L : std::log(a * y + 1.0) / L;
}
double lfoTable(int shapeIdx, uint32_t phase) {
  const int i = static_cast<int>(phase >> 18);  // 0..16383
  switch (shapeIdx) {
    case 1:  // triangle, same phase as the sine
      if (i < 4096) return i / 4096.0;
      if (i < 12288) return 1.0 - (i - 4096) / 4096.0;
      return -1.0 + (i - 12288) / 4096.0;
    case 2: return i < 8192 ? 1.0 : -1.0;
    default: return std::sin(kTwoPi * i / 16384.0);
  }
}

}  // namespace

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
  return cutoff == o.cutoff && reso == o.reso && type == o.type;
}

bool InsSettings::active() const {
  if (cutoff < 256 || reso > 0) return true;
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
    }
  }
  return ok;
}

// ------------------------------------------------------------------ runtime
double InsVoice::ticks(int v, bool sync) const {
  const double u = v <= 100 ? 0.0 : std::exp(v / 65536.0 * 6.90875477931522) - 1.0;
  const double S = (ppq_ / 4.0) / 48.0 * 24.576 * (sync ? 1.0 : bpm_ / 120.0);
  return std::round(u * S);
}

void InsVoice::start(const InsSettings* s, double sampleRate, double bpm, int ppq, double modX, double modY, double songTick) {
  s_ = s;
  if (!s) return;
  sr_ = sampleRate > 0 ? sampleRate : 44100.0;
  bpm_ = bpm > 0 ? bpm : 120.0;
  ppq_ = ppq > 0 ? ppq : 96;
  spt_ = sr_ * 60.0 / (bpm_ * ppq_);
  modX_ = modX, modY_ = modY;
  tickPos_ = 0;
  first_ = true;
  for (int k = 0; k < InsTargets; ++k) {
    const InsTarget& t = s->t[k];
    Env& e = st_[k];
    e = Env{};
    const bool envSync = t.flags & 1, lfoSync = t.flags & 2;
    if (t.on && ticks(t.predelay, envSync) >= 1) {
      e.stage = kPredelay;
    } else if ((!t.on && (t.flags & 4)) || (t.on && t.attack == 100)) {
      e.x = e.y = 1;
      e.stage = t.on && ticks(t.hold, envSync) >= 1 ? kHold : kDecay;
    } else {
      e.stage = kAttack;
    }
    if (!t.on) e.stage = kDone, e.y = (t.flags & 4) ? 1.0 : 0.0;
    e.lfoDelayLeft = static_cast<int>(ticks(t.lfoDelay, lfoSync));
    e.lfoInc = static_cast<uint32_t>(4294967296.0 / std::max(3.0, ticks(t.lfoSpeed, lfoSync)));
    if (t.flags & 0x20)  // global: one song-locked phase shared by every voice
      e.lfoPhase = static_cast<uint32_t>((static_cast<uint64_t>(std::max(0.0, std::floor(songTick))) * e.lfoInc) & 0xFFFFFFFFull);
  }
  // Filter mode.
  static const int kMode[8] = {0, 1, 2, 3, 4, 1, 6, 6};
  mode_ = kMode[std::clamp(s->type, 0, 7)];
  doubled_ = s->type == 5 || s->type == 7;
  fastOn_ = false;
  fv_[0] = fv_[1] = fy_[0] = fy_[1] = 0;
  for (auto& a : bx_)
    for (auto& b : a)
      for (double& c : b) c = 0;
  for (auto& a : by_)
    for (auto& b : a)
      for (double& c : b) c = 0;
  for (auto& a : svfLow_)
    for (double& c : a) c = 0;
  for (auto& a : svfBand_)
    for (double& c : a) c = 0;
}

void InsVoice::release() {
  if (!s_) return;
  for (int k = 1; k < InsTargets; ++k) {  // the panning envelope never releases
    const InsTarget& t = s_->t[k];
    Env& e = st_[k];
    if (!t.on || e.stage >= kRelease) continue;
    e.x = inverse(curveC(t.tensionR), e.y);
    e.counter = 0;
    e.stage = kRelease;
  }
}

void InsVoice::tick() {
  double lfo[InsTargets];
  for (int k = 0; k < InsTargets; ++k) {
    const InsTarget& t = s_->t[k];
    Env& e = st_[k];
    const bool sync = t.flags & 1;
    if (t.on) {
      switch (e.stage) {
        case kPredelay:
          if (++e.counter >= ticks(t.predelay, sync)) e.stage = kAttack, e.counter = 0;
          break;
        case kAttack:
          e.x += 1.0 / (ticks(t.attack, sync) + 1.0);
          if (e.x >= 1.0) {
            e.x = 1.0;
            e.counter = 0;
            e.stage = ticks(t.hold, sync) >= 1 ? kHold : kDecay;
          }
          e.y = shape(curveC(t.tensionA), e.x);
          break;
        case kHold:
          if (++e.counter >= ticks(t.hold, sync)) e.stage = kDecay;
          break;
        case kDecay: {
          const double sus = t.sustain / 128.0;
          e.x -= 1.0 / (ticks(t.decay, sync) + 1.0);
          e.y = shape(curveC(t.tensionD), std::max(0.0, e.x));
          if (e.y <= sus) {
            e.y = sus;
            e.x = inverse(curveC(t.tensionD), sus);
            e.stage = kSustain;
          }
          break;
        }
        case kRelease:
          e.x -= 1.0 / (ticks(t.release, sync) + 1.0);
          e.y = e.x <= 0 ? 0.0 : shape(curveC(t.tensionR), e.x);
          break;
        default: break;
      }
      if (e.stage >= kDecay && e.stage != kDone && e.y < 5.96e-8) e.y = 0, e.stage = kDone;
    }
    // LFO.
    if (e.lfoDelayLeft > 0) {
      --e.lfoDelayLeft;
      e.lfoOut = 0;
    } else {
      e.lfoRamp = t.lfoAttack <= 100 ? 1.0 : std::min(1.0, e.lfoRamp + 1.0 / (ticks(t.lfoAttack, t.flags & 2) + 1.0));
      e.lfoOut = lfoTable(t.lfoShape, e.lfoPhase) * e.lfoRamp;
      e.lfoPhase += e.lfoInc;
    }
    lfo[k] = e.lfoOut;
  }
  const InsTarget* t = s_->t;
  const double gain = std::max(0.0, (1.0 + lfo[InsVol] * t[InsVol].lfoAmount / 128.0) * st_[InsVol].y);
  const double pan = std::clamp(2.0 * lfo[InsPan] * t[InsPan].lfoAmount / 128.0, -1.0, 1.0);
  const double x = s_->cutoff / 256.0 + modX_ + st_[InsModX].y * t[InsModX].amount / 128.0 + lfo[InsModX] * t[InsModX].lfoAmount / 128.0;
  const double r = s_->reso / 256.0 + modY_ + st_[InsModY].y * t[InsModY].amount / 128.0 + lfo[InsModY] * t[InsModY].lfoAmount / 128.0;
  pitchCents_ = std::round(t[InsPitch].amount * st_[InsPitch].y + t[InsPitch].lfoAmount * lfo[InsPitch]);
  // Filter coefficients.
  const double F = 44100.0 / sr_;
  double fg = 1, lpg = 1;
  if (mode_ == 0) {
    fg = x <= 0 ? 0.0 : (x >= 1 ? 1.0 : std::min(1.0, F * (std::pow(601.0, x) - 1.0) / 600.0));
    fd_ = r <= 0 ? 0.0 : 1.0 - (std::pow(601.0, 1.0 - 0.625 * std::min(r, 1.0)) - 1.0) / 600.0;
    if (fg != 1.0 || fd_ != 0.0) fastOn_ = true;
  } else if (mode_ == 6) {
    svfF_ = x <= 0 ? 0.0 : std::min(1.0, F * (std::pow(26.0, x) - 1.0) * 0.04);
    svfQ_ = r <= 0 ? 1.0 : (r >= 1 ? 0.2 : 1.0 - 0.8 * r);
  } else {
    const double w = std::clamp(F * (0.0596 * (std::pow(51.0, x) - 1.0) + 0.02), F * 0.02, 3.0);
    const double Q = r <= 0 ? 0.1 : 0.4 * (std::exp(std::min(r, 1.0) * 2.457391) - 1.0) + 0.1;
    const double al = std::sin(w) / (2.0 * Q), cw = std::cos(w), a0 = 1.0 + al;
    a1_ = -2.0 * cw / a0, a2_ = (1.0 - al) / a0;
    switch (mode_) {
      case 1: b1_ = (1.0 - cw) / a0, b0_ = b2_ = b1_ * 0.5; break;
      case 2: b0_ = al / a0, b1_ = 0, b2_ = -al / a0; break;
      case 3: b1_ = -(1.0 + cw) / a0, b0_ = b2_ = (1.0 + cw) / (2.0 * a0); break;
      default: b0_ = b2_ = 1.0 / a0, b1_ = a1_; break;
    }
    if (doubled_) lpg = 1.0 - (3.2 - w) * Q * 0.046875;
  }
  lpGain_ = lpg;
  if (first_) {
    gain_ = gain, pan_ = pan, fg_ = fg;
    gainStep_ = panStep_ = fgStep_ = 0;
    first_ = false;
  } else {
    gainStep_ = (gain - gain_) / spt_;
    panStep_ = (pan - pan_) / spt_;
    fgStep_ = (fg - fg_) / spt_;
  }
  gainTarget_ = gain, panTarget_ = pan, fgTarget_ = fg;
}

void InsVoice::process(float& l, float& r) {
  if (!s_) return;
  if (tickPos_ <= 0) {
    tick();
    tickPos_ += spt_;
  }
  tickPos_ -= 1;
  double io[2] = {l, r};
  for (int c = 0; c < 2; ++c) {
    double v = io[c];
    if (mode_ == 0) {
      if (fastOn_) {
        fv_[c] += (v - fy_[c]) * fg_;
        fy_[c] += fv_[c];
        fv_[c] *= fd_;
        v = fy_[c];
      }
    } else if (mode_ == 6) {
      for (int st = 0; st < (doubled_ ? 2 : 1); ++st) {
        const double q = st == 0 ? svfQ_ : svfQ_ + (1.0 - svfQ_) * 0.25;
        svfLow_[st][c] += svfF_ * svfBand_[st][c];
        svfBand_[st][c] += svfF_ * (v - q * svfBand_[st][c] - svfLow_[st][c]);
        v = svfLow_[st][c];
      }
    } else {
      for (int st = 0; st < (doubled_ ? 2 : 1); ++st) {
        double* x = bx_[st][c];
        double* y = by_[st][c];
        const double out = b0_ * v + b1_ * x[0] + b2_ * x[1] - a1_ * y[0] - a2_ * y[1];
        x[1] = x[0], x[0] = v, y[1] = y[0], y[0] = std::fabs(out) < 1e-30 ? 0.0 : out;
        v = y[0];
      }
    }
    io[c] = v;
  }
  const double g = gain_ * lpGain_;
  const double pl = pan_ > 0 ? 1.0 - pan_ : 1.0, pr = pan_ < 0 ? 1.0 + pan_ : 1.0;
  l = static_cast<float>(io[0] * g * pl);
  r = static_cast<float>(io[1] * g * pr);
  if (gainStep_ != 0) {
    gain_ += gainStep_;
    if ((gainStep_ > 0 && gain_ > gainTarget_) || (gainStep_ < 0 && gain_ < gainTarget_)) gain_ = gainTarget_, gainStep_ = 0;
  }
  if (panStep_ != 0) {
    pan_ += panStep_;
    if ((panStep_ > 0 && pan_ > panTarget_) || (panStep_ < 0 && pan_ < panTarget_)) pan_ = panTarget_, panStep_ = 0;
  }
  if (fgStep_ != 0) {
    fg_ += fgStep_;
    if ((fgStep_ > 0 && fg_ > fgTarget_) || (fgStep_ < 0 && fg_ < fgTarget_)) fg_ = fgTarget_, fgStep_ = 0;
  }
}

}  // namespace gl
