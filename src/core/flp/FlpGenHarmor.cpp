// Gridline — FLP import: the two-part additive / resynthesis synth → Resynth ("gl.harmor").
//
// State (worked out from the 325 shipped presets, the render test and every
// demo-project channel — 424 states, all walk to their exact end):
//   u32 version (11, 12, 16, 17, or 0x120003 / 0x130003 = 18 / 19 in the high word)
//   u32 section mask (bit 0 part A, bit 1 part B, bit 2 global)
//   per part: i32 size, then the part:
//     i32 params[386] (385 before v16; a 0 is inserted at 376): 0..94 control
//       tags, 343..385 articulator output smoothing
//     i32 unit order[12]
//     part record: 13 i32 (blend, declick, ramp, prism mode, level→prism,
//       clip mode, sub mode, pitch mode, resynthesis mode, denoise, 200 +
//       phaser type, view scale, f32 global EQ mix), 4 i32 (filter shapes,
//       100 + resonance types), u8 enabled, u8 mid/side, u8 pluck→blur,
//       i32 auto-gain mode (v11+)
//     43 articulators (42 before v16): i32 3, f32[3] header (h0 = 0: bipolar),
//       envelope, LFO, 9 mapping curves. Curve: i32 kind (1 env, 2 LFO, 3 map),
//       i32 format, i32 n, n × {f64 dx, f64 y, f32 tension, u8 type, u8 arp,
//       u8, i8}, i32 flags (1 tempo, 2 global), enabled, decay, loop start,
//       sustain points; envelope + 4 i32 (A/D/R scales, sustain offset /128);
//       LFO + 4 i32 (speed, tension, skew, width) + u32 phase
//     26 shaping curves, the image block (version, source, loop, crossfade,
//       loop start / end, ?, cols, rows, 3 planes of cols × rows floats when
//       embedded), the image path (u8 / 0xFF + i32 length)
//   global: i32 params[53] (tags 10000.., 20000..), i32 fx order[6], 14 i32
//     (… mono, voice random, quality, GUI, distortion, compression), 5 u8,
//     info text (ANSI before v18, UTF-16 after), u8 flags (v19)
// The DSP is Gridline's own, so the mapping is approximate (exact = false).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "../../packs/models/harmor.h"
#include "FlpGen.h"

