// Gridline — "samplers" pack, J-33 group "dwave": Wave Program runtime (see
// dwave.h). Control values (matrix, mod envelopes, LFOs, glide, filter
// coefficients) update once per control block of 256 samples (8 with "high
// quality") on the instrument's own sample clock; the amp envelope, gains,
// filters, insert effects and buses run per sample.
#include "../../engine/TimeStretch.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include "../../engine/Dsp.h"
#include "../Packs.h"
#include "../synths/hex_fx.h"
#include "dwave.h"
#include "samplers.h"

namespace gl::packs::samplers {

using namespace dw;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr int kVoices = 256;

double frq(double v) { return 440.0 * std::exp2((127.0 * v) / 12.0 - 5.0); }
double timeOf(double v) { return 0.001 + 10.0 * v * v * v * v; }
double c01(double v) { return std::clamp(v, 0.0, 1.0); }
double amount(double v) {
  const double a = 2.0 * v - 1.0;
  return a * a * a;
}

struct WpAux final : AuxData {
  DwProgram prog;
  std::vector<SamplePtr> samples;  // per zone (null = missing)
};

struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double z1[2] = {0, 0}, z2[2] = {0, 0};
  double run(double x, int c) {
    const double y = b0 * x + z1[c];
    z1[c] = b1 * x - a1 * y + z2[c];
    z2[c] = b2 * x - a2 * y;
    return y;
  }
  void clear() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
  // RBJ cookbook: 1 LP, 2 HP, 3 band pass (constant skirt), 4 notch, 5 all-pass.
  void set(int type, double hz, double Q, double sr) {
    const double w = kTwoPi * std::clamp(hz, 1.0, 0.49 * sr) / sr, sn = std::sin(w), cs = std::cos(w), al = sn / (2.0 * Q);
    double B0 = 1, B1 = 0, B2 = 0;
    const double A0 = 1 + al, A1 = -2 * cs, A2 = 1 - al;
    switch (type) {
      case 1: B0 = (1 - cs) / 2, B1 = 1 - cs, B2 = (1 - cs) / 2; break;
      case 2: B0 = (1 + cs) / 2, B1 = -(1 + cs), B2 = (1 + cs) / 2; break;
      case 3: B0 = sn / 2, B1 = 0, B2 = -sn / 2; break;
      case 4: B0 = 1, B1 = -2 * cs, B2 = 1; break;
      default: B0 = 1 - al, B1 = -2 * cs, B2 = 1 + al; break;
    }
    b0 = B0 / A0, b1 = B1 / A0, b2 = B2 / A0, a1 = A1 / A0, a2 = A2 / A0;
  }
};

struct Env {
  int st = 0;  // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
  double e = 0;
  void trig() { st = 1, e = 0; }
  void rel() {
    if (st != 0) st = 4;
  }
  // One tick with per-tick steps; returns the output e².
  double tick(double a, double d, double s, double r) {
    switch (st) {
      case 1:
        e += a;
        if (e >= 1) e = 1, st = 2;
        break;
      case 2:
        e -= d;
        if (e <= s) e = s, st = 3;
        break;
      case 3: e = s; break;
      case 4:
        e -= r;
        if (e <= 0) e = 0, st = 0;
        break;
      default: break;
    }
    return e * e;
  }
};

struct Lfo {
  double phi = 0, ramp = 1, sh = 0, lp = 0, out = 0;
};

uint32_t g_seed = 0x2F6B3A1u;

class WaveProgramGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : v_)
      if (v.on) return true;
    return tail_ > 0;
  }

 protected:
  struct Voice {
    bool on = false;
    uint32_t id = 0;
    int zone = -1, note = 60;
    double vel = 0, rnd = 0, flFine = 0;
    double pos = 0, dir = 1;
    bool ended = false, fast = false, released = false;
    Env amp, menv[2];
    Lfo lfo[2];
    double glideNote = 60, glideTarget = 60, glideStep = 0;
    double dest[48] = {};
    double inc = 0;
    double tgL = 0, tgR = 0, gL = 0, gR = 0;
    double cut[2] = {440, 440}, q[2] = {1, 1};
    bool ctlFresh = true;
    Biquad f[2][2];
    double follower = 0, followerBlock = 0;
    double aStep = 1, dStep = 1, sLvl = 1, rStep = 1;
    double ringPh = 0, decCount = 0, decL = 0, decR = 0;
    double phS[2][8] = {}, phY[2] = {0, 0};
    uint64_t age = 0;
  };

  void configure(const PluginState& st, const ChannelAux& aux, const ProcessContext& ctx) override {
    const double sr = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    tempo_ = ctx.tempo > 1 ? ctx.tempo : 120.0;
    a_ = dynamic_cast<const WpAux*>(aux.custom.get());
    for (int i = 0; i < DwCount; ++i) t_[i] = i < static_cast<int>(st.params.size()) ? st.params[static_cast<size_t>(i)] : 0.f;
    if (sr != sr_ || dl_[0].empty()) {
      sr_ = sr;
      dl_[0].assign(65536, 0.f), dl_[1].assign(65536, 0.f);
      ch_[0].assign(16384, 0.f), ch_[1].assign(16384, 0.f);
      rev_.configure(sr_);
      dTime_ = -1, cD_[0] = cD_[1] = -1;
    }
    block_ = a_ && a_->prog.hq ? 8 : 256;
    if (!a_) return;
    const DwProgram& p = a_->prog;
    // Reverb: the plate engine in this instrument's build (wet tail only, a send bus).
    rev_.setSamplerMode(t_[DwRevRoom], t_[DwRevDamp], t_[DwRevDiffusion], t_[DwRevDecay]);
    // Delay feedback filters.
    for (int c = 0; c < 2; ++c) {
      dHp_[c].set(2, frq(t_[DwDelayLowCut]), 0.70711, sr_);
      dLp_[c].set(1, frq(t_[DwDelayHighCut]), 0.70711, sr_);
    }
    (void)p;
  }

  double ctlRate() const { return sr_ / block_; }

  // Source values of the matrix.
  double source(const Voice& v, int s) const {
    const DwZone& z = a_->prog.zones[static_cast<size_t>(v.zone)];
    switch (s) {
      case 1: return v.note / 127.0;
      case 2: return v.vel;
      case 3: return a_->prog.cc1;
      case 4: return 0.0;  // pitch bend
      case 5: return plfo_[0].out;
      case 6: return plfo_[1].out;
      case 9: return v.menv[0].e * v.menv[0].e;
      case 10: return v.menv[1].e * v.menv[1].e;
      case 11: return v.lfo[0].out;
      case 12: return v.lfo[1].out;
      case 13: return v.followerBlock;
      case 14: {
        const SamplePtr& sp = a_->samples[static_cast<size_t>(v.zone)];
        return sp && sp->frames() ? v.pos / static_cast<double>(sp->frames()) : 0.0;
      }
      case 15: return v.rnd;
      case 16: case 17: case 18: case 19: return t_[DwModVal1 + s - 16];
      default: (void)z; return 0.0;
    }
  }

  static double lfoShape(int w, Lfo& l) {
    const double p = l.phi;
    switch (w) {
      case 0: return std::sin(p);
      case 1: return std::fabs(std::sin(p));
      case 2: return p < kPi ? -1.0 + 2.0 * p / kPi : 3.0 - 2.0 * p / kPi;
      case 3: return p < kPi ? -1.0 : 1.0;
      case 4: return p / kPi - 1.0;
      case 5: return 1.0 - p / kPi;
      case 6: return l.sh;
      default: return l.lp;
    }
  }
  void stepLfo(Lfo& l, const DwLfo& d, double rate, double att, bool advance) {
    const double R = ctlRate();
    double inc;
    if (d.sync) {
      const double ticks = std::floor(127.5 * rate * rate + 1.0);
      inc = kTwoPi / (R * 15.0 / tempo_ * ticks);
    } else {
      inc = 20.0 * rate * rate * kTwoPi / R;
    }
    if (advance) {
      l.phi += inc;
      while (l.phi >= kTwoPi) {
        l.phi -= kTwoPi;
        g_seed = g_seed * 1664525u + 1013904223u;
        l.sh = (g_seed >> 8) * (2.0 / 16777216.0) - 1.0;
      }
      l.lp += (l.sh - l.lp) * std::min(1.0, inc);
      l.ramp = std::min(1.0, l.ramp + 1.0 / (R * timeOf(att)));
    }
    l.out = lfoShape(d.wave, l) * l.ramp;
  }
  void trigLfo(Lfo& l, double phase) {
    if (phase < 1.0) l.phi = kTwoPi * phase;
    l.ramp = 0;
  }

  // Control values of a voice (once per block; `advance` steps its block-rate parts).
  void control(Voice& v, bool advance) {
    const DwProgram& p = a_->prog;
    const DwZone& z = p.zones[static_cast<size_t>(v.zone)];
    const double R = ctlRate();
    if (advance) {
      for (int k = 0; k < 2; ++k)
        v.menv[k].tick(1.0 / (R * timeOf(z.menv[k][0])), 1.0 / (R * timeOf(z.menv[k][1])), z.menv[k][2], 1.0 / (R * timeOf(z.menv[k][3])));
      v.followerBlock = v.follower;
      // Glide.
      if (v.glideNote != v.glideTarget) {
        if (std::fabs(v.glideTarget - v.glideNote) <= v.glideStep) v.glideNote = v.glideTarget;
        else v.glideNote += v.glideTarget > v.glideNote ? v.glideStep : -v.glideStep;
      }
    }
    for (int k = 0; k < 2; ++k) stepLfo(v.lfo[k], z.lfo[k], z.lfo[k].rate, z.lfo[k].attack, advance);
    // Matrix on normalised values.
    const SamplePtr& sp = a_->samples[static_cast<size_t>(v.zone)];
    const double frames = sp ? static_cast<double>(std::max<size_t>(1, sp->frames())) : 1.0;
    double* d = v.dest;
    d[1] = z.tune, d[2] = p.compatVelocity ? z.gain : 1.0, d[3] = c01(z.pan);
    d[4] = z.sampleStart / frames, d[5] = z.loopStart / frames, d[6] = z.loopEnd / frames;
    for (int f = 0; f < 2; ++f) d[7 + 3 * f] = z.f[f].cut, d[8 + 3 * f] = z.f[f].res, d[9 + 3 * f] = z.f[f].shape;
    for (int k = 0; k < 4; ++k) d[13 + k] = z.amp[k];
    for (int k = 0; k < 4; ++k) d[17 + 2 * k] = z.fxA[k], d[18 + 2 * k] = z.fxB[k];
    for (int k = 0; k < 4; ++k) d[25 + k] = z.send[k];
    d[29] = z.tsTime, d[30] = z.tsGrain, d[31] = z.tsSmooth;
    for (int k = 0; k < 16; ++k) d[32 + k] = z.slot[k].amt;
    bool targeted[48] = {};
    for (int k = 15; k >= 0; --k) {  // slots scaling other slots first
      const DwSlot& s = z.slot[k];
      if (s.src <= 0 || s.dst < 32 || s.dst > 47) continue;
      d[s.dst] = c01(d[s.dst] + amount(d[32 + k]) * source(v, s.src));
    }
    for (int k = 0; k < 16; ++k) {
      const DwSlot& s = z.slot[k];
      if (s.src <= 0 || s.dst <= 0 || s.dst >= 32) continue;
      targeted[s.dst] = true;
      const double a = amount(d[32 + k]);
      if (s.dst == 2 && s.src == 2 && !p.compatVelocity) d[2] *= a * v.vel;
      else d[s.dst] += a * source(v, s.src);
      if (s.dst != 2 || p.clipVolume) d[s.dst] = c01(d[s.dst]);
    }
    // Pitch (cents) → increment.
    const double cents = (c01(d[1]) * 2400.0 - 1200.0) + z.fine + 100.0 * z.transpose + z.keytrack * (v.glideNote - z.root) + v.flFine;
    const double srate = sp ? sp->sampleRate : z.rate;
    v.inc = std::exp2(cents / 1200.0) * srate / sr_;
    if (z.beatSync && z.ticks > 0 && sp) v.inc *= (frames / srate) / (z.ticks * 15.0 / tempo_);
    // Gains (equal power, −6 dB per side at centre).
    const double level = d[2] * (p.compatVelocity ? 1.0 : z.gain) * t_[DwMaster] * p.cc7 * p.cc11;
    const double pan = c01(d[3]), cp = p.cc10;
    v.tgL = level * std::sqrt((1 - cp) * (1 - pan)) * (p.compatVelocity ? 2.0 : 1.0);
    v.tgR = level * std::sqrt(cp * pan) * (p.compatVelocity ? 2.0 : 1.0);
    // Amp envelope steps (per sample).
    v.aStep = 1.0 / (sr_ * timeOf(c01(d[13])));
    v.dStep = 1.0 / (sr_ * timeOf(c01(d[14])));
    v.sLvl = c01(d[15]);
    v.rStep = 1.0 / (sr_ * timeOf(v.fast ? 0.25 : c01(d[16])));
    // Filters (cutoff / resonance smoothed per block).
    const double cf = std::exp(-1.0 / (R * 0.005));
    for (int f = 0; f < 2; ++f) {
      if (z.f[f].type == 0) continue;
      const double hz = frq(c01(d[7 + 3 * f])), Q = 0.70711 / (1.01 - c01(d[8 + 3 * f]));
      if (v.ctlFresh) v.cut[f] = hz, v.q[f] = Q;
      else v.cut[f] = hz + (v.cut[f] - hz) * cf, v.q[f] = Q + (v.q[f] - Q) * cf;
      const int type = std::clamp(z.f[f].type, 1, 5);
      v.f[f][0].set(type, v.cut[f], v.q[f], sr_);
      v.f[f][1] = Biquad{v.f[f][0].b0, v.f[f][0].b1, v.f[f][0].b2, v.f[f][0].a1, v.f[f][0].a2, {v.f[f][1].z1[0], v.f[f][1].z1[1]},
                         {v.f[f][1].z2[0], v.f[f][1].z2[1]}};
    }
    // Sample start / loop points from the matrix when targeted.
    (void)targeted;
    if (v.ctlFresh) v.gL = v.tgL, v.gR = v.tgR;
    v.ctlFresh = false;
  }

  void startVoice(int zi, uint32_t id, int note, double vel, double flFine, double glideFrom) {
    Voice* v = nullptr;
    for (auto& x : v_)
      if (!x.on) {
        v = &x;
        break;
      }
    if (!v) return;  // no stealing
    const DwProgram& p = a_->prog;
    const DwZone& z = p.zones[static_cast<size_t>(zi)];
    const SamplePtr& sp = a_->samples[static_cast<size_t>(zi)];
    *v = Voice{};
    v->on = true, v->id = id, v->zone = zi, v->note = note, v->vel = vel, v->flFine = flFine, v->age = ++clock_;
    g_seed = g_seed * 1664525u + 1013904223u;
    v->rnd = (g_seed >> 8) / 16777216.0;
    v->glideTarget = note;
    v->glideNote = glideFrom;
    v->glideStep = std::fabs(note - glideFrom) / (ctlRate() * timeOf(t_[DwGlide]));
    for (int k = 0; k < 2; ++k) trigLfo(v->lfo[k], z.lfo[k].phase), v->lfo[k].phi = z.lfo[k].phase < 1 ? kTwoPi * z.lfo[k].phase : 0.0;
    v->amp.trig();
    v->menv[0].trig(), v->menv[1].trig();
    control(*v, false);
    // Start position.
    double frames = sp ? static_cast<double>(sp->frames()) : 0.0;
    bool startTargeted = false;
    for (const auto& s : z.slot) startTargeted = startTargeted || (s.src > 0 && s.dst == 4);
    v->pos = startTargeted ? c01(v->dest[4]) * frames : z.sampleStart;
    if (z.waveSlice && z.ticks > 0 && note >= z.root) v->pos = ((note - z.root) % z.ticks) * frames / z.ticks;
  }

  void releaseVoice(Voice& v, bool forced) {
    const DwZone& z = a_->prog.zones[static_cast<size_t>(v.zone)];
    if (z.loopMode == 1 && !forced) return;  // one-shot ignores note-off
    v.released = true;
    v.amp.rel(), v.menv[0].rel(), v.menv[1].rel();
  }

  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    if (!a_) return;
    const DwProgram& p = a_->prog;
    const int note = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    const int vel = std::clamp(static_cast<int>(std::lround(velocity * 127.0)), 1, 127);
    const double flFine = (pitch - note) * 100.0;
    bool otherHeld = false;
    for (int k = 0; k < 128; ++k) otherHeld = otherHeld || (held_[k] && k != note);
    held_[note] = vel;
    // Program LFOs retrigger.
    trigLfo(plfo_[0], t_[DwLfo1Phase]);
    trigLfo(plfo_[1], t_[DwLfo2Phase]);
    // Candidate zones by trigger group (no allocation: scanned per group).
    auto matches = [&](size_t i, int g) {
      const DwZone& z = p.zones[i];
      if (std::clamp(z.trigGroup, 0, 99) != g) return false;
      const double ov = p.trig[static_cast<size_t>(g)].overlap;
      return note >= z.lo && note <= z.hi && vel >= z.vlo - 127.0 * ov && vel <= z.vhi + 127.0 * ov;
    };
    double glideFrom = note;
    if (t_[DwGlide] > 0 && !(p.glideAuto && !otherHeld)) glideFrom = lastNote_;
    for (int g = 0; g < 100; ++g) {
      int n = 0;
      for (size_t i = 0; i < p.zones.size(); ++i) n += matches(i, g);
      if (n == 0) continue;
      const DwTrigger& tg = p.trig[static_cast<size_t>(g)];
      if (tg.prob < 1.f) {
        g_seed = g_seed * 1664525u + 1013904223u;
        if (tg.prob <= (g_seed >> 8) / 16777216.0) continue;
      }
      int only = -1;  // index among the candidates when one is chosen
      if (n > 1 && tg.type != 0) {
        g_seed = g_seed * 1664525u + 1013904223u;
        const int r = static_cast<int>((g_seed >> 8) % static_cast<uint32_t>(n));
        if (tg.type == 1) only = (last_[g] + 1) % n;
        else if (tg.type == 2) only = r;
        else only = last_[g] < 0 ? r : (last_[g] + 1 + static_cast<int>((g_seed >> 8) % static_cast<uint32_t>(n - 1))) % n;
        last_[g] = only;
      }
      int ci = -1;
      for (size_t i = 0; i < p.zones.size(); ++i) {
        if (!matches(i, g)) continue;
        ++ci;
        if (only >= 0 && ci != only) continue;
        const int zi = static_cast<int>(i);
        const DwZone& z = p.zones[i];
        if (z.muteGroup != 0)
          for (auto& v : v_)
            if (v.on && p.zones[static_cast<size_t>(v.zone)].muteGroup == z.muteGroup) releaseVoice(v, true);
        if (p.playMode == 1) {
          for (auto& v : v_)
            if (v.on && v.amp.st != 0) v.fast = true, releaseVoice(v, true), control(v, false);
        } else if (p.playMode == 2) {
          Voice* lv = nullptr;
          for (auto& v : v_)
            if (v.on && v.amp.st >= 1 && v.amp.st <= 3) lv = &v;
          if (lv) {
            lv->id = id, lv->note = note, lv->glideTarget = note;
            lv->glideNote = t_[DwGlide] > 0 ? lastNote_ : note;
            lv->glideStep = std::fabs(note - lv->glideNote) / (ctlRate() * timeOf(t_[DwGlide]));
            continue;
          }
        }
        startVoice(zi, id, note, vel / 127.0, flFine, glideFrom);
      }
    }
    lastNote_ = note;
  }

  void onNoteOff(uint32_t id) override {
    if (!a_) return;
    int note = -1;
    for (auto& v : v_)
      if (v.on && v.id == id) note = v.note;
    if (note >= 0) held_[note] = 0;
    for (auto& v : v_)
      if (v.on && v.id == id && !v.released) releaseVoice(v, false);
    const int mode = a_->prog.playMode;
    if (mode != 0 && note == lastNote_) {
      for (int k = 0; k < 128; ++k)
        if (held_[k]) {
          onNoteOn(id, k, held_[k] / 127.f);
          break;
        }
    }
  }
  void onAllOff() override {
    for (auto& v : v_)
      if (v.on && a_) releaseVoice(v, false);
    std::fill(std::begin(held_), std::end(held_), 0);
  }
  void onKill() override {
    for (auto& v : v_) v.on = false;
    std::fill(std::begin(held_), std::end(held_), 0);
    for (auto& d : dl_) std::fill(d.begin(), d.end(), 0.f);
    for (auto& d : ch_) std::fill(d.begin(), d.end(), 0.f);
    rev_.reset();
    tail_ = 0;
  }

  float readFrame(const Sample& s, int ch, int64_t i, const Voice& v, const DwZone& z, bool looping, int64_t ls, int64_t le) const {
    (void)v, (void)z;
    if (looping && i >= le) i = ls + (i - le);
    if (i < 0 || i >= static_cast<int64_t>(s.frames())) return 0.f;
    return ch == 0 ? s.left[static_cast<size_t>(i)] : s.right[static_cast<size_t>(i)];
  }

  void renderVoice(Voice& v, int n) {
    const DwProgram& p = a_->prog;
    const DwZone& z = p.zones[static_cast<size_t>(v.zone)];
    const SamplePtr& sp = a_->samples[static_cast<size_t>(v.zone)];
    const double gc = std::exp(-200.0 / sr_);
    const double frames = sp ? static_cast<double>(sp->frames()) : 0.0;
    bool lsT = false, leT = false;
    for (const auto& s : z.slot) lsT = lsT || (s.src > 0 && s.dst == 5), leT = leT || (s.src > 0 && s.dst == 6);
    int64_t ls = lsT ? static_cast<int64_t>(c01(v.dest[5]) * frames) : z.loopStart;
    int64_t le = leT ? static_cast<int64_t>(c01(v.dest[6]) * frames) : z.loopEnd;
    if (ls > le) std::swap(ls, le);
    const bool hasLoop = le > ls;
    const double s0 = v.dest[25], s1 = v.dest[26], s2 = v.dest[27], dry = v.dest[28];
    for (int i = 0; i < n && v.on; ++i) {
      const bool looping = hasLoop && (z.loopMode == 2 || z.loopMode == 4 || (z.loopMode == 3 && v.amp.st != 4));
      double x[2] = {0, 0};
      if (sp && !v.ended) {
        const int64_t ip = static_cast<int64_t>(std::floor(v.pos));
        const double fr = v.pos - static_cast<double>(ip);
        const bool lp = looping && z.loopMode != 4;
        if (fr < 1e-9 && std::fabs(v.inc) <= 1.000001) {  // exact unless reading faster
          for (int c = 0; c < 2; ++c) x[c] = readFrame(*sp, c, ip, v, z, lp, ls, le);
        } else {
          // Band-limited read through the shared kernel (J-87, J-140): zones
          // pitched away from their root (rendered instruments, multisamples)
          // neither alias nor dull; the taps follow the loop like the frames do.
          float o[2];
          pitch::readWith<2>([&](int64_t k, int c) { return readFrame(*sp, c, k, v, z, lp, ls, le); }, v.pos, std::fabs(v.inc),
                             pitch::table(pitch::Quality::Realtime), o);
          x[0] = o[0], x[1] = o[1];
        }
      }
      v.follower = std::max({std::fabs(x[0]), std::fabs(x[1]), v.follower * gc});
      // Filters.
      if (z.f[0].type || z.f[1].type) {
        double y[2];
        for (int c = 0; c < 2; ++c) {
          auto run = [&](int f, double in) {
            if (z.f[f].type == 0) return in;
            double o = v.f[f][1].run(v.f[f][0].run(in, c), c);
            const double sh = c01(v.dest[9 + 3 * f]);
            if (sh > 0) o = std::clamp(o, -(1.0 - sh), 1.0 - sh);
            return o;
          };
          y[c] = z.parallel ? run(0, x[c]) + run(1, x[c]) : run(1, run(0, x[c]));
        }
        const double pk = std::max(std::fabs(y[0]), std::fabs(y[1]));
        if (pk > 1.0) y[0] /= pk, y[1] /= pk;
        x[0] = y[0], x[1] = y[1];
      }
      // Insert effects.
      if (z.fxOn[0]) {
        const double mix = c01(v.dest[18]);
        const double sn = std::sin(v.ringPh);
        v.ringPh += kTwoPi * frq(c01(v.dest[17])) / sr_;
        if (v.ringPh > kTwoPi) v.ringPh -= kTwoPi;
        for (auto& c : x) c = (1 - mix) * c + mix * c * sn;
      }
      if (z.fxOn[1]) {
        const double mix = c01(v.dest[20]);
        v.decCount += 1.0 - 0.99 * c01(v.dest[19]);
        if (v.decCount >= 1) v.decL = x[0], v.decR = x[1], v.decCount -= 1;
        x[0] = (1 - mix) * x[0] + mix * v.decL, x[1] = (1 - mix) * x[1] + mix * v.decR;
      }
      if (z.fxOn[2]) {
        const double mix = c01(v.dest[22]);
        const double q = std::exp2(static_cast<int>((1.0 - c01(v.dest[21])) * 13.0) + 3);
        for (auto& c : x) c = (1 - mix) * c + mix * std::trunc(c * q) / q;
      }
      if (z.fxOn[3]) {  // phaser: 8 first-order all-passes with feedback (constants not confirmed)
        const double mix = c01(v.dest[24]);
        const double w = std::tan(kPi * std::min(frq(c01(v.dest[23])), 0.45 * sr_) / sr_);
        const double a = (1 - w) / (1 + w);
        for (int c = 0; c < 2; ++c) {
          double in = x[c] + 0.5 * v.phY[c];
          for (int k = 0; k < 8; ++k) {
            const double o = a * in + v.phS[c][k];
            v.phS[c][k] = in - a * o;
            in = o;
          }
          v.phY[c] = in;
          x[c] += mix * in;
        }
        const double pk = std::max(std::fabs(x[0]), std::fabs(x[1]));
        if (pk > 1.0) x[0] /= pk, x[1] /= pk;
      }
      // Amp envelope and gains.
      const double env = v.amp.tick(v.aStep, v.dStep, v.sLvl, v.rStep);
      v.gL = v.tgL + (v.gL - v.tgL) * gc, v.gR = v.tgR + (v.gR - v.tgR) * gc;
      const double oL = x[0] * env * v.gL, oR = x[1] * env * v.gR;
      // Buses: 0/1 main, 2/3 delay, 4/5 chorus, 6/7 reverb.
      if (p.delayOn && s0 > 0) dBus_[0][i] += static_cast<float>(oL * s0 * s0), dBus_[1][i] += static_cast<float>(oR * s0 * s0);
      if (p.choOn && s1 > 0) cBus_[0][i] += static_cast<float>(oL * s1 * s1), cBus_[1][i] += static_cast<float>(oR * s1 * s1);
      if (p.revOn && s2 > 0) rBus_[0][i] += static_cast<float>(oL * s2 * s2), rBus_[1][i] += static_cast<float>(oR * s2 * s2);
      const double dg = dry >= 1.0 ? 1.0 : dry * dry;
      mBus_[0][i] += static_cast<float>(oL * dg), mBus_[1][i] += static_cast<float>(oR * dg);
      // Advance the sample position.
      if (sp && !v.ended) {
        if (z.loopMode == 4 && hasLoop) {
          v.pos += v.inc * v.dir;
          if (v.dir > 0 && v.pos >= static_cast<double>(le)) v.pos = 2.0 * le - v.pos, v.dir = -1;
          else if (v.dir < 0 && v.pos <= static_cast<double>(ls)) v.pos = 2.0 * ls - v.pos, v.dir = 1;
          v.pos = std::max(0.0, v.pos);
        } else {
          v.pos += v.inc;
          if (looping && v.pos >= static_cast<double>(le)) v.pos = ls + std::fmod(v.pos - ls, static_cast<double>(le - ls));
          else if (v.pos >= frames) {
            v.ended = true, v.fast = true;
            v.rStep = 1.0 / (sr_ * timeOf(0.25));
            v.amp.rel();
          }
        }
      }
      if (v.amp.st == 0) v.on = false;
    }
  }

  void renderBlock(float* L, float* R, int n) {
    if (!a_) return;
    const DwProgram& p = a_->prog;
    for (int c = 0; c < 2; ++c) {
      std::fill(mBus_[c], mBus_[c] + n, 0.f), std::fill(dBus_[c], dBus_[c] + n, 0.f);
      std::fill(cBus_[c], cBus_[c] + n, 0.f), std::fill(rBus_[c], rBus_[c] + n, 0.f);
    }
    bool any = false;
    for (auto& v : v_)
      if (v.on) renderVoice(v, n), any = true;
    // Program effects: delay → chorus → reverb, then the drives.
    if (p.delayOn) {
      const double T = std::max(0.001 * sr_, t_[DwDelayTime] * 60.0 * sr_ / tempo_);
      if (dTime_ < 0) dTime_ = T;
      for (int i = 0; i < n; ++i) {
        dTime_ += (T - dTime_) * 16.0 / sr_;
        const double d = std::clamp(dTime_, 1.0, 65530.0);
        const int di = static_cast<int>(d);
        const double fr = d - di, a = (1 - fr) / (1 + fr);
        double out[2];
        for (int c = 0; c < 2; ++c) {
          const auto& b = dl_[c];
          const double x0 = b[static_cast<size_t>((dw_ - di) & 65535)], x1 = b[static_cast<size_t>((dw_ - di - 1) & 65535)];
          out[c] = a * x0 + x1 - a * dAp_[c];
          dAp_[c] = out[c];
        }
        const double fb = t_[DwDelayFb];
        double in[2];
        if (p.pingPong) in[0] = dBus_[0][i] + dBus_[1][i] + fb * out[1], in[1] = fb * out[0];
        else in[0] = dBus_[0][i] + fb * out[0], in[1] = dBus_[1][i] + fb * out[1];
        for (int c = 0; c < 2; ++c) {
          const double y = dLp_[c].run(dHp_[c].run(in[c], 0), 0);
          dl_[c][static_cast<size_t>(dw_)] = static_cast<float>(y);
          mBus_[c][i] += static_cast<float>(out[c]);
        }
        dw_ = (dw_ + 1) & 65535;
      }
    }
    if (p.choOn) {
      const double depth = 24.0 * t_[DwChoDepth] * t_[DwChoDepth];
      const double d0 = 47.9 * t_[DwChoDelay] * t_[DwChoDelay] + 0.1 + depth / 2;
      for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
          const double ms = d0 + depth / 2 * std::sin(cPh_ + (c ? kPi / 2 : 0.0));
          const double T = ms * 0.001 * sr_;
          if (cD_[c] < 0) cD_[c] = T;
          cD_[c] += (T - cD_[c]) * 16.0 / sr_;
          const double d = std::clamp(cD_[c], 1.0, 16380.0);
          const int di = static_cast<int>(d);
          const double fr = d - di;
          const auto& b = ch_[c];
          const double y0 = b[static_cast<size_t>((cw_ - di) & 16383)], y1 = b[static_cast<size_t>((cw_ - di - 1) & 16383)];
          const double y = y0 + (y1 - y0) * fr;
          cFb_ += (t_[DwChoFb] - cFb_) * 16.0 / sr_;
          ch_[c][static_cast<size_t>(cw_)] = static_cast<float>(cBus_[c][i] + cFb_ * y);
          mBus_[c][i] += static_cast<float>(y);
        }
        cw_ = (cw_ + 1) & 16383;
        cPh_ += kTwoPi * 20.0 * t_[DwChoRate] * t_[DwChoRate] / sr_;
        if (cPh_ > kTwoPi) cPh_ -= kTwoPi;
      }
    }
    if (p.revOn) {
      rev_.process(rBus_[0], rBus_[1], n);
      for (int i = 0; i < n; ++i) mBus_[0][i] += rBus_[0][i], mBus_[1][i] += rBus_[1][i];
    }
    for (int k = 0; k < 2; ++k) {
      if (!p.driveOn[k]) continue;
      const double kk = 1.0 - 0.75 * t_[DwDriveA + k], k3 = kk * kk * kk;
      for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) mBus_[c][i] = static_cast<float>(k == 0 ? kk * std::tanh(mBus_[c][i] / k3) : kk * std::sin(mBus_[c][i] / k3));
    }
    for (int i = 0; i < n; ++i) L[i] += mBus_[0][i], R[i] += mBus_[1][i];
    if (any) tail_ = static_cast<int>(sr_ * ((p.delayOn || p.revOn) ? 8 : 1));
    else if (tail_ > 0) tail_ = std::max(0, tail_ - n);
  }

  void render(float* L, float* R, int n) override {
    int i = 0;
    while (i < n) {
      if (grid_ == 0 && a_) {
        // Control block: program LFOs, then every voice.
        stepLfo(plfo_[0], a_->prog.plfo[0], t_[DwLfo1Rate], t_[DwLfo1Attack], true);
        stepLfo(plfo_[1], a_->prog.plfo[1], t_[DwLfo2Rate], t_[DwLfo2Attack], true);
        for (auto& v : v_)
          if (v.on) control(v, true);
      }
      const int len = std::min({n - i, block_ - grid_, 256});
      renderBlock(L + i, R + i, len);
      grid_ = (grid_ + len) % block_;
      i += len;
    }
  }

 private:
  const WpAux* a_ = nullptr;
  double sr_ = 0, tempo_ = 120;
  float t_[DwCount] = {};
  int block_ = 256, grid_ = 0;
  Voice v_[kVoices];
  Lfo plfo_[2];
  uint8_t held_[128] = {};
  int last_[100] = {-1};
  double lastNote_ = 60;
  uint64_t clock_ = 0;
  int tail_ = 0;
  float mBus_[2][256] = {}, dBus_[2][256] = {}, cBus_[2][256] = {}, rBus_[2][256] = {};
  std::vector<float> dl_[2], ch_[2];
  int dw_ = 0, cw_ = 0;
  double dTime_ = -1, dAp_[2] = {0, 0}, cD_[2] = {-1, -1}, cFb_ = 0, cPh_ = 0;
  Biquad dHp_[2], dLp_[2];
  synths::hex::PlateReverb rev_;

 public:
  WaveProgramGen() { std::fill(std::begin(last_), std::end(last_), -1); }
};

}  // namespace

std::unique_ptr<Generator> makeWaveProgram() { return std::make_unique<WaveProgramGen>(); }

bool prepareWaveProgramAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != "gl.waveprogram") return false;
  auto a = std::make_shared<WpAux>();
  if (!parseProgram(state.data, a->prog) || a->prog.zones.empty()) {  // empty program: one built-in zone
    a->prog = DwProgram{};
    DwZone z;
    z.path = "builtin:tomlo";
    a->prog.zones.push_back(z);
  }
  for (const auto& z : a->prog.zones) a->samples.push_back(z.path.empty() ? nullptr : resolveSample(z.path));
  aux.custom = a;
  aux.samples.clear();
  aux.slices.clear();
  return true;
}

}  // namespace gl::packs::samplers
