// Gridline — FLP import, J-22 (metal group): the metallic timbre generator and
// its preset player → Alloy ("gl.alloy"), the four-program spectral morpher →
// Spectra Morph ("gl.spectramorph"), the granular sample player → Grain
// Scanner ("gl.grainscan"), the spline scratcher → Spline Scratcher
// ("gl.splinescratch"). Layouts: see each decoder; all were worked out from FL
// Studio's shipped presets / projects (every one walks to its exact end) and,
// for field meanings, the plugins' own behaviour (clean-room notes in
// the J-22 metal-group notes in the task report).
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <vector>
#include "../../packs/models/metal.h"
#include "FlpGen.h"
#include "FlpJ22.h"
#include "FlpJ22Metal.h"

namespace gl::flp::j22 {

namespace {

namespace mt = gl::packs::models::metal;
namespace hm = gl::packs::models::hm;
using gen::Bytes;

// Sequential bounds-checked reader.
struct Rd {
  const uint8_t* p;
  size_t n, at = 0;
  bool bad = false;
  bool need(size_t k) {
    if (bad || at > n || k > n - at) bad = true;
    return !bad;
  }
  int32_t i32() {
    if (!need(4)) return 0;
    uint32_t v = static_cast<uint32_t>(p[at]) | (static_cast<uint32_t>(p[at + 1]) << 8) | (static_cast<uint32_t>(p[at + 2]) << 16) |
                 (static_cast<uint32_t>(p[at + 3]) << 24);
    at += 4;
    return static_cast<int32_t>(v);
  }
  uint8_t u8() { return need(1) ? p[at++] : 0; }
  float f32() {
    const uint32_t u = static_cast<uint32_t>(i32());
    float f;
    std::memcpy(&f, &u, 4);
    return std::isfinite(f) ? f : 0.0f;
  }
  double f64() {
    if (!need(8)) return 0;
    uint64_t u = 0;
    for (int i = 7; i >= 0; --i) u = (u << 8) | p[at + static_cast<size_t>(i)];
    at += 8;
    double d;
    std::memcpy(&d, &u, 8);
    return std::isfinite(d) ? d : 0.0;
  }
  void skip(size_t k) {
    if (need(k)) at += k;
  }
  int32_t peek(size_t off) const {
    if (at + off + 4 > n) return 0;
    const uint8_t* q = p + at + off;
    return static_cast<int32_t>(static_cast<uint32_t>(q[0]) | (static_cast<uint32_t>(q[1]) << 8) | (static_cast<uint32_t>(q[2]) << 16) |
                                (static_cast<uint32_t>(q[3]) << 24));
  }
};

std::string latin1(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    const unsigned c = p[i];
    if (c == 0) break;
    if (c < 0x80) s += static_cast<char>(c);
    else s += static_cast<char>(0xC0 | (c >> 6)), s += static_cast<char>(0x80 | (c & 0x3F));
  }
  return s;
}
std::string utf16(const uint8_t* p, size_t chars) {
  std::string s;
  for (size_t i = 0; i < chars; ++i) {
    uint32_t c = static_cast<uint32_t>(p[2 * i]) | (static_cast<uint32_t>(p[2 * i + 1]) << 8);
    if (c >= 0xE000 && c < 0xF900) continue;  // private-use formatting marks
    if (c < 0x80) s += static_cast<char>(c);
    else if (c < 0x800) s += static_cast<char>(0xC0 | (c >> 6)), s += static_cast<char>(0x80 | (c & 0x3F));
    else s += static_cast<char>(0xE0 | (c >> 12)), s += static_cast<char>(0x80 | ((c >> 6) & 0x3F)), s += static_cast<char>(0x80 | (c & 0x3F));
  }
  return s;
}
// Plain text of an RTF memo (control words / groups dropped, \par → newline).
std::string rtfText(const std::string& rtf) {
  if (rtf.rfind("{\\rtf", 0) != 0) return rtf;
  std::string out;
  int depth = 0, skipDepth = -1;
  for (size_t i = 0; i < rtf.size(); ++i) {
    const char ch = rtf[i];
    if (ch == '{') {
      ++depth;
      if (i + 1 < rtf.size() && (rtf.compare(i + 1, 8, "\\fonttbl") == 0 || rtf.compare(i + 1, 9, "\\colortbl") == 0 ||
                                 rtf.compare(i + 1, 2, "\\*") == 0) && skipDepth < 0)
        skipDepth = depth;
      continue;
    }
    if (ch == '}') {
      if (depth == skipDepth) skipDepth = -1;
      --depth;
      continue;
    }
    if (skipDepth >= 0) continue;
    if (ch == '\\') {
      size_t j = i + 1;
      while (j < rtf.size() && std::isalpha(static_cast<unsigned char>(rtf[j]))) ++j;
      const std::string word = rtf.substr(i + 1, j - i - 1);
      if (j < rtf.size() && (rtf[j] == '-' || std::isdigit(static_cast<unsigned char>(rtf[j]))))
        while (j < rtf.size() && (rtf[j] == '-' || std::isdigit(static_cast<unsigned char>(rtf[j])))) ++j;
      if (j < rtf.size() && rtf[j] == ' ') ++j;
      if (word == "par" || word == "line") out += '\n';
      if (word.empty() && j < rtf.size()) out += rtf[j++];  // escaped char
      i = j - 1;
      continue;
    }
    if (ch == '\r' || ch == '\n') continue;
    out += ch;
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

// ============================================================ Alloy (Ogun)
// One drawn curve: i32 kind (1 envelope, 2 LFO, 3 map), i32 format, i32 n,
// n points {x, y (f64 from format 2, else f32), f32 tension, (format ≥ 1) u8
// segment type + 3 bytes}, then i32 flags (1 tempo, 2 global), enabled, decay,
// loop start, sustain point.
bool readCurve(Rd& r, int kind, hm::Curve& c) {
  const int k = r.i32(), fmt = r.i32(), n = r.i32();
  if (r.bad || k != kind || fmt < 0 || fmt > 3 || n < 0 || n > 200000) return r.bad = true, false;
  c.pts.clear();
  std::vector<hm::Pt> pts;
  pts.reserve(static_cast<size_t>(std::min(n, 65536)));
  for (int i = 0; i < n && !r.bad; ++i) {
    hm::Pt p;
    double x, y;
    if (fmt >= 2) x = r.f64(), y = r.f64();
    else x = r.f32(), y = r.f32();
    p.tension = std::clamp(r.f32(), -1.0f, 1.0f);
    if (fmt >= 1) {
      p.kind = static_cast<uint8_t>(std::min<int>(r.u8(), 12));
      r.skip(3);
    }
    p.dt = static_cast<float>(std::max(0.0, x));
    p.y = static_cast<float>(std::clamp(y, -4.0, 4.0));
    pts.push_back(p);
  }
  const int flags = r.i32(), enabled = r.i32(), decay = r.i32(), loop = r.i32(), sus = r.i32();
  if (r.bad) return false;
  // Very dense curves (resynthesised timbre EQs): keep every point up to the
  // stored limit, else thin evenly (the curve is sampled per harmonic anyway).
  constexpr size_t kKeep = 16000;
  if (pts.size() > kKeep) {
    std::vector<hm::Pt> thin;
    const double step = static_cast<double>(pts.size()) / kKeep;
    size_t last = 0;
    for (size_t i = 0; i < kKeep; ++i) {
      const size_t idx = std::min(pts.size() - 1, static_cast<size_t>(i * step));
      hm::Pt q = pts[idx];
      float dt = 0;
      for (size_t j = last + 1; j <= idx; ++j) dt += pts[j].dt;
      q.dt = i == 0 ? pts[0].dt : dt;
      last = idx;
      thin.push_back(q);
    }
    pts.swap(thin);
  }
  c.pts = std::move(pts);
  const int np = static_cast<int>(c.pts.size());
  c.enabled = enabled != 0;
  c.tempo = (flags & 1) != 0;
  c.global = (flags & 2) != 0;
  c.decay = decay >= 0 && decay < np ? decay : -1;
  c.loopStart = loop >= 0 && loop < np ? loop : -1;
  c.sustain = sus >= 0 && sus < np ? sus : -1;
  return true;
}

// One articulator: i32 record version (2; 3 adds the LFO phase), f32 × 3,
// envelope curve, i32 attack / decay / sustain / release knobs, LFO curve,
// i32 speed / tension / skew / width (+ u32 phase), then the key, velocity,
// X and Y mapping curves.
bool readArt(Rd& r, mt::AlloyArtData& A, int knobs[8]) {
  const int ver = r.i32();
  if (ver < 1 || ver > 3) return r.bad = true, false;
  r.f32(), r.f32(), r.f32();
  if (!readCurve(r, 1, A.env)) return false;
  for (int k = 0; k < 4; ++k) knobs[k] = r.i32();
  if (!readCurve(r, 2, A.lfo)) return false;
  for (int k = 4; k < 8; ++k) knobs[k] = r.i32();
  A.lfoPhase = 0;
  if (ver >= 3) A.lfoPhase = static_cast<float>(static_cast<uint32_t>(r.i32()) / 4294967296.0);
  A.att = std::clamp(knobs[0] / 128.0f, 0.0f, 8.0f), A.dec = std::clamp(knobs[1] / 128.0f, 0.0f, 8.0f);
  A.sus = std::clamp(knobs[2] / 128.0f, -1.0f, 1.0f), A.rel = std::clamp(knobs[3] / 128.0f, 0.0f, 8.0f);
  A.lfoSpeed = static_cast<float>(std::clamp((std::pow(10001.0, std::clamp(knobs[4], 0, 65536) / 65536.0) - 1.0) / 625.0, 0.0, 200.0));
  A.lfoTension = std::clamp(knobs[5] / 128.0f, -1.0f, 1.0f), A.lfoSkew = std::clamp(knobs[6] / 128.0f, -1.0f, 1.0f),
  A.lfoPw = std::clamp(knobs[7] / 128.0f, -1.0f, 1.0f);
  for (int m = 0; m < mt::AmCount; ++m)
    if (!readCurve(r, 3, A.maps[static_cast<size_t>(m)])) return false;
  return !r.bad;
}

// J-44: an articulator an old state does not store (version < 4: 4..5,
// version < 10: 11..12) keeps the plugin's fresh one — the articulator every
// FL-written state carries for an untouched slot: record version 2, header
// {0, 1, 1}, envelope (0,0) (0.75,1) (1.5,0.5) (0.25,0) at tension 0.2, off,
// decay point 1, sustain point 2; LFO (0,0.5) (1,0.5) (0.25,1) (1,0.5), sustain
// point 2; knobs 128 128 0 128 / 40000 0 0 0; four flat maps (0,1) (1,1).
void freshArt(mt::AlloyArtData& A, int knobs[8]) {
  std::vector<uint8_t> b;
  auto i32 = [&b](int32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
  };
  auto f32 = [&i32](float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    i32(static_cast<int32_t>(u));
  };
  auto curve = [&](int kind, std::initializer_list<std::array<float, 3>> pts, std::array<int32_t, 5> tail) {
    i32(kind), i32(1), i32(static_cast<int32_t>(pts.size()));
    for (const auto& p : pts) f32(p[0]), f32(p[1]), f32(p[2]), i32(0);
    for (int32_t v : tail) i32(v);
  };
  i32(2);
  f32(0), f32(1), f32(1);
  curve(1, {{0, 0, 0}, {0.75f, 1, 0.2f}, {1.5f, 0.5f, 0.2f}, {0.25f, 0, 0.2f}}, {0, 0, 1, -1, 2});
  for (int v : {128, 128, 0, 128}) i32(v);
  curve(2, {{0, 0.5f, 0}, {1, 0.5f, 0}, {0.25f, 1, 0}, {1, 0.5f, 0}}, {0, 0, -1, -1, 2});
  for (int v : {40000, 0, 0, 0}) i32(v);
  for (int m = 0; m < mt::AmCount; ++m) curve(3, {{0, 1, 0}, {1, 1, 0}}, {0, -1, -1, -1, -1});
  Rd r{b.data(), b.size()};
  readArt(r, A, knobs);
}

// Option block defaults (object offsets 0x200..) for fields an old version does not store.
void alloyOptionDefaults(uint8_t* o) {
  std::memset(o, 0, 64);
  o[0x21] = 1;                    // portamento: variable time
  o[0x38] = 0xAE, o[0x39] = 0x01;  // magic dust 430
}
}  // namespace

bool decodeAlloyState(const uint8_t* p, size_t n, AlloyDecoded& out, bool autogun) {
  out = AlloyDecoded{};
  if (n < 8) return false;
  const uint32_t raw = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                       (static_cast<uint32_t>(p[3]) << 24);
  if (raw == 0x7FFFFFFF || raw >= 0x130004) return false;
  const int ver = raw > 0xFFFF ? static_cast<int>(raw >> 16) : static_cast<int>(raw);
  out.version = ver;
  std::vector<uint8_t> z;
  if (!gen::inflateZlib(Bytes(p + 4, n - 4), z, 32u << 20) || z.size() < 64) return false;
  out.inflated = z.size();
  Rd r{z.data(), z.size()};
  // Parameters: tags 0..2 (version < 8: 0..1, fine pitch = 0), then tags 3..48
  // and the option block as one record whose tail grew with the versions.
  for (int t = 0; t < (ver < 8 ? 2 : 3); ++t) out.params[t] = r.i32();
  int len = 0xF4;
  if (ver < 1) len -= 1;
  if (ver < 2) len -= 3;
  if (ver < 7) len -= 1;
  if (ver < 9) len -= 16;
  if (ver < 11) len -= 1;
  if (ver < 12) len -= 1;
  if (ver < 13) len -= 6;
  // Record bytes from object offset 0x148 (tag 3) onwards.
  uint8_t rec[0xF4 + 16] = {};
  {
    uint8_t o[64];
    alloyOptionDefaults(o);
    std::memcpy(rec + (0x200 - 0x148), o, 0xF4 - (0x200 - 0x148));
  }
  if (!r.need(static_cast<size_t>(len))) return false;
  std::memcpy(rec, r.p + r.at, static_cast<size_t>(len));
  r.at += static_cast<size_t>(len);
  for (int t = 3; t < 49; ++t) {
    const size_t at = static_cast<size_t>(t - 3) * 4;
    out.params[t] = static_cast<int32_t>(static_cast<uint32_t>(rec[at]) | (static_cast<uint32_t>(rec[at + 1]) << 8) |
                                         (static_cast<uint32_t>(rec[at + 2]) << 16) | (static_cast<uint32_t>(rec[at + 3]) << 24));
  }
  std::memcpy(out.opt, rec + (0x200 - 0x148), 0x3C);
  if (ver < 14) out.opt[0x36] = 0;
  for (auto& b : out.extra3) b = r.u8();
  for (int& t : out.eqTypes) t = r.i32();
  // Patch notes: RTF (i32 length) before version 18, then UTF-16 text with a
  // compact length (u8, 0xFF = i32 follows).
  if (ver < 18) {
    const int32_t l = r.i32();
    if (l < 0 || !r.need(static_cast<size_t>(l))) return false;
    out.data.info = rtfText(latin1(r.p + r.at, static_cast<size_t>(l)));
    r.at += static_cast<size_t>(l);
  } else {
    int32_t l = r.u8();
    if (l == 0xFF) l = r.i32();
    if (l < 0 || !r.need(static_cast<size_t>(l) * 2)) return false;
    out.data.info = utf16(r.p + r.at, static_cast<size_t>(l));
    r.at += static_cast<size_t>(l) * 2;
    // The preset player stores one more word after the notes (version 19).
    if (autogun && ver >= 19 && r.peek(16) != 1) r.i32();
  }
  if (r.bad) return false;
  // Articulators 0..12 (older versions leave out 4..5 and 11..12), timbre EQ curves 13..16.
  for (int a = 0; a < mt::AaCount; ++a) {
    int knobs[8];
    if ((ver < 4 && (a == 4 || a == 5)) || (ver < 10 && (a == 11 || a == 12))) {
      freshArt(out.data.arts[static_cast<size_t>(a)], knobs);
      if (a < mt::kAlloyEnvArts) std::memcpy(out.knobs[a], knobs, sizeof knobs);
      continue;
    }
    if (!readArt(r, out.data.arts[static_cast<size_t>(a)], knobs)) return false;
    if (a < mt::kAlloyEnvArts) std::memcpy(out.knobs[a], knobs, sizeof knobs);
  }
  for (int e = 0; e < mt::AeCount; ++e) {
    if (ver < 5 && e == 0) continue;
    hm::Curve c;
    if (!readCurve(r, 3, c)) return false;
    if (ver < 6) {
      // Old curves span the harmonics linearly; resample onto log2(harmonic) / 15.
      const int rich = std::clamp(static_cast<int>(out.opt[0x0C]), 1, 16);
      const double h = std::max(1.0, std::pow(2.0, rich - 1) - 1.0);
      double len = 0;
      for (const auto& pt : c.pts) len += pt.dt;
      hm::Curve nc;
      for (int i = 0; i <= 15; ++i) {
        const double x = std::min(1.0, std::pow(2.0, i) / h) * std::max(1e-9, len);
        nc.pts.push_back({i == 0 ? 0.0f : 1.0f / 15.0f, static_cast<float>(hm::curveAt(c, x)), 0, 0});
      }
      c = nc;
    }
    out.data.eq[static_cast<size_t>(e)] = c;
  }
  out.consumed = r.at;
  out.consumedAll = !r.bad && r.at == z.size();
  return !r.bad;
}

namespace {

int optI32(const uint8_t* o, int off) {
  return static_cast<int32_t>(static_cast<uint32_t>(o[off]) | (static_cast<uint32_t>(o[off + 1]) << 8) |
                              (static_cast<uint32_t>(o[off + 2]) << 16) | (static_cast<uint32_t>(o[off + 3]) << 24));
}

bool decodeAlloy(Context& c, const std::vector<Event>& evs, PluginMapping& m, bool autogun) {
  Bytes b = gen::stateOf(evs);
  AlloyDecoded d;
  if (!b.p) return false;
  if (!decodeAlloyState(b.p, b.n, d, autogun)) {
    // Not a complete record (never written by the reference, but tolerated):
    // keep whatever control tags precede the damage, the rest at defaults.
    std::vector<uint8_t> z;
    if (b.n < 8 || !gen::inflateZlib(Bytes(b.p + 4, b.n - 4), z, 1u << 20) || z.size() < 12) return false;
    d = AlloyDecoded{};
    const PluginInfo* info = findPlugin("gl.alloy");
    for (int t = 0; t < 49; ++t)
      d.params[t] = static_cast<size_t>(t) * 4 + 4 <= z.size()
                        ? static_cast<int32_t>(static_cast<uint32_t>(z[static_cast<size_t>(t) * 4]) | (static_cast<uint32_t>(z[static_cast<size_t>(t) * 4 + 1]) << 8) |
                                               (static_cast<uint32_t>(z[static_cast<size_t>(t) * 4 + 2]) << 16) |
                                               (static_cast<uint32_t>(z[static_cast<size_t>(t) * 4 + 3]) << 24))
                        : (info ? static_cast<int>(info->params[static_cast<size_t>(t)].def) : 0);
    std::memset(d.opt, 0, sizeof d.opt);
    d.opt[0x0C] = 14, d.opt[0x1A] = 1, d.opt[0x1B] = 1, d.opt[0x1E] = 1, d.opt[0x21] = 1, d.opt[0x38] = 0xAE, d.opt[0x39] = 0x01;
    for (auto& k : d.knobs) k[0] = k[1] = k[3] = 128, k[4] = 40000;
  }
  m.glType = "gl.alloy";
  m.state = gen::base("gl.alloy");
  for (int t = 0; t < 49; ++t) gen::setp(m.state, t, d.params[t]);
  const uint8_t* o = d.opt;
  // Option block (object offsets relative to 0x200).
  // Option block (object offsets relative to 0x200), as the plugin's own
  // option handlers read and write them.
  gen::setp(m.state, mt::AlFltType, std::clamp(optI32(o, 0x00), 0, 3));
  gen::setp(m.state, mt::AlFltMode, std::clamp(optI32(o, 0x04), 0, 4));
  gen::setp(m.state, mt::AlFltFlat, optI32(o, 0x08) != 0);
  gen::setp(m.state, mt::AlRichness, std::clamp(optI32(o, 0x0C) - 5, 1, 11));
  gen::setp(m.state, mt::AlRvOn, o[0x10] != 0);
  gen::setp(m.state, mt::AlRvTempo, o[0x11] != 0);
  gen::setp(m.state, mt::AlQuality, std::clamp(optI32(o, 0x14), 0, 4));
  gen::setp(m.state, mt::AlModSmooth, o[0x18] != 0);
  gen::setp(m.state, mt::AlOneVoicePerKey, o[0x19] != 0);
  gen::setp(m.state, mt::AlVelVol, o[0x1A] != 0);
  gen::setp(m.state, mt::AlKeyPitch, o[0x1B] != 0);
  gen::setp(m.state, mt::AlKeySeed, o[0x1C] != 0);
  gen::setp(m.state, mt::AlVelAttTime, (o[0x1D] & 1) != 0);
  gen::setp(m.state, mt::AlVelAttLen, (o[0x1D] & 2) != 0);
  gen::setp(m.state, mt::AlDeclick, o[0x1E] != 0);
  gen::setp(m.state, mt::AlRelLink, o[0x1F] != 0);
  gen::setp(m.state, mt::AlPorta, o[0x20] != 0);
  gen::setp(m.state, mt::AlPortaVar, o[0x21] != 0);
  gen::setp(m.state, mt::AlPortaLimit, std::clamp(static_cast<int>(o[0x22]), 0, 5));
  gen::setp(m.state, mt::AlNatScale, o[0x23] != 0);
  {  // harmonic comb: one per timbre EQ curve (0x24..0x33); Alloy has one for the timbre
    int comb = 0;
    for (int k = 0; k < 4 && comb == 0; ++k) comb = std::clamp(optI32(o, 0x24 + 4 * k), 0, 7);
    gen::setp(m.state, mt::AlComb, comb);
  }
  gen::setp(m.state, mt::AlAltDecay, o[0x34] != 0);
  gen::setp(m.state, mt::AlAutoLoud, o[0x35] != 0);
  gen::setp(m.state, mt::AlNatWeight, o[0x36] != 0);
  gen::setp(m.state, mt::AlDlTempo, d.extra3[0] != 0);
  gen::setp(m.state, mt::AlDlOn, d.extra3[1] != 0);
  gen::setp(m.state, mt::AlDust, std::clamp(optI32(o, 0x38), 0, 1000));
  for (int k = 0; k < 3; ++k) gen::setp(m.state, mt::AlEqType1 + k, std::clamp(d.eqTypes[k], 0, 2));
  for (int a = 0; a < mt::kAlloyEnvArts; ++a)
    for (int k = 0; k < mt::AkCount; ++k) gen::setp(m.state, mt::alKnob(a, k), d.knobs[a][k]);
  m.state.data = mt::serializeAlloy(d.data);
  // Automation: FL index = tag (0..48) and 49 + art × 8 + knob for the articulator knobs.
  gen::ParamTable t;
  const PluginInfo* info = findPlugin("gl.alloy");
  for (int tag = 0; tag < 49 + 40 && info; ++tag) {
    const int gl = tag < 49 ? tag : mt::AlArtKnobs + (tag - 49);
    const auto& pd = info->params[static_cast<size_t>(gl)];
    t.lin(tag, gl, pd.min, pd.max);
    t.range(tag, pd.min, pd.max);
  }
  t.install(m);
  m.exact = false;
  (void)c;
  return true;
}

}  // namespace

// ================================================ Spline Scratcher (Wave Traveller)
// i32 1000, i32 header size (12 + path length), i32 1000005 (1000001: no
// sample), i32 current patch key, i32 n + n bytes sample path; then patches
// until the trailer {i32 3000, i32 0}: i32 2000, i32 (size of the patch in an
// older layout), i32 key, f32 length (beats), f32 × 3 per-patch speed /
// attack / release, f32 start, f32 end (0..1 of the sample), i32 flags, i32
// name length + name, i32 mute toggles + {i32 on, f32 x} each, then the
// position and volume splines: i32 count + 60-byte points {ptr, f32, f32 x,
// f32 y, f32, f32 × 4 handles, f32 tension, i32, i32 segment type, i32, i32
// locked}.
bool decodeScratchState(const uint8_t* p, size_t n, mt::SsData& out, size_t* consumed) {
  out = mt::SsData{};
  for (auto& k : out.keys) k = mt::ssDefaultPatch();
  Rd r{p, n};
  if (r.i32() != 1000) return false;
  const int hdr = r.i32();
  const int magic = r.i32();
  out.current = std::clamp(r.i32(), 0, mt::kSsKeys - 1);
  const int plen = r.i32();
  if (r.bad || (magic != 1000005 && magic != 1000001) || plen < 0 || plen > 4096 || hdr != 12 + plen || !r.need(static_cast<size_t>(plen)))
    return false;
  out.path = latin1(r.p + r.at, static_cast<size_t>(plen));
  r.at += static_cast<size_t>(plen);
  int patches = 0;
  while (!r.bad) {
    const int tag = r.i32();
    if (tag == 3000) {
      r.i32();
      break;
    }
    if (tag != 2000 || ++patches > 4 * mt::kSsKeys) return false;
    r.i32();
    const int key = r.i32();
    mt::SsPatch pt;
    pt.beats = r.f32();
    pt.speed = r.f32(), pt.attack = r.f32(), pt.release = r.f32();
    pt.start = r.f32(), pt.end = r.f32();
    r.i32();
    const int nl = r.i32();
    if (nl < 0 || nl > 1024 || !r.need(static_cast<size_t>(nl))) return false;
    pt.name = latin1(r.p + r.at, static_cast<size_t>(nl));
    r.at += static_cast<size_t>(nl);
    const int nm = r.i32();
    if (nm < 0 || nm > 4096) return false;
    for (int i = 0; i < nm && !r.bad; ++i) {
      const int on = r.i32();
      const float x = r.f32();
      pt.mutes.push_back({on != 0, std::clamp(x, 0.0f, 1.0f)});
    }
    for (auto* v : {&pt.pos, &pt.vol}) {
      const int cnt = r.i32();
      if (cnt < 0 || cnt > 4096 || !r.need(static_cast<size_t>(cnt) * 60)) return false;
      v->clear();
      for (int i = 0; i < cnt; ++i) {
        Rd q{r.p + r.at, 60};
        q.i32();
        q.f32();
        mt::SsPoint s;
        s.x = std::clamp(q.f32(), 0.0f, 1.0f);
        s.y = std::clamp(q.f32(), 0.0f, 1.0f);
        q.f32(), q.f32(), q.f32(), q.f32(), q.f32(), q.f32();
        s.tension = std::clamp(q.f32(), 0.0f, 1.0f);
        q.i32();
        const int type = q.i32();
        q.i32();
        s.locked = q.i32() != 0;
        s.curve = static_cast<uint8_t>(type == 2 ? mt::SsHalfCosine : type == 3 ? mt::SsBezier : type == 4 ? mt::SsTension
                                       : type == 5 ? mt::SsQuarterSine : type == 6 ? mt::SsQuarterCosine : mt::SsLinear);
        v->push_back(s);
        r.at += 60;
      }
      std::stable_sort(v->begin(), v->end(), [](const mt::SsPoint& a, const mt::SsPoint& b) { return a.x < b.x; });
    }
    if (r.bad) return false;
    if (!std::isfinite(pt.beats) || !(pt.beats > 0)) pt.beats = 4;
    for (float* f : {&pt.speed, &pt.attack, &pt.release})
      if (!std::isfinite(*f)) *f = 1;
    if (!std::isfinite(pt.start)) pt.start = 0;
    if (!std::isfinite(pt.end)) pt.end = 1;
    pt.start = std::clamp(pt.start, 0.0f, 1.0f), pt.end = std::clamp(pt.end, 0.0f, 1.0f);
    if (pt.pos.empty()) pt.pos = mt::ssDefaultPatch().pos;
    if (pt.vol.empty()) pt.vol = mt::ssDefaultPatch().vol;
    if (key < 0 || key >= mt::kSsKeys) continue;
    // Only patches that differ from the untouched default are kept as edited.
    const mt::SsPatch def = mt::ssDefaultPatch();
    auto samePts = [](const std::vector<mt::SsPoint>& a, const std::vector<mt::SsPoint>& b) {
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); ++i)
        if (std::fabs(a[i].x - b[i].x) > 1e-5f || std::fabs(a[i].y - b[i].y) > 1e-5f) return false;
      return true;
    };
    pt.used = !(samePts(pt.pos, def.pos) && samePts(pt.vol, def.vol) && pt.mutes.size() <= 1 && pt.name.empty() &&
                std::fabs(pt.beats - 4) < 1e-4f && pt.start == 0 && pt.end == 1 && pt.speed == 1 && pt.attack == 1 && pt.release == 1);
    out.keys[static_cast<size_t>(key)] = pt;
  }
  if (consumed) *consumed = r.at;
  return !r.bad && patches > 0;
}

