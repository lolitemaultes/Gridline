// Gridline — FLP export: stand-ins for Gridline-only plugins (J-80). See FlpStandIn.h.
#include "FlpStandIn.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include "../../engine/Program.h"
#include "../../engine/Render.h"
#include "../../engine/Sample.h"
#include "../../engine/SampledInstrument.h"
#include "../../engine/Slicing.h"
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/samplers/dwave.h"
#include "../../packs/synths/samplekeys.h"
#include "../../packs/utility/utility.h"
#include "../Params.h"
#include "../PluginParams.h"
#include "../WavWriter.h"
#include "FlpEncode.h"
#include "FlpOrigin.h"

namespace gl::flp {

namespace fs = std::filesystem;

namespace {

float prm(const PluginState& s, int k, float d = 0.f) {
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : d;
}
void put(PluginState& s, int k, double v) {
  if (k >= 0 && static_cast<size_t>(k) < s.params.size()) s.params[static_cast<size_t>(k)] = static_cast<float>(v);
}
int iprm(const PluginState& s, int k, int d = 0) { return static_cast<int>(std::lround(prm(s, k, static_cast<float>(d)))); }
double clampd(double v, double lo, double hi) { return std::clamp(v, lo, hi); }
double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

// ------------------------------------------------------------ catalogue

const std::set<std::string>& drumTypes() {
  static const std::set<std::string> s = {"gl.kick", "gl.snare", "gl.hat", "gl.clap", "gl.pads"};
  return s;
}

}  // namespace

StandInPolicy standInPolicy(const std::string& type) {
  static const std::map<std::string, StandInPolicy> table = [] {
    std::map<std::string, StandInPolicy> m;
    auto mapped = [&](const char* t, const char* fl) { m[t] = {StandInKind::Mapped, fl, ""}; };
    auto neutral = [&](const char* t, const char* why) { m[t] = {StandInKind::Neutral, "", why}; };
    // Effects: the closest FL plugin, settings translated.
    mapped("gl.chorus", "Fruity Chorus");
    mapped("gl.delay", "Fruity Delay 3");
    mapped("gl.flanger", "Fruity Flanger");
    mapped("gl.phaser", "Fruity Phaser");
    mapped("gl.dist", "Fruity Fast Dist");
    mapped("gl.bitcrush", "Fruity Squeeze");
    mapped("gl.gate", "Fruity Limiter");
    mapped("gl.maximizer", "Fruity Limiter");
    mapped("gl.scduck", "Fruity Limiter");
    mapped("gl.drawneq", "Fruity Parametric EQ 2");
    mapped("gl.analyzer", "Wave Candy");
    mapped("gl.bussend", "Fruity Send");
    // Instruments with a close FL instrument.
    mapped("gl.keys", "FL Keys");
    // Neutral, with the reason.
    neutral("gl.busreturn", "bus return: the audio arrives through the bus send's Fruity Send route; nothing to process here");
    neutral("gl.shaper", "step volume / filter shaper: no FL effect plays a tempo-synced 16-step volume, filter and pan pattern");
    neutral("gl.arp", "note processor: FL's channel arpeggiator carries one arpeggiator (rendered instruments include it)");
    neutral("gl.chord", "note processor: FL channels have no chord generator; a rendered instrument plays the chords as rendered");
    return m;
  }();
  if (auto it = table.find(type); it != table.end()) return it->second;
  const PluginInfo* pi = findPlugin(type);
  if (pi && pi->kind == PluginKind::Generator) return {StandInKind::Rendered, "", ""};
  if (pi && pi->legacyModelParam >= 0) return {StandInKind::Mapped, "", ""};  // its reference engine's FL plugin
  return {StandInKind::Neutral, "", "no FL counterpart"};
}

// ------------------------------------------------------------ used notes

UsedNotes usedNotes(const Song& song, ObjectId channel) {
  UsedNotes u;
  const Channel* c = song.channel(channel);
  if (!c) return u;
  std::vector<const Channel*> players = {c};  // the channel, and layers that play it
  for (const Channel& l : song.channels)
    if (l.id != channel && std::find(l.layer.begin(), l.layer.end(), channel) != l.layer.end()) players.push_back(&l);
  const double secPerTick = 60.0 / (std::max(1.0, song.tempo) * std::max(1, song.ppq));
  std::map<int, std::set<int>> v;
  for (const Pattern& p : song.patterns)
    for (const Channel* pl : players)
      for (const NoteEv& e : compileChannelNotes(song, p, *pl)) {
        if (e.flags & kNoteSlide) continue;
        const int key = std::clamp(static_cast<int>(std::lround(e.pitch)), 0, 127);
        const int vel = std::clamp(static_cast<int>(std::lround(e.velocity * 127.0f)), 1, 127);
        v[key].insert(vel);
        u.longestSeconds = std::max(u.longestSeconds, static_cast<double>(e.off - e.on) * secPerTick);
      }
  for (auto& [k, s] : v) u.velocities[k] = std::vector<int>(s.begin(), s.end());
  return u;
}

namespace {

// ------------------------------------------------------------ effect mappings
// Each returns the stand-in state (the FL plugin's Gridline type) and a note.

struct Mapping {
  PluginState state;
  std::string note;
  bool ok = false;
};

Mapping mapChorus(const PluginState& g) {
  using namespace pp;
  Mapping m;
  m.state = g;
  PluginState& s = m.state;
  // The Gridline engine sweeps base·(1 ± depth/2); the FL one sweeps delay .. delay + depth.
  const double base = prm(g, ChoDelay, 12), depth = prm(g, ChoDepth, 0.5);
  put(s, ChoClassic, 1);
  put(s, ChoDelay, clampd(base * (1 - depth * 0.5), 0.05, 30));
  put(s, ChoDepthMs, clampd(base * depth, 0, 5));
  const int voices = std::clamp(iprm(g, ChoVoices) + 1, 1, 3);
  for (int k = voices; k < 3; ++k) put(s, k == 1 ? ChoRate2 : ChoRate3, prm(g, ChoRate, 0.6f));  // fewer voices: in step
  const int wave = prm(g, ChoShape) > 0.5f ? 1 : 0;
  put(s, ChoWave1, wave), put(s, ChoWave2, wave), put(s, ChoWave3, wave);
  put(s, ChoWetOnly, prm(g, ChoMix) >= 0.95f ? 1 : prm(g, ChoWetOnly));
  m.note = "dry / wet mix and feedback approximated (FL's chorus mixes 0.7 dry + 0.6 wet)";
  m.ok = true;
  return m;
}

Mapping mapDelay(const PluginState& g) {
  using namespace pp;
  Mapping m;
  m.state = g;
  PluginState& s = m.state;
  put(s, DlyModel, 1);
  const int ti = std::clamp(iprm(g, DlyTime, 8), 0, kDelayBeatCount - 1);
  put(s, DlyTicks, clampd(std::round(kDelayBeats[ti] * 192.0), 1, 768));
  static const int kType[5] = {1, 2, 1, 0, 3};  // normal, ping-pong, invert, mono, off → true stereo / ping-pong / fake stereo / off
  put(s, DlyType3, kType[std::clamp(iprm(g, DlyMode, 1), 0, 4)]);
  {  // stereo offset (ms) → R/L offset fraction of the time (T·x²/4)
    const double tSec = prm(g, DlySync, 1) >= 0.5f ? kDelayBeats[ti] * 60.0 / 120.0 : prm(g, DlyTimeMs, 300) * 0.001;
    const double off = prm(g, DlyOffset) * 0.001;
    const double x = std::min(1.0, std::sqrt(4.0 * std::fabs(off) / std::max(1e-3, tSec)));
    put(s, DlyTimeR, off < 0 ? -x : x);
  }
  static const int kFilter[5] = {0, 0, 1, 2, 3};  // classic / LP / HP / BP / off
  put(s, DlyFilter3, kFilter[std::clamp(iprm(g, DlyFilterType), 0, 4)]);
  put(s, DlyCutoff3, clampd(prm(g, DlyCutoff, 7000), 20, 22000));
  put(s, DlyModRate3, clampd(prm(g, DlyModRate, 0.5f), 0.11, 20));
  put(s, DlyRate3, clampd(prm(g, DlyRateReduce, 48000), 150, 48000));
  put(s, DlySpread3, prm(g, DlyStereoSpread, 1));
  m.note = "feedback low cut and saturation not carried";
  m.ok = true;
  return m;
}

Mapping mapDist(const PluginState& g) {
  namespace dy = gl::packs::dynamics;
  using namespace pp;
  Mapping m;
  m.state = defaultPluginState("gl.fastdist");
  PluginState& s = m.state;
  put(s, dy::FdClassic, 1);
  const int mode = iprm(g, DstMode);
  put(s, dy::FdType, mode == 1 || mode == 2 || mode == 5 || mode == 7 ? 1 : 0);  // hard / fold / fast B / rectify → B
  put(s, dy::FdDrive, clampd(std::round(1 + 9 * prm(g, DstDrive, 12) / 40.0), 1, 10));
  put(s, dy::FdMixK, clampd(prm(g, DstMix, 1), 0, 1));
  const double post = dbToGain(prm(g, DstOutput));
  put(s, dy::FdPostK, clampd(std::log(10 * post + 1) / std::log(11.0), 0, 1));
  m.note = "drive, tone and pre band approximated by the clipper's drive";
  m.ok = true;
  return m;
}

Mapping mapBitcrush(const PluginState& g) {
  namespace dy = gl::packs::dynamics;
  using namespace pp;
  Mapping m;
  m.state = defaultPluginState("gl.squeeze");
  PluginState& s = m.state;
  put(s, dy::SqModel, 1);
  put(s, dy::SqStepsRef, clampd(std::round(std::pow(2.0, prm(g, BcBits, 8))), 1, 32768));
  put(s, dy::SqMix, prm(g, BcMix, 1));
  put(s, dy::SqFilterOn, prm(g, BcFilterOn));
  put(s, dy::SqFilterType, std::clamp(iprm(g, BcFilterType), 0, 3));
  put(s, dy::SqCutoffPos, clampd(std::log(prm(g, BcFilterFreq, 2000) / 20.0) / std::log(1000.0), 0, 1));
  put(s, dy::SqResonance, prm(g, BcFilterReso));
  put(s, dy::SqFilterPos, prm(g, BcFilterPos));
  put(s, dy::SqPreserveMs, clampd(prm(g, BcPreserve, 0.1f) * 1000.0, 0, 1000));
  put(s, dy::SqImpactMs, clampd(prm(g, BcImpact, 0.1f) * 1000.0, 0, 1000));
  put(s, dy::SqAmountRef, prm(g, BcPunch));
  put(s, dy::SqGainRef, clampd(dbToGain(prm(g, BcOutput)), 0, 1.2598));
  m.note = "sample-rate reduction and squarize not carried";
  m.ok = true;
  return m;
}

PluginState limiterBase() {
  using namespace pp;
  PluginState s = defaultPluginState("gl.limiter");
  put(s, LimGain, 0);
  put(s, LimCeiling, 12);  // no limiting
  put(s, LimCompThresh, 0);
  put(s, LimCompRatio, 1);
  return s;
}

Mapping mapGate(const PluginState& g) {
  using namespace pp;
  Mapping m;
  m.state = limiterBase();
  put(m.state, LimGateThresh, prm(g, GtThreshold, -40));
  put(m.state, LimGateFloor, prm(g, GtRange, -80));
  put(m.state, LimGateRelease, clampd(prm(g, GtRelease, 0.1f), 0.001, 2));
  m.note = "noise gate section; attack, hold, hysteresis and key filters not carried";
  m.ok = true;
  return m;
}

Mapping mapMaximizer(const PluginState& g) {
  using namespace pp;
  Mapping m;
  m.state = defaultPluginState("gl.limiter");
  put(m.state, LimGain, clampd(prm(g, MxDrive), -36, 24));
  put(m.state, LimCeiling, prm(g, MxCeiling));
  put(m.state, LimRelease, clampd(prm(g, MxRelease, 0.1f), 0.001, 2));
  m.note = "single-band limiter: band gains and band compressors not carried";
  m.ok = true;
  return m;
}

Mapping mapDucker(const PluginState& g) {
  using namespace pp;
  namespace mo = gl::packs::modulation;
  Mapping m;
  m.state = limiterBase();
  PluginState& s = m.state;
  put(s, LimView, 1);
  put(s, LimCompThresh, prm(g, mo::SdThreshold, -24));
  put(s, LimCompRatio, clampd(prm(g, mo::SdRatio, 4), 0.25, 20));
  put(s, LimCompAttack, clampd(prm(g, mo::SdAttack, 0.005f), 0.0001, 1));
  put(s, LimCompRelease, clampd(prm(g, mo::SdRelease, 0.2f), 0.001, 2));
  put(s, LimSidechain, iprm(g, mo::SdSource) == 0 ? 1 : 0);
  m.note = "compressor section keyed by the sidechain; depth limit not carried";
  m.ok = true;
  return m;
}

Mapping mapCurveEq(const PluginState& g) {
  using namespace pp;
  Mapping m;
  m.state = defaultPluginState("gl.eq");
  PluginState& s = m.state;
  // The drawn curve at the morph position (bilinear between curves A..D), on its 23 points.
  constexpr int kN = 23;
  const double x = prm(g, 92, 0), y = prm(g, 93, 0), depth = prm(g, 94, 1);
  std::vector<double> curve(kN), freq(kN);
  for (int i = 0; i < kN; ++i) {
    freq[static_cast<size_t>(i)] = 20.0 * std::pow(1000.0, i / static_cast<double>(kN - 1));
    const double a = prm(g, i), b = prm(g, kN + i), c = prm(g, 2 * kN + i), d = prm(g, 3 * kN + i);
    curve[static_cast<size_t>(i)] = depth * ((a * (1 - x) + b * x) * (1 - y) + (c * (1 - x) + d * x) * y);
  }
  // Greedy fit: a peak band at the largest remaining deviation, up to seven.
  for (int band = 0; band < kEqBands; ++band) {
    size_t best = 0;
    for (size_t i = 1; i < curve.size(); ++i)
      if (std::fabs(curve[i]) > std::fabs(curve[best])) best = i;
    const double gain = curve[best];
    if (std::fabs(gain) < 0.5) break;
    put(s, eqParam(band, EqType), EqPeak);
    put(s, eqParam(band, EqFreq), freq[best]);
    put(s, eqParam(band, EqGain), clampd(gain, -18, 18));
    put(s, eqFl(band, EqWidth), 0.3);
    for (size_t i = 0; i < curve.size(); ++i) {
      const double oct = std::log2(freq[i] / freq[best]);
      curve[i] -= gain * std::exp(-(oct / 0.6) * (oct / 0.6));
    }
  }
  put(s, EqMain, clampd(prm(g, 97), -18, 18));
  m.note = "drawn curve fitted with peak bands";
  m.ok = true;
  return m;
}

Mapping mapAnalyzer(const PluginState&) {
  namespace ut = gl::packs::utility;
  Mapping m;
  m.state = defaultPluginState("gl.scope");
  put(m.state, ut::ScEngine, 1);
  m.note = "display only (does not change the sound)";
  m.ok = true;
  return m;
}

// Generic: the type's reference engine (its FL plugin), the instance's other settings kept.
Mapping mapReferenceEngine(const PluginState& g, const enc::EncodeContext& ec) {
  Mapping m;
  const PluginInfo* pi = findPlugin(g.type);
  if (!pi || pi->legacyModelParam < 0) return m;
  const ParamDesc& d = pi->params[static_cast<size_t>(pi->legacyModelParam)];
  const int cur = iprm(g, pi->legacyModelParam);
  for (int v = static_cast<int>(std::lround(d.max)); v >= static_cast<int>(std::lround(d.min)); --v) {
    if (v == cur) continue;
    PluginState q = g;
    put(q, pi->legacyModelParam, v);
    const enc::Result r = enc::encode(q, "", nullptr, ec);
    if (r.quality != enc::Result::None && !r.flName.empty()) {
      m.state = q;
      m.note = "Gridline's own engine written as the reference engine's FL plugin; its own settings approximated";
      m.ok = true;
      return m;
    }
  }
  return m;
}

// gl.keys → FL Keys (Sample Keys stand-in, the reference's raw knobs).
Mapping mapKeys(const PluginState& g) {
  using namespace pp;
  namespace sk = gl::packs::synths::skeys;
  Mapping m;
  m.state = defaultPluginState("gl.samplekeys");
  PluginState& s = m.state;
  // Log-scaled value around its default → raw −1000..1000 (0 = the reference's default).
  auto around = [](double v, double lo, double def, double hi) {
    v = clampd(v, lo, hi);
    return v >= def ? 1000.0 * std::log(v / def) / std::log(hi / def) : -1000.0 * std::log(def / v) / std::log(def / lo);
  };
  auto lin = [](double v, double lo, double def, double hi) {
    v = clampd(v, lo, hi);
    return v >= def ? 1000.0 * (v - def) / std::max(1e-9, hi - def) : -1000.0 * (def - v) / std::max(1e-9, def - lo);
  };
  put(s, sk::SkDecay, around(prm(g, KyDecay, 3.5f), 0.3, 3.5, 12));
  put(s, sk::SkRelease, around(prm(g, KyRelease, 0.25f), 0.02, 0.25, 3));
  put(s, sk::SkHardness, lin(prm(g, KyHardness, 0.45f), 0, 0.45, 1));
  put(s, sk::SkVelHardness, -1000.0 + 2000.0 * clampd(prm(g, KyVelHardness), 0, 1));
  {
    const double hz = clampd(prm(g, KyMuffle, 9000), 500, 20000);
    const double raw = hz >= 9000 ? 784 + (1000 - 784) * std::log(hz / 9000) / std::log(20000.0 / 9000)
                                  : 784 * (1 - std::log(9000 / hz) / std::log(9000.0 / 500));
    put(s, sk::SkMuffle, raw);
  }
  put(s, sk::SkVelMuffle, 2000.0 * clampd(prm(g, KyVelMuffle), 0, 1));
  {
    const double vs = clampd(prm(g, KyVelocity, 0.7f), 0, 1);
    put(s, sk::SkVelSens, vs >= 0.7 ? -248 + 1248 * (vs - 0.7) / 0.3 : -248 - 752 * (0.7 - vs) / 0.7);
  }
  put(s, sk::SkStereo, 2000.0 * clampd(prm(g, KyStereo, 0.5f), 0, 1));
  put(s, sk::SkTune, clampd(prm(g, KyTune) * 10.0, -1000, 1000));
  put(s, sk::SkDetune, clampd(123.0 * std::sqrt(std::max(0.0f, prm(g, KyDetune, 4)) / 4.0), 0, 500));
  put(s, sk::SkStretch, clampd(prm(g, KyStretch) * 10.0, -500, 500));
  put(s, sk::SkTreble, clampd(prm(g, KyTreble) / 12.0 * 1000.0, -1000, 1000));
  put(s, sk::SkPanTrem, clampd(prm(g, KyTremDepth, 0.15f), 0, 1) * (iprm(g, KyModMode) == 1 ? 1000.0 : -1000.0));
  put(s, sk::SkLfoRate, clampd((std::log(clampd(prm(g, KyTremRate, 4.5f), 0.1, 10)) + 4.61) / 9.22, 0, 1) * 65536.0);
  put(s, sk::SkOverdrive, 1000.0 * clampd(prm(g, KyDrive, 0.1f), 0, 1));
  for (auto& v : s.params) v = static_cast<float>(std::round(v));
  static const char* kSet[6] = {"Rhodes", "Rhodes", "Rhodes", "Rhodes", "Grand Piano", "mda Piano"};
  s.data = kSet[std::clamp(iprm(g, KyModel), 0, 5)];
  m.note = "Gridline's modelled keys played by FL Keys' sampled set; tone settings approximated";
  m.ok = true;
  return m;
}

// ------------------------------------------------------------ rendering

std::string safeStem(const std::string& name) {
  std::string out;
  for (char c : name) out += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
                              static_cast<unsigned char>(c) < 0x20)
                                 ? '-'
                                 : c;
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out.empty() ? "Channel" : out;
}

// Velocities to render: clusters of the used velocities (neighbours closer
// than 8 merge; the loudest of each is rendered), at most eight.
std::vector<int> velocityClusters(const std::set<int>& used) {
  std::vector<int> v(used.begin(), used.end()), out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (!out.empty() && v[i] - out.back() < 8 && i > 0) out.back() = v[i];
    else out.push_back(v[i]);
  }
  while (out.size() > 8) {  // merge the closest pair
    size_t best = 1;
    for (size_t i = 1; i < out.size(); ++i)
      if (out[i] - out[i - 1] < out[best] - out[best - 1]) best = i;
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(best) - 1);
  }
  return out;
}

