// Gridline — "samplers" pack: audio runtimes. Included once, by samplers_dsp.cpp.
// Prepared data (zone maps, moves) reaches the generators through ChannelAux:
// samples[0] is a pack-private Sample subclass tagged by its path.
#pragma once
#include "samplers.h"

namespace gl::packs::samplers {

namespace {

const char* const kMsTag = "\x01gl.multisampler.map";
const char* const kScTag = "\x01gl.scratch.map";

// ------------------------------------------------------------- prepared maps
struct ZoneRt {
  Zone z;
  SamplePtr smp;
  int64_t n = 0, start = 0, end = 0, ls = 0, le = 0, xf = 0;
  bool hasLoop = false;
  float gainLin = 1, panL = 1, panR = 1;
};
struct MsRuntime : Sample {
  std::vector<ZoneRt> zones;
};
struct ScRuntime : Sample {
  SamplePtr smp;
  std::array<ScratchMove, kMoves> moves;
};

// Recently built maps by text, so rebuilding a program (any edit) keeps the
// same pointer and the old one is not freed on the audio thread.
template <typename T>
struct RecentCache {
  std::mutex mu;
  std::vector<std::pair<std::string, std::shared_ptr<const T>>> items;
  std::shared_ptr<const T> find(const std::string& k) {
    for (size_t i = 0; i < items.size(); ++i)
      if (items[i].first == k) {
        auto it = items[i];
        items.erase(items.begin() + static_cast<long>(i));
        items.push_back(it);
        return it.second;
      }
    return nullptr;
  }
  void put(const std::string& k, std::shared_ptr<const T> v) {
    items.emplace_back(k, std::move(v));
    if (items.size() > 24) items.erase(items.begin());
  }
};

std::shared_ptr<const MsRuntime> buildMsRuntime(const std::string& data, int set) {
  static RecentCache<MsRuntime> cache;
  std::string key = std::to_string(set) + "\n" + data;
  std::lock_guard<std::mutex> lk(cache.mu);
  if (auto r = cache.find(key)) return r;
  auto rt = std::make_shared<MsRuntime>();
  rt->path = kMsTag;
  ZoneMap m = parseZoneMap(data, set);
  for (const auto& z : m.zones) {
    SamplePtr s = resolveSample(z.path);
    if (!s || s->frames() < 2) continue;
    ZoneRt r;
    r.z = z;
    r.smp = s;
    r.n = static_cast<int64_t>(s->frames());
    r.start = std::clamp<int64_t>(z.start, 0, r.n - 2);
    r.end = z.end > 0 ? std::clamp<int64_t>(z.end, r.start + 2, r.n) : r.n;
    r.ls = std::clamp<int64_t>(z.loopStart, 0, r.end - 2);
    r.le = z.loopEnd > 0 ? std::clamp<int64_t>(z.loopEnd, r.ls + 2, r.end) : r.end;
    r.hasLoop = (z.loop == LoopForward || z.loop == LoopSustain || z.loop == LoopBounce) && r.le - r.ls >= 2;
    r.xf = std::min({z.xfade, r.ls, r.le - r.ls});
    r.gainLin = static_cast<float>(dsp::dbToGain(z.gain));
    r.panL = std::min(1.f, 1.f - z.pan);
    r.panR = std::min(1.f, 1.f + z.pan);
    rt->zones.push_back(std::move(r));
  }
  cache.put(key, rt);
  return rt;
}

std::shared_ptr<const ScRuntime> buildScRuntime(const std::string& data) {
  static RecentCache<ScRuntime> cache;
  std::lock_guard<std::mutex> lk(cache.mu);
  if (auto r = cache.find(data)) return r;
  auto rt = std::make_shared<ScRuntime>();
  rt->path = kScTag;
  ScratchData d = parseScratch(data);
  rt->smp = d.path.empty() ? scratchDefaultSample() : resolveSample(d.path);
  rt->moves = d.moves;
  cache.put(data, rt);
  return rt;
}

// ------------------------------------------------------------- DSP helpers
float prm(const PluginState& s, int i, float d = 0.f) {
  return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : d;
}

constexpr int kSincTaps = 8, kSincPhases = 256;
const std::array<float, (kSincPhases + 1) * kSincTaps>& sincTable() {
  static const auto t = [] {
    std::array<float, (kSincPhases + 1) * kSincTaps> a{};
    for (int p = 0; p <= kSincPhases; ++p) {
      double frac = static_cast<double>(p) / kSincPhases, sum = 0;
      double w[kSincTaps];
      for (int j = 0; j < kSincTaps; ++j) {
        double x = (j - 3) - frac;
        double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        double u = (x + 4.0) / 8.0;  // 0..1 across the window
        double win = 0.42 - 0.5 * std::cos(dsp::kTwoPi * u) + 0.08 * std::cos(2 * dsp::kTwoPi * u);
        w[j] = sinc * win;
        sum += w[j];
      }
      for (int j = 0; j < kSincTaps; ++j) a[static_cast<size_t>(p * kSincTaps + j)] = static_cast<float>(w[j] / sum);
    }
    return a;
  }();
  return t;
}

// Sample reader: clamps at the ends, wraps past the loop end while looping.
struct Reader {
  const float* d = nullptr;
  int64_t n = 0, ls = 0, le = 0;
  bool wrap = false;
  float at(int64_t i) const {
    if (wrap && i >= le) i = ls + (i - le) % (le - ls);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return d[i];
  }
  float read(double pos, int mode) const {
    int64_t i = static_cast<int64_t>(std::floor(pos));
    float f = static_cast<float>(pos - static_cast<double>(i));
    if (mode == InterpLinear) {
      float a = at(i), b = at(i + 1);
      return a + (b - a) * f;
    }
    if (mode == InterpSinc) {
      const auto& t = sincTable();
      float ph = f * kSincPhases;
      int p0 = static_cast<int>(ph);
      float pf = ph - static_cast<float>(p0);
      const float* w0 = &t[static_cast<size_t>(p0 * kSincTaps)];
      const float* w1 = &t[static_cast<size_t>(std::min(p0 + 1, kSincPhases) * kSincTaps)];
      float acc = 0;
      for (int j = 0; j < kSincTaps; ++j) acc += at(i - 3 + j) * (w0[j] + (w1[j] - w0[j]) * pf);
      return acc;
    }
    // 4-point Catmull-Rom.
    float xm = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
  }
};

// Attack (linear) – hold – decay – sustain – release (exponential, -60 dB at the set time).
struct Env {
  enum St { Idle, Att, Hold, Dec, Sus, Rel } st = Idle;
  double lvl = 0, atkInc = 1, holdN = 0, holdLeft = 0, decCoef = 0, relCoef = 0, sus = 1;
  void set(double sr, double a, double h, double d, double s, double r) {
    atkInc = 1.0 / std::max(1.0, a * sr);
    holdN = h * sr;
    decCoef = std::exp(-6.9 / std::max(1.0, d * sr));
    relCoef = std::exp(-6.9 / std::max(1.0, r * sr));
    sus = std::clamp(s, 0.0, 1.0);
  }
  void on() {
    st = Att;
    holdLeft = holdN;
  }
  void off() {
    if (st != Idle) st = Rel;
  }
  bool active() const { return st != Idle; }
  double next() {
    switch (st) {
      case Idle: return 0;
      case Att:
        lvl += atkInc;
        if (lvl >= 1) {
          lvl = 1;
          st = holdN > 0 ? Hold : Dec;
        }
        break;
      case Hold:
        if (--holdLeft <= 0) st = Dec;
        break;
      case Dec:
        lvl = sus + (lvl - sus) * decCoef;
        if (std::fabs(lvl - sus) < 1e-5) {
          lvl = sus;
          st = Sus;
        }
        break;
      case Sus: lvl = sus; break;
      case Rel:
        lvl *= relCoef;
        if (lvl < 1e-5) {
          lvl = 0;
          st = Idle;
        }
        break;
    }
    return lvl;
  }
};

uint32_t xorshift(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

constexpr int kChunk = 128;

// =========================================================================
// Multisampler
// =========================================================================
class MultiSamplerGen final : public Generator {
 public:
  MultiSamplerGen() { (void)sincTable(); }
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Voice {
    bool on = false, held = false, looping = false, fading = false;
    uint32_t id = 0;
    int note = 60, gen = 0, dir = 1, ctl = 0;
    float vel = 1;
    uint64_t age = 0;
    const ZoneRt* zr = nullptr;
    double pos = 0, pitch = 60, target = 60, rate = 1;
    Env amp, mod;
    double lfoPh = 0, lfoT = 0, lfoRnd = 0, lfoVal = 0;
    dsp::Svf fL, fR;
    float gL = 1, gR = 1, lfoAmp = 1, fade = 1, fadeStep = 0;
    int ftype = FiltOff;
    uint32_t rng = 1;
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    SamplePtr rt = aux.sample(0);
    if (rt.get() != map_.get()) {
      for (auto& v : voices_) {
        if (!v.on) continue;
        if (v.gen >= 1) v.on = false;  // two edits behind: its zone data is gone
        else v.gen = 1;
      }
      prev_ = std::move(map_);
      if (rt && rt->path == kMsTag) map_ = std::static_pointer_cast<const MsRuntime>(rt);
      else map_.reset();
    }
    volume_ = prm(s, MsVolume, 0.8f);
    if (volZ_ < 0) volZ_ = volume_;
    transpose_ = prm(s, MsTranspose) + prm(s, MsFine) / 100.0;
    poly_ = std::clamp(static_cast<int>(prm(s, MsPolyphony, 32) + 0.5f), 1, kMaxVoices);
    mode_ = static_cast<int>(prm(s, MsPlayMode) + 0.5f);
    glide_ = prm(s, MsGlide);
    rr_ = static_cast<int>(prm(s, MsRoundRobin, 1) + 0.5f);
    velAmp_ = prm(s, MsVelAmp, 0.7f);
    interp_ = static_cast<int>(prm(s, MsInterp, 1) + 0.5f);
    for (int k = 0; k < 5; ++k) env_[k] = prm(s, MsAttack + k);
    ftype_ = static_cast<int>(prm(s, MsFilterType) + 0.5f);
    cutoff_ = prm(s, MsCutoff, 8000);
    reso_ = prm(s, MsResonance, 0.1f);
    keyTrack_ = prm(s, MsKeyTrack);
    velCut_ = prm(s, MsVelCutoff);
    envAmt_ = prm(s, MsEnvAmount);
    for (int k = 0; k < 4; ++k) modEnv_[k] = prm(s, MsModAttack + k);
    modPitch_ = prm(s, MsModPitch);
    lfoShape_ = static_cast<int>(prm(s, MsLfoShape) + 0.5f);
    lfoRate_ = prm(s, MsLfoRate, 5);
    lfoDelay_ = prm(s, MsLfoDelay);
    lfoPitch_ = prm(s, MsLfoPitch);
    lfoCut_ = prm(s, MsLfoCutoff);
    lfoAmpAmt_ = prm(s, MsLfoAmp);
    lfoPan_ = prm(s, MsLfoPan);
    glideCoef_ = glide_ > 1e-4 ? 1.0 - std::exp(-1.0 / (glide_ * sr_ * 0.25)) : 1.0;
    for (auto& v : voices_)
      if (v.on) setEnvs(v);
  }

  void setEnvs(Voice& v) {
    const Zone& z = v.zr->z;
    if (z.ownEnv) v.amp.set(sr_, z.attack, z.hold, z.decay, z.sustain, z.release);
    else v.amp.set(sr_, env_[0], env_[1], env_[2], env_[3], env_[4]);
    v.mod.set(sr_, modEnv_[0], 0, modEnv_[1], modEnv_[2], modEnv_[3]);
  }

  int activeCount() const {
    int c = 0;
    for (const auto& v : voices_)
      if (v.on && !v.fading) ++c;
    return c;
  }
  void fadeOut(Voice& v) {
    if (!v.on || v.fading) return;
    v.fading = true;
    v.fadeStep = static_cast<float>(1.0 / std::max(1.0, 0.004 * sr_));
  }
  Voice* freeVoice() {
    for (auto& v : voices_)
      if (!v.on) return &v;
    Voice* best = nullptr;  // all busy: take the quietest fading voice, else the oldest
    for (auto& v : voices_)
      if (v.fading && (!best || v.fade < best->fade)) best = &v;
    if (!best)
      for (auto& v : voices_)
        if (!best || v.age < best->age) best = &v;
    return best;
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    if (!map_ || map_->zones.empty()) return;
    const int key = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    const int v127 = std::clamp(static_cast<int>(std::lround(vel * 127.f)), 1, 127);
    // Candidate zones, then one per round-robin group.
    int cand[256], nc = 0;
    const auto& zs = map_->zones;
    for (size_t i = 0; i < zs.size() && nc < 256; ++i) {
      const Zone& z = zs[i].z;
      if (key >= z.lo && key <= z.hi && v127 >= z.vlo && v127 <= z.vhi) cand[nc++] = static_cast<int>(i);
    }
    int chosen[256], nch = 0;
    bool groupDone[kMaxGroups] = {};
    for (int c = 0; c < nc; ++c) {
      int g = zs[static_cast<size_t>(cand[c])].z.group;
      if (g == 0 || rr_ == RrLayer) {
        chosen[nch++] = cand[c];
        continue;
      }
      if (groupDone[g]) continue;
      groupDone[g] = true;
      int members[256], nm = 0;
      for (int c2 = c; c2 < nc; ++c2)
        if (zs[static_cast<size_t>(cand[c2])].z.group == g) members[nm++] = cand[c2];
      int pick = 0;
      if (rr_ == RrCycle) {
        pick = static_cast<int>(rrCount_[g]++ % static_cast<uint32_t>(nm));
      } else {
        pick = static_cast<int>(xorshift(rng_) % static_cast<uint32_t>(nm));
        if (rr_ == RrAvoid && nm > 1 && members[pick] == rrLast_[g]) pick = (pick + 1 + static_cast<int>(xorshift(rng_) % static_cast<uint32_t>(nm - 1))) % nm;
      }
      rrLast_[g] = members[pick];
      chosen[nch++] = members[pick];
    }
    if (nch == 0) return;

    double startPitch = pitch;
    if (glide_ > 1e-4 && hasLast_) startPitch = lastPitch_;
    if (mode_ != PlayPoly) {
      if (nStack_ < 16) stack_[nStack_++] = {id, pitch};
      bool anyHeld = false;
      for (auto& v : voices_)
        if (v.on && v.held && !v.fading) anyHeld = true;
      if (mode_ == PlayLegato && anyHeld) {
        for (auto& v : voices_)
          if (v.on && v.held && !v.fading) {
            v.target = pitch;
            v.id = id;
            v.note = key;
          }
        lastPitch_ = pitch;
        hasLast_ = true;
        return;
      }
      if (anyHeld && glide_ <= 1e-4) startPitch = pitch;
      for (auto& v : voices_) fadeOut(v);
    }
    lastPitch_ = pitch;
    hasLast_ = true;
    // Exclusive classes cut each other.
    for (int c = 0; c < nch; ++c) {
      int ch = zs[static_cast<size_t>(chosen[c])].z.choke;
      if (ch > 0)
        for (auto& v : voices_)
          if (v.on && v.zr && v.zr->z.choke == ch) fadeOut(v);
    }
    // Polyphony: fade the oldest sounding voices.
    int limit = mode_ == PlayPoly ? poly_ : std::max(poly_, nch);
    while (activeCount() + nch > limit) {
      Voice* old = nullptr;
      for (auto& v : voices_)
        if (v.on && !v.fading && (!old || v.age < old->age)) old = &v;
      if (!old) break;
      fadeOut(*old);
    }
    for (int c = 0; c < nch; ++c) {
      Voice& v = *freeVoice();
      const ZoneRt& zr = zs[static_cast<size_t>(chosen[c])];
      v = Voice{};
      v.on = v.held = true;
      v.id = id;
      v.note = key;
      v.vel = vel;
      v.age = ++clock_;
      v.zr = &zr;
      v.gen = 0;
      v.dir = zr.z.reverse ? -1 : 1;
      v.pos = zr.z.reverse ? static_cast<double>(zr.end - 1) : static_cast<double>(zr.start);
      v.looping = zr.hasLoop;
      v.pitch = startPitch;
      v.target = pitch;
      v.rng = 0x9E3779B9u ^ static_cast<uint32_t>(clock_ * 2654435761u);
      setEnvs(v);
      v.amp.on();
      v.mod.on();
    }
  }

  void onNoteOff(uint32_t id) override {
    if (mode_ != PlayPoly) {
      int k = 0;
      for (int i = 0; i < nStack_; ++i)
        if (stack_[i].id != id) stack_[k++] = stack_[i];
      bool wasCurrent = false;
      for (auto& v : voices_)
        if (v.on && v.held && v.id == id) wasCurrent = true;
      nStack_ = k;
      if (wasCurrent && nStack_ > 0) {  // fall back to the previous held note
        const auto& prev = stack_[nStack_ - 1];
        for (auto& v : voices_)
          if (v.on && v.held && v.id == id) {
            v.id = prev.id;
            v.target = prev.pitch;
          }
        lastPitch_ = prev.pitch;
        return;
      }
    }
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) release(v);
  }
  void release(Voice& v) {
    v.held = false;
    if (v.zr->z.loop == LoopOneShot) return;
    v.amp.off();
    v.mod.off();
    if (v.zr->z.loop == LoopSustain) v.looping = false;
  }
  void onAllOff() override {
    nStack_ = 0;
    for (auto& v : voices_)
      if (v.on) {
        v.held = false;
        v.amp.off();
        v.mod.off();
        if (v.zr->z.loop == LoopSustain) v.looping = false;
        if (v.zr->z.loop == LoopOneShot) fadeOut(v);
      }
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
    nStack_ = 0;
  }

  // Pitch, filter, pan and LFO at control rate (every 16 frames per voice).
  void control(Voice& v) {
    const Zone& z = v.zr->z;
    // LFO
    double lfo = 0;
    switch (lfoShape_) {
      case LfoSine: lfo = std::sin(dsp::kTwoPi * v.lfoPh); break;
      case LfoTriangle: lfo = 1.0 - 4.0 * std::fabs(v.lfoPh - 0.5); break;
      case LfoSquare: lfo = v.lfoPh < 0.5 ? 1.0 : -1.0; break;
      case LfoSaw: lfo = 2.0 * v.lfoPh - 1.0; break;
      default: lfo = v.lfoRnd; break;
    }
    double depth = lfoDelay_ > 1e-3 ? std::min(1.0, v.lfoT / lfoDelay_) : 1.0;
    lfo *= depth;
    v.lfoVal = lfo;
    double modE = v.mod.lvl;
    double semis = (v.pitch + transpose_ - z.root) * z.keyTrack + z.tune / 100.0 + lfo * lfoPitch_ / 100.0 + modE * modPitch_;
    v.rate = std::exp2(semis / 12.0) * v.zr->smp->sampleRate / sr_;
    // Filter
    int type = ftype_;
    double fc = cutoff_;
    if (type == FiltOff && z.cutoff > 0) {
      type = FiltLow;
      fc = z.cutoff;
    }
    v.ftype = type;
    if (type != FiltOff) {
      double oct = keyTrack_ * (v.pitch - 60.0) / 12.0 + velCut_ * 4.0 * (v.vel - 1.0) + envAmt_ * modE + lfoCut_ * lfo;
      double hz = std::clamp(fc * std::exp2(oct), 16.0, sr_ * 0.49);
      v.fL.set(sr_, hz, reso_);
      v.fR.set(sr_, hz, reso_);
    }
    // Pan and tremolo.
    double pan = std::clamp(static_cast<double>(z.pan) + lfo * lfoPan_, -1.0, 1.0);
    v.gL = static_cast<float>(std::min(1.0, 1.0 - pan));
    v.gR = static_cast<float>(std::min(1.0, 1.0 + pan));
    v.lfoAmp = static_cast<float>(1.0 - lfoAmpAmt_ * 0.5 * (1.0 - lfo));
  }

  void render(float* L, float* R, int n) override {
    for (int off = 0; off < n; off += kChunk) {
      int m = std::min(kChunk, n - off);
      float vol[kChunk];
      for (int i = 0; i < m; ++i) vol[i] = static_cast<float>(volZ_ = volume_ + (volZ_ - volume_) * 0.999);
      for (auto& v : voices_)
        if (v.on) renderVoice(v, L + off, R + off, m, vol);
    }
  }

  void renderVoice(Voice& v, float* L, float* R, int m, const float* vol) {
    const ZoneRt& zr = *v.zr;
    const Zone& z = zr.z;
    const Sample& s = *zr.smp;
    Reader rl{s.left.data(), zr.n, zr.ls, zr.le, false};
    Reader rr{s.right.data(), zr.n, zr.ls, zr.le, false};
    const double lfoInc = lfoRate_ / sr_, dt = 1.0 / sr_;
    const float vg = static_cast<float>((1.0 - velAmp_) + velAmp_ * v.vel * v.vel) * zr.gainLin;
    for (int i = 0; i < m; ++i) {
      if (v.ctl-- <= 0) {
        v.ctl = 15;
        control(v);
      }
      // Glide toward the target pitch.
      if (v.pitch != v.target) {
        v.pitch += (v.target - v.pitch) * glideCoef_;
        if (std::fabs(v.target - v.pitch) < 1e-4) v.pitch = v.target;
      }
      double a = v.amp.next();
      v.mod.next();
      v.lfoT += dt;
      v.lfoPh += lfoInc;
      if (v.lfoPh >= 1.0) {
        v.lfoPh -= std::floor(v.lfoPh);
        v.lfoRnd = static_cast<double>(xorshift(v.rng) & 0xFFFF) / 32767.5 - 1.0;
      }
      if (!v.amp.active()) {
        v.on = false;
        return;
      }
      const bool wrap = v.looping && z.loop != LoopBounce;
      rl.wrap = rr.wrap = wrap;
      float l = rl.read(v.pos, interp_), r = rr.read(v.pos, interp_);
      if (wrap && zr.xf > 0 && v.dir > 0 && v.pos >= static_cast<double>(zr.le - zr.xf)) {
        double w = (v.pos - static_cast<double>(zr.le - zr.xf)) / static_cast<double>(zr.xf);
        double back = v.pos - static_cast<double>(zr.le - zr.ls);
        float c0 = static_cast<float>(std::cos(w * kPi * 0.5)), c1 = static_cast<float>(std::sin(w * kPi * 0.5));
        l = l * c0 + rl.read(back, interp_) * c1;
        r = r * c0 + rr.read(back, interp_) * c1;
      }
      if (v.ftype != FiltOff) {
        double bl, hl, br, hr;
        double lo = v.fL.process(l, &bl, &hl), ro = v.fR.process(r, &br, &hr);
        switch (v.ftype) {
          case FiltLow: l = static_cast<float>(lo), r = static_cast<float>(ro); break;
          case FiltHigh: l = static_cast<float>(hl), r = static_cast<float>(hr); break;
          case FiltBand: l = static_cast<float>(bl), r = static_cast<float>(br); break;
          default: l = static_cast<float>(lo + hl), r = static_cast<float>(ro + hr); break;
        }
      }
      // Fade near a non-looping end (anti-click), and steal/choke fades.
      float edge = 1.f;
      if (!v.looping) {
        double left = v.dir > 0 ? (static_cast<double>(zr.end - 1) - v.pos) : (v.pos - static_cast<double>(zr.start));
        double framesLeft = left / std::max(1e-6, v.rate);
        if (framesLeft < 48) edge = static_cast<float>(std::max(0.0, framesLeft / 48.0));
      }
      if (v.fading) {
        v.fade -= v.fadeStep;
        if (v.fade <= 0) {
          v.on = false;
          return;
        }
      }
      float g = static_cast<float>(a) * vg * v.lfoAmp * v.fade * edge * vol[i];
      L[i] += g * v.gL * l;
      R[i] += g * v.gR * r;
      // Advance and loop.
      v.pos += v.rate * v.dir;
      if (v.looping) {
        const double ls = static_cast<double>(zr.ls), le = static_cast<double>(zr.le), len = le - ls;
        if (z.loop == LoopBounce) {
          const double top = le - 1;
          if (v.dir > 0 && v.pos >= top) {
            v.pos = top - std::fmod(v.pos - top, len);
            v.dir = -1;
          } else if (v.dir < 0 && v.pos < ls) {
            v.pos = ls + std::fmod(ls - v.pos, len);
            v.dir = 1;
          }
          v.pos = std::clamp(v.pos, ls, top);
        } else if (v.dir > 0 && v.pos >= le) {
          v.pos = ls + std::fmod(v.pos - ls, len);
        } else if (v.dir < 0 && v.pos < ls) {
          v.pos = le - std::fmod(le - v.pos, len);
        }
      } else if ((v.dir > 0 && v.pos >= static_cast<double>(zr.end - 1)) || (v.dir < 0 && v.pos < static_cast<double>(zr.start))) {
        v.on = false;
        return;
      }
    }
  }

 private:
  std::array<Voice, kMaxVoices> voices_{};
  std::shared_ptr<const MsRuntime> map_, prev_;
  double sr_ = 44100, transpose_ = 0, glide_ = 0, glideCoef_ = 1, velAmp_ = 0.7, volume_ = 0.8, volZ_ = -1;
  double env_[5] = {0.002, 0, 1, 1, 0.3}, modEnv_[4] = {0.005, 0.6, 0, 0.3};
  double cutoff_ = 8000, reso_ = 0.1, keyTrack_ = 0, velCut_ = 0, envAmt_ = 0, modPitch_ = 0;
  double lfoRate_ = 5, lfoDelay_ = 0, lfoPitch_ = 0, lfoCut_ = 0, lfoAmpAmt_ = 0, lfoPan_ = 0;
  int poly_ = 32, mode_ = PlayPoly, rr_ = RrCycle, interp_ = InterpCubic, ftype_ = FiltOff, lfoShape_ = LfoSine;
  uint32_t rrCount_[kMaxGroups] = {};
  int rrLast_[kMaxGroups] = {};
  uint32_t rng_ = 0x2545F491u;
  struct Held {
    uint32_t id;
    double pitch;
  };
  Held stack_[16] = {};
  int nStack_ = 0;
  double lastPitch_ = 60;
  bool hasLast_ = false;
  uint64_t clock_ = 0;
};

// =========================================================================
// Scratch Sampler
// =========================================================================
class ScratchGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Voice {
    bool on = false, held = false, done = false;
    uint32_t id = 0;
    int move = 0, gen = 0;
    float vel = 1;
    uint64_t age = 0;
    double t = 0, tRate = 0, pos = 0, lastPos = 0, muteG = 1, lpL = 0, lpR = 0, fade = 1, ratio = 1, head = 0, spd = 0;
    bool fading = false;
    Env amp;
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    tempo_ = ctx.tempo > 1 ? ctx.tempo : 140.0;
    SamplePtr rt = aux.sample(0);
    if (rt.get() != map_.get()) {
      for (auto& v : voices_) {
        if (!v.on) continue;
        if (v.gen >= 1) v.on = false;
        else v.gen = 1;
      }
      prev_ = std::move(map_);
      if (rt && rt->path == kScTag) map_ = std::static_pointer_cast<const ScRuntime>(rt);
      else map_.reset();
    }
    volume_ = prm(s, ScVolume, 0.8f);
    start_ = prm(s, ScStart);
    end_ = prm(s, ScEnd, 0.5f);
    length_ = std::max(0.05f, prm(s, ScLength, 1));
    sync_ = prm(s, ScSync, 1) >= 0.5f;
    keyMode_ = static_cast<int>(prm(s, ScKeyMode) + 0.5f);
    root_ = static_cast<int>(prm(s, ScRoot, 60) + 0.5f);
    follow_ = prm(s, ScPitchFollow, 1);
    double smooth = prm(s, ScSmoothing, 0.3f);
    smoothA_ = smooth <= 1e-3 ? 1.0 : 1.0 - std::exp(-1.0 / (smooth * 0.03 * sr_));
    attack_ = prm(s, ScAttack, 0.003f);
    release_ = prm(s, ScRelease, 0.08f);
    loopMove_ = prm(s, ScLoopMove, 1) >= 0.5f;
    interp_ = static_cast<int>(prm(s, ScInterp, 1) + 0.5f);
    speed_ = std::max(0.01f, prm(s, ScSpeed, 1));
    motion_ = static_cast<int>(prm(s, ScMotion) + 0.5f);
    muteCoef_ = 1.0 - std::exp(-1.0 / (0.002 * sr_));
    for (auto& v : voices_)
      if (v.on) v.amp.set(sr_, attack_, 0, 1, 1, release_);
  }

