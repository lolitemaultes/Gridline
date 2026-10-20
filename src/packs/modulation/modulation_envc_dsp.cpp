// Gridline — Envelope Controller articulator model: text form of the data
// (see envc.h).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include "envc.h"

namespace gl::packs::modulation {

bool EnvcArt::operator==(const EnvcArt& o) const {
  return flags == o.flags && version == o.version && keyLo == o.keyLo && keyHi == o.keyHi && env == o.env && lfo == o.lfo && maps == o.maps;
}

bool EnvcModel::operator==(const EnvcModel& o) const {
  return version == o.version && cur == o.cur && part == o.part && sel == o.sel && uiBits == o.uiBits && arts == o.arts;
}

EnvcModel defaultEnvc() {
  EnvcModel m;
  for (size_t a = 0; a < m.arts.size(); ++a) {
    EnvcArt& r = m.arts[a];
    r.env = flart::defaultEnvelope();
    r.env.enabled = a == 0 ? -1 : 0;
    r.lfo = flart::defaultLfo();
    for (auto& mp : r.maps) mp = flart::defaultMap(0.5);
    // The knob values (time scalers, LFO speed …) are parameters.
    std::fill(r.env.extra, r.env.extra + 4, 0);
    std::fill(r.lfo.extra, r.lfo.extra + 4, 0);
  }
  return m;
}

std::string writeEnvc(const EnvcModel& m) {
  std::string s;
  char buf[128];
  std::snprintf(buf, sizeof buf, "envc 1 %d %d %d %d %d\n", m.version, m.cur, m.part, m.sel, m.uiBits);
  s += buf;
  for (const auto& a : m.arts) {
    std::snprintf(buf, sizeof buf, "art %d %d %d %d\n", a.flags, a.version, a.keyLo, a.keyHi);
    s += buf;
    s += flart::writeText(a.env) + "\n";
    s += flart::writeText(a.lfo) + "\n";
    for (const auto& mp : a.maps) s += flart::writeText(mp) + "\n";
  }
  return s;
}

bool readEnvc(const std::string& text, EnvcModel& out) {
  out = defaultEnvc();
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line)) return false;
  int fmt = 0;
  if (std::sscanf(line.c_str(), "envc %d %d %d %d %d %d", &fmt, &out.version, &out.cur, &out.part, &out.sel, &out.uiBits) != 6 || fmt != 1)
    return false;
  for (auto& a : out.arts) {
    if (!std::getline(in, line)) return false;
    if (std::sscanf(line.c_str(), "art %d %d %d %d", &a.flags, &a.version, &a.keyLo, &a.keyHi) != 4) return false;
    flart::Record* recs[2 + kEnvcMaps] = {&a.env, &a.lfo};
    for (int k = 0; k < kEnvcMaps; ++k) recs[2 + k] = &a.maps[static_cast<size_t>(k)];
    const int kinds[2 + kEnvcMaps] = {flart::KindEnv, flart::KindLfo, flart::KindMap, flart::KindMap,
                                      flart::KindMap, flart::KindMap, flart::KindMap};
    for (int k = 0; k < 2 + kEnvcMaps; ++k) {
      if (!std::getline(in, line) || !flart::readText(line, *recs[k]) || recs[k]->kind != kinds[k]) return false;
    }
  }
  return true;
}

EnvcModel envcOf(const std::string& data) {
  EnvcModel m;
  if (data.empty() || !readEnvc(data, m)) m = defaultEnvc();
  return m;
}

// ------------------------------------------------------------- FL state

namespace {

int32_t rd32(const uint8_t* p) {
  return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                              (static_cast<uint32_t>(p[3]) << 24));
}
float rdf(const uint8_t* p) {
  const uint32_t u = static_cast<uint32_t>(rd32(p));
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void wr32(std::vector<uint8_t>& o, int32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
}
void wrf(std::vector<uint8_t>& o, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  wr32(o, static_cast<int32_t>(u));
}
// Divisor of knob k (11 per articulator, then mod X / Y).
double knobScale(int knob) {
  if (knob >= kArts * AfFields) return 65536.0;
  switch (knob % AfFields) {
    case AfBase: case AfEnvAmt: case AfLfoAmt: case AfLfoSkew: return 256.0;
    case AfLfoSpeed: return 65536.0;
    default: return 128.0;
  }
}
// Default knob values of a fresh articulator (the plugin database state).
int32_t defaultKnob(int knob) {
  if (knob >= kArts * AfFields) return 32768;
  static const int32_t d[AfFields] = {128, 256, 128, 128, 128, 0, 128, 40898, 0, 0, 0};
  return d[knob % AfFields];
}

}  // namespace