// True when the takes at lower velocities are the loudest take scaled by
// velocity (the sampler / DirectWave velocity law): one layer suffices.
bool velocityIsLevel(const std::vector<sampled::Take>& takes) {
  if (takes.size() < 2) return true;
  const sampled::Take& top = takes.back();
  double topE = 0;
  for (size_t c = 0; c < top.audio.channels; ++c)
    for (size_t i = 0; i < top.audio.frames; ++i) topE += static_cast<double>(top.audio.at(c, i)) * top.audio.at(c, i);
  if (topE < 1e-12) return true;
  for (size_t k = 0; k + 1 < takes.size(); ++k) {
    const sampled::Take& t = takes[k];
    const double g = static_cast<double>(t.velocity) / top.velocity;
    double err = 0, ref = 0;
    const size_t n = std::min(t.audio.frames, top.audio.frames);
    for (size_t c = 0; c < std::min(t.audio.channels, top.audio.channels); ++c)
      for (size_t i = 0; i < n; ++i) {
        const double want = top.audio.at(c, i) * g, d = t.audio.at(c, i) - want;
        err += d * d, ref += want * want;
      }
    for (size_t c = 0; c < t.audio.channels; ++c)  // anything longer counts as difference
      for (size_t i = n; i < t.audio.frames; ++i) err += static_cast<double>(t.audio.at(c, i)) * t.audio.at(c, i);
    if (ref < 1e-12 || err / ref > 0.02) return false;  // > −17 dB difference: audibly different
  }
  return true;
}

