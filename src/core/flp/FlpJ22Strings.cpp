// Gridline — FLP import, J-22 (Sakura, Plucked!, Transistor Bass) → the
// dedicated string-group engines (packs/models/strings.h):
//   Sakura           → Twin String ("gl.twinstring"): "SSLF", u32 payload size
//                      (= length − 8), "1rkS" (id stored reversed), 114 float32
//                      in control-tag order (knobs / sliders / switches 0..1;
//                      combo boxes and digit wheels raw: 79 + 4k mod source
//                      0..8, 82 + 4k destination 0..14, 111 string-2 ratio
//                      0..6, 112 delay sync 0..9), then the 128-byte preset
//                      name (NUL padded). 596 bytes in every shipped state.
//                      Parameter index = tag: every stored value maps 1:1.
//   Plucked!         → Plucker ("gl.plucker"): 5 × i32 = tags 0..4 (decay
//                      0..256, colour 0..128, normalize, gate, widen).
//   Transistor Bass  → Step Bass ("gl.stepbass"): see parseStepBass.
// Layouts worked out from FL's shipped presets / projects (all walk to their
// exact end) and the plugins' control lists; no Image-Line code is used.
#include "FlpJ22Strings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include "../../packs/models/strings.h"
#include "../../packs/models/strings_laws.h"
#include "FlpGen2.h"
#include "FlpJ22.h"

