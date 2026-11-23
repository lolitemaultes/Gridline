// Gridline: the Patch Player (gl.patchplayer) runtime: a SoundFont voice as the format describes
// it. Generators and modulators per zone (the default set, the instrument's own, the preset's),
// DAHDSR volume envelope in decibels, DAHDSR modulation envelope, modulation and vibrato LFOs,
// a resonant low-pass, exclusive classes, stereo sample pairs, reverb and chorus sends (two extra
// outputs). Program change and bank select switch the patch for the next notes.
// Audio thread: no allocation, no locks, no I/O. Control values update every 32 frames; gains
// ramp between updates, so the output does not depend on the block size.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include "../../core/PluginParams.h"
#include "../../core/Song.h"
#include "../../engine/Dsp.h"
#include "../../engine/Generators.h"
#include "../../engine/Program.h"
#include "../../engine/SampleTap.h"
#include "patchplayer.h"

namespace gl::packs::samplers {

// ===================================================================== state text, families
PatchRef parsePatchRef(const std::string& data) {
  PatchRef r;
  size_t nl = data.find('\n');
  r.path = data.substr(0, nl);
  while (!r.path.empty() && (r.path.back() == '\r' || r.path.back() == ' ')) r.path.pop_back();
  if (nl != std::string::npos) {
    std::string rest = data.substr(nl + 1);
    if (rest.rfind("patch ", 0) == 0) {
      int b = 0, p = 0, used = 0;
      if (std::sscanf(rest.c_str() + 6, "%d %d %n", &b, &p, &used) >= 2) {
        r.bank = std::clamp(b, 0, 128), r.program = std::clamp(p, 0, 127);
        std::string nm = rest.substr(6 + static_cast<size_t>(used));
        size_t e = nm.find('\n');
        if (e != std::string::npos) nm.resize(e);
        r.name = nm;
      }
    }
  }
  return r;
}

std::string serializePatchRef(const PatchRef& r) {
  std::string o = r.path;
  if (!r.name.empty() || r.bank != 0 || r.program != 0) {
    std::string nm = r.name;
    for (auto& c : nm)
      if (c == '\n' || c == '\r') c = ' ';
    o += "\npatch " + std::to_string(r.bank) + " " + std::to_string(r.program) + " " + nm;
  }
  return o;
}

void patchSends(const Sf2File& f, size_t preset, float& reverb, float& chorus) {
  reverb = chorus = 0;
  for (const auto& r : sf2PresetRegions(f, preset)) {
    reverb = std::max(reverb, std::clamp(static_cast<float>(r.gen[kReverbSend]) / 1000.f, 0.f, 1.f));
    chorus = std::max(chorus, std::clamp(static_cast<float>(r.gen[kChorusSend]) / 1000.f, 0.f, 1.f));
  }
}

void setPatch(PluginState& st, const std::string& path, int bank, int program, const std::string& name) {
  if (st.params.size() < static_cast<size_t>(PpCount)) st.params.resize(PpCount, 0.f);
  PatchRef r{path, std::clamp(bank, 0, 128), std::clamp(program, 0, 127), name};
  st.data = serializePatchRef(r);
  st.params[PpBank] = 0;  // "Chosen": the patch saved with the file
  st.params[PpPatch] = 0;
  if (auto f = loadSf2(path)) {
    int i = sf2FindPreset(*f, r.bank, r.program);
    if (i >= 0) {
      float rv = 0, ch = 0;
      patchSends(*f, static_cast<size_t>(i), rv, ch);
      st.params[PpReverb] = rv;
      st.params[PpChorus] = ch;
    }
  }
}

const std::vector<std::string>& patchFamilies() {
  static const std::vector<std::string> v = {"Piano", "Chromatic", "Organ", "Guitar", "Bass", "Strings", "Ensemble", "Brass", "Reed",
                                             "Pipe", "Synth lead", "Synth pad", "Synth effects", "World", "Percussive", "Sound effects",
                                             "Drum kits"};
  return v;
}

const char* patchFamily(int bank, int program) {
  const auto& v = patchFamilies();
  if (bank >= 120) return v.back().c_str();  // 128 (and the 120..127 kit banks some files use)
  return v[static_cast<size_t>(std::clamp(program, 0, 127) / 8)].c_str();
}

PatchInfo patchInfo(const Sf2File& f, size_t preset) {
  PatchInfo pi;
  auto regs = sf2PresetRegions(f, preset);
  pi.zones = static_cast<int>(regs.size());
  pi.bits24 = !f.data24.empty();
  pi.compressed = f.sf3;
  std::vector<int> samples, vlos, classes;
  std::vector<std::pair<int, int>> rs;
  for (const auto& r : regs) {
    if (std::find(samples.begin(), samples.end(), r.sample) == samples.end()) {
      samples.push_back(r.sample);
      const auto& h = f.samples[static_cast<size_t>(r.sample)];
      pi.memory += static_cast<size_t>(h.end > h.start ? h.end - h.start : 0) * (f.data24.empty() ? 2 : 3);
      if (h.type & 4) ++pi.stereoPairs;
    }
    if (std::find(vlos.begin(), vlos.end(), r.vlo) == vlos.end()) vlos.push_back(r.vlo);
    pi.keyLo = std::min(pi.keyLo, r.klo), pi.keyHi = std::max(pi.keyHi, r.khi);
    const int modes = r.gen[kSampleModes] & 3;
    if (modes == 1 || modes == 3) pi.loops = true;
    else pi.allLoop = false;
    if (r.gen[kModEnvToPitch] != 0) pi.modEnvPitch = true;
    if (r.gen[kModEnvToFc] != 0) pi.modEnvFilter = true;
    if (r.gen[kModLfoToPitch] != 0 || r.gen[kModLfoToFc] != 0 || r.gen[kModLfoToVol] != 0) pi.modLfo = true;
    if (r.gen[kVibLfoToPitch] != 0) pi.vibLfo = true;
    pi.filterQcB = std::max(pi.filterQcB, r.gen[kFilterQ]);
    if (r.gen[kFilterFc] < 13500) pi.lowPass = true;
    if (r.gen[kExclusive] != 0 && std::find(classes.begin(), classes.end(), r.gen[kExclusive]) == classes.end()) classes.push_back(r.gen[kExclusive]);
    pi.reverb = std::max(pi.reverb, std::clamp(static_cast<float>(r.gen[kReverbSend]) / 1000.f, 0.f, 1.f));
    pi.chorus = std::max(pi.chorus, std::clamp(static_cast<float>(r.gen[kChorusSend]) / 1000.f, 0.f, 1.f));
    int own = 0;
    for (const auto& m : r.mods) {
      bool isDefault = false;
      for (const auto& d : sf2DefaultMods())
        if (d.sameAs(m) && d.amount == m.amount) isDefault = true;
      if (!isDefault) ++own;
    }
    pi.ownMods = std::max(pi.ownMods, own);
    rs.emplace_back(r.klo, r.khi);
  }
  if (regs.empty()) pi.allLoop = false, pi.keyLo = pi.keyHi = 0;
  pi.lfos = (pi.modLfo ? 1 : 0) + (pi.vibLfo ? 1 : 0);
  pi.samples = static_cast<int>(samples.size());
  pi.velocityLayers = std::max(1, static_cast<int>(vlos.size()));
  pi.exclusiveClasses = static_cast<int>(classes.size());
  std::sort(rs.begin(), rs.end());
  for (const auto& r : rs) {
    if (!pi.ranges.empty() && r.first <= pi.ranges.back().second) pi.ranges.back().second = std::max(pi.ranges.back().second, r.second);
    else pi.ranges.push_back(r);
  }
  return pi;
}

// ===================================================================== prepared data
namespace {

const char* const kPpTag = "\x01gl.patchplayer.rt";

struct PpRegion {
  Sf2Region r;
  int64_t start = 0, end = 0, ls = 0, le = 0;  // absolute frames in the file's data
  double srcRate = 44100;
  int root = 60, corr = 0;
  int partner = -1;  // stereo pair: index of the right zone this left zone plays with
  bool right = false;  // a right zone played by its left partner
};
struct PpPreset {
  int bank = 0, program = 0;
  std::vector<PpRegion> regions;
};
struct PpRuntime : Sample {
  std::shared_ptr<const Sf2File> file;
  std::vector<PpPreset> presets;  // same order as file->presets
};

std::shared_ptr<const PpRuntime> buildRuntime(const std::string& path) {
  static std::mutex mu;
  static std::vector<std::shared_ptr<const PpRuntime>> recent;  // kept: never freed on the audio thread
  bool failed = false;
  auto f = requestSf2(path, &failed);
  std::lock_guard<std::mutex> lk(mu);
  if (!f) {
    static auto empty = [] {
      auto e = std::make_shared<PpRuntime>();
      e->path = kPpTag;
      return std::shared_ptr<const PpRuntime>(e);
    }();
    return empty;
  }
  for (size_t i = 0; i < recent.size(); ++i)
    if (recent[i]->file == f) {
      auto r = recent[i];
      recent.erase(recent.begin() + static_cast<long>(i));
      recent.push_back(r);
      return r;
    }
  auto rt = std::make_shared<PpRuntime>();
  rt->path = kPpTag;
  rt->file = f;
  const int64_t n = static_cast<int64_t>(f->data.size());
  for (size_t p = 0; p < f->presets.size(); ++p) {
    PpPreset pr;
    pr.bank = f->presets[p].bank, pr.program = f->presets[p].program;
    for (auto& r : sf2PresetRegions(*f, p)) {
      const auto& h = f->samples[static_cast<size_t>(r.sample)];
      PpRegion x;
      const int64_t s0 = std::clamp<int64_t>(h.start, 0, n), s1 = std::clamp<int64_t>(h.end, s0, n);
      if (s1 - s0 < 2) continue;
      x.start = std::clamp<int64_t>(s0 + r.gen[kStartOfs] + 32768LL * r.gen[kStartCoarse], s0, s1 - 2);
      x.end = std::clamp<int64_t>(s1 + r.gen[kEndOfs] + 32768LL * r.gen[kEndCoarse], x.start + 2, s1);
      x.ls = std::clamp<int64_t>(static_cast<int64_t>(h.loopStart) + r.gen[kLoopStartOfs] + 32768LL * r.gen[kLoopStartCoarse], x.start, x.end - 1);
      x.le = std::clamp<int64_t>(static_cast<int64_t>(h.loopEnd) + r.gen[kLoopEndOfs] + 32768LL * r.gen[kLoopEndCoarse], x.ls + 1, x.end);
      x.srcRate = h.rate > 0 ? h.rate : 44100;
      x.root = r.gen[kRootKey] >= 0 && r.gen[kRootKey] <= 127 ? r.gen[kRootKey] : (h.originalPitch <= 127 ? h.originalPitch : 60);
      x.corr = h.correction;
      x.r = std::move(r);
      pr.regions.push_back(std::move(x));
    }
    // Stereo pairs: a left zone and the right zone of its linked sample with the same ranges.
    for (size_t i = 0; i < pr.regions.size(); ++i) {
      auto& a = pr.regions[i];
      const auto& ha = f->samples[static_cast<size_t>(a.r.sample)];
      if (!(ha.type & 4)) continue;
      for (size_t j = 0; j < pr.regions.size(); ++j) {
        auto& b = pr.regions[j];
        const auto& hb = f->samples[static_cast<size_t>(b.r.sample)];
        if (j == i || b.right || !(hb.type & 2) || ha.link != static_cast<uint16_t>(b.r.sample)) continue;
        if (a.r.klo != b.r.klo || a.r.khi != b.r.khi || a.r.vlo != b.r.vlo || a.r.vhi != b.r.vhi) continue;
        a.partner = static_cast<int>(j);
        b.right = true;
        break;
      }
    }
    rt->presets.push_back(std::move(pr));
  }
  recent.push_back(rt);
  if (recent.size() > 12) recent.erase(recent.begin());
  return rt;
}

// ===================================================================== modulators
double concave(double x) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  return std::min(1.0, -(5.0 / 6.0) * std::log10(1.0 - x));
}
double convex(double x) { return 1.0 - concave(1.0 - x); }

struct Ctl {
  const uint8_t* cc = nullptr;
  int key = 60, vel = 100, polyPressure = 0, chanPressure = 0, wheel = 8192;
  double wheelSens = 2;  // semitones
};

// A source word through its curve; false when the source is not one a modulator may use.
bool source(uint16_t s, const Ctl& c, double& out) {
  const int idx = s & 0x7F;
  double x = 0;
  if (s & 0x80) {
    if (idx == 0 || idx == 6 || (idx >= 32 && idx <= 63) || (idx >= 98 && idx <= 101) || idx >= 120) return false;
    x = c.cc[idx] / 127.0;
  } else {
    switch (idx) {
      case 0: out = 1; return true;  // no controller: a constant one
      case 2: x = c.vel / 127.0; break;
      case 3: x = c.key / 127.0; break;
      case 10: x = c.polyPressure / 127.0; break;
      case 13: x = c.chanPressure / 127.0; break;
      case 14: x = c.wheel / 16384.0; break;
      case 16: x = c.wheelSens / 127.0; break;
      default: return false;  // links and unknown sources
    }
  }
  // A 7-bit bipolar source is centred on 64 (pan at 64 is the middle), still reaching both ends.
  if ((s & 0x200) && ((s & 0x80) || idx != 14)) {  // (the pitch wheel is 14 bits, centred already)
    const double raw = x * 127.0;
    x = raw <= 64.0 ? raw / 128.0 : 0.5 + (raw - 64.0) / 126.0;
  }
  if (s & 0x100) x = 1.0 - x;
  const int type = s >> 10;
  if (!(s & 0x200)) {
    out = type == 1 ? concave(x) : type == 2 ? convex(x) : type == 3 ? (x >= 0.5 ? 1 : 0) : x;
  } else {
    switch (type) {
      case 1: out = x > 0.5 ? concave(2 * x - 1) : -concave(1 - 2 * x); break;
      case 2: out = x > 0.5 ? convex(2 * x - 1) : -convex(1 - 2 * x); break;
      case 3: out = x >= 0.5 ? 1 : -1; break;
      default: out = 2 * x - 1; break;
    }
  }
  return true;
}

double tcSeconds(double tc) { return std::pow(2.0, std::clamp(tc, -12000.0, 8000.0) / 1200.0); }
double tcSecondsOrZero(double tc) { return tc <= -12000 ? 0.0 : tcSeconds(tc); }
double absCentsHz(double c) { return 8.176 * std::pow(2.0, c / 1200.0); }

constexpr int kCtl = 32;  // frames between control updates

// Volume envelope: delay, attack (linear amplitude), hold, decay and release (linear in decibels:
// the set time covers 100 dB), sustain as attenuation.
struct VolEnv {
  enum St { Delay, Attack, Hold, Decay, Sustain, Release, Done } st = Delay;
  double left = 0, a = 0, cb = 0, susCb = 0, attackN = 0, holdN = 0, decayRate = 0, releaseRate = 0;
  void start(double delayN, double atkN, double hldN, double decN, double sus, double relN) {
    st = Delay, left = delayN, a = 0, cb = 0;
    attackN = atkN, holdN = hldN;
    decayRate = 1000.0 / std::max(1.0, decN);
    susCb = std::clamp(sus, 0.0, 1440.0);
    releaseRate = 1000.0 / std::max(1.0, relN);
    if (left <= 0) enterAttack();
  }
  void enterAttack() {
    st = Attack;
    if (attackN <= 0) a = 1, enterHold();
  }
  void enterHold() {
    st = Hold, left = holdN, cb = 0;
    if (left <= 0) st = Decay;
  }
  void release() {
    if (st == Done || st == Release) return;
    if (st == Delay) {
      st = Done;
      return;
    }
    if (st == Attack) cb = a > 1e-6 ? -200.0 * std::log10(a) : 1000.0;
    st = Release;
  }
  void advance(double n) {
    switch (st) {
      case Delay:
        if ((left -= n) <= 0) enterAttack();
        break;
      case Attack:
        a += n / std::max(1.0, attackN);
        if (a >= 1) a = 1, enterHold();
        break;
      case Hold:
        if ((left -= n) <= 0) st = Decay;
        break;
      case Decay:
        cb += decayRate * n;
        if (cb >= susCb) cb = susCb, st = Sustain;
        if (cb >= 960) st = Done;
        break;
      case Sustain:
        if (cb >= 960) st = Done;
        break;
      case Release:
        cb += releaseRate * n;
        if (cb >= 960) st = Done;
        break;
      case Done: break;
    }
  }
  double gain() const {
    switch (st) {
      case Delay: case Done: return 0;
      case Attack: return a;
      default: return std::pow(10.0, -cb / 200.0);
    }
  }
};

// Modulation envelope: 0..1, linear segments; the set decay and release times cover the full range.
struct ModEnv {
  enum St { Delay, Attack, Hold, Decay, Sustain, Release, Done } st = Delay;
  double left = 0, v = 0, sus = 0, attackN = 0, holdN = 0, decayN = 1, releaseN = 1;
  void start(double delayN, double atkN, double hldN, double decN, double s, double relN) {
    st = Delay, left = delayN, v = 0, attackN = atkN, holdN = hldN, decayN = std::max(1.0, decN), sus = std::clamp(s, 0.0, 1.0),
    releaseN = std::max(1.0, relN);
    if (left <= 0) st = Attack;
  }
  void release() {
    if (st != Done) st = Release;
  }
  void advance(double n) {
    switch (st) {
      case Delay:
        if ((left -= n) <= 0) st = Attack;
        break;
      case Attack:
        v += attackN > 0 ? n / attackN : 1.0;
        if (v >= 1) v = 1, st = Hold, left = holdN;
        break;
      case Hold:
        if ((left -= n) <= 0) st = Decay;
        break;
      case Decay:
        v -= n / decayN;
        if (v <= sus) v = sus, st = Sustain;
        break;
      case Sustain: break;
      case Release:
        v -= n / releaseN;
        if (v <= 0) v = 0, st = Done;
        break;
      case Done: break;
    }
  }
};

// Triangle LFO from 0 rising, after a delay.
struct Lfo {
  double delay = 0, phase = 0, inc = 0;
  void start(double delayN, double hz, double sr) {
    delay = delayN, phase = 0, inc = hz / sr;
  }
  void advance(double n) {
    if (delay > 0) {
      delay -= n;
      if (delay > 0) return;
      n = -delay;
      delay = 0;
    }
    phase += inc * n;
    phase -= std::floor(phase);
  }
  double value() const {
    if (delay > 0) return 0;
    const double p = phase;
    return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
  }
};

// =====================================================================
class PatchPlayerGen final : public Generator {
 public:
  PatchPlayerGen() {
    resetControllers();
    for (auto& b : aux_) b.fill(0.f);
  }
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }
  bool handlesPitchBend() const override { return true; }
  bool supportsGlide() const override { return true; }
  float noteBendRange() const override { return static_cast<float>(bendSens()); }
  bool handlesNotePan() const override { return false; }
  int auxOutputCount() const override { return 2; }
  bool auxOutput(int k, const float*& L, const float*& R) const override {
    if (k < 0 || k > 1) return false;
    L = aux_[static_cast<size_t>(k * 2)].data();
    R = aux_[static_cast<size_t>(k * 2 + 1)].data();
    return true;
  }
  // Tests and the editor: the patch the player plays now (index into the file's presets, -1 none).
  int currentPreset() const { return cur_; }