struct RenderOut {
  PluginState state;
  std::vector<std::string> files;
  std::string note;
  std::string error;
  bool cancelled = false;
};

RenderOut renderInstrument(const Song& song, const Channel& ch, const UsedNotes& un, const std::string& dir, const std::string& stem,
                           const StandInOptions& o, double p0, double p1) {
  RenderOut out;
  const bool drum = drumTypes().count(ch.plugin.type) > 0;
  sampled::Options so;
  so.sampleRate = o.sampleRate;
  so.bits = 24;
  so.noteSeconds = clampd(un.longestSeconds, 0.05, drum ? 4.0 : 6.0);
  so.tailSeconds = drum ? 1.5 : 2.5;
  so.autoTrim = true;
  so.trimDb = -72;
  so.findLoops = !drum;
  so.name = stem;
  std::vector<int> keys;
  std::set<int> allVel;
  for (const auto& [k, vs] : un.velocities) {
    keys.push_back(k);
    allVel.insert(vs.begin(), vs.end());
  }
  if (keys.size() > 32) {  // a wide range: every few semitones across it
    const int lo = keys.front(), hi = keys.back(), step = std::max(1, (hi - lo + 23) / 24);
    keys.clear();
    for (int k = lo; k < hi; k += step) keys.push_back(k);
    keys.push_back(hi);
  }
  auto progress = [&](double f) {
    if (o.progress) o.progress(p0 + (p1 - p0) * clampd(f, 0, 1));
  };
  std::string err;
  // Velocity: probe the busiest note at each cluster; one layer when only the level changes.
  std::vector<int> layers = velocityClusters(allVel);
  if (layers.size() > 1) {
    int probe = keys.front(), most = -1;
    for (const auto& [k, vs] : un.velocities)
      if (static_cast<int>(vs.size()) > most) probe = k, most = static_cast<int>(vs.size());
    sampled::Options po = so;
    po.notes = {probe};
    po.velocities = layers;
    po.autoTrim = false, po.findLoops = false;
    std::vector<sampled::Take> takes;
    if (!sampled::renderTakes(song, ch.id, po, takes, &err, [&](double f) { progress(f * 0.1); }, o.cancel)) {
      out.cancelled = o.cancel && o.cancel->load();
      out.error = err;
      return out;
    }
    if (velocityIsLevel(takes)) layers = {layers.back()};
  }
  so.notes = keys;
  so.velocities = layers;
  std::vector<sampled::Take> takes;
  if (!sampled::renderTakes(song, ch.id, so, takes, &err, [&](double f) { progress(0.1 + f * 0.85); }, o.cancel)) {
    out.cancelled = o.cancel && o.cancel->load();
    out.error = err;
    return out;
  }
  float peak = 0;
  for (const auto& t : takes) peak = std::max(peak, t.audio.peak());
  if (peak < 1e-6f) {
    out.error = "the instrument made no sound";
    return out;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    out.error = "cannot create " + dir;
    return out;
  }
  const double sampVol = 0.8;  // gl.sampler / FL sampler channel default volume
  const bool single = takes.size() == 1;
  const double samplerGain = single ? 127.0 / takes[0].velocity / sampVol : 1.0;
  if (single && drum && peak * samplerGain <= 0.999) {
    // One note at one level: an FL sampler channel with the take as its sample.
    sampled::Take& t = takes[0];
    for (size_t c = 0; c < t.audio.channels; ++c)
      for (size_t i = 0; i < t.audio.frames; ++i) t.audio.at(c, i) = static_cast<float>(t.audio.at(c, i) * samplerGain);
    const std::string file = (fs::path(dir) / (stem + "_" + sampled::noteLabel(t.note) + ".wav")).string();
    if (!writeWav(file, t.audio, so.sampleRate, 24)) {
      out.error = "cannot write " + file;
      return out;
    }
    PluginState s = defaultPluginState("gl.sampler");
    s.data = file;
    put(s, pp::SmpRoot, t.note);
    put(s, pp::SmpDeclick, 1);  // FL has no "off": "out only" (the take already fades out)
    out.state = s;
    out.files.push_back(file);
    out.note = "rendered to a sampler channel (" + sampled::noteLabel(t.note) + ")";
    progress(1);
    return out;
  }
  // DirectWave: one zone per note and layer; levels above full scale are
  // written lower and made up by the zone gain.
  const double scale = peak > 0.999f ? 0.999 / peak : 1.0;
  for (auto& t : takes) {
    if (scale != 1.0)
      for (size_t c = 0; c < t.audio.channels; ++c)
        for (size_t i = 0; i < t.audio.frames; ++i) t.audio.at(c, i) = static_cast<float>(t.audio.at(c, i) * scale);
    std::string f = stem + "_" + sampled::noteLabel(t.note);
    if (layers.size() > 1) f += "_v" + std::to_string(t.velocity);
    t.file = (fs::path(dir) / (f + ".wav")).string();
    if (!writeWav(t.file, t.audio, so.sampleRate, 24)) {
      out.error = "cannot write " + t.file;
      return out;
    }
    out.files.push_back(t.file);
  }
  PluginState st = sampled::programState(takes, so);
  namespace dwn = gl::packs::samplers::dw;
  dwn::DwProgram prog;
  if (dwn::parseProgram(st.data, prog)) {
    for (auto& z : prog.zones) {
      z.gain = static_cast<float>(z.gain / scale);
      if (drum) z.amp[3] = 1.f;  // one-shot: the take plays on after the note off
    }
    st.data = dwn::formatProgram(prog);
  }
  out.state = st;
  out.note = "rendered to DirectWave: " + std::to_string(keys.size()) + " note(s)" +
             (layers.size() > 1 ? " × " + std::to_string(layers.size()) + " velocity layers" : std::string());
  progress(1);
  return out;
}