namespace {

namespace fs = std::filesystem;
bool fileExists(const std::string& p) {
  std::error_code ec;
  return !p.empty() && fs::is_regular_file(p, ec);
}
// FL sample path → local file (factory data tags, other machines' FL folders,
// samples moved into the factory "Legacy" pack folder).
std::string resolveSample(const Context& c, std::string raw) {
  if (raw.empty()) return raw;
  for (char& ch : raw)
    if (ch == '\\') ch = '/';
  for (const char* tag : {"%FLStudioFactoryData%", "%FLStudioData%"}) {
    const std::string t = tag;
    if (raw.rfind(t, 0) == 0 && !c.factoryData.empty()) {
      const std::string q = c.factoryData + (t == "%FLStudioData%" ? "/Data" : "") + raw.substr(t.size());
      if (fileExists(q)) return q;
    }
  }
  std::string p = c.resolvePath(raw);
  if (fileExists(p) || c.factoryData.empty()) return p;
  const size_t at = raw.find("/Data/Patches/");
  if (at != std::string::npos) {
    const std::string tail = raw.substr(at + 14);
    for (const std::string& q : {c.factoryData + "/Data/Patches/" + tail,
                                 tail.rfind("Packs/", 0) == 0 ? c.factoryData + "/Data/Patches/Packs/Legacy/" + tail.substr(6) : std::string()})
      if (fileExists(q)) return q;
  }
  return p;
}

bool decodeScratch(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Bytes b = gen::stateOf(evs);
  mt::SsData d;
  size_t used = 0;
  if (!b.p || !decodeScratchState(b.p, b.n, d, &used)) return false;
  d.path = resolveSample(c, d.path);
  if (d.path.empty())
    for (const Event& e : evs)
      if (e.id == 196) d.path = resolveSample(c, e.text());
  m.glType = "gl.splinescratch";
  m.state = gen::base("gl.splinescratch");
  m.state.data = mt::serializeScratchSpl(d);
  m.exact = false;
  return true;
}

}  // namespace