double envcKnobToParam(int knob, int32_t v) { return static_cast<double>(v) / knobScale(knob); }
int32_t envcParamToKnob(int knob, double v) { return static_cast<int32_t>(std::lround(v * knobScale(knob))); }

EnvcState defaultEnvcState() {
  EnvcState s;
  s.model = defaultEnvc();
  for (int k = 0; k < kEnvcKnobs; ++k) s.knobs[static_cast<size_t>(k)] = defaultKnob(k);
  s.options = 2;
  return s;
}

bool parseEnvcState(const uint8_t* p, size_t n, EnvcState& out) {
  out = defaultEnvcState();
  size_t at = 0;
  auto has = [&](size_t k) { return at <= n && n - at >= k; };
  if (!has(4)) return false;
  const int32_t word = rd32(p);
  at = 4;
  const int32_t ver = word > 0xFFFF ? (word >> 16) : word;
  if (ver < 0 || ver > 6) return false;
  out.model.version = ver >= 6 ? word : 0x00060003;  // written back in the 8-articulator layout
  const int arts = ver >= 6 ? kArts : 4;
  const int knobs = ver >= 6 ? kEnvcKnobs : 4 * AfFields;
  if (!has(static_cast<size_t>(knobs) * 4 + 12)) return false;
  for (int k = 0; k < knobs; ++k, at += 4) out.knobs[static_cast<size_t>(k)] = rd32(p + at);
  out.model.cur = rd32(p + at), at += 4;
  if (ver >= 1) out.model.part = rd32(p + at), at += 4;
  if (!has(8)) return false;
  out.options = rd32(p + at), at += 4;
  out.model.uiBits = out.options & ~4;
  out.model.sel = rd32(p + at), at += 4;
  for (int a = 0; a < arts; ++a) {
    EnvcArt& r = out.model.arts[static_cast<size_t>(a)];
    if (ver > 1) {
      if (!has(4)) return false;
      r.flags = rd32(p + at), at += 4;
    } else {
      r.flags = 0;
    }
    if (!has(16)) return false;
    r.version = rd32(p + at);
    float f[3] = {rdf(p + at + 4), rdf(p + at + 8), rdf(p + at + 12)};
    at += 16;
    if (!flart::readRecord(p, n, at, flart::KindEnv, r.env) || !flart::readRecord(p, n, at, flart::KindLfo, r.lfo)) return false;
    const int maps = ver >= 6 ? kEnvcMaps : 2;
    for (int k = 0; k < maps; ++k)
      if (!flart::readRecord(p, n, at, flart::KindMap, r.maps[static_cast<size_t>(k)])) return false;
    if (!has(8)) return false;
    r.keyLo = rd32(p + at), r.keyHi = rd32(p + at + 4);
    at += 8;
    // The knob block feeds the engine; the record duplicates it.
    const size_t kb = static_cast<size_t>(a) * AfFields;
    const int32_t* kn = &out.knobs[kb];
    if (f[0] != static_cast<float>(kn[AfBase] / 256.0) || f[1] != static_cast<float>(kn[AfEnvAmt] / 256.0) ||
        f[2] != static_cast<float>(kn[AfLfoAmt] / 256.0))
      out.recordsMatchKnobs = false;
    for (int k = 0; k < 4; ++k) {
      if (r.env.extra[k] != kn[AfAttack + k]) out.recordsMatchKnobs = false;
      if (r.lfo.extra[k] != kn[AfLfoSpeed + k]) out.recordsMatchKnobs = false;
    }
    std::fill(r.env.extra, r.env.extra + 4, 0);
    std::fill(r.lfo.extra, r.lfo.extra + 4, 0);
  }
  return at == n;
}