// RMS of one channel's part in the song (the others muted), optionally with
// another instrument on it.
double channelRms(const Song& song, ObjectId channel, const PluginState* replace) {
  Song s = song;
  for (auto& c : s.channels) {
    c.muted = c.id != channel && std::find(c.layer.begin(), c.layer.end(), channel) == c.layer.end();
    if (c.id == channel && replace) c.plugin = *replace;
  }
  for (auto& t : s.mixer) t.slots.clear();  // the instrument's own level (and much faster)
  // Eight bars from where the channel first plays.
  auto plays = [&](const Clip& c) {
    if (c.kind != ClipKind::Pattern) return false;
    const Pattern* p = s.pattern(c.pattern);
    if (!p) return false;
    for (const Channel& ch : s.channels)
      if (!ch.muted && (p->notes.count(ch.id) || p->steps.count(ch.id))) return true;
    return false;
  };
  Tick t0 = -1;
  for (const Clip& c : s.playlist)
    if (plays(c) && (t0 < 0 || c.start < t0)) t0 = c.start;
  if (t0 < 0) return 0;
  const Tick t1 = t0 + 8 * s.barTicks();
  std::vector<Clip> kept;
  for (Clip c : s.playlist)
    if (c.kind == ClipKind::Pattern && c.start >= t0 && c.start < t1) {
      c.start -= t0;
      c.length = std::min<Tick>(s.clipLength(c), t1 - t0 - c.start);
      kept.push_back(c);
    }
  s.playlist = std::move(kept);
  s.automations.clear();
  RenderOptions ro;
  ro.tail = s.ppq * 4;
  const AudioBuffer a = renderSong(s, ro);
  double e = 0;
  for (float v : a.data) e += static_cast<double>(v) * v;
  return a.data.empty() ? 0 : std::sqrt(e / static_cast<double>(a.data.size()));
}