  double moveSeconds() const { return sync_ ? length_ * 60.0 / tempo_ : length_; }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    if (!map_ || !map_->smp || map_->smp->frames() < 4) return;
    int note = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    double ratio;
    int move;
    if (keyMode_ == KeyPitched) {
      move = root_ % 12;
      ratio = std::exp2(follow_ * (pitch - root_) / 12.0);
    } else {
      move = note % 12;
      ratio = std::exp2(follow_ * static_cast<double>(note / 12 - root_ / 12));
    }
    Voice* pick = nullptr;
    for (auto& v : voices_)
      if (!v.on) {
        pick = &v;
        break;
      }
    if (!pick)
      for (auto& v : voices_)
        if (!pick || v.age < pick->age) pick = &v;
    Voice& v = *pick;
    v = Voice{};
    v.on = v.held = true;
    v.id = id;
    v.move = move;
    v.vel = vel;
    v.age = ++clock_;
    v.tRate = speed_ * ratio / (moveSeconds() * sr_);
    v.ratio = ratio;
    double n = static_cast<double>(map_->smp->frames() - 1);
    v.pos = v.lastPos = (start_ + evalMove(map_->moves[static_cast<size_t>(move)], 0) * (end_ - start_)) * n;
    if (motion_ == MotionSpeed) v.pos = v.lastPos = v.head = start_ * n;
    v.muteG = (map_->moves[static_cast<size_t>(move)].mutes & 1u) ? 0 : 1;
    v.amp.set(sr_, attack_, 0, 1, 1, release_);
    v.amp.on();
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) {
        v.held = false;
        v.amp.off();
      }
  }
  void onAllOff() override {
    for (auto& v : voices_)
      if (v.on) {
        v.held = false;
        v.amp.off();
      }
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
  }

  void render(float* L, float* R, int n) override {
    if (!map_ && !prev_) return;
    for (auto& v : voices_) {
      if (!v.on) continue;
      const ScRuntime* rt = v.gen == 0 ? map_.get() : prev_.get();
      if (!rt || !rt->smp) {
        v.on = false;
        continue;
      }
      const Sample& s = *rt->smp;
      const ScratchMove& mv = rt->moves[static_cast<size_t>(v.move)];
      const int64_t frames = static_cast<int64_t>(s.frames());
      const double span = static_cast<double>(frames - 1);
      Reader rl{s.left.data(), frames, 0, 0, false}, rr{s.right.data(), frames, 0, 0, false};
      for (int i = 0; i < n; ++i) {
        if (motion_ == MotionSpeed) {
          // The curve is the platter speed; the head wraps inside the region.
          double target = (evalMove(mv, static_cast<float>(v.t)) * 4.0 - 2.0) * v.ratio * speed_ * s.sampleRate / sr_;
          v.spd += (target - v.spd) * smoothA_;
          double a0 = std::min(start_, end_) * span, a1 = std::max(start_, end_) * span, len = std::max(1.0, a1 - a0);
          v.head += v.spd;
          if (v.head >= a1) v.head = a0 + std::fmod(v.head - a0, len);
          if (v.head < a0) v.head = a1 - std::fmod(a1 - v.head, len);
          v.pos = v.head;
        } else {
          double target = (start_ + evalMove(mv, static_cast<float>(v.t)) * (end_ - start_)) * span;
          v.pos += (target - v.pos) * smoothA_;
        }
        double speed = std::fabs(v.pos - v.lastPos);
        v.lastPos = v.pos;
        float l = rl.read(v.pos, interp_), r = rr.read(v.pos, interp_);
        // Fast motion: one-pole low-pass against aliasing (cutoff ~ Nyquist / speed).
        double c = speed > 1.0 ? std::exp(-dsp::kTwoPi * 0.45 / speed) : 0.0;
        v.lpL = l + (v.lpL - l) * c;
        v.lpR = r + (v.lpR - r) * c;
        int step = std::min(kMuteSteps - 1, static_cast<int>(v.t * kMuteSteps));
        double mt = (mv.mutes >> step) & 1u ? 0.0 : 1.0;
        v.muteG += (mt - v.muteG) * muteCoef_;
        double a = v.amp.next();
        if (!v.amp.active()) {
          v.on = false;
          break;
        }
        float g = static_cast<float>(a * v.muteG * v.vel * volume_);
        L[i] += g * static_cast<float>(v.lpL);
        R[i] += g * static_cast<float>(v.lpR);
        v.t += v.tRate;
        if (v.t >= 1.0) {
          if (loopMove_) {
            v.t -= std::floor(v.t);
          } else {
            v.t = 1.0;
            if (!v.done) {
              v.done = true;
              v.amp.off();
            }
          }
        }
      }
    }
  }

 private:
  std::array<Voice, 8> voices_{};
  std::shared_ptr<const ScRuntime> map_, prev_;
  double sr_ = 44100, tempo_ = 140, start_ = 0, end_ = 0.5, length_ = 1, follow_ = 1, smoothA_ = 1, attack_ = 0.003,
         release_ = 0.08, speed_ = 1, muteCoef_ = 1, volume_ = 0.8;
  bool sync_ = true, loopMove_ = true;
  int keyMode_ = KeyMoves, root_ = 60, interp_ = InterpCubic, motion_ = MotionPosition;
  uint64_t clock_ = 0;
};

}  // namespace

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (type == "gl.multisampler") return std::make_unique<MultiSamplerGen>();
  if (type == "gl.scratch") return std::make_unique<ScratchGen>();
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
  (void)type;
  return nullptr;
}

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type == "gl.multisampler") {
    int set = static_cast<int>(prm(state, MsFactorySet) + 0.5f);
    aux.samples = {buildMsRuntime(state.data, set)};
    aux.slices.clear();
    return true;
  }
  if (state.type == "gl.scratch") {
    aux.samples = {buildScRuntime(state.data)};
    aux.slices.clear();
    return true;
  }
  return false;
}

}  // namespace gl::packs::samplers