std::vector<uint8_t> writeEnvcState(const EnvcState& s) {
  std::vector<uint8_t> o;
  const int32_t word = s.model.version;
  const int32_t ver = word > 0xFFFF ? (word >> 16) : word;
  wr32(o, ver >= 6 ? word : 0x00060003);  // always the 8-articulator layout
  for (int k = 0; k < kEnvcKnobs; ++k) wr32(o, s.knobs[static_cast<size_t>(k)]);
  wr32(o, s.model.cur);
  wr32(o, s.model.part);
  wr32(o, (s.model.uiBits & ~4) | (s.options & 4));
  wr32(o, s.model.sel);
  for (int a = 0; a < kArts; ++a) {
    const EnvcArt& r = s.model.arts[static_cast<size_t>(a)];
    const int32_t* kn = &s.knobs[static_cast<size_t>(a) * AfFields];
    wr32(o, r.flags);
    wr32(o, r.version);
    wrf(o, static_cast<float>(kn[AfBase] / 256.0));
    wrf(o, static_cast<float>(kn[AfEnvAmt] / 256.0));
    wrf(o, static_cast<float>(kn[AfLfoAmt] / 256.0));
    flart::Record env = r.env, lfo = r.lfo;
    env.kind = flart::KindEnv, lfo.kind = flart::KindLfo;
    for (int k = 0; k < 4; ++k) env.extra[k] = kn[AfAttack + k], lfo.extra[k] = kn[AfLfoSpeed + k];
    flart::writeRecord(o, env);
    flart::writeRecord(o, lfo);
    for (const auto& mp : r.maps) {
      flart::Record m = mp;
      m.kind = flart::KindMap;
      flart::writeRecord(o, m);
    }
    wr32(o, r.keyLo);
    wr32(o, r.keyHi);
  }
  return o;
}

// ------------------------------------------------------------------ runtime

std::shared_ptr<const AuxData> prepareEnvc(const PluginState& s) {
  auto a = std::make_shared<EnvcAux>();
  a->model = envcOf(s.data);
  for (size_t k = 0; k < a->arts.size(); ++k) {
    const EnvcArt& m = a->model.arts[k];
    EnvcAux::Art& p = a->arts[k];
    p.bipolar = m.bipolar();
    const double neutral = p.bipolar ? 0.5 : 1.0;
    p.env = flart::prepare(m.env, neutral);
    p.lfo = flart::prepare(m.lfo, 0.5);
    for (size_t j = 0; j < p.maps.size(); ++j) p.maps[j] = flart::prepare(m.maps[j], neutral);
  }
  return a;
}

namespace {

inline float prmv(const PluginState& s, int k, float d) {
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : d;
}
inline double comb(bool sum, double a, double b) { return sum ? a + b : a * b; }

flart::Scales scalesOf(const PluginState& s, int a) {
  flart::Scales sc;
  sc.attack = prmv(s, artParam(a, AfAttack), 1);
  sc.decay = prmv(s, artParam(a, AfDecay), 1);
  sc.sustain = prmv(s, artParam(a, AfSustain), 0);
  sc.release = prmv(s, artParam(a, AfRelease), 1);
  return sc;
}

flart::LfoShape lfoShapeOf(const PluginState& s, int a, int32_t lfoFlags) {
  flart::LfoShape sh;
  sh.set(static_cast<int>(std::lround(prmv(s, artParam(a, AfLfoTension), 0) * 128.0)),
         static_cast<int>(std::lround(prmv(s, artParam(a, AfLfoSkew), 0) * 256.0)),
         static_cast<int>(std::lround(prmv(s, artParam(a, AfLfoPw), 0) * 128.0)), (lfoFlags & flart::kBipolarTen) != 0);
  return sh;
}

int speedOf(const PluginState& s, int a) {
  return static_cast<int>(std::lround(std::clamp(static_cast<double>(prmv(s, artParam(a, AfLfoSpeed), 0.624f)), 0.0, 1.0) * 65536.0));
}

}  // namespace