// Parameters of a replaced plugin can no longer be targeted: their automation
// and links are dropped (the clip stays, without a target).
int dropTargets(Song& s, ObjectId owner, int slot) {
  auto hit = [&](ObjectId o, uint32_t index) {
    if (o != owner) return false;
    if (slot < 0) return index >= kChannelPluginBase && index < kNoteFxBase;
    if (index < kSlotBase) return false;
    const uint32_t sl = (index - kSlotBase) / kSlotStride, k = (index - kSlotBase) % kSlotStride;
    return static_cast<int>(sl) == slot && k != kSlotMix && k != kSlotEnable;
  };
  int n = 0;
  for (auto& a : s.automations)
    if (hit(a.owner, a.index)) a.owner = kNoId, a.index = 0, ++n;
  for (auto& p : s.patterns) {
    const size_t before = p.automation.size();
    p.automation.erase(std::remove_if(p.automation.begin(), p.automation.end(), [&](const PatternAutomation& a) { return hit(a.owner, a.index); }),
                       p.automation.end());
    n += static_cast<int>(before - p.automation.size());
  }
  {
    const size_t before = s.links.size();
    s.links.erase(std::remove_if(s.links.begin(), s.links.end(), [&](const auto& l) { return hit(l.owner, l.index); }), s.links.end());
    n += static_cast<int>(before - s.links.size());
  }
  {
    const size_t before = s.modLinks.size();
    s.modLinks.erase(std::remove_if(s.modLinks.begin(), s.modLinks.end(), [&](const auto& l) { return hit(l.owner, l.index); }), s.modLinks.end());
    n += static_cast<int>(before - s.modLinks.size());
  }
  return n;
}