// ================================================ Spectra Morph (Morphine)
// Optional "SSLF" + u32 size (plugin state; the .mrp preset files start at
// the next field), "Mor0" and "MPR0" (stored byte-reversed), then four corner
// programs. Blocks are {u32 size, tag, content} with the size counted from the
// tag. Program: MVC0 { MZH0 { i32 zones, per zone: i32 root key, f32 gain,
// MSP0 { i32 frames, per frame: i32 hold (analysis steps), f32 level, MBP0
// { harmonic amplitudes, 8 per group: u8 mask + one f32 per set bit } },
// then u32 1060-byte loop / curve record: i32 loop start, i32 loop end (steps),
// i32, i32, f32 × 3, i32, f32[256] } }, then a 28-byte program record }. After
// the programs: tagged global blocks (morph path "MML0", envelopes "MMD0" with
// "MEG0" children), the master parameter record (u32 size + floats) and the
// preset name buffer.
namespace {
uint32_t tag4(const char* s) {  // reversed storage of a 4-char id
  return static_cast<uint32_t>(static_cast<uint8_t>(s[3])) | (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24);
}
// Block at r.at: returns its tag and sets `end`; content starts after the tag.
bool openBlock(Rd& r, uint32_t want, size_t& end) {
  const int32_t size = r.i32();
  if (r.bad || size < 4 || static_cast<size_t>(size) > r.n - r.at) return r.bad = true, false;
  end = r.at + static_cast<size_t>(size);
  const uint32_t t = static_cast<uint32_t>(r.i32());
  if (t != want) return r.bad = true, false;
  return true;
}
}  // namespace