namespace gl::flp::j22 {

namespace strs = gl::packs::models::strs;

namespace {

struct Rd {
  const uint8_t* p;
  size_t n, at = 0;
  bool bad = false;
  bool need(size_t k) {
    if (bad || at > n || k > n - at) bad = true;
    return !bad;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = static_cast<uint32_t>(p[at]) | (static_cast<uint32_t>(p[at + 1]) << 8) | (static_cast<uint32_t>(p[at + 2]) << 16) |
                 (static_cast<uint32_t>(p[at + 3]) << 24);
    at += 4;
    return v;
  }
  int32_t i32() { return static_cast<int32_t>(u32()); }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v = static_cast<uint16_t>(p[at] | (p[at + 1] << 8));
    at += 2;
    return v;
  }
  uint8_t u8() { return need(1) ? p[at++] : 0; }
  float f32() {
    uint32_t u = u32();
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  double f64() {
    if (!need(8)) return 0;
    uint64_t u = 0;
    for (int i = 7; i >= 0; --i) u = (u << 8) | p[at + static_cast<size_t>(i)];
    at += 8;
    double d;
    std::memcpy(&d, &u, 8);
    return d;
  }
  std::string str(size_t len) {
    if (!need(len)) return {};
    std::string s(reinterpret_cast<const char*>(p + at), len);
    at += len;
    if (auto z = s.find('\0'); z != std::string::npos) s.resize(z);
    return s;
  }
};

double clamp01(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; }

// ---------------------------------------------------------------- Sakura
bool decodeSakura(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  gen2::Blob b = gen2::stateBlob(evs);
  TwinStringState st;
  if (!parseTwinString(b.p, b.n, st)) return false;
  gen2::begin(m, "gl.twinstring");
  for (int t = 0; t < strs::TsCount; ++t) {
    double v = st.tags[static_cast<size_t>(t)];
    if (!std::isfinite(v)) v = 0;
    // Combos / digit wheels keep their raw index; everything else is 0..1.
    bool raw = t == strs::TsS2Ratio || t == strs::TsDlySync ||
               (t >= strs::TsMod1 && t < strs::TsS2Ratio && ((t - strs::TsMod1) % strs::TmStride == strs::TmSource ||
                                                              (t - strs::TsMod1) % strs::TmStride == strs::TmDest));
    if (t == strs::TsTranspose)
      gen2::set(m, t, strs::law::tsOctaveStep(v));  // the engine snaps the knob to octave steps
    else
      gen2::set(m, t, raw ? std::round(v) : clamp01(v));
  }
  m.exact = true;  // identity layout: each stored control drives the same Twin String control
  // Automation: FL index = tag = Gridline index; FL's normalized value is the
  // knob position (choices spread over their item count by fromNormalized).
  std::vector<gen2::Link> links;
  std::vector<RawRange> ranges;
  for (int t = 0; t < strs::TsCount; ++t) {
    if (t == strs::TsTranspose)
      links.push_back({t, t, [](double n) { return static_cast<double>(strs::law::tsOctaveStep(n)); }});
    else
      links.push_back({t, t, {}});
    double hi = 256;
    if (t == strs::TsS2Ratio) hi = 6;
    else if (t == strs::TsDlySync) hi = 9;
    else if (t == strs::TsTranspose) hi = 6;  // stored −3..3 → raw range of 6 steps (offset applied below)
    else if (t >= strs::TsMod1 && t < strs::TsS2Ratio) {
      int f = (t - strs::TsMod1) % strs::TmStride;
      hi = f == strs::TmSource ? 8 : f == strs::TmDest ? 14 : f == strs::TmNegative ? 1 : 256;
    } else if (t == strs::TsS1Inv || t == strs::TsS2Inv || t == strs::TsCombFbInv || t == strs::TsCombLevelInv ||
               (t >= strs::TsResOn1 && t < strs::TsResOn1 + strs::kTsRes) || t == strs::TsDlyOn || t == strs::TsChoOn ||
               t == strs::TsRevOn)
      hi = 1;
    ranges.push_back({t, t == strs::TsTranspose ? -3.0 : 0.0, t == strs::TsTranspose ? 3.0 : hi});
  }
  gen2::linkParams(m, std::move(links), std::move(ranges));
  return true;
}

// -------------------------------------------------------------- Plucked!
bool decodePlucked(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  gen2::Blob b = gen2::stateBlob(evs);
  PluckerState st;
  if (!parsePlucker(b.p, b.n, st)) return false;
  gen2::begin(m, "gl.plucker");
  gen2::set(m, strs::PkDecay, std::clamp(st.v[0], 0, 256));
  gen2::set(m, strs::PkColor, std::clamp(st.v[1], 0, 128));
  gen2::set(m, strs::PkNormalize, st.v[2] != 0 ? 1 : 0);
  gen2::set(m, strs::PkGate, st.v[3] != 0 ? 1 : 0);
  gen2::set(m, strs::PkWiden, st.v[4] != 0 ? 1 : 0);
  m.exact = true;  // identity layout (tags 0..4 = Plucker parameters 0..4)
  gen2::linkParams(m, {{0, strs::PkDecay}, {1, strs::PkColor}, {2, strs::PkNormalize}, {3, strs::PkGate}, {4, strs::PkWiden}},
                   {{0, 0, 256}, {1, 0, 128}, {2, 0, 1}, {3, 0, 1}, {4, 0, 1}});
  return true;
}

// ------------------------------------------------------- Transistor Bass
bool decodeTransistorBass(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  gen2::Blob b = gen2::stateBlob(evs);
  StepBassState st;
  if (!parseStepBass(b.p, b.n, st)) return false;
  gen2::begin(m, "gl.stepbass");
  // The trailer's three zero words select program / sequence / patch (all
  // shipped states sit on program 0); the program names its patch.
  const uint32_t prog = std::min<uint32_t>(st.trailer[1], strs::kSbPrograms - 1);
  const auto& pa = st.patches[std::min<size_t>(st.programs[prog].patch, st.patches.size() - 1)];
  for (int i = 0; i < strs::SbPatchCount; ++i) gen2::set(m, i, clamp01(pa.v[i]));
  gen2::set(m, strs::SbProgram, prog);
  gen2::set(m, strs::SbSeqOn, st.flag ? 1 : 0);
  gen2::set(m, strs::SbRoot, std::clamp<uint32_t>(st.root, 0, 127));
  gen2::set(m, strs::SbBpm, std::clamp(st.sequences[st.programs[prog].sequence].bpm, 10.0, 522.0));
  if (!c.paramsOnly) m.state.data = stepBassData(st);
  // Per-step f64 / f32 fields (0.5 everywhere), sequence tails, the trailer's
  // first / fifth words and the second patch copy are kept in no control.
  m.exact = false;
  std::vector<gen2::Link> links;
  std::vector<RawRange> ranges;
  for (int t = 0; t < strs::SbPatchCount; ++t) {
    links.push_back({t, t, {}});
    const bool sw = t == strs::SbRevOn || t == strs::SbDistOn || t == strs::SbDlyOn || t == strs::SbDlySync || t == strs::SbClassicPulse;
    ranges.push_back({t, 0, sw ? 1.0 : t == 0 ? 24576.0 : 65536.0});
  }
  gen2::linkParams(m, std::move(links), std::move(ranges));
  return true;
}

}  // namespace

bool parseTwinString(const uint8_t* p, size_t n, TwinStringState& out) {
  out = TwinStringState{};
  if (!p || n < 12 + 4 * static_cast<size_t>(strs::TsCount)) return false;
  if (std::memcmp(p, "SSLF", 4) != 0 || std::memcmp(p + 8, "1rkS", 4) != 0) return false;
  Rd r{p, n, 4};
  uint32_t size = r.u32();
  if (size != n - 8) return false;
  r.at = 12;
  out.tags.resize(strs::TsCount);
  for (int t = 0; t < strs::TsCount; ++t) {
    float v = r.f32();
    if (!std::isfinite(v)) return false;
    out.tags[static_cast<size_t>(t)] = v;
  }
  // Preset name block (128 bytes in every shipped state; shorter tails tolerated).
  const size_t rest = n - r.at;
  if (rest > 4096) return false;
  out.name = r.str(rest);
  out.consumed = r.bad ? 0 : r.at;
  return !r.bad && out.consumed == n;
}

bool parsePlucker(const uint8_t* p, size_t n, PluckerState& out) {
  out = PluckerState{};
  if (!p || n != 20) return false;
  Rd r{p, n};
  for (int i = 0; i < 5; ++i) out.v[i] = r.i32();
  if (out.v[0] < 0 || out.v[0] > 256 || out.v[1] < 0 || out.v[1] > 128) return false;
  for (int i = 2; i < 5; ++i)
    if (out.v[i] != 0 && out.v[i] != 1) return false;
  out.consumed = r.at;
  return !r.bad;
}

bool parseStepBass(const uint8_t* p, size_t n, StepBassState& out) {
  out = StepBassState{};
  if (!p || n < 0x40 || n > (1u << 22)) return false;
  Rd r{p, n};
  for (auto& h : out.head) h = r.u32();
  if (out.head[0] != 1 || out.head[2] != 8 || out.head[7] != n - 36 || out.head[10] != n - 48) return false;
  out.version = r.u16();
  out.root = r.u32();
  out.root2 = r.u32();
  out.flag = r.u8();
  auto name = [&r]() {
    uint32_t len = r.u32();
    if (len > 256) {
      r.bad = true;
      return std::string();
    }
    return r.str(len);
  };
  out.programs.resize(strs::kSbPrograms);
  for (auto& pr : out.programs) {
    pr.a = r.u16();
    pr.b = r.u8();
    pr.name = name();
    pr.patch = r.u32();
    pr.sequence = r.u32();
    pr.c = r.u32();
    if (pr.patch >= 256 || pr.sequence >= static_cast<uint32_t>(strs::kSbPrograms)) r.bad = true;
  }
  out.patches.resize(256);
  for (auto& pa : out.patches) {
    pa.name = name();
    for (float& v : pa.v) {
      v = r.f32();
      if (!std::isfinite(v)) r.bad = true;
    }
  }
  out.sequences.resize(strs::kSbPrograms);
  for (auto& sq : out.sequences) {
    sq.pre = r.u16();
    sq.name = name();
    sq.b = r.u16();
    sq.length = r.u32();
    if (sq.length < 1 || sq.length > 32) r.bad = true;
    for (auto& st : sq.steps) {
      st.lead = r.u8();
      st.b1 = r.u8();
      st.accent = r.u8();
      st.note = r.i32();
      st.gate = r.u8();
      st.d = r.f64();
      st.x = r.f32();
      st.slide = r.u8();
      if (st.note < 0 || st.note > 127) r.bad = true;
    }
    sq.bpm = r.f64();
    if (!std::isfinite(sq.bpm)) r.bad = true;
    for (auto& t : sq.tail) t = r.u8();
  }
  for (auto& t : out.trailer) t = r.u32();
  out.consumed = r.bad ? 0 : r.at;
  return !r.bad && r.at == n;
}

std::string stepBassData(const StepBassState& s) {
  // Gridline's text form of the bank (UI / runtime parse it; see stepbass_dsp.cpp).
  std::ostringstream o;
  o << "stepbass 1\n";
  char buf[64];
  for (int k = 0; k < strs::kSbPrograms; ++k) {
    const auto& pr = s.programs[static_cast<size_t>(k)];
    o << "prog " << k << ' ' << pr.patch << ' ' << pr.sequence << '\n';
  }
  for (size_t k = 0; k < s.patches.size(); ++k) {
    const auto& pa = s.patches[k];
    o << "patch " << k;
    for (int i = 0; i < strs::SbPatchCount; ++i) {
      std::snprintf(buf, sizeof buf, " %.6g", static_cast<double>(pa.v[i]));
      o << buf;
    }
    o << " | " << pa.name << '\n';
  }
  for (size_t k = 0; k < s.sequences.size(); ++k) {
    const auto& sq = s.sequences[k];
    std::snprintf(buf, sizeof buf, "seq %zu %u %.6g", k, sq.length, sq.bpm);
    o << buf;
    for (const auto& st : sq.steps) o << ' ' << st.note << ':' << (st.gate ? 1 : 0) << ':' << (st.accent ? 1 : 0) << ':' << (st.slide ? 1 : 0);
    o << " | " << sq.name << '\n';
  }
  return o.str();
}

void registerStrings(std::map<std::string, PluginDecoder>& d) {
  set(d, "Sakura", decodeSakura);
  set(d, "Plucked!", decodePlucked);
  set(d, "Transistor Bass", decodeTransistorBass);
}

}  // namespace gl::flp::j22