namespace gl::flp::gen {

namespace {

namespace hm = gl::packs::models::hm;

double clampd(double v, double lo, double hi) { return std::clamp(v, lo, hi); }

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
  int32_t strLen() {
    int32_t len = u8();
    if (len == 0xFF) len = i32();
    if (len < 0 || len > (1 << 24)) bad = true;
    return bad ? 0 : len;
  }
  std::string astr() {
    const int32_t len = strLen();
    std::string s;
    if (need(static_cast<size_t>(len))) s.assign(reinterpret_cast<const char*>(p + at), static_cast<size_t>(len)), at += static_cast<size_t>(len);
    return s;
  }
};

struct RawCurve {
  hm::Curve c;
  int flags = 0;
};

// One curve body; `kind` must match (1 envelope, 2 LFO, 3 map / shape).
bool readCurve(Rd& r, int kind, RawCurve& out) {
  const int k = r.i32(), fmt = r.i32(), n = r.i32();
  if (r.bad || k != kind || fmt < 0 || fmt > 3 || n < 0 || n > 100000) return r.bad = true, false;
  out.c.pts.clear();
  for (int i = 0; i < n && !r.bad; ++i) {
    hm::Pt p;
    double x, y;
    if (fmt >= 2) x = r.f64(), y = r.f64();
    else x = kind == 6 ? r.f64() : r.f32(), y = r.f32();
    // FL's segment tension bends the opposite way to Resynth's: positive FL
    // tension starts fast (1364 of the 1528 bent falling envelope segments in
    // the shipped presets use it), positive Resynth tension starts slow — so
    // the sign flips (as for Sytrus, J-3b).
    p.tension = -std::clamp(r.f32(), -1.0f, 1.0f);
    if (fmt >= 1) {
      p.kind = static_cast<uint8_t>(std::min<int>(r.u8(), 12));
      r.skip(3);
    }
    p.dt = static_cast<float>(std::max(0.0, x));
    p.y = static_cast<float>(clampd(y, -4, 4));
    if (out.c.pts.size() < 4096) out.c.pts.push_back(p);
  }
  out.flags = r.i32();
  const int enabled = r.i32(), decay = r.i32(), loop = r.i32(), sus = r.i32();
  const int np = static_cast<int>(out.c.pts.size());
  out.c.enabled = enabled != 0;
  out.c.tempo = (out.flags & 1) != 0;
  out.c.global = (out.flags & 2) != 0;
  out.c.decay = decay >= 0 && decay < np ? decay : -1;
  out.c.loopStart = loop >= 0 && loop < np ? loop : -1;
  out.c.sustain = sus >= 0 && sus < np ? sus : -1;
  return !r.bad;
}

bool flatAt(const hm::Curve& c, float v) {
  for (const auto& p : c.pts)
    if (std::fabs(p.y - v) > 1e-4f || p.kind == 6) return false;
  return true;
}

struct RawPart {
  int params[386] = {};
  int blend = 0, prismMul = 0, clipMode = 0, subMode = 0, phType = 0, fType[2] = {4, 18}, rType[2] = {0, 0};
  float eqMix = 1;
  bool enabled = false;
  hm::PartData data;
  std::string imagePath;
  int imgSource = 0, imgLoop = 1;
};

bool readPart(Rd& r, int ver, RawPart& out) {
  const int np = ver >= 16 ? 386 : 385;
  for (int i = 0; i < np; ++i) out.params[i] = r.i32();
  if (ver < 16) {
    for (int i = 385; i > 376; --i) out.params[i] = out.params[i - 1];
    out.params[376] = 0;
  }
  for (int i = 0; i < 12; ++i) r.i32();  // unit order
  int rec[13];
  for (int& v : rec) v = r.i32();
  out.blend = rec[0], out.prismMul = rec[3], out.clipMode = rec[5], out.subMode = rec[6];
  // J-36: offsets in 64 bits (a damaged state overflowed int: fuzzer, flp target).
  auto off = [](int32_t v, int d) { return static_cast<int>(std::clamp<int64_t>(static_cast<int64_t>(v) - d, -1000000, 1000000)); };
  out.phType = off(rec[10], 200);
  {
    float f;
    std::memcpy(&f, &rec[12], 4);
    out.eqMix = std::isfinite(f) ? f : 1.0f;
  }
  out.fType[0] = r.i32(), out.fType[1] = r.i32();
  out.rType[0] = off(r.i32(), 100);
  out.rType[1] = off(r.i32(), 100);
  out.enabled = r.u8() != 0;
  r.u8(), r.u8();
  if (ver >= 11) r.i32();
  for (int a = 0; a < hm::kArts && !r.bad; ++a) {
    hm::ArtData& A = out.data.arts[static_cast<size_t>(a)];
    A.bipolar = hm::artBipolarDefault(a);
    if (ver < 16 && a == hm::ArGAttack) continue;
    if (r.i32() != 3) return r.bad = true, false;
    const float h0 = r.f32();
    r.f32(), r.f32();
    A.bipolar = h0 < 0.5f;
    RawCurve env, lfo;
    if (!readCurve(r, 1, env)) return false;
    A.att = r.i32() / 128.0f, A.dec = r.i32() / 128.0f, A.sus = r.i32() / 128.0f, A.rel = r.i32() / 128.0f;
    A.att = std::clamp(A.att, 0.0f, 4.0f), A.dec = std::clamp(A.dec, 0.0f, 4.0f), A.sus = std::clamp(A.sus, -1.0f, 1.0f),
    A.rel = std::clamp(A.rel, 0.0f, 4.0f);
    A.env = env.c;
    if (!readCurve(r, 2, lfo)) return false;
    const int speed = r.i32();
    r.i32();
    A.lfoSkew = std::clamp(r.i32() / 128.0f, -1.0f, 1.0f);
    A.lfoPw = std::clamp(r.i32() / 128.0f, -1.0f, 1.0f);
    if (lfo.c.pts.size() || true) {
      if (std::max(0, 3) == 3) {
      }
    }
    const uint32_t ph = static_cast<uint32_t>(r.i32());  // fmt ≥ 3 (always in the corpus)
    A.lfoPhase = static_cast<float>(ph / 4294967296.0);
    A.lfoSpeed = static_cast<float>(clampd((std::pow(10001.0, clampd(speed, 0, 65536) / 65536.0) - 1.0) / 625.0, 0, 100));
    A.lfo = lfo.c;
    const float neutral = A.bipolar ? 0.5f : 1.0f;
    for (int m = 0; m < hm::kMaps; ++m) {
      RawCurve mc;
      if (!readCurve(r, 3, mc)) return false;
      if (!flatAt(mc.c, neutral)) {
        mc.c.enabled = true;
        A.maps[static_cast<size_t>(m)] = mc.c;
      }
    }
  }
  for (int s = 0; s < hm::kShapes && !r.bad; ++s) {
    RawCurve sc;
    if (!readCurve(r, 3, sc)) return false;
    if (!flatAt(sc.c, hm::neutralShape(s))) out.data.shapes[static_cast<size_t>(s)] = sc.c;
  }
  // Image.
  if (r.i32() > 0) return r.bad = true, false;
  out.imgSource = r.i32();
  out.imgLoop = r.i32();
  r.f32();
  r.f64(), r.f64(), r.f64();
  const int cols = r.i32(), rows = r.i32();
  hm::ImageData& im = out.data.image;
  if (out.imgSource == 0) {
    std::vector<float> plane[3];
    for (int k = 0; k < 3 && !r.bad; ++k) {
      const int cnt = r.i32();
      if (cnt != 0 && (cols <= 0 || rows <= 0 || static_cast<int64_t>(cnt) != static_cast<int64_t>(cols) * rows)) return r.bad = true, false;
      if (cnt > 0 && r.need(static_cast<size_t>(cnt) * 4)) {
        plane[k].resize(static_cast<size_t>(cnt));
        for (int i = 0; i < cnt; ++i) plane[k][static_cast<size_t>(i)] = r.f32();
      }
    }
    if ((!plane[0].empty() || !plane[1].empty()) && cols <= 20000 && rows <= 1024) {
      im.cols = cols, im.rows = rows, im.loop = std::clamp(out.imgLoop, 0, 2);
      im.colSec = 0.05f;
      im.gain.resize(static_cast<size_t>(cols) * static_cast<size_t>(rows), 255);
      if (!plane[0].empty())
        for (size_t i = 0; i < im.gain.size(); ++i) im.gain[i] = static_cast<uint8_t>(std::clamp(std::lround(plane[0][i]), 0L, 255L));
      if (!plane[1].empty()) {
        im.freq.resize(im.gain.size());
        for (size_t i = 0; i < im.freq.size(); ++i) im.freq[i] = static_cast<int8_t>(std::clamp(std::lround(plane[1][i] * 64.0f), -127L, 127L));
      }
    }
  }
  out.imagePath = r.astr();
  return !r.bad;
}

double l3(double y) { return 0.5 * (std::pow(3.0, clampd(y, 0, 2)) - 1.0); }
double l11(double y) { return 0.1 * (std::pow(11.0, clampd(y, 0, 1)) - 1.0); }
double fxHz(double v) { return clampd(16.3516 * std::pow(2.0, v / 100.0), 16, 20000); }

}  // namespace