 protected:
  struct Voice {
    bool on = false, held = false, sustained = false, looping = false, releasing = false, fading = false;
    uint32_t id = 0;
    int key = 60, vel = 100, gen = 0, excl = 0;
    uint64_t age = 0;
    const PpRegion* reg = nullptr;
    const PpRegion* reg2 = nullptr;  // stereo pair's right zone
    const PpPreset* preset = nullptr;
    double pitch = 60, pos = 0;  // pos: frames from the zone's start
    PitchGlide slide;
    VolEnv vol;
    ModEnv mod;
    Lfo modLfo, vibLfo;
    dsp::Biquad fL, fR;
    bool filterOn = false;
    double lastFc = -1, lastQ = -1;
    double rate = 1;
    int ctl = 0;
    // Gains ramp per frame between control updates.
    float g = 0, dg = 0, gL = 1, gR = 1, rev = 0, cho = 0, fade = 1, fadeStep = 0;
    std::array<int32_t, kSf2GenCount> base{};  // the zone's generators (modulators add to these)
  };

  void resetControllers() {
    cc_.fill(0);
    cc_[7] = 100, cc_[10] = 64, cc_[11] = 127;
    cc_[101] = cc_[100] = 127;
    chanPressure_ = 0;
    polyPressure_.fill(0);
    wheel_ = 8192;
  }
  double bendSens() const { return rpnSens_ >= 0 ? rpnSens_ : bendParam_; }

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    blockPos_ = 0;
    SamplePtr rt = aux.sample(0);
    bool newMap = false;
    if (rt.get() != map_.get()) {
      for (auto& v : voices_) {
        if (!v.on) continue;
        if (v.gen >= 1) v.on = false;  // two builds behind: its zone data is gone
        else v.gen = 1;
      }
      prev_ = std::move(map_);
      if (rt && rt->path == kPpTag) map_ = std::static_pointer_cast<const PpRuntime>(rt);
      else map_.reset();
      newMap = true;
    }
    auto p = [&](int i, float d = 0.f) { return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : d; };
    // The patch chosen in the editor (saved with the file, aux.slices = {bank, program}), unless the
    // Bank or Patch parameter holds a number.
    const int db = aux.slices.size() >= 2 ? static_cast<int>(aux.slices[0]) : 0;
    const int dp = aux.slices.size() >= 2 ? static_cast<int>(aux.slices[1]) : 0;
    const int nb = static_cast<int>(std::lround(p(PpBank))), np = static_cast<int>(std::lround(p(PpPatch)));
    const int pb = std::clamp(nb > 0 ? nb - 1 : db, 0, 128);
    const int pp = std::clamp(np > 0 ? np - 1 : dp, 0, 127);
    if (pb != paramBank_ || pp != paramProg_) {  // the choice moved: it chooses the patch again
      paramBank_ = pb, paramProg_ = pp;
      bank_ = pb, prog_ = pp;
      // K-348: a program change without a bank select stays in the chosen bank (a drum kit
      // channel picks another kit); a bank select message still chooses another bank.
      bankSelected_ = false;
      newMap = true;
    }
    if (newMap) resolve();
    volume_ = p(PpVolume, -3) <= -59.9f ? 0.0 : std::pow(10.0, p(PpVolume, -3) / 20.0);
    pan_ = std::clamp(p(PpPan), -1.f, 1.f);
    reverb_ = std::clamp(p(PpReverb), 0.f, 1.f);
    chorus_ = std::clamp(p(PpChorus), 0.f, 1.f);
    transpose_ = p(PpTranspose) * 100.0 + p(PpFine);
    bendParam_ = std::clamp(static_cast<double>(p(PpBendRange, 2)), 0.0, 48.0);
    glide_ = std::max(0.f, p(PpGlide)) / 1000.0;
    poly_ = std::clamp(static_cast<int>(std::lround(p(PpPolyphony, 64))), 1, kPpMaxVoices);
    mono_ = p(PpMono) >= 0.5f;
    velCurve_ = static_cast<int>(std::lround(p(PpVelocity)));
    cutoffOfs_ = p(PpCutoff) * 72.0;
    resoOfs_ = p(PpResonance) * 3.0;
    keyTrack_ = p(PpKeyTrack);
    envMode_ = static_cast<int>(std::lround(p(PpEnvMode)));
    for (int k = 0; k < 4; ++k) envScale_[k] = p(PpAttackScale + k, 1);
    for (int k = 0; k < 4; ++k) envSet_[k] = p(PpAttack + k);
    vibOwn_ = std::lround(p(PpVibMode)) == PpVibOwn;
    vibDepth_ = p(PpVibDepth), vibRate_ = std::max(0.01f, p(PpVibRate, 5)), vibDelay_ = std::max(0.f, p(PpVibDelay)) / 1000.0;
    modWheel_ = p(PpModWheel, 50);
  }

  // The preset for the bank and program now chosen (nearest when the file lacks it).
  void resolve() {
    cur_ = -1;
    if (!map_ || !map_->file || map_->presets.empty()) return;
    cur_ = sf2NearestPreset(*map_->file, bank_, prog_);
  }

  void onMidi(uint8_t st, uint8_t d1, uint8_t d2) override {
    const uint8_t kind = st & 0xF0;
    if (kind == 0xC0) {
      prog_ = d1 & 0x7F;
      if (!bankSelected_) {
        bank_ = paramBank_ < 0 ? 0 : paramBank_;  // K-348: the chosen patch's bank (a drum kit channel picks another kit)
      } else {
        bank_ = std::min(128, bankMsb_ != 0 || bankLsb_ == 0 ? bankMsb_ : bankLsb_);
        if (bankMsb_ == 120 || bankMsb_ == 127) bank_ = 128;  // the usual drum kit bank numbers
      }
      resolve();
    } else if (kind == 0xB0) {
      const int c = d1 & 0x7F, v = d2 & 0x7F;
      cc_[static_cast<size_t>(c)] = static_cast<uint8_t>(v);
      switch (c) {
        case 0: bankMsb_ = v, bankSelected_ = true; break;
        case 32: bankLsb_ = v, bankSelected_ = true; break;
        case 6:  // data entry: RPN 0 is the pitch wheel sensitivity
          if (cc_[101] == 0 && cc_[100] == 0) rpnSens_ = v + rpnCents_ / 100.0;
          break;
        case 38:
          if (cc_[101] == 0 && cc_[100] == 0) rpnCents_ = v, rpnSens_ = (rpnSens_ >= 0 ? std::floor(rpnSens_) : bendParam_) + v / 100.0;
          break;
        case 64:
          if (v < 64)
            for (auto& x : voices_)
              if (x.on && x.sustained && !x.held) release(x);
          break;
        case 120: onKill(); break;
        case 121: {
          const int msb = bankMsb_, lsb = bankLsb_;
          resetControllers();
          bankMsb_ = msb, bankLsb_ = lsb;
          for (auto& x : voices_)
            if (x.on && x.sustained && !x.held) release(x);
          break;
        }
        case 123: onAllOff(); break;
        default: break;
      }
    } else if (kind == 0xE0) {
      wheel_ = (d1 & 0x7F) | ((d2 & 0x7F) << 7);
    } else if (kind == 0xD0) {
      chanPressure_ = d1 & 0x7F;
    } else if (kind == 0xA0) {
      polyPressure_[static_cast<size_t>(d1 & 0x7F)] = static_cast<uint8_t>(d2 & 0x7F);
    }
  }

  int velocity127(float vel) const {
    double v = std::clamp(static_cast<double>(vel), 0.0, 1.0);
    switch (velCurve_) {
      case PpVelSoft: v = std::sqrt(v); break;
      case PpVelHard: v = v * v; break;
      case PpVelFixed: v = 1; break;
      default: break;
    }
    return std::clamp(static_cast<int>(std::lround(v * 127.0)), 1, 127);
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
  Voice& freeVoice() {
    for (auto& v : voices_)
      if (!v.on) return v;
    Voice* best = nullptr;
    for (auto& v : voices_)
      if (v.fading && (!best || v.fade < best->fade)) best = &v;
    if (!best)
      for (auto& v : voices_)
        if (!best || v.age < best->age) best = &v;
    return *best;
  }

  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    if (!map_ || cur_ < 0 || cur_ >= static_cast<int>(map_->presets.size())) return;
    const PpPreset& pr = map_->presets[static_cast<size_t>(cur_)];
    const int key = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    const int vel = velocity127(velocity);
    int chosen[64], n = 0;
    for (size_t i = 0; i < pr.regions.size() && n < 64; ++i) {
      const PpRegion& r = pr.regions[i];
      if (r.right || !r.r.contains(key, vel)) continue;
      chosen[n++] = static_cast<int>(i);
    }
    if (n == 0) return;
    double startPitch = pitch;
    if (mono_) {
      bool anyHeld = false;
      for (auto& v : voices_)
        if (v.on && v.held && !v.fading) anyHeld = true;
      if (anyHeld && glide_ > 1e-4 && hasLast_) startPitch = lastPitch_;
      for (auto& v : voices_)
        if (v.on) fadeOut(v);
    }
    lastPitch_ = pitch;
    hasLast_ = true;
    // Exclusive classes: a new zone of a class ends the sounding ones of that class.
    for (int c = 0; c < n; ++c) {
      const int ex = pr.regions[static_cast<size_t>(chosen[c])].r.gen[kExclusive];
      if (ex == 0) continue;
      for (auto& v : voices_)
        if (v.on && v.excl == ex && v.preset == &pr && v.id != id) fadeOut(v);
    }
    while (activeCount() + n > poly_ && activeCount() > 0) {
      Voice* old = nullptr;
      for (auto& v : voices_)
        if (v.on && !v.fading && (!old || v.age < old->age)) old = &v;
      if (!old) break;
      fadeOut(*old);
    }
    for (int c = 0; c < n; ++c) {
      const PpRegion& r = pr.regions[static_cast<size_t>(chosen[c])];
      Voice& v = freeVoice();
      v = Voice{};
      v.on = v.held = true;
      v.id = id;
      v.key = key;
      v.vel = vel;
      v.age = ++clock_;
      v.reg = &r;
      v.reg2 = r.partner >= 0 ? &pr.regions[static_cast<size_t>(r.partner)] : nullptr;
      v.preset = &pr;
      v.pitch = startPitch;
      if (startPitch != pitch) v.slide.start(startPitch, pitch, glide_, sr_);
      v.excl = r.r.gen[kExclusive];
      v.base = r.r.gen;
      const int modes = r.r.gen[kSampleModes] & 3;
      v.looping = modes == 1 || modes == 3;
      startVoice(v);
    }
  }

  void onGlide(uint32_t id, double pitch, double seconds) override {
    for (auto& v : voices_)
      if (v.on && v.id == id) v.slide.start(v.pitch, pitch, seconds, sr_);
  }

  void onNoteOff(uint32_t id) override {
    for (auto& v : voices_) {
      if (!v.on || !v.held || v.id != id) continue;
      v.held = false;
      if (cc_[64] >= 64) v.sustained = true;
      else release(v);
    }
  }
  void onCut(uint32_t id) override {
    for (auto& v : voices_)
      if (v.on && v.id == id) v.held = false, fadeOut(v);
  }
  void release(Voice& v) {
    v.sustained = false;
    if (v.releasing) return;
    v.releasing = true;
    v.vol.release();
    v.mod.release();
    if ((v.reg->r.gen[kSampleModes] & 3) == 3) v.looping = false;  // loop until release: play on to the end
  }
  void onAllOff() override {
    for (auto& v : voices_)
      if (v.on) v.held = false, release(v);
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
  }

  // Modulator sums for every destination (the voice's controllers now).
  void modSums(const Voice& v, double* out) const {
    std::fill(out, out + kSf2GenCount, 0.0);
    Ctl c;
    c.cc = cc_.data();
    c.key = v.key, c.vel = v.vel;
    c.polyPressure = polyPressure_[static_cast<size_t>(v.key)];
    c.chanPressure = chanPressure_;
    c.wheel = wheel_;
    c.wheelSens = bendSens();
    for (const auto& m : v.reg->r.mods) {
      if (m.dest >= kSf2GenCount) continue;  // links to other modulators are not followed
      double a = 0, b = 0;
      if (!source(m.src, c, a) || !source(m.amtSrc, c, b)) continue;
      double amount = m.amount;
      if (m.src == 0x0081 && m.dest == kVibLfoToPitch) amount = modWheel_;  // the Mod wheel control
      double o = amount * a * b;
      if (m.trans == 2) o = std::fabs(o);
      out[m.dest] += o;
    }
  }

  void startVoice(Voice& v) {
    double mods[kSf2GenCount];
    modSums(v, mods);
    auto g = [&](int op) { return static_cast<double>(v.base[static_cast<size_t>(op)]) + mods[op]; };
    const int keyNum = v.base[kKeynum] >= 0 ? v.base[kKeynum] : v.key;
    // Volume envelope (from the patch, scaled, or replaced by the player's own).
    double delay = tcSecondsOrZero(g(kDelayVolEnv)), atk = tcSeconds(g(kAttackVolEnv));
    double hold = tcSecondsOrZero(g(kHoldVolEnv) + (60 - keyNum) * g(kKeyToVolEnvHold));
    double dec = tcSeconds(g(kDecayVolEnv) + (60 - keyNum) * g(kKeyToVolEnvDecay));
    double susCb = std::clamp(g(kSustainVolEnv), 0.0, 1440.0), rel = tcSeconds(g(kReleaseVolEnv));
    if (envMode_ == PpEnvScale) {
      atk *= envScale_[0], dec *= envScale_[1], rel *= envScale_[3];
      const double amp = std::clamp(std::pow(10.0, -susCb / 200.0) * envScale_[2], 1e-5, 1.0);
      susCb = -200.0 * std::log10(amp);
    } else if (envMode_ == PpEnvReplace) {
      delay = 0, hold = 0;
      atk = envSet_[0], dec = envSet_[1], rel = envSet_[3];
      const double amp = std::clamp(static_cast<double>(envSet_[2]), 1e-5, 1.0);
      susCb = -200.0 * std::log10(amp);
    }
    v.vol.start(delay * sr_, atk * sr_, hold * sr_, dec * sr_, susCb, std::max(0.001, rel) * sr_);
    v.mod.start(tcSecondsOrZero(g(kDelayModEnv)) * sr_, tcSeconds(g(kAttackModEnv)) * sr_,
                tcSecondsOrZero(g(kHoldModEnv) + (60 - keyNum) * g(kKeyToModEnvHold)) * sr_,
                tcSeconds(g(kDecayModEnv) + (60 - keyNum) * g(kKeyToModEnvDecay)) * sr_, 1.0 - std::clamp(g(kSustainModEnv), 0.0, 1000.0) / 1000.0,
                tcSeconds(g(kReleaseModEnv)) * sr_);
    v.modLfo.start(tcSecondsOrZero(g(kDelayModLfo)) * sr_, absCentsHz(g(kFreqModLfo)), sr_);
    if (vibOwn_) v.vibLfo.start(vibDelay_ * sr_, vibRate_, sr_);
    else v.vibLfo.start(tcSecondsOrZero(g(kDelayVibLfo)) * sr_, absCentsHz(g(kFreqVibLfo)), sr_);
    v.pos = static_cast<double>(0);
    v.ctl = 0;
    v.g = 0, v.dg = 0;
    control(v, true);
  }

  // Pitch, level, pan, sends and filter from the generators, modulators, envelopes and LFOs.
  void control(Voice& v, bool first) {
    double mods[kSf2GenCount];
    modSums(v, mods);
    auto g = [&](int op) { return static_cast<double>(v.base[static_cast<size_t>(op)]) + mods[op]; };
    const PpRegion& r = *v.reg;
    const double me = v.mod.v, ml = v.modLfo.value(), vl = v.vibLfo.value();
    const double keyF = v.base[kKeynum] >= 0 ? static_cast<double>(v.base[kKeynum]) : v.pitch;
    const double vibDepth = vibOwn_ ? vibDepth_ + mods[kVibLfoToPitch] : g(kVibLfoToPitch);
    const double cents = (keyF - r.root) * g(kScaleTuning) + g(kCoarseTune) * 100.0 + g(kFineTune) + r.corr + transpose_ +
                         me * g(kModEnvToPitch) + ml * g(kModLfoToPitch) + vl * vibDepth;
    v.rate = std::pow(2.0, cents / 1200.0) * r.srcRate / sr_;
    // Level: attenuation (the EMU-style 0.4 scale of the format's centibels), tremolo, envelope.
    const double att = std::clamp(g(kAttenuation), 0.0, 1440.0) * 0.4 - ml * g(kModLfoToVol);
    const double target = v.vol.gain() * std::pow(10.0, -att / 200.0) * volume_;
    if (first) v.g = static_cast<float>(target), v.dg = 0;
    else v.dg = static_cast<float>((target - v.g) / kCtl);
    // Pan (the zone's, then the player's), sends.
    const double pan = std::clamp(g(kPan) / 500.0 + pan_, -1.0, 1.0);
    if (v.reg2) {  // a stereo pair keeps its two sides; the pan control balances them
      v.gL = static_cast<float>(std::min(1.0, 1.0 - pan_)), v.gR = static_cast<float>(std::min(1.0, 1.0 + pan_));
    } else {
      v.gL = static_cast<float>(std::min(1.0, 1.0 - pan)), v.gR = static_cast<float>(std::min(1.0, 1.0 + pan));
    }
    v.rev = static_cast<float>(std::clamp(reverb_ + mods[kReverbSend] / 1000.0, 0.0, 1.0));
    v.cho = static_cast<float>(std::clamp(chorus_ + mods[kChorusSend] / 1000.0, 0.0, 1.0));
    // Low-pass: cutoff in absolute cents, Q in centibels above the pass band.
    const double fc = g(kFilterFc) + me * g(kModEnvToFc) + ml * g(kModLfoToFc) + cutoffOfs_ + keyTrack_ * (v.pitch - 60.0) * 100.0;
    const double q = std::clamp(g(kFilterQ) + resoOfs_, 0.0, 960.0);
    const bool on = fc < 13500 || q > 0;
    if (on) {
      const double hz = std::clamp(absCentsHz(std::clamp(fc, 1500.0, 13500.0)), 20.0, sr_ * 0.45);
      if (!v.filterOn || std::fabs(hz - v.lastFc) > 1e-3 || q != v.lastQ) {
        const double qq = std::pow(10.0, (q / 10.0 - 3.01) / 20.0);  // 0 cB: no peak (0.707)
        v.fL.set(dsp::Biquad::LowPass, sr_, hz, qq, 0);
        v.fR.set(dsp::Biquad::LowPass, sr_, hz, qq, 0);
        if (!v.filterOn) v.fL.x1 = v.fL.x2 = v.fL.y1 = v.fL.y2 = v.fR.x1 = v.fR.x2 = v.fR.y1 = v.fR.y2 = 0;
        v.lastFc = hz, v.lastQ = q;
      }
    }
    v.filterOn = on;
  }

  // One frame of a zone's sample (cubic Hermite), relative position `p`, wrapping inside the loop.
  float readAt(const PpRegion& r, double p, bool wrap) const {
    const Sf2File& f = *map_->file;
    const int64_t len = r.end - r.start, ls = r.ls - r.start, le = r.le - r.start;
    const int64_t i = static_cast<int64_t>(std::floor(p));
    const float t = static_cast<float>(p - static_cast<double>(i));
    auto at = [&](int64_t k) -> float {
      if (wrap && k >= le && le > ls) k = ls + (k - le) % (le - ls);
      if (k < 0 || k >= len) return 0.f;
      return f.at(static_cast<size_t>(r.start + k));
    };
    const float x0 = at(i - 1), x1 = at(i), x2 = at(i + 1), x3 = at(i + 2);
    const float c1 = 0.5f * (x2 - x0), c2 = x0 - 2.5f * x1 + 2.f * x2 - 0.5f * x3, c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
  }

  void render(float* L, float* R, int n) override {
    const int base = blockPos_;
    const int auxN = std::clamp(kEngineMaxBlock - base, 0, n);
    for (auto& b : aux_) std::fill(b.begin() + base, b.begin() + base + auxN, 0.f);
    blockPos_ += n;
    for (auto& v : voices_)
      if (v.on) renderVoice(v, L, R, n, base, auxN);
  }

  void renderVoice(Voice& v, float* L, float* R, int n, int base, int auxN) {
    const PpRegion& r = *v.reg;
    const double len = static_cast<double>(r.end - r.start), ls = static_cast<double>(r.ls - r.start), le = static_cast<double>(r.le - r.start);
    float* rl = aux_[0].data() + base;
    float* rr = aux_[1].data() + base;
    float* cl = aux_[2].data() + base;
    float* cr = aux_[3].data() + base;
    for (int i = 0; i < n; ++i) {
      if (v.ctl-- <= 0) {
        v.ctl = kCtl - 1;
        v.vol.advance(kCtl);
        v.mod.advance(kCtl);
        v.modLfo.advance(kCtl);
        v.vibLfo.advance(kCtl);
        if (v.slide.active()) v.slide.advance(v.pitch, kCtl);
        if (v.vol.st == VolEnv::Done && v.g < 1e-6f) {
          v.on = false;
          return;
        }
        control(v, false);
      }
      const bool wrap = v.looping && le - ls >= 1;
      float sl = readAt(r, v.pos, wrap);
      float sr = v.reg2 ? readAt(*v.reg2, v.pos, wrap) : sl;
      if (v.filterOn) {
        sl = static_cast<float>(v.fL.process(sl));
        sr = v.reg2 ? static_cast<float>(v.fR.process(sr)) : sl;
      }
      if (v.fading) {
        v.fade -= v.fadeStep;
        if (v.fade <= 0) {
          v.on = false;
          return;
        }
      }
      // Fade over the last frames of a sample that does not loop (no click at its end).
      float edge = 1.f;
      if (!wrap) {
        const double framesLeft = (len - 1.0 - v.pos) / std::max(1e-6, v.rate);
        if (framesLeft < 32) edge = static_cast<float>(std::max(0.0, framesLeft / 32.0));
      }
      const float gg = v.g * v.fade * edge;
      v.g += v.dg;
      const float ol = gg * v.gL * sl, orr = gg * v.gR * sr;
      L[i] += ol;
      R[i] += orr;
      if (i < auxN) {
        rl[i] += ol * v.rev, rr[i] += orr * v.rev;
        cl[i] += ol * v.cho, cr[i] += orr * v.cho;
      }
      v.pos += v.rate;
      if (wrap) {
        if (v.pos >= le) v.pos = ls + std::fmod(v.pos - ls, le - ls);
      } else if (v.pos >= len - 1) {
        v.on = false;
        return;
      }
    }
  }

 private:
  std::array<Voice, kPpMaxVoices> voices_{};
  std::shared_ptr<const PpRuntime> map_, prev_;
  std::array<std::array<float, kEngineMaxBlock>, 4> aux_{};  // reverb L/R, chorus L/R
  int blockPos_ = 0;
  double sr_ = 44100, volume_ = 0.7, transpose_ = 0, bendParam_ = 2, glide_ = 0, cutoffOfs_ = 0, resoOfs_ = 0, keyTrack_ = 0;
  double vibDepth_ = 0, vibRate_ = 5, vibDelay_ = 0, modWheel_ = 50, rpnSens_ = -1, rpnCents_ = 0;
  float pan_ = 0, reverb_ = 0, chorus_ = 0;
  float envScale_[4] = {1, 1, 1, 1}, envSet_[4] = {0.01f, 1, 1, 0.3f};
  int poly_ = 64, velCurve_ = PpVelNormal, envMode_ = PpEnvPatch;
  bool mono_ = false, vibOwn_ = false;
  bool bankSelected_ = false;  // K-348: a bank select arrived since the patch was chosen
  int paramBank_ = -1, paramProg_ = -1, bank_ = 0, prog_ = 0, bankMsb_ = 0, bankLsb_ = 0, cur_ = -1;
  std::array<uint8_t, 128> cc_{};
  std::array<uint8_t, 128> polyPressure_{};
  int chanPressure_ = 0, wheel_ = 8192;
  double lastPitch_ = 60;
  bool hasLast_ = false;
  uint64_t clock_ = 0;
};

}  // namespace