bool decodeMorphState(const uint8_t* p, size_t n, mt::SmData& out, size_t* consumed) {
  out = mt::SmData{};
  Rd r{p, n};
  if (n >= 8 && std::memcmp(p, "SSLF", 4) == 0) r.at = 8;
  if (r.i32() != static_cast<int32_t>(tag4("Mor0")) || r.i32() != static_cast<int32_t>(tag4("MPR0"))) return false;
  for (int prog = 0; prog < mt::kSmPrograms; ++prog) {
    size_t vEnd = 0, zEnd = 0;
    if (!openBlock(r, tag4("MVC0"), vEnd) || !openBlock(r, tag4("MZH0"), zEnd)) return false;
    const int zones = r.i32();
    if (zones < 0 || zones > 64) return false;
    for (int zi = 0; zi < zones && !r.bad; ++zi) {
      mt::SmZone z;
      z.key = std::clamp(r.i32(), 0, 127);
      z.gain = std::clamp(r.f32(), 0.0f, 16.0f);
      size_t sEnd = 0;
      if (!openBlock(r, tag4("MSP0"), sEnd)) return false;
      const int frames = r.i32();
      if (frames < 0 || frames > 100000) return false;
      bool any = false;
      for (int f = 0; f < frames && !r.bad; ++f) {
        mt::SmFrame fr;
        fr.hold = std::max(0, r.i32());
        fr.level = std::clamp(r.f32(), 0.0f, 16.0f);
        size_t bEnd = 0;
        if (!openBlock(r, tag4("MBP0"), bEnd)) return false;
        while (r.at < bEnd && !r.bad) {
          const uint8_t mask = r.u8();
          for (int k = 0; k < 8; ++k) {
            const float a = (mask & (1 << k)) ? r.f32() : 0.0f;
            if (fr.amp.size() < static_cast<size_t>(mt::kSmMaxPartials)) fr.amp.push_back(std::clamp(a, 0.0f, 16.0f));
          }
        }
        if (r.at != bEnd) return false;
        while (!fr.amp.empty() && fr.amp.back() <= 0.0f) fr.amp.pop_back();
        any = any || !fr.amp.empty();
        z.frames.push_back(std::move(fr));
      }
      // Loop / curve record.
      if (r.need(12)) {
        const size_t recAt = r.at;
        r.i32();
        z.loopStart = r.i32(), z.loopEnd = r.i32();
        r.at = recAt;
      }
      r.at = sEnd;
      if (any) out.prog[static_cast<size_t>(prog)].zones.push_back(std::move(z));
    }
    if (r.bad || r.at != zEnd) return false;
    r.at = vEnd;  // program record (level, pan, …)
  }
  // Global blocks, master parameters, preset name.
  while (!r.bad && r.at + 8 <= n) {
    const int32_t size = r.peek(0);
    const uint32_t t = static_cast<uint32_t>(r.peek(4));
    // (J-34: the global block ids are "MML0" / "MMD0" as stored; the former
    // "MLM0" / "MDM0" never matched, so the preset name was read from inside
    // the envelope block.)
    const bool tagged = t == tag4("MML0") || t == tag4("MMD0") || t == tag4("MEG0");
    if (!tagged) break;
    if (size < 4 || static_cast<size_t>(size) > n - r.at - 4) return false;
    r.at += 4 + static_cast<size_t>(size);
  }
  if (r.at + 4 <= n) {
    // (J-39: the master record's size counts its own size word — 1476 =
    // 4 + 368 floats; read as "after the word", the record was taken for the name)
    const int32_t size = r.peek(0);
    if (size >= 4 && static_cast<size_t>(size) <= n - r.at) r.at += static_cast<size_t>(size);
  }
  if (r.at + 4 <= n) {
    r.i32();
    const size_t nameAt = r.at;
    out.name = latin1(p + nameAt, n - nameAt);
    r.at = n;
  }
  if (consumed) *consumed = r.at;
  return !r.bad;
}