bool decodeHarmorFull(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  if (!b.has(0, 8)) return false;
  Rd r{b.p, b.n};
  const uint32_t raw = static_cast<uint32_t>(r.i32());
  const int ver = raw >= 0x10000 ? static_cast<int>(raw >> 16) : static_cast<int>(raw);
  if (raw == 0xFFFF || raw > 0x130003 || ver < 1) return false;
  const int sections = r.i32();
  RawPart parts[2];
  int nParts = 0;
  const int targets = (sections & 3) == 3 ? 2 : (sections & 3) ? 1 : 0;
  for (int k = 0; k < targets; ++k) {
    const int size = r.i32();
    if (r.bad || size < 0 || !r.need(static_cast<size_t>(size))) return false;
    Rd pr{b.p, r.at + static_cast<size_t>(size)};
    pr.at = r.at;
    if (!readPart(pr, ver, parts[k]) || pr.at != r.at + static_cast<size_t>(size)) return false;
    r.at += static_cast<size_t>(size);
    ++nParts;
  }
  int g[53];
  for (int& v : g) v = 0;
  const int gDef[][2] = {{0, 256}, {1, 256}, {2, 256}, {10, 500}, {11, 1000}, {12, 1000}, {13, 1}, {15, 1000}};
  for (const auto& d : gDef) g[d[0]] = d[1];
  int monoSel = 0, distSel = 0, compSel = 0;
  if (sections & 4) {
    for (int& v : g) v = r.i32();
    for (int i = 0; i < 6; ++i) r.i32();
    int rec[14];
    for (int& v : rec) v = r.i32();
    monoSel = rec[2], distSel = rec[12], compSel = rec[13];
    for (int i = 0; i < 5; ++i) r.u8();
    if (ver < 18) {
      const int n = r.i32();
      if (n < 0 || n > (1 << 24)) return false;
      r.skip(static_cast<size_t>(n));
    } else {
      const int n = r.strLen();
      r.skip(static_cast<size_t>(n) * 2);
    }
    if (ver > 18) r.u8();
  }
  if (r.bad || nParts == 0) return false;
  auto G = [&](int tag) { return tag >= 20000 ? g[17 + tag - 20000] : g[tag - 10000]; };

  namespace q = hm;
  PluginState s = base("gl.harmor");
  hm::PatchData data;
  int images = 0, external = 0, missing = 0;
  for (int p = 0; p < nParts; ++p) {
    const RawPart& R = parts[p];
    const int* t = R.params;
    auto set = [&](int f, double v) { setp(s, q::pf(p, f), v); };
    auto fset = [&](int k, int f, double v) { setp(s, q::flt(p, k, f), v); };
    set(q::PfEnabled, R.enabled ? 1 : 0);
    set(q::PfPhase, clampd(t[0] / 1000.0, 0, 1));
    set(q::PfPhaseRand, clampd((t[1] < 0 ? -1 : 1) * (t[1] / 1000.0) * (t[1] / 1000.0), -1, 1));
    set(q::PfPredelay, t[2] < 0 ? clampd((std::pow(2.0, -t[2] / 48.0) - 1.0) / 255.0, 0, 1) : 0.0);
    set(q::PfRelMul, clampd(t[3] / 1000.0, 0, 1));
    set(q::PfTimbreMix, clampd(t[4] / 1000.0, 0, 1));
    set(q::PfBlend, std::clamp(R.blend, 0, 5));
    set(q::PfSub1, clampd(t[5] / 1024.0, 0, 2)), set(q::PfSub3, clampd(t[6] / 1024.0, 0, 2)), set(q::PfSub4, clampd(t[7] / 1024.0, 0, 2));
    set(q::PfSubMode, std::clamp(R.subMode, 0, 1));
    set(q::PfProtect, t[8] >= 1024 ? 1.0 : l11(t[8] / 1024.0));
    set(q::PfClip, clampd(t[9] / 1024.0, 0, 1));
    set(q::PfClipMode, std::clamp(R.clipMode, 0, 4));
    set(q::PfFxMix, clampd(t[10] / 1000.0, -1, 1));
    set(q::PfVolume, clampd(t[11] / 1000.0, -2, 2));
    set(q::PfEnvVolume, clampd(l3(t[12] / 1000.0), 0, 4));
    set(q::PfAutoGain, clampd((t[13] / 1000.0) * (t[13] / 1000.0), 0, 1));
    set(q::PfVelAttTime, t[14] ? 1 : 0), set(q::PfVelAttLen, t[15] ? 1 : 0), set(q::PfVelRelLen, t[16] ? 1 : 0);
    set(q::PfTremolo, clampd(t[17] / 1000.0, 0, 1));
    set(q::PfTremHz, clampd(std::pow(2.0, 6.0 - t[18] / 1000.0), 0.0156, 64));
    set(q::PfTremGap, clampd(t[19] / 1000.0, 0, 1));
    set(q::PfBlurMix, clampd(t[20] / 1000.0, 0, 1));
    set(q::PfTBlurAtt, clampd((std::pow(2.0, t[21] / 100.0) - 1.0) / 1000.0, 0, 10));
    set(q::PfTBlurDec, clampd((std::pow(2.0, t[22] / 100.0) - 1.0) / 1000.0, 0, 10));
    set(q::PfHBlur, clampd(1.0 + 15.9375 * (std::pow(17.0, t[23] / 10000.0) - 1.0), 1, 256));
    set(q::PfHBlurLo, clampd(t[24] / 1000.0, 0, 1)), set(q::PfHBlurHi, clampd(t[25] / 1000.0, 0, 1));
    set(q::PfPrism, clampd(t[26] / 1000.0, -1, 1));
    set(q::PfPrismMul, R.prismMul ? 1 : 0);
    set(q::PfHzMix, clampd(t[27] / 1000.0, 0, 1)), set(q::PfHzWidth, clampd(t[28] / 1000.0, 0, 1));
    set(q::PfHzStrength, clampd(t[29] / 1024.0, 0, 1));
    set(q::PfHzShift, std::clamp(t[30], -2, 7)), set(q::PfHzShiftStep, std::clamp(t[31], -7, 8));
    set(q::PfHzGap, std::clamp(t[32], -1, 8)), set(q::PfHzGapStep, std::clamp(t[33], -7, 8));
    set(q::PfUniOrder, std::clamp(t[34], 1, 9));
    set(q::PfUniType, std::clamp(t[35], 0, 4));
    set(q::PfUniAlt, t[36] ? 1 : 0);
    set(q::PfUniPan, t[37] <= 192 ? clampd(t[37] / 192.0, 0, 1) : clampd(1.0 + (t[37] - 192) * 0.046875, 0, 4));
    set(q::PfUniPitch, clampd(1.875 * (std::exp(t[38] / 256.0 * 3.71357) - 1.0), 0, 75));
    set(q::PfUniPhase, clampd(t[39] / 1000.0, -1, 1));
    set(q::PfFreqMul, clampd(t[40] / 10000.0, 1, 16)), set(q::PfFreqDiv, std::clamp(t[41], 1, 9));
    set(q::PfDetMul, clampd(t[42] / 10000.0, 1, 16)), set(q::PfDetDiv, std::clamp(t[43], 1, 9));
    set(q::PfPitchArt, clampd(t[44], -6000, 6000));
    set(q::PfVibrato, clampd(t[45] * 0.1, 0, 100));
    set(q::PfVibHz, clampd(std::pow(2.0, 3.0 - t[46] / 1000.0), 0.125, 8));
    set(q::PfPortaLog, t[47] ? 1 : 0), set(q::PfPortaVar, t[48] ? 1 : 0);
    set(q::PfPortaTime, t[49] < 0 ? clampd((std::pow(2.0, -t[49] / 64.0) - 1.0) / 42.0, 0, 1.5) : clampd(t[49] / 192.0 * 0.5, 0, 1.5));
    set(q::PfPortaLimit, clampd(t[50], 0, 1200));
    for (int k = 0; k < 2; ++k) {
      const int o = k * 12;
      int ft = R.fType[k];
      ft = ft >= 17 ? ft + 1 : ft;  // custom shapes after "Off"
      fset(k, q::PfF1Type, std::clamp(ft, 0, q::kFltShapes - 1));
      fset(k, q::PfF1ResType, std::clamp(R.rType[k], 0, q::kResShapes - 1));
      fset(k, q::PfF1Adapt, t[51 + o] ? 1 : 0);
      fset(k, q::PfF1Cut, clampd(t[52 + o] / 8192.0, 0, 1));
      fset(k, q::PfF1Width, clampd(t[54 + o] / 1000.0, 0, 1));
      fset(k, q::PfF1Key, clampd(t[55 + o] / 1000.0, -2, 2));
      fset(k, q::PfF1Env, clampd(t[56 + o] / 1000.0, -1, 1));
      fset(k, q::PfF1Res, clampd(t[57 + o] / 1000.0, 0, 1));
      fset(k, q::PfF1ResWidth, clampd(t[59 + o] / 1000.0, 0, 1));
      fset(k, q::PfF1ResOfs, clampd(t[60 + o], -2400, 2400));
      fset(k, q::PfF1ResAdapt, t[61 + o] ? 1 : 0);
      fset(k, q::PfF1Osc, k == 0 ? clampd(std::pow(t[62] / 1024.0, 2.0) / 4.0, 0, 1) : 0.0);
    }
    set(q::PfFMix, clampd(t[74] / 1000.0, -1, 1));
    set(q::PfPluck, clampd(t[75] / 1000.0, 0, 1));
    set(q::PfPhType, std::clamp(R.phType, 0, 10));
    set(q::PfPhMix, clampd(t[76] / 1000.0, 0, 1));
    set(q::PfPhScale, std::clamp(t[77], 0, 2));
    set(q::PfPhWidth, clampd(t[78] / 10000.0, 0, 1));
    set(q::PfPhOfs, clampd(t[79] / 1000.0, 0, 1));
    set(q::PfPhMotion, clampd(t[80] / 1000.0, -8, 8));
    set(q::PfPhKey, clampd(t[81] / 1000.0, -2, 2));
    set(q::PfImgTimeSmooth, clampd((t[82] / 1000.0) * (t[82] / 1000.0) * 2.0, 0, 2));
    set(q::PfImgTime, clampd(t[83] / 1048576.0, 0, 1));
    set(q::PfImgSpeed, clampd(t[84] / 10000.0, -2, 2));
    set(q::PfImgSpeedOct, clampd(t[85] / 10000.0, -6, 6));
    set(q::PfImgSharp, clampd(t[86] / 1000.0, -1, 1));
    set(q::PfImgGainScale, clampd(t[88] / 1000.0, 0, 1));
    set(q::PfImgGainMix, clampd(t[89] / 1000.0, 0, 1));
    set(q::PfImgFreqMode, std::clamp(t[90], 0, 3));
    set(q::PfImgFreqScale, clampd(t[92] / 1000.0, -2, 2));
    set(q::PfImgFormant, clampd(t[93] / 100.0, -6, 6));
    set(q::PfImgFormantMix, clampd(t[94] / 1000.0, 0, 1));
    set(q::PfImgLoop, std::clamp(R.imgLoop, 0, 2));
    hm::PartData pd = R.data;
    for (int a = 0; a < q::kArts; ++a) {
      const int v = t[343 + a];
      pd.arts[static_cast<size_t>(a)].smooth = static_cast<float>(clampd((std::pow(2.0, v * 0.009) - 1.0) / 0.0511 / 1000.0, 0, 10));
    }
    if (!pd.image.gain.empty()) ++images;
    if (R.imgSource == 1 && !R.imagePath.empty()) {
      ++external;
      std::string path = c.resolvePath(R.imagePath);
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) ++missing;
      pd.image.source = path;
      pd.image.loop = std::clamp(R.imgLoop, 0, 2);
    }
    data.part[static_cast<size_t>(p)] = pd;
  }
  // Global.
  setp(s, q::GmModX, clampd(G(10000) / 512.0, 0, 1)), setp(s, q::GmModY, clampd(G(10001) / 512.0, 0, 1)),
      setp(s, q::GmModZ, clampd(G(10002) / 512.0, 0, 1));
  setp(s, q::GmLegatoLimit, clampd(G(10003), 0, 2400));
  setp(s, q::GmLegato, G(10004) ? 1 : 0), setp(s, q::GmPorta, G(10005) ? 1 : 0), setp(s, q::GmVelPorta, G(10006) ? 1 : 0);
  setp(s, q::GmStrumMode, std::clamp(G(10007), 0, 3));
  setp(s, q::GmStrumTime, clampd(G(10008) / 1000.0, 0, 1)), setp(s, q::GmStrumTension, clampd(G(10009) / 1000.0, -1, 1));
  setp(s, q::GmPartMix, clampd(G(10010) / 1000.0, 0, 1));
  setp(s, q::GmVolume, clampd(l3(G(10011) / 1000.0), 0, 4)), setp(s, q::GmPostVolume, clampd(l3(G(10012) / 1000.0), 0, 4));
  setp(s, q::GmVelVol, G(10013) ? 1 : 0);
  setp(s, q::GmPitch, clampd(G(10014), -2400, 2400));
  setp(s, q::GmLfoAmt, clampd(G(10015) / 1000.0, -1, 1));
  setp(s, q::GmMono, std::clamp(monoSel, 0, 3));
  // Effects.
  setp(s, q::FxDistType, std::clamp(distSel, 0, 11));
  setp(s, q::FxDistDrive, clampd(G(20000) / 1000.0, 0, 1)), setp(s, q::FxDistAsym, clampd(G(20001) / 1000.0, -0.5, 0.5));
  setp(s, q::FxDistOut, clampd(G(20002) / 1000.0, 0, 2)), setp(s, q::FxDistMix, clampd(G(20003) / 1000.0, -1, 1));
  setp(s, q::FxDistTone, clampd(fxHz(G(20004)), 500, 20000));
  setp(s, q::FxRvOn, G(20005) ? 1 : 0);
  setp(s, q::FxRvLowCut, fxHz(G(20006))), setp(s, q::FxRvHighCut, clampd(fxHz(G(20007)), 300, 20000));
  setp(s, q::FxRvPredelay, G(20008) < 0 ? clampd((std::pow(2.0, -G(20008) / 64.0) - 1.0) * 15.873 / 1000.0, 0, 0.5) : 0.0);
  setp(s, q::FxRvRoom, clampd(G(20009) / 100.0, 0, 1)), setp(s, q::FxRvDiffusion, clampd(G(20010) / 100.0, 0, 1));
  setp(s, q::FxRvDecay, clampd(G(20011) / 100.0, 0.1, 9)), setp(s, q::FxRvDamp, clampd(fxHz(G(20012)), 300, 20000));
  setp(s, q::FxRvColor, clampd(G(20013), 0, 4)), setp(s, q::FxRvMix, clampd(G(20014) / 1000.0, -1, 1));
  setp(s, q::FxChOrder, std::clamp(G(20015), 0, 9));
  setp(s, q::FxChDepth, clampd(G(20016) / 1024.0, 0, 1)), setp(s, q::FxChSpeed, clampd(G(20017) / 1024.0, 0, 1));
  setp(s, q::FxChDelay, clampd(G(20018) / 128.0, 0, 1)), setp(s, q::FxChSpread, clampd(G(20019) / 128.0, 0, 1));
  setp(s, q::FxChCross, clampd(G(20020) / 128.0, -1, 1)), setp(s, q::FxChMix, clampd(G(20021) / 1000.0, -1, 1));
  setp(s, q::FxDlOn, G(20022) ? 1 : 0), setp(s, q::FxDlMode, std::clamp(G(20023), 0, 2));
  setp(s, q::FxDlFeedback, clampd(G(20024) / 1000.0, 0, 1));
  const double tempo = c.song.tempo > 0 ? c.song.tempo : 120.0;
  const int dt = G(20025);
  setp(s, q::FxDlTime, dt < 0 ? clampd((std::pow(2.0, -dt / 96.0) - 1.0) * 66.67 / 1000.0 * tempo / 60.0, 0.01, 4) : clampd(dt / 192.0, 0.01, 4));
  setp(s, q::FxDlOffset, clampd(G(20026) / 512.0, -1, 1)), setp(s, q::FxDlVol, clampd(G(20027) / 1000.0, -1, 1));
  setp(s, q::FxDlPan, clampd(G(20028) / 128.0, -1, 1));
  setp(s, q::FxDlLowCut, fxHz(G(20029))), setp(s, q::FxDlHighCut, clampd(fxHz(G(20030)), 300, 20000));
  setp(s, q::FxDlDamp, G(20031) ? 1 : 0);
  setp(s, q::FxCompType, std::clamp(compSel, 0, 4));
  setp(s, q::FxCompAmount, clampd(G(20032) / 1000.0, 0, 1));
  setp(s, q::FxCompLow, clampd(G(20033) / 1000.0, 0, 2)), setp(s, q::FxCompMid, clampd(G(20034) / 1000.0, 0, 2)),
      setp(s, q::FxCompHigh, clampd(G(20035) / 1000.0, 0, 2));
  setp(s, q::FxEqMix, clampd(parts[0].eqMix, 0, 1));
  if (!c.paramsOnly) s.data = hm::serializePatch(data);
  m.glType = "gl.harmor";
  m.state = s;
  m.exact = false;
  char buf[256];
  std::snprintf(buf, sizeof buf, "v%d → Resynth: %d part(s), articulators / shaping curves / FX decoded%s%s", ver, nParts,
                images ? ", embedded image layer" : "", external ? (missing ? ", resynthesis from audio (file missing)" : ", resynthesis from audio") : "");
  note(c, "Harmor", buf);
  if (missing) c.report.warn("Harmor: " + std::to_string(missing) + " resynthesis audio file(s) not found");
  // Automation (FL parameter = control tag of part A / global / FX; normalized over the control range).
  ParamTable t;
  auto lin = [&](int tag, int gl, double rawLo, double rawHi, double div, double lo, double hi) {
    t.fn(tag, gl, [=](double n) { return clampd((rawLo + (rawHi - rawLo) * n) / div, lo, hi); });
    t.range(tag, rawLo, rawHi);
  };
  lin(52, q::flt(0, 0, q::PfF1Cut), 0, 8192, 8192, 0, 1);
  lin(64, q::flt(0, 1, q::PfF1Cut), 0, 8192, 8192, 0, 1);
  lin(57, q::flt(0, 0, q::PfF1Res), 0, 1000, 1000, 0, 1);
  lin(69, q::flt(0, 1, q::PfF1Res), 0, 1000, 1000, 0, 1);
  lin(76, q::pf(0, q::PfPhMix), 0, 1000, 1000, 0, 1);
  lin(79, q::pf(0, q::PfPhOfs), 0, 999, 1000, 0, 1);
  lin(27, q::pf(0, q::PfHzMix), 0, 1000, 1000, 0, 1);
  lin(26, q::pf(0, q::PfPrism), -1000, 1000, 1000, -1, 1);
  lin(4, q::pf(0, q::PfTimbreMix), 0, 1000, 1000, 0, 1);
  lin(10000, q::GmModX, 0, 512, 512, 0, 1);
  lin(10001, q::GmModY, 0, 512, 512, 0, 1);
  lin(10002, q::GmModZ, 0, 512, 512, 0, 1);
  lin(10010, q::GmPartMix, 0, 1000, 1000, 0, 1);
  lin(10014, q::GmPitch, -2400, 2400, 1, -2400, 2400);
  t.fn(10011, q::GmVolume, [](double n) { return clampd(l3(2.0 * n), 0, 4); });
  t.range(10011, 0, 2000);
  t.install(m);
  return true;
}

}  // namespace gl::flp::gen
