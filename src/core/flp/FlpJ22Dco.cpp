// Gridline — FLP import, J-22 (Kepler, Kepler Exo, SimSynth, Fruity DX10):
// full state decoders onto dedicated Gridline engines whose parameters are the
// reference controls in tag order (automation maps one-to-one).
//
//   Kepler / Kepler Exo → DCO Synth (gl.dcosynth)
//   SimSynth            → Warm Trio (gl.warmsynth)
//   Fruity DX10         → FM Duo (gl.fmduo)
//
// Every decoder walks the state to its exact end and refuses anything else
// (truncated / unknown layouts), so a decoded state is a fully read one.
// Layouts (worked out from FL's shipped presets / projects, the reference
// controls and, for SimSynth, the engine's load routine read with REA):
//
// Fruity DX10: i32 version (1 or 2), then i32 per control tag 0..21 (0..65536;
//   tag 21 octave −3..3) — 92 bytes. Version 2 appends a copy of the channel
//   (i32, five 68-byte envelope / LFO records, the 24-byte levels record) —
//   460 bytes; FL applies channel envelopes to native generators, so a channel
//   envelope / filter in use makes the mapping approximate.
//
// SimSynth (564 bytes): 45 f64 (oscillator blocks of 7 at 0 / 7 / 14 — pulse
//   width, coarse = (st + 24) / 48, fine, level, LFO, env, shape —, LFO delay
//   21, rate 22, 23 spare, LFO shape 24, 25 spare, filter cut 26, emphasis 27,
//   env 28, LFO 29, key 30, 31 spare, high 32, band 33, 34 spare, amp ADSR
//   35..38, velocity 39, 40 spare, filter ADSR 41..44), 19 spare i32, 19 i32
//   switches (osc on / +1 / +2 / warm × 3, LFO on, retrigger, filter on, 15
//   spare, track amp, ring, chorus), 12 spare i32, i32 version (14; older
//   versions are converted like the reference does: v0 velocity × 0.75, ≤ 4
//   oscillator coarse ±12 with the octave shapes, < 13 coarse quantized).
//   Shape doubles 0.0 / 0.1 / 0.2 / 0.3 / 0.4 = selector entries 2 / 1 / 0 / 4 / 3.
//
// Kepler (v1, "Kepler-1"): u32 0, u32 size − 12, u32 0, u32 name length, name,
//   u32 version (2 / 4), u32 count (34 / 36), count × f32 (tags), then option
//   bytes: u8 arp quantize, f32 velocity → volume, f32 chorus noise (+ 3 bytes
//   in version 4). Mapped onto the DCO Synth's one-DCO panel: tags 0..35 →
//   the equivalent DCO Synth controls (see kepler1Tag).
// Kepler Exo ("Kepler-2"): u32 1, u32 1, u32 size − 12, u32 0, u32 0,
//   u32 size − 24, u32 0, u32 name length, name, u32 version (1 / 2),
//   u32 count (151), count × f32 (tags 0..150), u32 1, u32 8, 8 × (i32 source,
//   i32 destination tag; 256 / 257 = DCO 1 / DCO 2 pitch; −1 = empty), then
//   u32 2, u64 56 and 56 bytes of runtime data (ignored: rebuilt on load).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include "../../packs/synths/dco.h"
#include "FlpGen.h"
#include "FlpJ22.h"

