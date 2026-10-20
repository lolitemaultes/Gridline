// Gridline — FLP import, second wave: pad samplers and drum synths.
//   FPC                       → gl.pads (pads, layers, samples, note map, cut groups)
//   Drumpad / DrumPad         → gl.drumkit (one modelled drum, pitched key mode)
//   Drumaxx                   → gl.drumkit (16 modelled drum pads)
//   Fruity DrumSynth Live     → gl.drumkit (the patches of the keys the channel plays)
//   BassDrum                  → gl.kick
// Layouts: see analysis/demos/instruments_wave2.md (pads section).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include "../../engine/Slicing.h"
#include "../../packs/models/models.h"
#include "../PluginParams.h"
#include "FlpGen2.h"

namespace gl::flp::gen2 {

namespace {

using namespace gl::pp;
namespace md = gl::packs::models;

double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// FL sample path → local path: the FL data folder variables and absolute
// paths into another FL installation's resources map onto the factory data.
std::string samplePath(const Context& c, std::string p) {
  for (char& ch : p)
    if (ch == '\\') ch = '/';
  const std::string dataTag = "%FLStudioData%";
  if (p.rfind(dataTag, 0) == 0) return c.factoryData + "/Data" + p.substr(dataTag.size());
  const std::string res = "/Contents/Resources/FL/";
  if (size_t k = p.find(res); k != std::string::npos && p.rfind("/Applications/", 0) == 0)
    return c.factoryData + "/" + p.substr(k + res.size());
  return c.resolvePath(p);
}

// FL channel index whose event list is `events` (-1 when not a channel).
int channelIndexOf(const Context& c, const std::vector<Event>& events) {
  for (const auto& [idx, evs] : c.sec.channels)
    if (&evs == &events) return idx;
  return -1;
}

// Keys played by FL channel `idx` in all patterns (key → note count).
std::map<int, int> channelKeys(const Context& c, int idx) {
  std::map<int, int> keys;
  if (idx < 0) return keys;
  for (const auto& [pat, evs] : c.sec.patterns)
    for (const Event& e : evs) {
      if (e.id != 224) continue;
      for (uint32_t o = 0; o + 24 <= e.size; o += 24)
        if (e.u16(o + 6) == idx) keys[e.u16(o + 12)]++;
    }
  return keys;
}

// ------------------------------------------------------------------ FPC
// State: u32 magic 0x1E84E403, then chunks {u32 id, u64 size, payload}.
// Chunk 2 = one pad (32 pads: banks A and B), containing 0x64 pad settings
// (16 × i32), 0x65 name (UTF-16), 0x66 trigger options, 0x69 layers
// (0x12C path, 0x12D settings 7 × i32), 0x67/0x68 envelopes.
// Older kits (FL 10 / 11, kept in the shipped packs) use other magics and
// store the pad name and sample paths as 8-bit text: 0x000F424B (FL 10),
// 0x000F424E (FL 11), 0x001E848E (FL 12, UTF-16 like the current one).
constexpr uint32_t kFpcMagic = 0x1E84E403;
bool fpcMagic(uint32_t v) { return v == kFpcMagic || v == 0x001E848E || v == 0x000F424E || v == 0x000F424B; }

struct FpcLayer {
  int vol = 127, pan = 0, lo = 0, hi = 127, tune = 0;
  std::string path;
};
// Pad envelope (chunks 0x67 = {kind 0 volume / 1 pan, 0}, 0x68 = points):
// i32 version 1, i32 3, i32 count, count × {f64 delta time (s), f64 value, f32 tension, u32 flags},
// then i32 0, i32 enabled (0 = off, -1 = on), i32 sustain point, i32 loop start, i32 loop end, …
struct FpcEnv {
  bool present = false, on = false;
  int sustain = -1, loopStart = -1, loopEnd = -1;
  std::vector<std::array<double, 3>> pts;  // time, value, tension
};
struct FpcPad {
  int vol = 128, pan = 0, flags = 0, note = 60, cut = -1, cutBy = -1, tune = 0, trigger = 1;
  uint32_t color = 0;
  std::string name;
  std::vector<FpcLayer> layers;
  FpcEnv env[2];
  bool hasSample() const {
    for (const auto& l : layers)
      if (!l.path.empty()) return true;
    return false;
  }
};

template <class F>
void forChunks(const Blob& b, size_t at, size_t end, F&& f) {
  while (at + 12 <= end) {
    uint32_t id = b.u32(at);
    uint64_t size = static_cast<uint64_t>(b.u32(at + 4)) | (static_cast<uint64_t>(b.u32(at + 8)) << 32);
    if (size > end - at - 12) return;
    f(id, at + 12, static_cast<size_t>(size));
    at += 12 + static_cast<size_t>(size);
  }
}

FpcEnv parseFpcEnv(const Blob& b, size_t p, size_t n) {
  FpcEnv e;
  if (n < 12) return e;
  const int count = b.i32(p + 8);
  if (count < 0 || count > 256 || 12 + static_cast<size_t>(count) * 24 + 20 > n) return e;
  size_t q = p + 12;
  for (int i = 0; i < count; ++i, q += 24) e.pts.push_back({b.f64(q), b.f64(q + 8), static_cast<double>(b.f32(q + 16))});
  e.present = true;
  e.on = b.i32(q + 4) != 0;
  e.sustain = b.i32(q + 8), e.loopStart = b.i32(q + 12), e.loopEnd = b.i32(q + 16);
  return e;
}

std::vector<FpcPad> parseFpc(const Blob& b) {
  std::vector<FpcPad> pads;
  if (!fpcMagic(b.u32(0))) return pads;
  const bool narrow = (b.u32(0) >> 16) == 0x000F;  // 8-bit strings
  auto text = [&](size_t p, size_t n) { return narrow ? b.ascii(p, n) : b.utf16(p, n / 2); };
  forChunks(b, 4, b.size(), [&](uint32_t id, size_t at, size_t size) {
    if (id != 2) return;
    FpcPad pad;
    int envKind = -1;
    forChunks(b, at, at + size, [&](uint32_t id2, size_t p, size_t n) {
      if (id2 == 0x64 && n >= 32) {
        pad.vol = b.i32(p), pad.pan = b.i32(p + 4), pad.flags = b.i32(p + 8), pad.note = b.i32(p + 12);
        pad.cut = b.i32(p + 20), pad.cutBy = b.i32(p + 24), pad.tune = b.i32(p + 28);
        if (n >= 36) pad.color = b.u32(p + 32);
      } else if (id2 == 0x65) {
        pad.name = text(p, n);
      } else if (id2 == 0x66 && n >= 4) {
        pad.trigger = b.i32(p);
      } else if (id2 == 0x67 && n >= 4) {
        envKind = b.i32(p);
      } else if (id2 == 0x68) {
        if (envKind == 0 || envKind == 1) pad.env[envKind] = parseFpcEnv(b, p, n);
        envKind = -1;
      } else if (id2 == 0x69) {
        FpcLayer l;
        forChunks(b, p, p + n, [&](uint32_t id3, size_t q, size_t m) {
          if (id3 == 0x12C) l.path = text(q, m);
          if (id3 == 0x12D && m >= 28) {
            l.vol = b.i32(q), l.pan = b.i32(q + 4), l.lo = b.i32(q + 8), l.hi = b.i32(q + 12), l.tune = b.i32(q + 20);
          }
        });
        pad.layers.push_back(l);
      }
    });
    pads.push_back(std::move(pad));
  });
  return pads;
}

// FPC pad / layer tune: -128..128 = -1200..+1200 cents (9.375 cents a step) → semitones.
double fpcTune(int v) { return v * 12.0 / 128.0; }

// FPC envelope → Gridline pad envelope text (false when it cannot be kept exactly).
bool fpcEnvText(const FpcEnv& e, std::string& out) {
  PadEnvelope g;
  g.on = e.on;
  bool exact = static_cast<int>(e.pts.size()) <= PadEnvelope::kMax;
  g.n = std::min<int>(PadEnvelope::kMax, static_cast<int>(e.pts.size()));
  for (int i = 0; i < g.n; ++i) {
    g.t[i] = static_cast<float>(i == 0 ? 0.0 : std::max(0.0, e.pts[static_cast<size_t>(i)][0]));
    g.v[i] = static_cast<float>(clamp01(e.pts[static_cast<size_t>(i)][1]));
    // FL tension 0..±1 bends like the automation curves (positive = slow start).
    g.ten[i] = static_cast<float>(std::clamp(e.pts[static_cast<size_t>(i)][2], -1.0, 1.0));
  }
  g.sustain = e.sustain >= 0 && e.sustain < g.n ? e.sustain : -1;
  g.loopStart = e.loopStart >= 0 && e.loopStart < g.n ? e.loopStart : -1;
  g.loopEnd = e.loopEnd >= 0 && e.loopEnd < g.n ? e.loopEnd : -1;
  out = formatPadEnvelope(g);
  return exact;
}

bool decodeFpc(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  std::vector<FpcPad> pads = parseFpc(stateBlob(events));
  if (pads.empty()) return false;
  begin(m, "gl.pads");
  bool exact = pads.size() <= static_cast<size_t>(kPadsAll);
  if (!exact) c.report.warn("FPC: more than 32 pads (extra pads dropped)");
  if (pads.size() > static_cast<size_t>(kPadsAll)) pads.resize(kPadsAll);
  // FL cut groups (-1 = none, 0..99) → Gridline groups 1..8 in order of appearance.
  std::map<int, int> groups;
  auto group = [&](int g) {
    if (g < 0) return 0;
    auto it = groups.find(g);
    if (it != groups.end()) return it->second;
    int n = static_cast<int>(groups.size()) + 1;
    if (n > 8) {
      if (exact) c.report.warn("FPC: more than 8 cut groups (extra groups merged into group 8)");
      exact = false;
      return 8;
    }
    groups[g] = n;
    return n;
  };
  std::string data;
  for (int k = 0; k < kPads; ++k) data = setPadSamplePath(data, k, 0, "none");
  // Pads keep their bank position: FL pad k (bank A 1..16, bank B 17..32) → Gridline pad k.
  for (size_t k = 0; k < pads.size(); ++k) {
    const FpcPad& p = pads[k];
    const int pk = static_cast<int>(k);
    const std::string pn = std::to_string(pk + 1);
    if (!p.name.empty()) data = padDataSet(data, "N" + pn, p.name);
    if (p.color != 0) {
      char col[8];
      std::snprintf(col, sizeof col, "%06X", ((p.color & 0xFF) << 16) | (p.color & 0xFF00) | ((p.color >> 16) & 0xFF));
      data = padDataSet(data, "C" + pn, col);
    }
    set(m, padP(pk, PadLevel), clamp01(p.vol / 127.0));
    set(m, padP(pk, PadPan), std::clamp(p.pan / 128.0, -1.0, 1.0));
    set(m, padP(pk, PadTune), fpcTune(p.tune));
    // FL stores the pad's own cut group and the group that silences it ("off by").
    // Gridline: PadCut = the pad's choke group (its voices are silenced by
    // pads whose PadCutBy names it), PadCutBy = the group this pad silences.
    set(m, padP(pk, PadCut), group(p.cutBy));
    set(m, padE(pk, PadCutBy), group(p.cut));
    set(m, padE(pk, PadNote), std::clamp(p.note, 0, 127));
    // Overlapping velocity layers with "trigger randomly" (option bit 2).
    bool random = (p.trigger & 2) != 0;
    set(m, padE(pk, PadLayerMode), random ? LayerRandom : LayerStack);
    // Every layer with a sample: 1..4 as parameters, 5..32 in the data text.
    std::vector<FpcLayer> ls;
    for (const auto& l : p.layers)
      if (!l.path.empty()) ls.push_back(l);
    if (ls.size() > static_cast<size_t>(kPadMaxLayers)) {
      c.report.warn("FPC: pad with more than " + std::to_string(kPadMaxLayers) + " layers (extra layers dropped)");
      exact = false, ls.resize(kPadMaxLayers);
    }
    if (ls.empty()) {
      if (pk >= kPads) data = setPadSamplePath(data, pk, 0, "none");
      set(m, padL(pk, 0, LayEnabled), 0);
    }
    for (size_t g = 0; g < ls.size(); ++g) {
      const FpcLayer& l = ls[g];
      const int lg = static_cast<int>(g);
      if (lg < kPadLayers) {
        set(m, padL(pk, lg, LayEnabled), 1);
        set(m, padL(pk, lg, LayVelLow), std::clamp(l.lo, 0, 127));
        set(m, padL(pk, lg, LayVelHigh), std::clamp(l.hi, 0, 127));
        set(m, padL(pk, lg, LayLevel), clamp01(l.vol / 127.0));
        set(m, padL(pk, lg, LayPan), std::clamp(l.pan / 128.0, -1.0, 1.0));
        set(m, padL(pk, lg, LayTune), fpcTune(l.tune));
      } else {
        PadLayerX x;
        x.lo = std::clamp(l.lo, 0, 127), x.hi = std::clamp(l.hi, 0, 127);
        x.level = static_cast<float>(clamp01(l.vol / 127.0));
        x.pan = static_cast<float>(std::clamp(l.pan / 128.0, -1.0, 1.0));
        x.tune = static_cast<float>(fpcTune(l.tune));
        data = padDataSet(data, "S" + pn + "." + std::to_string(lg + 1), formatPadLayer(x));
      }
      data = setPadSamplePath(data, pk, lg, samplePath(c, l.path));
    }
    // Volume / pan envelopes (kept when switched on).
    for (int e = 0; e < 2; ++e) {
      const FpcEnv& env = p.env[e];
      if (!env.present || !env.on || env.pts.empty()) continue;
      std::string txt;
      if (!fpcEnvText(env, txt)) {
        c.report.warn("FPC: pad envelope with more than " + std::to_string(PadEnvelope::kMax) + " points (truncated)");
        exact = false;
      }
      data = padDataSet(data, "E" + pn + (e == 0 ? ".v" : ".p"), txt);
    }
  }
  m.state.data = data;
  m.exact = exact;
  // Automation: FL parameters 0..15 volume, 16..31 pan, 32..47 mute,
  // 48..63 solo, 64..79 tune (±1200 cents) of the pads of bank A.
  std::vector<Link> links;
  for (int fl = 0; fl < kPads && fl < static_cast<int>(pads.size()); ++fl) {
    links.push_back({fl, padP(fl, PadLevel), nullptr});
    links.push_back({16 + fl, padP(fl, PadPan), nullptr});
    links.push_back({32 + fl, padE(fl, PadMute), nullptr});
    links.push_back({48 + fl, padE(fl, PadSolo), nullptr});
    links.push_back({64 + fl, padP(fl, PadTune), [](double n) { return (n * 2.0 - 1.0) * 12.0; }});
  }
  linkParams(m, std::move(links));
  return true;
}

// ------------------------------------------------------- modelled drums
// Drumpad / Drumaxx share one drum model block of 38 normalized floats:
//   0 mallet retrig, 1 retrig, 2 mallet amp, 3 mallet decay, 4 mallet noise,
//   5 mallet noise RP, 6 membrane decay, 7 membrane cutoff, 8 tension,
//   9 phase, 10 material, 11 size, 12 shape, 13 low freq, 14 low reso,
//   15 SOF freq, 16 SOF decay, 17 SOF level, 18 mid freq, 19 mid level,
//   20 mid res, 21 mid freq mod, 22 mid level mod, 23 mid attack, 24 high freq,
//   25 high level, 26 high res, 27 lo-fi, 28 hold, 29 pitch, 30..33 velocity
//   mod sources, 34..37 velocity mod amounts.
void modelToPad(PluginMapping& m, int pad, const float* t) {
  using namespace md;
  double size = t[11], tension = t[8], pitch = t[29], material = t[10];
  double hz = 40.0 * std::pow(2.0, (1.0 - size) * 4.5) * std::pow(2.0, (tension - 0.5) * 1.5) * std::pow(2.0, (pitch - 0.5) * 4.0);
  int shape = material >= 0.7 ? (size > 0.45 ? DsTom : DsMembrane) : (material < 0.55 ? DsBar : DsPlate);
  set(m, drumPad(pad, DpShape), shape);
  set(m, drumPad(pad, DpFreq), std::clamp(hz, 20.0, 5000.0));
  set(m, drumPad(pad, DpDecay), std::clamp(0.02 * std::pow(100.0, t[6]) * (0.6 + 0.8 * t[28]), 0.02, 6.0));
  set(m, drumPad(pad, DpMaterial), clamp01(0.2 + 0.6 * (1.0 - t[7])));
  set(m, drumPad(pad, DpPosition), clamp01(t[9]));
  set(m, drumPad(pad, DpBend), 0.0);
  set(m, drumPad(pad, DpMallet), clamp01(0.5 * t[2] + 0.5 * (1.0 - t[3])));
  set(m, drumPad(pad, DpClick), clamp01(0.6 * t[4]));
  set(m, drumPad(pad, DpNoise), clamp01(0.8 * t[25] + 0.3 * t[17]));
  set(m, drumPad(pad, DpNoiseDecay), std::clamp(0.01 * std::pow(100.0, t[16]), 0.01, 2.0));
  set(m, drumPad(pad, DpNoiseTone), std::clamp(200.0 * std::pow(80.0, t[24]), 200.0, 16000.0));
  set(m, drumPad(pad, DpWires), 0.0);
  set(m, drumPad(pad, DpDrive), 0.0);
}

void readModel(const Blob& b, size_t at, float* t) {
  for (int k = 0; k < 38; ++k) t[k] = std::clamp(b.f32(at + 4 * static_cast<size_t>(k)), 0.0f, 2.0f);
}

// Drumpad: "SSLF", u32 size, "3PMD", preset name (64 bytes), drum name (64),
// then the 38 model floats at 0x8C.
bool decodeDrumpad(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  (void)c;
  Blob b = stateBlob(events);
  if (b.size() < 0x8C + 38 * 4 || b.find("SSLF") != 0) return false;
  begin(m, "gl.drumkit");
  using namespace md;
  float t[38];
  readModel(b, 0x8C, t);
  set(m, DmBaseNote, 60);
  set(m, DmKeyMode, 1);
  set(m, DmLofi, clamp01(t[27]));
  for (int k = 0; k < kDrumPads; ++k) set(m, drumPad(k, DpMute), k == 0 ? 0 : 1);
  set(m, drumPad(0, DpLevel), 0.6);
  set(m, drumPad(0, DpPan), 0.0);
  modelToPad(m, 0, t);
  m.exact = false;
  return true;
}

// Drumaxx: "SSLF", u32 size, "4SMD", kit name, then 16 pad records of 44
// floats from 0x10C: volume, pan, mute, solo, key (MIDI note as a float),
// then the 38-float drum model block, then one spare.
bool decodeDrumaxx(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  (void)c;
  Blob b = stateBlob(events);
  constexpr size_t kBase = 0x10C, kStride = 44 * 4;
  if (b.size() < kBase + 16 * kStride || b.find("SSLF") != 0) return false;
  begin(m, "gl.drumkit");
  using namespace md;
  // Keys of the 16 pads; Gridline's pads are consecutive keys from a base note.
  int keys[16], lo = 127;
  for (int k = 0; k < 16; ++k) {
    keys[k] = static_cast<int>(std::lround(b.f32(kBase + static_cast<size_t>(k) * kStride + 16)));
    lo = std::min(lo, std::clamp(keys[k], 0, 127));
  }
  set(m, DmBaseNote, lo);
  for (int k = 0; k < kDrumPads; ++k) set(m, drumPad(k, DpMute), 1);
  bool exact = false;
  for (int k = 0; k < 16; ++k) {
    int pad = keys[k] - lo;
    if (pad < 0 || pad >= kDrumPads) continue;
    size_t at = kBase + static_cast<size_t>(k) * kStride;
    float t[38];
    readModel(b, at + 20, t);
    set(m, drumPad(pad, DpLevel), clamp01(b.f32(at)));
    set(m, drumPad(pad, DpPan), std::clamp(b.f32(at + 4) * 2.0 - 1.0, -1.0, 1.0));
    set(m, drumPad(pad, DpMute), b.f32(at + 8) >= 0.5f ? 1 : 0);
    modelToPad(m, pad, t);
  }
  m.exact = exact;
  return true;
}

// ------------------------------------------------- Fruity DrumSynth Live
// u32 version, u32 selected key, 120 Pascal strings (patch name per key
// 0..119), then 120 records of 38 × i32 (the patch parameters per key, in
// the editor's parameter order), then u32.
//   0 level 0..128, 1 pan -64..64, 2 tone -50..50, 3 osc1 harmonics -50..50,
//   4 osc1 freq 0..1000, 5 osc1 bandwidth, 6 osc1 sweep, 7 osc1 decay,
//   8 osc1 level, 9 osc2 shape, 10 osc2 freq 0..1000, 11 osc2 bandwidth,
//   12 osc2 attack, 13 osc2 decay, 14 osc2 level (0..127), 15 waveform -1..17,
//   16 wave tune, 17 noise <> wave mix, 18 retrigger count, 19 retrig rate,
//   20 retrig decay, 21 release, 22 phase invert, 23 click 0..100, 24 voice
//   mode, 25..31 velocity amounts, 32 osc1 mode (tone / noise band), 33 ring,
//   34 morph target, 35 send level, 36 send, 37 morph amount.
struct DslPatch {
  int v[38] = {};
};

// Version 1 (presets saved by FL 3): 24 patches, names as 24-byte fields, the
// same 38-value records; they are placed on keys 60..83 (C5 up).
constexpr int kDslV1Patches = 24, kDslV1Key = 60;

// The state's layout: where the 120 key records start (J-51: records are read
// only for the keys the pads use — the export's solver decodes thousands of
// times, and reading all 4560 values dominated each decode).
struct DslState {
  Blob b;
  bool v1 = false;
  size_t recAt = 0;
  DslPatch patch(int key) const {
    DslPatch d;  // (version 1: keys without a patch play at level 0)
    if (v1 && (key < kDslV1Key || key >= kDslV1Key + kDslV1Patches)) return d;
    const size_t at = recAt + static_cast<size_t>(v1 ? key - kDslV1Key : key) * 152;
    for (int i = 0; i < 38; ++i) d.v[i] = b.i32(at + 4 * static_cast<size_t>(i));
    return d;
  }
};

bool parseDsl(const Blob& b, int& selected, DslState& st) {
  if (b.size() < 8) return false;
  selected = b.i32(4);
  st.b = b;
  if (b.u32(0) == 1 && b.size() == 8 + kDslV1Patches * (24 + 152)) {
    st.v1 = true;
    st.recAt = 8 + kDslV1Patches * 24;
    selected = kDslV1Key;  // the editor's selected patch is not a played key here
    return true;
  }
  size_t p = 8;
  for (int k = 0; k < 120; ++k) {
    if (p >= b.size()) return false;
    p += 1 + b.u8(p);
  }
  if (p + 120 * 152 > b.size()) return false;
  st.recAt = p;
  return true;
}

// One pad's parameters from a patch: (pad parameter, value) in setting order.
struct DslPad : DslPadValues {
  void put(int p, double v) { param[n] = p, value[n] = v, ++n; }
};

DslPad dslPad(const int32_t* v) {
  using namespace md;
  DslPad o;
  double f1 = 20.0 * std::pow(2.0, v[4] / 100.0);
  double toneLevel = std::clamp(v[8] / 127.0, 0.0, 1.0), noiseLevel = std::clamp(v[14] / 127.0, 0.0, 1.0);
  bool noiseOsc1 = v[32] != 0;
  o.put(DpLevel, 0.7 * clamp01(v[0] / 128.0) * std::max(0.35, std::max(toneLevel, noiseLevel)));
  o.put(DpPan, std::clamp(v[1] / 64.0, -1.0, 1.0));
  if (toneLevel < 0.15 || noiseOsc1) {
    // Mostly noise: a short, high plate under the noise burst.
    o.put(DpShape, DsPlate);
    o.put(DpFreq, std::clamp(f1, 800.0, 5000.0));
    o.put(DpDecay, 0.03);
    o.put(DpNoise, 1.0);
  } else {
    o.put(DpShape, DsMembrane);
    o.put(DpFreq, std::clamp(f1, 20.0, 5000.0));
    o.put(DpDecay, std::clamp(0.02 * std::pow(150.0, v[7] / 127.0), 0.02, 6.0));
    o.put(DpNoise, clamp01(noiseLevel / std::max(0.2, toneLevel)));
  }
  o.put(DpSpread, 1.0);
  o.put(DpMaterial, clamp01(0.5 + v[3] / 100.0));
  o.put(DpBend, std::clamp(24.0 * v[6] / 127.0, 0.0, 36.0));
  o.put(DpBendTime, std::clamp(0.005 * std::pow(40.0, v[6] / 127.0), 0.003, 1.0));
  o.put(DpClick, clamp01(v[23] / 100.0));
  o.put(DpNoiseDecay, std::clamp(0.01 * std::pow(150.0, v[13] / 127.0), 0.01, 2.0));
  o.put(DpNoiseTone, std::clamp(200.0 * std::pow(2.0, v[10] / 160.0) * std::pow(2.0, v[2] / 50.0), 200.0, 16000.0));
  o.put(DpWires, 0.0);
  o.put(DpDrive, clamp01(std::fabs(v[3]) / 100.0));
  o.put(DpChoke, v[24] >= 2 ? std::min(8, v[24] - 1) : 0);
  o.put(DpMute, 0);
  return o;
}

void dslToPad(PluginMapping& m, int pad, const DslPatch& d) {
  // J-51: the last result per pad slot is kept (per thread): the export's
  // solver decodes the state thousands of times changing one patch value, and
  // the other pads' laws (several pow each) dominated each decode.
  struct Memo {
    bool valid = false;
    DslPatch key;
    DslPad out;
  };
  thread_local Memo memo[md::kDrumPads];
  Memo* mm = pad >= 0 && pad < md::kDrumPads ? &memo[pad] : nullptr;
  DslPad o;
  if (mm && mm->valid && std::equal(std::begin(d.v), std::end(d.v), std::begin(mm->key.v))) {
    o = mm->out;
  } else {
    o = dslPad(d.v);
    if (mm) mm->valid = true, mm->key = d, mm->out = o;
  }
  for (int i = 0; i < o.n; ++i) set(m, md::drumPad(pad, o.param[i]), o.value[i]);
}

bool decodeDrumSynthLive(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  int selected = 60;
  DslState patches;
  if (!parseDsl(stateBlob(events), selected, patches)) return false;
  begin(m, "gl.drumkit");
  using namespace md;
  std::map<int, int> keys = channelKeys(c, channelIndexOf(c, events));
  std::vector<int> ks;
  for (const auto& [k, n] : keys)
    if (k >= 0 && k < 120) ks.push_back(k);
  if (ks.empty()) ks.push_back(std::clamp(selected, 0, 119));
  int base = ks.front();
  set(m, DmBaseNote, std::min(base, 112));
  base = std::min(base, 112);
  for (int k = 0; k < kDrumPads; ++k) set(m, drumPad(k, DpMute), 1);
  for (int k = 0; k < kDrumPads && base + k < 120; ++k) dslToPad(m, k, patches.patch(base + k));
  m.exact = false;
  return true;
}

// ---------------------------------------------------------------- BassDrum
// "SSLF", u32 size, "1kiK", preset name, noise sample path (ASCII, 0x10C),
// then 23 normalized floats (parameters 0..22) at 0x50C:
//   0 level, 1 sampling ratio, 2 duration, 3 sub bass, 4 drive, 5 attack,
//   6 base freq, 7 peak freq, 8 slide time, 9..12 variation (base, peak,
//   slide, phase), 13 click cut, 14 click freq, 15 click level, 16..19 noise
//   A, D, S, start, 20 noise pitch, 21 noise mix, 22 noise filter.
bool decodeBassDrum(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  (void)c;
  Blob b = stateBlob(events);
  constexpr size_t kAt = 0x50C;
  if (b.size() < kAt + 23 * 4 || b.find("SSLF") != 0) return false;
  begin(m, "gl.kick");
  float t[23];
  for (int k = 0; k < 23; ++k) t[k] = std::clamp(b.f32(kAt + 4 * static_cast<size_t>(k)), 0.0f, 1.0f);
  set(m, KickVolume, clamp01(1.2 * t[0]));
  set(m, KickDecay, std::clamp(0.05 * std::pow(40.0, t[2]), 0.05, 2.0));
  set(m, KickSub, t[3]);
  set(m, KickDrive, 0.5 * t[4]);
  set(m, KickAttack, 0.02 * t[5]);
  set(m, KickEndPitch, std::clamp(25.0 * std::pow(2.0, t[6] * 4.0), 25.0, 200.0));
  set(m, KickPitch, std::clamp(40.0 * std::pow(25.0, t[7]), 40.0, 1000.0));
  set(m, KickSweep, std::clamp(0.005 * std::pow(100.0, t[8]), 0.005, 0.5));
  set(m, KickVariation, clamp01(0.5 * (t[9] + t[10]) * 0.5));
  set(m, KickClick, t[15]);
  set(m, KickClickTone, std::clamp(1000.0 * std::pow(12.0, t[14]), 1000.0, 12000.0));
  set(m, KickClickLen, std::clamp(0.001 * std::pow(20.0, 1.0 - t[13]), 0.001, 0.02));
  set(m, KickNoise, clamp01(t[21] * 0.6));
  set(m, KickNoiseDecay, std::clamp(0.005 * std::pow(200.0, t[17]), 0.005, 1.0));
  set(m, KickKeyTrack, 1);
  m.exact = false;
  // Automation: FL parameter index = the float index above.
  linkParams(m, {{2, KickDecay, [](double n) { return std::clamp(0.05 * std::pow(40.0, n), 0.05, 2.0); }},
                 {3, KickSub, nullptr},
                 {4, KickDrive, [](double n) { return 0.5 * n; }},
                 {6, KickEndPitch, [](double n) { return std::clamp(25.0 * std::pow(2.0, n * 4.0), 25.0, 200.0); }},
                 {7, KickPitch, [](double n) { return std::clamp(40.0 * std::pow(25.0, n), 40.0, 1000.0); }},
                 {8, KickSweep, [](double n) { return std::clamp(0.005 * std::pow(100.0, n), 0.005, 0.5); }},
                 {15, KickClick, nullptr}},
             {{2, 0, 256}, {3, 0, 256}, {4, 0, 256}, {6, 0, 256}, {7, 0, 256}, {8, 0, 256}, {15, 0, 256}});
  return true;
}

}  // namespace

DslPadValues drumSynthLivePad(const int32_t* patch) { return dslPad(patch); }

void registerPads(std::map<std::string, PluginDecoder>& d) {
  add(d, "FPC", decodeFpc);
  add(d, "Drumpad", decodeDrumpad);
  add(d, "DrumPad", decodeDrumpad);
  add(d, "Drumaxx", decodeDrumaxx);
  add(d, "Fruity DrumSynth Live", decodeDrumSynthLive);
  add(d, "BassDrum", decodeBassDrum);
}

}  // namespace gl::flp::gen2