std::shared_ptr<const Sample> buildPatchRuntime(const std::string& path) { return buildRuntime(path); }

std::unique_ptr<Generator> makePatchPlayer() { return std::make_unique<PatchPlayerGen>(); }

int patchPlayerPreset(const Generator* g) {
  auto* p = dynamic_cast<const PatchPlayerGen*>(g);
  return p ? p->currentPreset() : -2;
}

bool setPatchNearest(PluginState& st, const std::string& path, int bank, int program) {
  bank = std::clamp(bank, 0, 128), program = std::clamp(program, 0, 127);
  auto f = loadSf2(path);
  if (!f || f->presets.empty()) {
    setPatch(st, path, bank, program, "");
    return false;
  }
  const auto& p = f->presets[static_cast<size_t>(std::max(0, sf2NearestPreset(*f, bank, program)))];
  setPatch(st, path, p.bank, p.program, p.name);
  return true;
}

bool setPatchForMidiTrack(PluginState& st, const std::string& path, int midiChannel, int bank, int program) {
  if (midiChannel == 9) return setPatchNearest(st, path, 128, std::max(0, program));
  return setPatchNearest(st, path, bank >= 120 ? 0 : bank, std::max(0, program));
}

namespace {
bool trackUnused(const Song& s, const MixerTrack& m) {
  if (m.id == s.masterId()) return false;
  for (const auto& c : s.channels) {
    if (c.mixerTrack == m.id) return false;
    for (ObjectId x : c.auxOuts)
      if (x == m.id) return false;
  }
  for (const auto& t : s.mixer)
    for (const auto& snd : t.sends)
      if (snd.to == m.id) return false;
  for (const auto& sl : m.slots)
    if (!sl.plugin.empty()) return false;
  return m.name.empty();
}
}  // namespace