namespace gl::flp::j22 {

namespace {

namespace dc = gl::packs::synths::dco;
using gen::Bytes;
using gen::ParamTable;

void setp(PluginState& s, int idx, double v) { gen::setp(s, idx, v); }
double clamp01(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; }

// Channel envelopes / filter in use (FL applies them to native generators).
bool channelShaping(const std::vector<Event>& evs, Bytes envCopy = {}, Bytes levelsCopy = {}) {
  gen::ChanSettings cs = gen::channelSettings(evs, envCopy, levelsCopy);
  bool used = !cs.filter.open();
  for (const auto& e : cs.env) used = used || (e.enabled && e.amount != 0) || e.lfoOn();
  return used;
}

// ------------------------------------------------------------------ DX10
bool decodeDx10(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  const Event* ev = pluginState(evs);
  if (!ev) return false;
  Bytes b(*ev);
  const int32_t ver = b.i32(0);
  const bool v1 = ver == 1 && b.n == 92, v2 = ver == 2 && b.n == 460;
  if (!v1 && !v2) return false;
  PluginState s = gen::base("gl.fmduo");
  for (int t = 0; t <= 20; ++t) setp(s, t, std::clamp(b.i32(4 + 4 * static_cast<size_t>(t)), 0, 65536) / 65536.0);
  const int32_t oct = b.i32(4 + 4 * 21);
  if (oct < -3 || oct > 3) return false;
  setp(s, dc::FdOctave, oct);
  m.glType = "gl.fmduo";
  m.state = s;
  const bool shaped = v2 ? channelShaping(evs, b.sub(96, 5 * 68), b.sub(436, 24)) : channelShaping(evs);
  m.exact = !shaped;
  if (shaped) gen::note(c, "Fruity DX10", "channel envelope / filter (applied by FL to native generators) not carried");
  ParamTable t;
  for (int tag = 0; tag <= 20; ++tag) {
    t.lin(tag, tag, 0.0, 1.0);
    t.range(tag, 0, 65536);
  }
  t.fn(21, dc::FdOctave, [](double n) { return std::round(-3.0 + 6.0 * n); });
  t.range(21, -3, 3);
  t.install(m);
  return true;
}

// -------------------------------------------------------------- SimSynth
// Control tag → stored double (knobs 0..35, shape selectors 54..57) and → switch
// slot (tags 36..53), from the engine's parameter table.
constexpr int kSimDouble[36] = {3, 4, 5, 10, 11, 12, 17, 18, 19, 21, 22, 26, 27, 28, 29, 32, 33, 35,
                                36, 37, 38, 39, 41, 42, 43, 44, 0, 1, 2, 7, 8, 9, 14, 15, 16, 30};
constexpr int kSimSwitch[18] = {18, 13, 16, 12, 14, 0, 4, 8, 17, 1, 2, 3, 5, 6, 7, 9, 10, 11};
constexpr int kSimShapeDouble[4] = {6, 13, 20, 24};

// Shape double (0.0 / 0.1 / … / 0.4) → selector entry; -1 when not one of them.
int simShapeSel(double d) {
  static const double vals[5] = {0.2, 0.1, 0.0, 0.4, 0.3};  // selector order
  for (int i = 0; i < 5; ++i)
    if (std::fabs(d - vals[i]) < 1e-9) return i;
  return -1;
}

bool decodeSimSynth(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  (void)c;
  const Event* ev = pluginState(evs);
  if (!ev || ev->size != 564) return false;
  Bytes b(*ev);
  double d[45];
  for (int i = 0; i < 45; ++i) {
    d[i] = b.f64(8 * static_cast<size_t>(i));
    if (!std::isfinite(d[i]) || std::fabs(d[i]) > 1e6) return false;
  }
  int sw[19];
  for (int i = 0; i < 19; ++i) sw[i] = b.i32(436 + 4 * static_cast<size_t>(i));
  const int32_t ver = b.i32(560);
  if (ver < 0 || ver > 1000) return false;
  // Older states, converted like the reference does on load.
  if (ver == 0) d[39] *= 0.75;
  if (ver <= 4) {
    for (int L : {1, 8, 15}) {
      int iv = static_cast<int>(d[L] * 24.0 - 12.0);
      if (std::fabs(d[L + 5] - 0.3) < 1e-9) iv += 12;
      d[L] = (iv + 24) / 48.0;
    }
  }
  if (ver < 13)
    for (int L : {1, 8, 15}) d[L] = (static_cast<int>(d[L] * 48.0 - 24.0) + 24) / 48.0;
  PluginState s = gen::base("gl.warmsynth");
  for (int tag = 0; tag < 36; ++tag) {
    const double v = clamp01(d[kSimDouble[tag]]);
    if (tag == dc::WmOsc1Coarse || tag == dc::WmOsc2Coarse || tag == dc::WmOsc3Coarse)
      setp(s, tag, std::floor(v * 48.0) - 24.0);
    else
      setp(s, tag, v);
  }
  for (int k = 0; k < 18; ++k) setp(s, 36 + k, sw[kSimSwitch[k]] != 0 ? 1.0 : 0.0);
  for (int k = 0; k < 4; ++k) {
    const int sel = simShapeSel(clamp01(d[kSimShapeDouble[k]]));
    // Not one of the five values: the engine keeps its initial shape.
    setp(s, 54 + k, sel >= 0 ? sel : (k == 3 ? dc::WlTriangle : dc::WsSaw));
    if (k == 3 && sel == 4) setp(s, 57, dc::WlTriangle);  // 0.3 is not an LFO shape
  }
  m.glType = "gl.warmsynth";
  m.state = s;
  m.exact = true;
  ParamTable t;
  for (int tag = 0; tag < 36; ++tag) {
    if (tag == dc::WmOsc1Coarse || tag == dc::WmOsc2Coarse || tag == dc::WmOsc3Coarse) {
      t.fn(tag, tag, [](double n) { return std::round(n * 48.0) - 24.0; });
      t.range(tag, 0, 48);
    } else {
      t.lin(tag, tag, 0.0, 1.0);
      t.range(tag, 0, 65535);
    }
  }
  for (int tag = 36; tag <= 53; ++tag) t.fn(tag, tag, [](double n) { return n >= 0.5 ? 1.0 : 0.0; });
  for (int tag = 54; tag <= 57; ++tag) {
    const int n = tag == 57 ? 4 : 5;
    t.fn(tag, tag, [n](double x) { return std::round(x * (n - 1)); });
    t.range(tag, 0, n - 1);
  }
  t.install(m);
  return true;
}

// ------------------------------------------------------- Kepler / Kepler Exo
// Classic one-DCO tag → DCO Synth control (the engine's own table).
constexpr int kKepler1Tag[36] = {85, 89, 88, 86, 87, 48, 49, 50, 51, 81, 0, 5, 6, 7, 8, 9, 10, 34,
                                 36, 37, 38, 43, 44, 45, 39, 47, 61, 62, 63, 64, 69, 82, 83, 72, 52, 53};

// Synced LFO rates of states saved before the sync table changed (version
// 1 of the two-DCO state, versions ≤ 3 of the one-DCO state).
// (Old layout: position → beats per cycle on a 2^(7x) scale; new layout: the
// same length placed on the logarithmic 0..1 rate scale.)
double convertOldSyncRate(double x) {
  const double f = std::exp2(clamp01(x) * 7.0);
  const int steps = static_cast<int>((f - 1.0) * 1.007874 * 48.0 * 6143.0 / 6144.0 + 1.1);
  if (steps <= 0) return clamp01(x);
  const int len = static_cast<int>(1.0 / (steps / 48.0) * 4.0 * 48.0 + 0.1) - 1;
  const double u = std::max(0.0, len / 6143.0);
  return clamp01(std::log(u * 127.0 + 1.0) / -std::log(2.0) / 7.0 + 1.0);
}

// Stored integer ranges of the reference controls (recorded pattern values).
struct TagRange {
  int tag, lo, hi;
};
constexpr TagRange kKepler1Ranges[] = {{3, 0, 245760}, {5, 1, 245760}, {7, 0, 6000}, {10, 0, 6000}, {11, 0, 6000}, {16, 0, 6000}, {17, 0, 6000}, {18, 0, 6000}, {19, 0, 6000}, {20, 0, 6000}, {22, 0, 6000}, {23, 0, 6000}, {24, 0, 6000}, {26, 0, 6000}, {27, 0, 6000}, {28, 0, 6000}, {29, 0, 6000}, {33, 0, 65536}, {35, 1, 245760}};
constexpr TagRange kKeplerExoRanges[] = {{0, 0, 6000}, {2, 0, 6000}, {4, 0, 6000}, {5, 0, 6000}, {10, 0, 6000}, {11, 0, 6000}, {13, -48, 48}, {14, 0, 6000}, {17, 0, 6000}, {18, 0, 6000}, {19, 0, 6000}, {20, 0, 6000}, {22, 0, 6000}, {24, 0, 6000}, {28, 0, 245760}, {29, 1, 245760}, {32, 0, 245760}, {33, 0, 245760}, {34, 0, 245760}, {35, 0, 245760}, {36, 1000, 170000}, {37, 1000, 170000}, {38, 0, 6000}, {39, 0, 6000}, {41, 0, 6000}, {44, 0, 6000}, {45, 0, 6000}, {46, 0, 6000}, {48, 1, 245760}, {50, 0, 10000}, {53, 0, 10000}, {54, 1, 245760}, {56, 0, 10000}, {60, 0, 10000}, {61, 0, 10000}, {62, 0, 10000}, {63, 0, 6000}, {64, 0, 10000}, {65, 0, 10000}, {66, 0, 10000}, {67, 0, 6000}, {68, 0, 10000}, {70, 0, 6000}, {72, 0, 65536}, {73, 0, 65536}, {74, 0, 65536}, {80, 0, 65536}, {84, 0, 65536}, {86, 0, 245760}, {89, 0, 8}, {90, 0, 6000}, {92, 0, 6000}, {93, 0, 6000}, {94, 0, 6000}, {95, 0, 6000}, {96, 0, 6000}, {97, 0, 6000}, {98, 0, 6000}, {99, 0, 6000}, {101, 0, 6000}, {102, 0, 6000}, {103, 0, 6000}, {104, 0, 6000}, {105, 0, 6000}, {106, 0, 6000}, {107, 0, 21000}, {108, 0, 21000}, {109, 0, 21000}, {112, 0, 6000}, {113, 0, 6000}, {114, 0, 6000}, {115, 0, 6000}, {116, 0, 6000}, {118, 0, 6000}, {119, 0, 6000}, {120, 0, 6000}, {122, 190, 30000}, {123, 5000, 221000}, {124, 0, 6000}, {125, 0, 10000}, {126, 0, 10000}, {127, 0, 10000}, {128, 0, 6000}, {129, 0, 10000}, {130, 0, 10000}, {131, 0, 10000}, {132, 0, 6000}, {133, 0, 10000}, {135, 0, 6000}, {138, 0, 6000}, {139, 0, 6000}, {140, 0, 6000}, {141, 0, 6000}, {142, 0, 6000}, {143, 0, 6000}, {145, 0, 6000}, {146, 0, 6000}, {147, 0, 6000}, {148, 0, 6000}, {149, 0, 65536}, {150, 0, 65536}, {153, 0, 65536}};

// Automation: FL parameter index = control tag; switches / selectors turn the
// 0..1 position into their choice index.
void keplerLinks(PluginMapping& m, bool classic) {
  const PluginInfo* info = findPlugin("gl.dcosynth");
  ParamTable t;
  auto link = [&](int fl, int gl) {
    const ParamDesc* d = info && gl < static_cast<int>(info->params.size()) ? &info->params[static_cast<size_t>(gl)] : nullptr;
    if (d && d->kind == ParamKind::Choice) {
      const double k = static_cast<double>(d->choices.size() - 1);
      t.fn(fl, gl, [k](double n) { return std::round(n * k); });
    } else if (d && d->kind == ParamKind::Toggle) {
      t.fn(fl, gl, [](double n) { return n >= 0.5 ? 1.0 : 0.0; });
    } else {
      t.lin(fl, gl, 0.0, 1.0);
    }
  };
  if (classic) {
    for (int k = 0; k < 36; ++k) link(k, kKepler1Tag[k]);
    for (const auto& r : kKepler1Ranges) t.range(r.tag, r.lo, r.hi);
  } else {
    for (int tag = 0; tag < 151; ++tag) link(tag, tag);
    for (const auto& r : kKeplerExoRanges) t.range(r.tag, r.lo, r.hi);
  }
  t.install(m);
}

// Writes the 0..1 positions of tag `tag` (switches → choice index).
void setTag(PluginState& s, int tag, double v) {
  const PluginInfo* info = findPlugin("gl.dcosynth");
  v = clamp01(v);
  if (info && tag < static_cast<int>(info->params.size())) {
    const ParamDesc& d = info->params[static_cast<size_t>(tag)];
    if (d.kind == ParamKind::Choice) v = std::round(v * static_cast<double>(d.choices.size() - 1));
    else if (d.kind == ParamKind::Toggle) v = v >= 0.5 ? 1.0 : 0.0;
  }
  setp(s, tag, v);
}

bool decodeKepler1(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  (void)c;
  const Event* ev = pluginState(evs);
  if (!ev) return false;
  Bytes b(*ev);
  const uint32_t nameLen = b.u32(12);
  if (b.u32(0) != 0 || b.u32(8) != 0 || nameLen == 0 || nameLen > 64 || b.u32(4) + 12 != b.n) return false;
  size_t at = 16 + nameLen;
  const uint32_t ver = b.u32(at), count = b.u32(at + 4);
  if (ver < 2 || ver > 4 || count == 0 || count > 36) return false;
  at += 8;
  const size_t tail = ver > 2 ? 12 : 9;
  if (!b.has(at, 4 * static_cast<size_t>(count) + tail) || at + 4 * count + tail != b.n) return false;
  PluginState s = gen::base("gl.dcosynth");
  setp(s, dc::DcPanel, dc::PanelOne);
  for (uint32_t k = 0; k < count; ++k) {
    const float f = b.f32(at + 4 * k);
    if (!std::isfinite(f)) return false;
    setTag(s, kKepler1Tag[k], f);
  }
  at += 4 * static_cast<size_t>(count);
  setp(s, dc::DcArpQuantize, b.u8(at) != 0 ? 1.0 : 0.0);
  setTag(s, dc::DcVelVol, b.f32(at + 1));
  setTag(s, dc::DcDco1SubPhase, b.f32(at + 5));
  if (ver > 2) {
    setp(s, dc::DcChorusGate, b.u8(at + 9) != 0 ? 1.0 : 0.0);
    setTag(s, dc::DcChorusNoise, b.u8(at + 10) != 0 ? 0.09 : 0.0);
    // at + 11: oversampling factor (quality only).
  } else if (count > 6 && b.f32(16 + nameLen + 8 + 4 * 6) >= 0.5f) {
    setTag(s, dc::DcLfo1Rate, convertOldSyncRate(b.f32(16 + nameLen + 8 + 4 * 5)));
  }
  m.glType = "gl.dcosynth";
  m.state = s;
  m.exact = true;
  keplerLinks(m, true);
  return true;
}

bool decodeKeplerExo(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  const Event* ev = pluginState(evs);
  if (!ev) return false;
  Bytes b(*ev);
  const uint32_t nameLen = b.u32(28);
  if (b.u32(0) != 1 || b.u32(4) != 1 || nameLen == 0 || nameLen > 64) return false;
  size_t at = 32 + nameLen;
  const uint32_t ver = b.u32(at), count = b.u32(at + 4);
  if (ver < 1 || ver > 2 || count == 0 || count > 155) return false;
  at += 8;
  if (!b.has(at, 4 * static_cast<size_t>(count) + 8)) return false;
  PluginState s = gen::base("gl.dcosynth");
  setp(s, dc::DcPanel, dc::PanelTwo);
  for (uint32_t t = 0; t < count; ++t) {
    const float f = b.f32(at + 4 * t);
    if (!std::isfinite(f)) return false;
    setTag(s, static_cast<int>(t), f);
  }
  if (ver == 1) {
    if (count > 49 && b.f32(at + 4 * 49) >= 0.5f) setTag(s, dc::DcLfo1Rate, convertOldSyncRate(b.f32(at + 4 * 48)));
    if (count > 55 && b.f32(at + 4 * 55) >= 0.5f) setTag(s, dc::DcLfo2Rate, convertOldSyncRate(b.f32(at + 4 * 54)));
  }
  at += 4 * static_cast<size_t>(count);
  // Modulation matrix chunk.
  if (b.u32(at) != 1) return false;
  const uint32_t rows = b.u32(at + 4);
  if (rows > 8 || !b.has(at + 8, 8 * static_cast<size_t>(rows))) return false;
  at += 8;
  bool lost = false;
  for (uint32_t r = 0; r < rows; ++r) {
    const int32_t src = b.i32(at), dst = b.i32(at + 4);
    at += 8;
    int gsrc = src >= 0 && src < dc::kDcSources ? src + 1 : 0;
    int gdst = 0;
    if (dst >= 0 && dst < dc::kDcTags) gdst = dst + 1;
    else if (dst == 256) gdst = dc::kDcDestDco1Pitch;
    else if (dst == 257) gdst = dc::kDcDestDco2Pitch;
    if ((src >= 0 && gsrc == 0) || (dst >= 0 && gdst == 0)) lost = true;
    if (gsrc == 0 || gdst == 0) gsrc = gdst = 0;
    setp(s, dc::DcMxSrc1 + static_cast<int>(r), gsrc);
    setp(s, dc::DcMxDst1 + static_cast<int>(r), gdst);
  }
  // Runtime chunk (rebuilt on load): u32 2, u64 size, payload.
  if (b.u32(at) != 2) return false;
  const uint64_t extra = b.u64(at + 4);
  if (extra > 4096 || at + 12 + extra != b.n) return false;
  m.glType = "gl.dcosynth";
  m.state = s;
  m.exact = !lost;
  if (lost) gen::note(c, "Kepler Exo", "unknown modulation matrix source / destination dropped");
  keplerLinks(m, false);
  return true;
}

}  // namespace

void registerDco(std::map<std::string, PluginDecoder>& d) {
  set(d, "Fruity DX10", decodeDx10);
  set(d, "SimSynth", decodeSimSynth);
  set(d, "Kepler", decodeKepler1);
  set(d, "Kepler Exo", decodeKeplerExo);
}

}  // namespace gl::flp::j22