namespace {
bool decodeMorph(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  (void)c;
  Bytes b = gen::stateOf(evs);
  mt::SmData d;
  if (!b.p || !decodeMorphState(b.p, b.n, d, nullptr)) return false;
  m.glType = "gl.spectramorph";
  m.state = gen::base("gl.spectramorph");
  m.state.data = mt::serializeMorph(d);
  if (m.state.data == mt::serializeMorph(mt::SmData{})) m.state.data.clear();  // J-39: no programs = Spectra Morph's default (no data)
  m.exact = false;  // master parameters / morph path / envelopes stay at the engine defaults
  return true;
}
}  // namespace

namespace {

// ================================================== Grain Scanner (Granulizer)
// u32 version (1..7), then one i32 per control tag 0..11 (v1 stops after 9,
// v2 after 10 sample start), i32 transient source (v4+), i32 key mode (v5+),
// i32 tempo-lock flag (low byte, v7+); v7 then carries the plugin's own
// envelope panel: five envelope / LFO records (5 × 68 bytes, the channel record
// layout) and a levels record (24 bytes). The sample is the channel's (event 196).
int granulizerTags(int ver) { return ver <= 1 ? 10 : ver <= 3 ? 11 : ver == 4 ? 13 : ver <= 6 ? 14 : 15; }

bool decodeGrains(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Bytes b = gen::stateOf(evs);
  if (!b.p || b.n < 44) return false;
  const int ver = b.i32(0);
  if (ver < 1 || ver > 7) return false;
  const int tags = std::min(granulizerTags(ver), static_cast<int>((b.n - 4) / 4));
  m.glType = "gl.grainscan";
  m.state = gen::base("gl.grainscan");
  for (int t = 0; t < tags; ++t) gen::setp(m.state, t, t == mt::GsTempoLock ? (b.i32(4 + static_cast<size_t>(t) * 4) & 0xFF) != 0 : b.i32(4 + static_cast<size_t>(t) * 4));
  // Volume envelope: the channel's record (event 218), else the copy in the state.
  const size_t envAt = 4 + static_cast<size_t>(tags) * 4;
  gen::ChanSettings cs = gen::channelSettings(evs, ver >= 7 ? b.sub(envAt, 5 * 68) : Bytes(), ver >= 7 ? b.sub(envAt + 5 * 68, 24) : Bytes());
  const gen::ChanEnv& e = cs.env[1];
  if (cs.haveEnv && e.enabled) {
    gen::setp(m.state, mt::GsEnvOn, 1);
    gen::setp(m.state, mt::GsEnvAttack, gen::envSeconds(e.attack));
    gen::setp(m.state, mt::GsEnvHold, gen::envSeconds(e.hold));
    gen::setp(m.state, mt::GsEnvDecay, gen::envSeconds(e.decay));
    gen::setp(m.state, mt::GsEnvSustain, gen::envSustain(e.sustain));
    gen::setp(m.state, mt::GsEnvRelease, gen::envSeconds(e.release));
  }
  for (const Event& ev : evs)
    if (ev.id == 196) m.state.data = resolveSample(c, ev.text());
  gen::ParamTable t;
  if (const PluginInfo* info = findPlugin("gl.grainscan"))
    for (int tag = 0; tag < 12; ++tag) {
      const auto& pd = info->params[static_cast<size_t>(tag)];
      t.lin(tag, tag, pd.min, pd.max);
      t.range(tag, pd.min, pd.max);
    }
  t.install(m);
  m.exact = false;  // every control maps 1:1, but the grain time unit (attack / hold) is inferred, not measured
  return true;
}

}  // namespace

void registerMetal(std::map<std::string, PluginDecoder>& d) {
  set(d, "Ogun", [](Context& c, const std::vector<Event>& e, PluginMapping& m) { return decodeAlloy(c, e, m, false); });
  set(d, "Autogun", [](Context& c, const std::vector<Event>& e, PluginMapping& m) { return decodeAlloy(c, e, m, true); });
  set(d, "Wave Traveller", decodeScratch);
  set(d, "Morphine", decodeMorph);
  set(d, "Fruity Granulizer", decodeGrains);
  set(d, "Fruity granulizer", decodeGrains);
}

}  // namespace gl::flp::j22