void EnvcRuntime::configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) {
  st_ = &s;
  aux_ = dynamic_cast<const EnvcAux*>(aux.custom.get());
  ctx_ = ctx;
  if (!primed_ && aux_) {
    for (int a = 0; a < kArts; ++a) run_[static_cast<size_t>(a)].out = std::clamp(idle(a), 0.0, 1.0);
    primed_ = true;
  }
}

// Output of an articulator without a note: base and the mod X / Y maps at
// the global X / Y.
double EnvcRuntime::idle(int a) const {
  const EnvcAux::Art& p = aux_->arts[static_cast<size_t>(a)];
  const bool sum = p.bipolar;
  const double centre = sum ? 0.5 : 0.0;
  double v = prmv(*st_, artParam(a, AfBase), 0.5f);
  const double gx = prmv(*st_, EcModX, 0.5f), gy = prmv(*st_, EcModY, 0.5f);
  if (!p.maps[MapModX].flat) v = comb(sum, v, flart::mapValue(p.maps[MapModX], gx, centre));
  if (!p.maps[MapModY].flat) v = comb(sum, v, flart::mapValue(p.maps[MapModY], gy, centre));
  return v;
}

void EnvcRuntime::noteOn(uint32_t id, double pitch, float velocity, const NoteProps& props) {
  if (!aux_ || !st_) return;
  const int note = static_cast<int>(std::lround(pitch));
  for (int a = 0; a < kArts; ++a) {
    const EnvcArt& m = aux_->model.arts[static_cast<size_t>(a)];
    if (note < m.keyLo || note > m.keyHi) continue;
    const EnvcAux::Art& p = aux_->arts[static_cast<size_t>(a)];
    Run& r = run_[static_cast<size_t>(a)];
    const bool sum = p.bipolar;
    const double centre = sum ? 0.5 : 0.0;
    const flart::Scales sc = scalesOf(*st_, a);
    r.attached = true;
    r.voice = id;
    r.pitch = pitch;
    r.vx = props.modX - 0.5;  // FL note mod X (−1..1) × 0.5
    r.vy = props.modY - 0.5;
    if (p.env.enabled) flart::envStart(r.env, p.env, sc, centre);
    else r.env = flart::EnvState{};
    const flart::Scales unit;
    if (p.lfo.enabled) {
      flart::envStart(r.amp, p.lfo, unit, 0.5);
      const flart::LfoShape sh = lfoShapeOf(*st_, a, m.lfo.flags);
      r.phase = (m.lfo.flags & flart::kShared) ? gphase_[static_cast<size_t>(a)] : sh.startPhase(m.lfo.phase);
    } else {
      r.amp = flart::EnvState{};
    }
    // Velocity and random maps: once per note.
    double vr = sum ? 0.0 : 1.0;
    if (!p.maps[MapVel].flat) vr = comb(sum, vr, flart::mapValue(p.maps[MapVel], velocity, centre));
    if (!p.maps[MapRandom].flat) {
      rng_ ^= rng_ << 13, rng_ ^= rng_ >> 7, rng_ ^= rng_ << 17;
      const double u = static_cast<double>(rng_ >> 11) / 9007199254740992.0;
      vr = comb(sum, vr, flart::mapValue(p.maps[MapRandom], u, centre));
    }
    r.velRand = vr;
  }
}

void EnvcRuntime::noteOff(uint32_t id) {
  if (!aux_ || !st_) return;
  for (int a = 0; a < kArts; ++a) {
    Run& r = run_[static_cast<size_t>(a)];
    if (!r.attached || r.voice != id) continue;
    const EnvcAux::Art& p = aux_->arts[static_cast<size_t>(a)];
    const flart::Scales sc = scalesOf(*st_, a), unit;
    if (p.env.enabled) flart::envRelease(r.env, p.env, sc, p.bipolar ? 0.5 : 0.0);
    if (p.lfo.enabled) flart::envRelease(r.amp, p.lfo, unit, 0.5);
  }
}

void EnvcRuntime::allOff() {
  for (const auto& r : run_)
    if (r.attached) noteOff(r.voice);
}