std::pair<ObjectId, ObjectId> patchSendTracks(const Song& s, ObjectId channel) {
  const Channel* c = s.channel(channel);
  if (!c) return {kNoId, kNoId};
  return {!c->auxOuts.empty() ? c->auxOuts[0] : kNoId, c->auxOuts.size() > 1 ? c->auxOuts[1] : kNoId};
}

bool routePatchSends(Song& s, ObjectId channel) {
  Channel* c = s.channel(channel);
  if (!c || c->plugin.type != kPatchPlayerType || s.mixer.empty()) return false;
  ObjectId made[2] = {kNoId, kNoId};
  size_t next = 1;
  for (int k = 0; k < 2; ++k) {
    while (next < s.mixer.size() && !trackUnused(s, s.mixer[next])) ++next;
    if (next >= s.mixer.size()) {
      if (static_cast<int>(s.mixer.size()) > kMaxMixerInserts) return false;
      MixerTrack t;
      t.id = s.allocateId();
      Send toMaster;
      toMaster.to = s.masterId();
      t.sends.push_back(toMaster);
      s.mixer.push_back(t);
      next = s.mixer.size() - 1;
    }
    MixerTrack& m = s.mixer[next];
    // Fully wet: the dry sound already plays through the channel's own track.
    PluginState fx = defaultPluginState(k == 0 ? "gl.reverb" : "gl.chorus");
    if (k == 0) {
      if (fx.params.size() > pp::RvbWet) fx.params[pp::RvbDry] = 0.f, fx.params[pp::RvbWet] = 1.f;
    } else if (fx.params.size() > pp::ChoWetOnly) {
      fx.params[pp::ChoMix] = 1.f, fx.params[pp::ChoWetOnly] = 1.f;
    }
    m.slots[0].plugin = fx;
    m.name = c->name + (k == 0 ? " reverb" : " chorus");
    made[k] = m.id;
    ++next;
  }
  c->auxOuts = {made[0], made[1]};
  return true;
}

bool preparePatchPlayerAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != kPatchPlayerType) return false;
  const PatchRef r = parsePatchRef(state.data);
  aux.samples = {buildRuntime(r.path)};
  aux.slices = {r.bank, r.program};
  return true;
}

}  // namespace gl::packs::samplers