bool padsUseBuiltinSounds(const PluginState& p, const UsedNotes& un) {
  for (int k = 0; k < pp::kPadsAll; ++k) {
    int note = iprm(p, pp::padE(k, pp::PadNote), -1);
    if (note < 0) note = iprm(p, pp::PadsBaseNote, 60) + k;
    if (!un.velocities.count(note)) continue;
    if (k < pp::kPads && padSamplePath(p.data, k, 0).empty()) return true;
  }
  return false;
}

}  // namespace

bool applyStandIns(Song& song, const ExportOptions& eo, const StandInOptions& o, StandInResult& r) {
  r = StandInResult{};
  enc::EncodeContext ec;
  ec.tempo = song.tempo;
  ec.ppq = song.ppq;
  ec.factoryData = eo.factoryData;
  ec.projectDir = eo.projectDir;
  auto hasOrigin = [&](const PluginState& p, ObjectId owner, int slot) {
    if (!eo.useOrigin) return false;
    const EventList ev = originEvents(song, pluginKey(owner, slot));
    return !ev.empty() && originTypeMatches(ev, p.type);
  };
  const Song original = song;  // renders play the song as it is
  // Instruments first (the renders), then effects.
  struct Job {
    size_t channel;
    UsedNotes notes;
  };
  std::vector<Job> renders;
  std::set<std::string> stems;
  for (size_t i = 0; i < song.channels.size(); ++i) {
    Channel& ch = song.channels[i];
    PluginState& p = ch.plugin;
    if (p.empty() || p.type == "gl.sampler" || isHostedType(p.type) || !findPlugin(p.type) || hasOrigin(p, ch.id, -1)) continue;
    const std::string where = "channel '" + ch.name + "'";
    if (p.type == "gl.keys") {
      Mapping m = mapKeys(p);
      r.entries.push_back({ch.id, -1, where, p.type, "mapped", "FL Keys", m.note, {}});
      dropTargets(song, ch.id, -1);
      m.state.ins = p.ins;
      p = m.state;
      continue;
    }
    const enc::Result er = enc::encode(p, "", nullptr, ec);
    // (as the exporter decides: an encoder that is a native home, or has defaults)
    bool home = false;
    for (const auto& e : enc::encoders())
      if (e.glType == p.type && (!e.accepts || e.accepts(p)) && (e.nativeRank >= 0 || e.defaults)) home = true;
    bool render = !home || er.quality == enc::Result::None || er.flName.empty();
    std::string why = render ? "no FL instrument" : "";
    if (!render && er.quality == enc::Result::Approx) {
      const PluginInfo* pi = findPlugin(p.type);
      if (pi && pi->legacyModelParam >= 0 &&
          std::find(er.mismatched.begin(), er.mismatched.end(), pi->legacyModelParam) != er.mismatched.end())
        render = true, why = er.flName + " does not play Gridline's own engine";
    }
    UsedNotes un = usedNotes(original, ch.id);
    if (!render && p.type == "gl.pads" && padsUseBuiltinSounds(p, un)) render = true, why = "built-in kit sounds have no files for FPC";
    if (!render) continue;
    if (un.empty()) {  // nothing to render; written as the exporter can (an empty channel at worst)
      if (!home || er.quality == enc::Result::None || er.flName.empty())
        r.entries.push_back({ch.id, -1, where, p.type, "unused", "", "the channel plays no notes: nothing to render", {}});
      continue;
    }
    if (o.sampleDir.empty()) {
      r.entries.push_back({ch.id, -1, where, p.type, "neutral", "", "not rendered: no sample folder (" + why + ")", {}});
      continue;
    }
    renders.push_back({i, std::move(un)});
  }
  for (size_t j = 0; j < renders.size(); ++j) {
    Channel& ch = song.channels[renders[j].channel];
    std::string stem = safeStem(ch.name);
    for (int k = 2; !stems.insert(stem).second; ++k) stem = safeStem(ch.name) + " " + std::to_string(k);
    const double p0 = static_cast<double>(j) / renders.size(), p1 = static_cast<double>(j + 1) / renders.size();
    RenderOut ro = renderInstrument(original, *original.channel(ch.id), renders[j].notes, o.sampleDir, stem, o, p0, p1);
    if (ro.cancelled || (o.cancel && o.cancel->load())) {
      r.cancelled = true;
      r.error = "cancelled";
      return false;
    }
    const std::string where = "channel '" + ch.name + "'";
    if (!ro.error.empty()) {
      r.entries.push_back({ch.id, -1, where, ch.plugin.type, "neutral", "", "not rendered: " + ro.error, {}});
      continue;
    }
    if (ro.state.type == "gl.waveprogram" && !drumTypes().count(ch.plugin.type)) {
      // The takes are single notes; in the song the instrument can sound
      // louder or softer (a mono voice that carries on from note to note,
      // shared envelopes): the program's level follows the channel's part.
      sampleCache().trim();  // files of an earlier export under the same names
      const double before = channelRms(original, ch.id, nullptr);
      const double after = channelRms(original, ch.id, &ro.state);
      if (before > 1e-5 && after > 1e-7) {
        const double ratio = before / after, db = 20 * std::log10(ratio);
        if (std::fabs(db) > 0.5 && std::fabs(db) < 24) {
          namespace dwn = gl::packs::samplers::dw;
          dwn::DwProgram prog;
          if (dwn::parseProgram(ro.state.data, prog)) {
            for (auto& z : prog.zones) z.gain = static_cast<float>(z.gain * ratio);
            ro.state.data = dwn::formatProgram(prog);
            char b[64];
            std::snprintf(b, sizeof b, "; level matched to the song (%+.1f dB)", db);
            ro.note += b;
          }
        }
      }
      if (o.cancel && o.cancel->load()) {
        r.cancelled = true;
        r.error = "cancelled";
        return false;
      }
    }
    r.entries.push_back({ch.id, -1, where, ch.plugin.type, "rendered", ro.state.type == "gl.sampler" ? "" : "DirectWave", ro.note, ro.files});
    r.files.insert(r.files.end(), ro.files.begin(), ro.files.end());
    dropTargets(song, ch.id, -1);
    ch.plugin = ro.state;  // its channel envelope / filter are in the takes
  }
  // Effects.
  for (size_t i = 0; i < song.mixer.size(); ++i) {
    MixerTrack& t = song.mixer[i];
    for (size_t k = 0; k < t.slots.size() && k < static_cast<size_t>(kEffectSlots); ++k) {
      PluginState& p = t.slots[k].plugin;
      const int slot = static_cast<int>(k);
      if (p.empty() || isHostedType(p.type) || !findPlugin(p.type) || hasOrigin(p, t.id, slot)) continue;
      const enc::Result er = enc::encode(p, "", nullptr, ec);
      if (er.quality != enc::Result::None && !er.flName.empty()) continue;
      const std::string where = "insert " + std::to_string(i) + " slot " + std::to_string(k + 1);
      Mapping m;
      const std::string& ty = p.type;
      if (ty == "gl.chorus") m = mapChorus(p);
      else if (ty == "gl.delay") m = mapDelay(p);
      else if (ty == "gl.dist") m = mapDist(p);
      else if (ty == "gl.bitcrush") m = mapBitcrush(p);
      else if (ty == "gl.gate") m = mapGate(p);
      else if (ty == "gl.maximizer") m = mapMaximizer(p);
      else if (ty == "gl.scduck") m = mapDucker(p);
      else if (ty == "gl.drawneq") m = mapCurveEq(p);
      else if (ty == "gl.analyzer") m = mapAnalyzer(p);
      else if (ty == "gl.bussend") {
        // The bus's return track: a Fruity Send to it over a sidechain-only route.
        const int bus = iprm(p, 0);
        ObjectId to = kNoId;
        double ret = 1;
        for (const MixerTrack& u : song.mixer)
          for (const auto& sl : u.slots)
            if (to == kNoId && sl.plugin.type == "gl.busreturn" && iprm(sl.plugin, 0) == bus && u.id != t.id) to = u.id, ret = prm(sl.plugin, 1, 1);
        if (to != kNoId) {
          m.state = defaultPluginState("gl.send");
          put(m.state, 0, song.mixerIndex(to));
          put(m.state, 1, clampd(prm(p, 1, 1) * ret, 0, 1.25));
          put(m.state, 3, prm(p, 2, 1));
          put(m.state, 4, 0);
          bool routed = false;
          for (const Send& snd : t.sends) routed = routed || snd.to == to;
          if (!routed) t.sends.push_back(Send{to, 1.0f, true, false});
          m.note = "send to the bus's return track";
          m.ok = true;
        } else {
          r.entries.push_back({t.id, slot, where, p.type, "neutral", "Fruity Balance", "bus send without a bus return: nothing receives it", {}});
          continue;
        }
      }
      if (!m.ok) m = mapReferenceEngine(p, ec);
      if (m.ok) {
        const enc::Result mr = enc::encode(m.state, "", nullptr, ec);
        if (mr.quality == enc::Result::None || mr.flName.empty()) m.ok = false;
        else {
          if (m.state.type != p.type) dropTargets(song, t.id, slot);
          r.entries.push_back({t.id, slot, where, p.type, "mapped", mr.flName, m.note, {}});
          p = m.state;
          continue;
        }
      }
      const StandInPolicy pol = standInPolicy(p.type);
      r.entries.push_back({t.id, slot, where, p.type, "neutral", "Fruity Balance", pol.reason.empty() ? "no FL counterpart" : pol.reason, {}});
    }
  }
  return true;
}

}  // namespace gl::flp