void EnvcRuntime::kill() {
  for (auto& r : run_) {
    const double out = r.out;
    r = Run{};
    r.out = out;
  }
  gphase_.fill(0);
}

void EnvcRuntime::render(int n) {
  if (!aux_ || !st_) return;
  const double spt = ctx_.sampleRate * 60.0 / (std::max(1.0, ctx_.tempo) * std::max(1, ctx_.ppq));
  untilTick_ -= n;
  for (int guard = 0; untilTick_ <= 0 && guard < 1 << 16; ++guard) {
    tick();
    untilTick_ += spt;
  }
}

void EnvcRuntime::tick() {
  ++ticks_;
  const PluginState& s = *st_;
  const bool continuous = prmv(s, EcContinuous, 0) >= 0.5f;
  for (int a = 0; a < kArts; ++a) {
    const EnvcAux::Art& p = aux_->arts[static_cast<size_t>(a)];
    const EnvcArt& m = aux_->model.arts[static_cast<size_t>(a)];
    Run& r = run_[static_cast<size_t>(a)];
    const bool sum = p.bipolar;
    const double centre = sum ? 0.5 : 0.0;
    const bool lfoTempo = (m.lfo.flags & flart::kTempoTime) != 0;
    const uint32_t inc = flart::lfoIncrement(speedOf(s, a), flart::unitsPerTick(lfoTempo, ctx_.tempo, ctx_.ppq));
    const bool frozen = (m.lfo.flags & flart::kFrozen) != 0;
    const bool global = (m.lfo.flags & flart::kShared) != 0;
    if (global && !frozen) gphase_[static_cast<size_t>(a)] += inc;
    if (!r.attached) {
      if (continuous) r.out = std::clamp(idle(a), 0.0, 1.0);
      continue;
    }
    const flart::Scales sc = scalesOf(s, a), unit;
    const bool envOn = p.env.enabled, lfoOn = p.lfo.enabled;
    if (envOn) flart::envTick(r.env, p.env, sc, centre, flart::unitsPerTick(p.env.tempo, ctx_.tempo, ctx_.ppq));
    if (lfoOn) {
      const bool delayed = r.amp.predelay();
      flart::envTick(r.amp, p.lfo, unit, 0.5, flart::unitsPerTick(lfoTempo, ctx_.tempo, ctx_.ppq));
      if (global) r.phase = gphase_[static_cast<size_t>(a)];
      else if (!frozen && !delayed) r.phase += inc;
    }
    double V = prmv(s, artParam(a, AfBase), 0.5f);
    if (envOn) V += prmv(s, artParam(a, AfEnvAmt), 1) * r.env.out;
    if (lfoOn) {
      const double lfo = lfoShapeOf(s, a, m.lfo.flags).value(r.phase) * r.amp.out;
      const double amt = prmv(s, artParam(a, AfLfoAmt), 0.5f);
      V = sum ? V + amt * lfo : V * (1.0 + 2.0 * amt * lfo);
    }
    const double gx = prmv(s, EcModX, 0.5f), gy = prmv(s, EcModY, 0.5f);
    if (!p.maps[MapKey].flat) V = comb(sum, V, flart::mapValue(p.maps[MapKey], r.pitch / 120.0, centre));
    if (!p.maps[MapModX].flat) V = comb(sum, V, flart::mapValue(p.maps[MapModX], r.vx + gx, centre));
    if (!p.maps[MapModY].flat) V = comb(sum, V, flart::mapValue(p.maps[MapModY], r.vy + gy, centre));
    V = comb(sum, V, r.velRand);
    r.out = std::clamp(V, 0.0, 1.0);
    if ((!envOn || r.env.ended) && (!lfoOn || r.amp.ended)) r.attached = false;  // slot ends; the output holds
  }
}

int EnvcRuntime::outputs(float* out, int max) const {
  const int k = std::min(kArts, max);
  for (int i = 0; i < k; ++i) out[i] = static_cast<float>(run_[static_cast<size_t>(i)].out);
  return k;
}

}  // namespace gl::packs::modulation
