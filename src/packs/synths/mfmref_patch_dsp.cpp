// Gridline — "synths" pack: Matrix FM reference model, patch (mfmref.h).
//
// * parseNative: the instrument's own state layout (worked out from the
//   shipped states; every one walks to its exact end).
// * "x" lines: the exact patch kept in Matrix FM data (ignored by the legacy
//   model and by the data editor, which writes only its own lines):
//     xs <ver> <soften> <eqTarget> <b510> <b5f28>
//     xm <main ×240>          xu <mutes ×162>        xo <option bytes ×16>
//     xd <k> <23 bytes>       xr <40 bytes>          xe <EQ ×12>
//     xn <unit> <u ×10> <ratio> <offset> <noise> <tail ×5> <flags ×8> <fb ×12> <wsBip>
//     xh <op> <harmonic>:<level bits>:<phase bits> …   (floats as hex bits)
//     xa <unit> <art> <version> <base> <env> <lfo>     (float bits)
//     xc <unit> <art> <slot> <record text>             (slot 0 env, 1 LFO, 2.. maps)
//     xw <unit> <record text>                          (shaper curve)
//     xp <count> <parameter values>                    (as the importer wrote them)
// * patchFromState: the exact lines, then every parameter that differs from
//   the importer's value through the inverse of the importer's laws; without
//   the lines the whole patch comes from the parameters and drawn data.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include "../../core/PluginInfo.h"
#include "matrixfm.h"
#include "matrixfm_data.h"
#include "mfmref.h"

namespace gl::packs::synths::mfmref {

namespace mf = gl::packs::synths::mfm;

namespace {

// ------------------------------------------------------------------ native reader
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
    const uint32_t v = static_cast<uint32_t>(p[at]) | (static_cast<uint32_t>(p[at + 1]) << 8) | (static_cast<uint32_t>(p[at + 2]) << 16) |
                       (static_cast<uint32_t>(p[at + 3]) << 24);
    at += 4;
    return static_cast<int32_t>(v);
  }
  uint8_t u8() { return need(1) ? p[at++] : 0; }
  float f32() {
    const uint32_t u = static_cast<uint32_t>(i32());
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
  void bytes(uint8_t* dst, size_t k) {
    if (!need(k)) return;
    std::memcpy(dst, p + at, k);
    at += k;
  }
  void skip(size_t k) {
    if (need(k)) at += k;
  }
};

// Curve: kind word, point format, count, points, then flags, enabled, decay, loop, sustain; `extra` ints follow.
bool readRec(Rd& r, int kind, Record& out, int extra, bool phase) {
  out = Record{};
  out.kind = kind;
  out.hasKind = true;
  r.i32();
  const int fmt = r.i32(), n = r.i32();
  if (r.bad || n < 0 || n > 100000 || fmt < 0 || fmt > 3) return false;
  out.version = fmt;
  out.pts.resize(static_cast<size_t>(n));
  for (auto& pt : out.pts) {
    if (fmt <= 1) {
      pt.time = r.f32(), pt.value = r.f32(), pt.tension = r.f32();
    } else {
      pt.time = r.f64(), pt.value = r.f64(), pt.tension = r.f32();
    }
    if (fmt >= 1) pt.shape = r.u8(), pt.wait = r.u8(), pt.extra = r.u8(), pt.sign = static_cast<int8_t>(r.u8());
  }
  out.flags = r.i32(), out.enabled = r.i32(), out.decay = r.i32(), out.loop = r.i32(), out.sustain = r.i32();
  for (int i = 0; i < 4; ++i) out.extra[i] = i < extra ? r.i32() : 0;
  if (phase) out.phase = static_cast<uint32_t>(r.i32());
  return !r.bad;
}

bool readArt(Rd& r, Art& a) {
  a.version = r.i32();
  a.base = r.f32(), a.envAmt = r.f32(), a.lfoAmt = r.f32();
  if (r.bad) return false;
  if (!readRec(r, 1, a.env, 4, false)) return false;
  if (!readRec(r, 2, a.lfo, 4, a.version >= 3)) return false;
  for (auto& m : a.maps)
    if (!readRec(r, 3, m, 0, false)) return false;
  return true;
}

// ------------------------------------------------------------------ text helpers
uint32_t fbits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
float bitsf(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void putInts(std::string& s, const int32_t* v, int n) {
  char b[16];
  for (int i = 0; i < n; ++i) {
    std::snprintf(b, sizeof b, " %d", v[i]);
    s += b;
  }
}
void putBytes(std::string& s, const uint8_t* v, int n) {
  char b[8];
  for (int i = 0; i < n; ++i) {
    std::snprintf(b, sizeof b, " %u", static_cast<unsigned>(v[i]));
    s += b;
  }
}
template <typename T>
bool getInts(std::istringstream& in, T* v, int n, long lo, long hi) {
  for (int i = 0; i < n; ++i) {
    long x = 0;
    if (!(in >> x) || x < lo || x > hi) return false;
    v[i] = static_cast<T>(x);
  }
  return true;
}

int unitArtCount(int u) { return u < kOps ? 6 : u < kOps + kFilters ? 8 : 2; }

// ------------------------------------------------------------------ defaults (states without the exact lines)
Record mapRecord(double level) {
  Record r = gl::flart::defaultMap(level);
  r.version = 2;
  return r;
}

Patch defaultPatch() {
  Patch p;
  p.ver = 25;
  p.main[173] = 1;
  p.main[188] = 128;
  p.main[189] = -1;
  p.main[194] = 1;
  p.main[196] = 3;
  p.main[197] = 0x101;
  p.opt[5] = p.opt[8] = p.opt[10] = 1;
  static const int32_t kRev[9] = {75, 40, 0, 40, 100, 15, 40, 2, 25};
  std::memcpy(p.reverb, kRev, sizeof kRev);
  static const int32_t kEq[3][4] = {{0, 8950, 17500, 0}, {0, 33145, 17500, 1}, {0, 49305, 17500, 2}};
  std::memcpy(p.eq, kEq, sizeof kEq);
  for (int u = 0; u < kUnits; ++u) {
    Unit& U = p.units[u];
    if (u < kOps) {
      U.u[1] = 256, U.u[2] = 1200;
      U.lv.fill(0), U.ph.fill(0);
    } else if (u < kOps + kFilters) {
      U.u[0] = 1, U.u[1] = 128, U.u[2] = 128, U.u[4] = 128, U.u[7] = 128, U.u[8] = 128;
      U.ws = mapRecord(0);
      U.ws.pts[1].value = 1;
    }
    U.arts.resize(static_cast<size_t>(unitArtCount(u)));
    for (int a = 0; a < unitArtCount(u); ++a) {
      Art& A = U.arts[static_cast<size_t>(a)];
      A.version = 3;
      A.base = 0, A.envAmt = 1, A.lfoAmt = 1;
      A.env = gl::flart::defaultEnvelope();
      A.env.version = 2;
      A.env.enabled = 0;
      A.env.extra[0] = A.env.extra[1] = A.env.extra[3] = 128, A.env.extra[2] = 0;
      A.lfo = gl::flart::defaultLfo();
      A.lfo.version = 2;
      A.lfo.enabled = 0;
      const bool bip = mf::artBipolar(u, a);
      for (auto& m : A.maps) m = mapRecord(bip ? 0.5 : 1.0);
    }
  }
  return p;
}

int32_t rnd(double v) { return static_cast<int32_t>(std::lround(std::isfinite(v) ? v : 0.0)); }

void putI32(uint8_t* d, int32_t v) {
  const uint32_t u = static_cast<uint32_t>(v);
  for (int k = 0; k < 4; ++k) d[k] = static_cast<uint8_t>(u >> (8 * k));
}

// Gridline curve → point record (the importer's mapping read backwards: x as
// stored, tension sign flipped).
Record toRecord(const mf::Curve& c, int kind, const Record& base) {
  Record r = base;
  r.kind = kind;
  r.version = 2;
  r.pts.clear();
  for (const auto& pt : c.pts) {
    gl::flart::Point q;
    q.time = std::max(0.0, static_cast<double>(pt.dt));
    q.value = static_cast<double>(pt.y);
    q.tension = -std::clamp(pt.tension, -1.f, 1.f);
    r.pts.push_back(q);
  }
  return r;
}

// One parameter written through the inverse of the importer's laws.
void applyParam(Patch& p, int k, float fv) {
  const double v = static_cast<double>(fv);
  int32_t* M = p.main;
  using namespace mf;
  if (k == MVolume) M[0] = rnd(v * 128);
  else if (k == MLfoLevel) M[1] = rnd(v * 128);
  else if (k == MPitch) M[2] = rnd(v);
  else if (k >= MVolA && k <= MFltR) M[165 + (k - MVolA)] = rnd(v * 128);
  else if (k == MUniOrder) M[173] = std::clamp(rnd(v), 1, 9);
  else if (k == MUniPan) M[174] = rnd(v * 128);
  else if (k == MUniVol) M[175] = rnd(v * 128);
  else if (k == MUniPitch) M[176] = rnd(v * 256);
  else if (k == MUniSub) M[177] = rnd(v * 128);
  else if (k == MUniPhase) M[178] = rnd(v * 128);
  else if (k == MUniEnv) M[179] = rnd(v * 128);
  else if (k == MModX) M[190] = rnd(v * 512);
  else if (k == MModY) M[191] = rnd(v * 512);
  else if (k == MPorta) p.opt[4] = v >= 0.5 ? 1 : 0;
  else if (k == MGlobalPitch) M[197] = (M[197] & ~0xff0000) | (v >= 0.5 ? 0x10000 : 0);
  else if (k == MRandom) M[197] = (M[197] & ~0xff00) | (v >= 0.5 ? 0x100 : 0);
  else if (k == MSmooth) M[197] = (M[197] & ~0xff) | (v >= 0.5 ? 1 : 0);
  else if (k == MSoften) p.soften = rnd(v * 128);
  else if (k == MCenter) p.opt[1] = v >= 0.5 ? 1 : 0;
  else if (k == MGibbs) p.opt[2] = v >= 0.5 ? 1 : 0;
  else if (k == MEqTarget) p.eqTarget = std::clamp(rnd(v), 0, 3);
  else if (k >= MEq1Type && k < MEq1Type + 3 * kEqStride) {
    const int b = (k - MEq1Type) / kEqStride, f = (k - MEq1Type) % kEqStride;
    if (f == 0) p.eq[b][3] = std::clamp(rnd(v), 0, 2);
    else if (f == 1) p.eq[b][1] = std::clamp(rnd(65536.0 * std::log(std::max(20.0, v) / 20.0) / std::log(1000.0)), 0, 65536);
    else if (f == 2) p.eq[b][2] = std::clamp(rnd(v / 4.0 * 65536.0), 0, 65536);
    else p.eq[b][0] = rnd(v * 100);
  } else if (k == FxPan) M[180] = rnd(v * 128);
  else if (k == ChOrder) M[181] = std::clamp(rnd(v), 0, 9);
  else if (k == ChDepth) M[182] = rnd(v * 1024);
  else if (k == ChSpeed) M[183] = rnd(v * 1024);
  else if (k >= ChDelay && k <= ChVol) M[184 + (k - ChDelay)] = rnd(v * 128);
  else if (k == DlSerial) {
    for (auto& d : p.delays) d[21] = v >= 0.5 ? 1 : 0;
  } else if (k >= Dl1On && k < Dl1On + 3 * kDlStride) {
    const int i = (k - Dl1On) / kDlStride, f = (k - Dl1On) % kDlStride;
    uint8_t* d = p.delays[i];
    if (f == 0) d[22] = v >= 0.5 ? 1 : 0;
    else if (f == 1) putI32(d + 16, std::clamp(rnd(v), 0, 2));
    else if (f == 2) putI32(d, rnd(v * 128));
    else if (f == 3) putI32(d + 4, std::max(1, rnd(v * 192)));
    else if (f == 4) putI32(d + 8, rnd(v * 512));
    else if (f == 5) putI32(d + 12, rnd(v * 128));
    else if (f == 6) d[20] = v >= 0.5 ? 1 : 0;
  } else if (k >= RvOn && k <= RvTempo) {
    int32_t rv[9];
    std::memcpy(rv, p.reverb, sizeof rv);
    const bool tempo = p.reverb[37] != 0;
    switch (k) {
      case RvOn: p.reverb[36] = v >= 0.5 ? 1 : 0; break;
      case RvTempo: p.reverb[37] = v >= 0.5 ? 1 : 0; break;
      case RvLowCut: rv[0] = rnd(v); break;
      case RvHighCut: rv[1] = rnd(v / 100); break;
      case RvPreDelay: rv[2] = rnd(v * (tempo ? 192 : 384)); break;
      case RvRoom: rv[3] = rnd(v * 100); break;
      case RvDiff: rv[4] = rnd(v * 100); break;
      case RvDecay: rv[5] = rnd(v * 10); break;
      case RvHighDamp: rv[6] = rnd(v / 100); break;
      case RvColor: rv[7] = std::clamp(rnd(v), 0, 4); break;
      case RvVol: rv[8] = rnd(v * 128); break;
      default: break;
    }
    std::memcpy(p.reverb, rv, sizeof rv);
  } else if (k >= MatFm && k < MatFm + kRows * kCols) {
    const int i = k - MatFm;
    M[3 + i] = rnd(v * 256);
    p.mutes[i] = 0;
  } else if (k >= MatRm && k < MatRm + kRows * kCols) {
    const int i = k - MatRm;
    M[84 + i] = rnd(v * 256);
    p.mutes[81 + i] = 0;
  } else {
    // unit fields
    int u = -1, f = -1;
    if (k >= OpBase && k < FltBase) u = (k - OpBase) / kOpFields, f = (k - OpBase) % kOpFields;
    else if (k >= FltBase && k < kParamCount) u = kOps + (k - FltBase) / kFltFields, f = (k - FltBase) % kFltFields;
    else if (k >= MainArts && k < MatFm) u = kUnits - 1, f = -1;
    if (u < 0) return;
    Unit& U = p.units[u];
    int artBase = -1;
    if (u < kOps) {
      artBase = OpArts;
      switch (f) {
        case OpPhase: U.u[0] = rnd(v * 256); return;
        case OpVol: U.u[1] = rnd(v * 256); return;
        case OpPitchRange: U.u[2] = rnd(v * 100); return;
        case OpRatio: U.ratio = rnd(v * 20000); return;
        case OpFreq: U.freqOfs = rnd(v * 100); return;
        case OpNoise: U.noise = rnd(v * 128); return;
        case OpShape: U.tail[0] = rnd(v * 256); return;
        case OpTension: U.tail[1] = rnd(v * 128); return;
        case OpSkew: U.tail[2] = rnd(v * 128); return;
        case OpSine: U.tail[3] = rnd(v * 128); return;
        case OpFilter: U.tail[4] = rnd(v * 128); return;
        case OpHalf: U.flags[0] = v >= 0.5; return;
        case OpEven: U.flags[1] = v >= 0.5; return;
        case OpAbs: U.flags[2] = v >= 0.5; return;
        case OpGlobal: U.flags[3] = v >= 0.5; return;
        case OpCenter: U.flags[4] = v >= 0.5; return;
        case OpDeclick: U.flags[5] = v >= 0.5; return;
        case OpPluck: U.flags[6] = v >= 0.5; return;
        case OpBandLimit: U.flags[7] = v >= 0.5; return;
        default: break;
      }
    } else if (u < kOps + kFilters) {
      artBase = FlArts;
      switch (f) {
        case FlMode: U.u[0] = std::clamp(rnd(v), 0, 7); return;
        case FlEnv: U.u[1] = rnd(v * 128); return;
        case FlCut: U.u[2] = rnd(v * 256); return;
        case FlRes: U.u[3] = rnd(v * 256); return;
        case FlLow: U.u[4] = rnd(v * 128); return;
        case FlBand: U.u[5] = rnd(v * 128); return;
        case FlHigh: U.u[6] = rnd(v * 128); return;
        case FlWsAmp: U.u[7] = rnd(v * 128); return;
        case FlWsMix: U.u[8] = rnd(v * 128); return;
        case FlNext: U.u[9] = rnd(v * 256); return;
        case FlType: U.fb[11] = std::clamp(rnd(v), 0, kFltTypes - 1); return;
        case FlWsOn: U.fb[10] = v >= 0.5 ? 1 : 0; return;
        case FlHq: U.fb[0] = (U.fb[0] & ~0xff) | (v >= 0.5 ? 1 : 0); return;
        case FlFlat: U.fb[0] = (U.fb[0] & ~0xff00) | (v >= 0.5 ? 0x100 : 0); return;
        case FlWsBipolar: U.wsBip = v >= 0.5 ? 1 : 0; return;
        default: break;
      }
    }
    const int rel = u == kUnits - 1 ? k - MainArts : f - artBase;
    const int a = rel / kArtFields, af = rel % kArtFields;
    if (a < 0 || a >= static_cast<int>(U.arts.size())) return;
    Art& A = U.arts[static_cast<size_t>(a)];
    switch (af) {
      case AfEnvOn: A.env.enabled = v >= 0.5 ? -1 : 0; break;
      case AfLfoOn: A.lfo.enabled = v >= 0.5 ? -1 : 0; break;
      case AfAmount: A.envAmt = fv; break;
      case AfAtt: A.env.extra[0] = rnd(v * 128); break;
      case AfDec: A.env.extra[1] = rnd(v * 128); break;
      case AfSus: A.env.extra[2] = rnd(v * 128); break;
      case AfRel: A.env.extra[3] = rnd(v * 128); break;
      case AfLfoSpeed: {
        const double hz = (A.lfo.flags & 1) ? v * 2.0 : v;  // tempo: cycles per beat, drawn at 120 BPM
        A.lfo.extra[0] = std::clamp(rnd(std::log(std::max(0.0, hz) * 625.0 + 1.0) / std::log(10001.0) * 65536.0), 0, 65536);
        break;
      }
      case AfLfoSkew: A.lfo.extra[2] = rnd(v * 128); break;
      case AfTempo: A.lfo.flags = (A.lfo.flags & ~1) | (v >= 0.5 ? 1 : 0); break;
      case AfLfoGlobal: A.lfo.flags = (A.lfo.flags & ~2) | (v >= 0.5 ? 2 : 0); break;
      default: break;  // LFO depth: no counterpart
    }
  }
}

// Drawn data (states without the exact lines).
void applyData(Patch& p, const mf::PatchData& d) {
  for (int u = 0; u < kOps; ++u)
    for (int h = 0; h < 256; ++h) {
      p.units[u].lv[static_cast<size_t>(h)] = d.level[static_cast<size_t>(u)][static_cast<size_t>(h)];
      p.units[u].ph[static_cast<size_t>(h)] = d.phase[static_cast<size_t>(u)][static_cast<size_t>(h)];
    }
  for (int j = 0; j < kFilters; ++j) {
    const mf::Curve& c = d.ws[static_cast<size_t>(j)];
    if (!c.empty()) p.units[kOps + j].ws = toRecord(c, 3, mapRecord(0));
  }
  for (int u = 0; u < kUnits; ++u)
    for (int a = 0; a < unitArtCount(u) && a < mf::unitArts(u); ++a) {
      const mf::ArtData& ad = d.arts[static_cast<size_t>(u)][static_cast<size_t>(a)];
      Art& A = p.units[u].arts[static_cast<size_t>(a)];
      if (!ad.env.empty()) {
        const Record old = A.env;
        A.env = toRecord(ad.env, 1, old);
        const int n = static_cast<int>(A.env.pts.size());
        A.env.sustain = ad.env.sustain >= 0 && ad.env.sustain < n ? ad.env.sustain : -1;
        A.env.decay = ad.env.decay > 0 && ad.env.decay < n ? ad.env.decay : -1;
        A.env.loop = ad.env.loopStart;
        A.env.flags = (A.env.flags & ~1) | (ad.env.tempo ? 1 : 0);
      }
      if (!ad.lfo.empty()) {
        const Record old = A.lfo;
        A.lfo = toRecord(ad.lfo, 2, old);
      }
      A.lfo.extra[1] = rnd(ad.lfoTension * 128.0);
      A.lfo.extra[3] = rnd(ad.lfoPw * 128.0);
      A.lfo.phase = static_cast<uint32_t>(std::fmod(std::max(0.0, static_cast<double>(ad.lfoPhase)), 1.0) * 4294967296.0);
      for (int m = 0; m < 6; ++m)
        if (!ad.maps[static_cast<size_t>(m)].empty()) A.maps[static_cast<size_t>(m)] = toRecord(ad.maps[static_cast<size_t>(m)], 3, mapRecord(0));
    }
}

}  // namespace

namespace {
struct Wr {
  std::vector<uint8_t> b;
  void u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v)); }
  void i32(int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(u >> (8 * k)));
  }
  void f32(float f) { i32(static_cast<int32_t>(fbits(f))); }
  void f64(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    for (int k = 0; k < 8; ++k) b.push_back(static_cast<uint8_t>(u >> (8 * k)));
  }
  void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
};
void writeRec(Wr& w, const Record& r, int kind, int extra, bool phase) {
  w.i32(kind);
  const int fmt = std::clamp(r.version, 0, 3);
  w.i32(fmt);
  w.i32(static_cast<int32_t>(r.pts.size()));
  for (const auto& pt : r.pts) {
    if (fmt <= 1) {
      w.f32(static_cast<float>(pt.time)), w.f32(static_cast<float>(pt.value)), w.f32(pt.tension);
    } else {
      w.f64(pt.time), w.f64(pt.value), w.f32(pt.tension);
    }
    if (fmt >= 1) w.u8(pt.shape), w.u8(pt.wait), w.u8(pt.extra), w.u8(static_cast<uint8_t>(pt.sign));
  }
  w.i32(r.flags), w.i32(r.enabled), w.i32(r.decay), w.i32(r.loop), w.i32(r.sustain);
  for (int i = 0; i < extra; ++i) w.i32(r.extra[i]);
  if (phase) w.i32(static_cast<int32_t>(r.phase));
}
}  // namespace

std::vector<uint8_t> writeNative(const Patch& p) {
  Wr w;
  const int ver = p.ver;
  for (int i = 0; i < 240; ++i) w.i32(p.main[i]);
  w.u8(0), w.u8(0);
  for (int i = 0; i < 162; ++i) w.b[static_cast<size_t>(800 + i)] = p.mutes[i];
  if (ver > 3) w.i32(p.soften);
  if (ver > 5) w.u8(p.opt[0]);
  if (ver > 6) w.u8(p.opt[1]);
  if (ver > 12) w.u8(p.opt[2]);
  int n507 = 0;
  if (ver > 13) {
    n507 = ver > 14 ? 5 : 1;
    if (ver > 17) n507 += 3;
    if (ver > 18) n507 += 1;
  }
  w.bytes(p.opt + 3, static_cast<size_t>(n507));
  for (const auto& d : p.delays) w.bytes(d, 23);
  if (ver >= 10) w.bytes(p.reverb, ver > 10 ? 38 : 37);
  if (ver > 2) {
    w.i32(p.eqTarget);
    for (const auto& b : p.eq) {
      w.i32(b[0]), w.i32(b[1]), w.i32(b[2]);
      if (ver > 13) w.i32(b[3]);
    }
  }
  if (ver < 0x17) w.i32(0);
  else w.u8(0);
  for (int u = 0; u < kUnits; ++u) {
    const Unit& U = p.units[u];
    for (int v : U.u) w.i32(v);
    if (u < kOps) {
      if (ver < 8) {
        w.i32(U.ratio), w.i32(U.freqOfs);
        for (int h = 0; h < 24; ++h) w.f32(U.lv[static_cast<size_t>(h)]);
        for (int h = 0; h < 24; ++h) w.f32(U.ph[static_cast<size_t>(h)]);
        for (int v : U.tail) w.i32(v);
        w.i32(U.noise);
        for (int i = 0; i < 7; ++i) w.u8(U.flags[i]);
      } else {
        w.i32(U.ratio), w.i32(U.freqOfs), w.i32(U.noise);
        const int nh = ver < 0x11 ? 128 : 256;
        for (int h = 0; h < nh; ++h) w.f32(U.lv[static_cast<size_t>(h)]);
        for (int h = 0; h < nh; ++h) w.f32(U.ph[static_cast<size_t>(h)]);
        for (int v : U.tail) w.i32(v);
        for (uint8_t f : U.flags) w.u8(f);
      }
    } else if (u < kOps + kFilters) {
      const int nf = ver < 9 ? 10 : 12;
      for (int i = 0; i < nf; ++i) w.i32(U.fb[i]);
      if (ver < 9) w.u8(static_cast<uint32_t>(U.fb[10]));
      writeRec(w, U.ws, 3, 0, false);
      if (ver >= 12) w.u8(U.wsBip);
    }
    for (int a = 0; a < unitArtCount(u); ++a) {
      Art A;
      if (static_cast<size_t>(a) < U.arts.size()) A = U.arts[static_cast<size_t>(a)];
      w.i32(A.version), w.f32(A.base), w.f32(A.envAmt), w.f32(A.lfoAmt);
      writeRec(w, A.env, 1, 4, false);
      writeRec(w, A.lfo, 2, 4, A.version >= 3);
      for (const auto& m : A.maps) writeRec(w, m, 3, 0, false);
    }
  }
  if (ver > 0x17) w.u8(p.b510);
  if (ver > 0x18) w.u8(p.b5f28);
  return w.b;
}

bool parseNative(const uint8_t* p, size_t n, int ver, Patch& out) {
  Rd r{p, n};
  out.ver = ver;
  for (int i = 0; i < 240; ++i) out.main[i] = r.i32();
  // the mute bytes live inside the main block (bytes 800..961; the last two follow the ints)
  for (int i = 0; i < 160; ++i) out.mutes[i] = static_cast<uint8_t>(static_cast<uint32_t>(out.main[(800 + i) / 4]) >> (8 * ((800 + i) % 4)));
  out.mutes[160] = r.u8(), out.mutes[161] = r.u8();
  // option bytes missing from older states keep the instrument's defaults
  std::memset(out.opt, 0, sizeof out.opt);
  out.opt[5] = 1, out.opt[8] = 1, out.opt[10] = 1;
  out.soften = ver > 3 ? r.i32() : 0;
  if (ver > 5) out.opt[0] = r.u8();
  if (ver > 6) out.opt[1] = r.u8();
  if (ver > 12) out.opt[2] = r.u8();
  int n507 = 0;
  if (ver > 13) {
    n507 = ver > 14 ? 5 : 1;
    if (ver > 17) n507 += 3;
    if (ver > 18) n507 += 1;
  }
  r.bytes(out.opt + 3, static_cast<size_t>(n507));
  for (auto& d : out.delays) r.bytes(d, 23);
  std::memset(out.reverb, 0, sizeof out.reverb);
  if (ver >= 10) r.bytes(out.reverb, ver > 10 ? 38 : 37);
  out.eqTarget = 0;
  for (int bi = 0; bi < 3; ++bi) out.eq[bi][0] = out.eq[bi][1] = out.eq[bi][2] = 0, out.eq[bi][3] = bi;
  if (ver > 2) {
    out.eqTarget = r.i32();
    for (int bi = 0; bi < 3; ++bi) {
      auto& b = out.eq[bi];
      b[0] = r.i32(), b[1] = r.i32(), b[2] = r.i32();
      b[3] = ver > 13 ? r.i32() : bi;  // older states: low shelf, peak, high shelf
    }
  }
  if (ver < 0x17) {
    const int len = r.i32();
    if (len < 0) return false;
    r.skip(static_cast<size_t>(len));
  } else {
    int len = r.u8();
    if (len == 0xff) len = r.i32();
    if (len < 0) return false;
    r.skip(static_cast<size_t>(len) * 2);
  }
  for (int u = 0; u < kUnits && !r.bad; ++u) {
    Unit& U = out.units[u];
    for (int& v : U.u) v = r.i32();
    if (u < kOps) {
      U.lv.fill(0), U.ph.fill(0);
      if (ver < 8) {  // 24 partials; the noise amount follows the shape block; 7 flag bytes
        U.ratio = r.i32(), U.freqOfs = r.i32();
        for (int h = 0; h < 24; ++h) U.lv[static_cast<size_t>(h)] = r.f32();
        for (int h = 0; h < 24; ++h) U.ph[static_cast<size_t>(h)] = r.f32();
        for (int& v : U.tail) v = r.i32();
        U.noise = r.i32();
        for (int i = 0; i < 7; ++i) U.flags[i] = r.u8();
        U.flags[7] = 0;
      } else {
        U.ratio = r.i32(), U.freqOfs = r.i32(), U.noise = r.i32();
        const int nh = ver < 0x11 ? 128 : 256;
        for (int h = 0; h < nh; ++h) U.lv[static_cast<size_t>(h)] = r.f32();
        for (int h = 0; h < nh; ++h) U.ph[static_cast<size_t>(h)] = r.f32();
        for (int& v : U.tail) v = r.i32();
        for (uint8_t& f : U.flags) f = r.u8();
      }
    } else if (u < kOps + kFilters) {
      const int nf = ver < 9 ? 10 : 12;
      for (int i = 0; i < 12; ++i) U.fb[i] = i < nf ? r.i32() : 0;
      if (ver < 9) U.fb[10] = r.u8();  // shaper switch (the type stays 0)
      if (!readRec(r, 3, U.ws, 0, false)) return false;
      U.wsBip = ver >= 12 ? r.u8() : 0;
    }
    U.arts.resize(static_cast<size_t>(unitArtCount(u)));
    for (auto& a : U.arts)
      if (!readArt(r, a)) return false;
  }
  out.b510 = ver > 0x17 ? r.u8() : 0;
  out.b5f28 = ver > 0x18 ? r.u8() : 0;
  return !r.bad && r.at == n;
}

std::string serialize(const Patch& p, const std::vector<float>& params) {
  std::string s;
  char b[64];
  std::snprintf(b, sizeof b, "xs %d %d %d %u %u\n", p.ver, p.soften, p.eqTarget, static_cast<unsigned>(p.b510), static_cast<unsigned>(p.b5f28));
  s += b;
  s += "xm";
  putInts(s, p.main, 240);
  s += "\nxu";
  putBytes(s, p.mutes, 162);
  s += "\nxo";
  putBytes(s, p.opt, 16);
  s += "\n";
  for (int i = 0; i < 3; ++i) {
    std::snprintf(b, sizeof b, "xd %d", i);
    s += b;
    putBytes(s, p.delays[i], 23);
    s += "\n";
  }
  s += "xr";
  putBytes(s, p.reverb, 40);
  s += "\nxe";
  putInts(s, &p.eq[0][0], 12);
  s += "\n";
  for (int u = 0; u < kUnits; ++u) {
    const Unit& U = p.units[u];
    std::snprintf(b, sizeof b, "xn %d", u);
    s += b;
    putInts(s, U.u, 10);
    const int32_t op3[3] = {U.ratio, U.freqOfs, U.noise};
    putInts(s, op3, 3);
    putInts(s, U.tail, 5);
    putBytes(s, U.flags, 8);
    putInts(s, U.fb, 12);
    putBytes(s, &U.wsBip, 1);
    s += "\n";
    if (u < kOps) {
      std::snprintf(b, sizeof b, "xh %d", u);
      s += b;
      for (int h = 0; h < 256; ++h) {
        const float lv = U.lv[static_cast<size_t>(h)], ph = U.ph[static_cast<size_t>(h)];
        if (fbits(lv) == 0 && fbits(ph) == 0) continue;
        std::snprintf(b, sizeof b, " %d:%08x:%08x", h, fbits(lv), fbits(ph));
        s += b;
      }
      s += "\n";
    } else if (u < kOps + kFilters) {
      std::snprintf(b, sizeof b, "xw %d ", u);
      s += b;
      s += gl::flart::writeText(U.ws);
      s += "\n";
    }
    for (size_t a = 0; a < U.arts.size(); ++a) {
      const Art& A = U.arts[a];
      std::snprintf(b, sizeof b, "xa %d %zu %d %08x %08x %08x\n", u, a, A.version, fbits(A.base), fbits(A.envAmt), fbits(A.lfoAmt));
      s += b;
      for (int slot = 0; slot < 8; ++slot) {
        const Record& R = slot == 0 ? A.env : slot == 1 ? A.lfo : A.maps[static_cast<size_t>(slot - 2)];
        std::snprintf(b, sizeof b, "xc %d %zu %d ", u, a, slot);
        s += b;
        s += gl::flart::writeText(R);
        s += "\n";
      }
    }
  }
  if (!params.empty()) {
    std::snprintf(b, sizeof b, "xp %zu", params.size());
    s += b;
    for (float v : params) {
      std::snprintf(b, sizeof b, " %08x", fbits(v));
      s += b;
    }
    s += "\n";
  }
  return s;
}

std::string withoutExact(const std::string& data) {
  if (data.find("\nxs ") == std::string::npos && data.rfind("xs ", 0) != 0) return data;
  std::string out;
  size_t at = 0;
  while (at < data.size()) {
    size_t nl = data.find('\n', at);
    if (nl == std::string::npos) nl = data.size() - 1;
    if (data[at] != 'x') out.append(data, at, nl - at + 1);
    at = nl + 1;
  }
  return out == "mfm 1\n" ? std::string() : out;
}

bool parse(const std::string& data, Patch& out, std::vector<float>& params) {
  if (data.find("\nxs ") == std::string::npos && data.rfind("xs ", 0) != 0) return false;
  Patch p;
  params.clear();
  bool seenS = false, seenM = false;
  int unitsSeen = 0;
  std::istringstream in(data);
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 3 || line[0] != 'x') continue;
    const std::string tag = line.substr(0, 2);
    std::istringstream ls(line.substr(2));
    if (tag == "xs") {
      long v[5];
      for (long& x : v)
        if (!(ls >> x)) return false;
      p.ver = static_cast<int>(v[0]), p.soften = static_cast<int32_t>(v[1]), p.eqTarget = static_cast<int32_t>(v[2]);
      p.b510 = static_cast<uint8_t>(v[3]), p.b5f28 = static_cast<uint8_t>(v[4]);
      seenS = true;
    } else if (tag == "xm") {
      if (!getInts(ls, p.main, 240, INT32_MIN, INT32_MAX)) return false;
      seenM = true;
    } else if (tag == "xu") {
      if (!getInts(ls, p.mutes, 162, 0, 255)) return false;
    } else if (tag == "xo") {
      if (!getInts(ls, p.opt, 16, 0, 255)) return false;
    } else if (tag == "xd") {
      int i = -1;
      if (!(ls >> i) || i < 0 || i > 2 || !getInts(ls, p.delays[i], 23, 0, 255)) return false;
    } else if (tag == "xr") {
      if (!getInts(ls, p.reverb, 40, 0, 255)) return false;
    } else if (tag == "xe") {
      if (!getInts(ls, &p.eq[0][0], 12, INT32_MIN, INT32_MAX)) return false;
    } else if (tag == "xn") {
      int u = -1;
      if (!(ls >> u) || u < 0 || u >= kUnits) return false;
      Unit& U = p.units[u];
      int32_t op3[3];
      if (!getInts(ls, U.u, 10, INT32_MIN, INT32_MAX) || !getInts(ls, op3, 3, INT32_MIN, INT32_MAX) || !getInts(ls, U.tail, 5, INT32_MIN, INT32_MAX) ||
          !getInts(ls, U.flags, 8, 0, 255) || !getInts(ls, U.fb, 12, INT32_MIN, INT32_MAX) || !getInts(ls, &U.wsBip, 1, 0, 255))
        return false;
      U.ratio = op3[0], U.freqOfs = op3[1], U.noise = op3[2];
      U.arts.resize(static_cast<size_t>(unitArtCount(u)));
      ++unitsSeen;
    } else if (tag == "xh") {
      int u = -1;
      if (!(ls >> u) || u < 0 || u >= kOps) return false;
      std::string item;
      while (ls >> item) {
        int h = -1;
        unsigned a = 0, c = 0;
        if (std::sscanf(item.c_str(), "%d:%x:%x", &h, &a, &c) != 3 || h < 0 || h >= 256) return false;
        p.units[u].lv[static_cast<size_t>(h)] = bitsf(a);
        p.units[u].ph[static_cast<size_t>(h)] = bitsf(c);
      }
    } else if (tag == "xw") {
      int u = -1;
      if (!(ls >> u) || u < kOps || u >= kOps + kFilters) return false;
      std::string rest;
      std::getline(ls, rest);
      if (!gl::flart::readText(rest, p.units[u].ws)) return false;
    } else if (tag == "xa") {
      int u = -1, a = -1, ver = 0;
      unsigned b0 = 0, b1 = 0, b2 = 0;
      if (!(ls >> u >> a >> ver) || u < 0 || u >= kUnits || a < 0 || a >= unitArtCount(u)) return false;
      std::string hx;
      ls >> hx, b0 = static_cast<unsigned>(std::strtoul(hx.c_str(), nullptr, 16));
      ls >> hx, b1 = static_cast<unsigned>(std::strtoul(hx.c_str(), nullptr, 16));
      ls >> hx, b2 = static_cast<unsigned>(std::strtoul(hx.c_str(), nullptr, 16));
      auto& arts = p.units[u].arts;
      if (arts.size() < static_cast<size_t>(unitArtCount(u))) arts.resize(static_cast<size_t>(unitArtCount(u)));
      Art& A = arts[static_cast<size_t>(a)];
      A.version = ver, A.base = bitsf(b0), A.envAmt = bitsf(b1), A.lfoAmt = bitsf(b2);
    } else if (tag == "xc") {
      int u = -1, a = -1, slot = -1;
      if (!(ls >> u >> a >> slot) || u < 0 || u >= kUnits || a < 0 || a >= unitArtCount(u) || slot < 0 || slot > 7) return false;
      std::string rest;
      std::getline(ls, rest);
      auto& arts = p.units[u].arts;
      if (arts.size() < static_cast<size_t>(unitArtCount(u))) arts.resize(static_cast<size_t>(unitArtCount(u)));
      Art& A = arts[static_cast<size_t>(a)];
      Record& R = slot == 0 ? A.env : slot == 1 ? A.lfo : A.maps[static_cast<size_t>(slot - 2)];
      if (!gl::flart::readText(rest, R)) return false;
    } else if (tag == "xp") {
      size_t n = 0;
      if (!(ls >> n) || n > 100000) return false;
      params.resize(n);
      for (float& v : params) {
        std::string hx;
        if (!(ls >> hx)) return false;
        v = bitsf(static_cast<uint32_t>(std::strtoul(hx.c_str(), nullptr, 16)));
      }
    }
  }
  if (!seenS || !seenM || unitsSeen != kUnits) return false;
  out = std::move(p);
  return true;
}

Patch patchFromState(const PluginState& s) {
  Patch p;
  std::vector<float> snap;
  const bool exact = parse(s.data, p, snap);
  if (!exact) {
    p = defaultPatch();
    mf::PatchData d;
    mf::parsePatch(s.data, d);
    applyData(p, d);
  }
  const int n = std::min(static_cast<int>(s.params.size()), static_cast<int>(mf::kParamCount));
  for (int k = 0; k < n; ++k) {
    const float v = s.params[static_cast<size_t>(k)];
    if (exact && static_cast<size_t>(k) < snap.size() && fbits(snap[static_cast<size_t>(k)]) == fbits(v)) continue;
    applyParam(p, k, v);
  }
  return p;
}

bool exactPatch(const Patch& p, std::string* why) {
  bool random = false;
  const bool randomOn = ((p.main[197] >> 8) & 0xff) != 0;
  for (const Unit& U : p.units)
    for (const Art& A : U.arts) {
      const Record& m = A.maps[4];
      if (randomOn)
        for (const auto& pt : m.pts)
          if (pt.value != m.pts[0].value) random = true;
    }
  if (why) *why = random ? "random mapping curves" : "";
  return !random;
}

LiveSpec liveSpec(const PluginState& s) {
  LiveSpec spec;
  Patch p;
  std::vector<float> snap;
  const bool exact = parse(s.data, p, snap);
  int at = 0;
  auto field = [&](int param, float scale, int32_t value, uint8_t mute) {
    LiveSpec::Field& f = spec.f[at++];
    f.param = param, f.scale = scale;
    f.hasSnap = exact && param >= 0 && static_cast<size_t>(param) < snap.size();
    f.snap = f.hasSnap ? fbits(snap[static_cast<size_t>(param)]) : 0;
    f.exact = value, f.mute = mute;
  };
  field(mf::MVolume, 128, p.main[0], 0);
  field(mf::MModX, 512, p.main[190], 0);
  field(mf::MModY, 512, p.main[191], 0);
  for (int i = 0; i < 81; ++i) field(mf::MatFm + i, 256, p.main[3 + i], p.mutes[i]);
  for (int i = 0; i < 81; ++i) field(mf::MatRm + i, 256, p.main[84 + i], p.mutes[81 + i]);
  for (int u = 0; u < kOps; ++u) field(mf::op(u, mf::OpVol), 256, p.units[u].u[1], 0);
  static const int kFl[8] = {mf::FlCut, mf::FlRes, mf::FlLow, mf::FlBand, mf::FlHigh, mf::FlWsAmp, mf::FlWsMix, mf::FlNext};
  static const float kSc[8] = {256, 256, 128, 128, 128, 128, 128, 256};
  for (int j = 0; j < kFilters; ++j)
    for (int i = 0; i < 8; ++i) field(mf::flt(j, kFl[i]), kSc[i], p.units[kOps + j].u[2 + i], 0);
  return spec;
}

void liveFromParams(const LiveSpec& spec, const float* params, size_t n, Live& out) {
  int at = 0;
  auto value = [&](int32_t& dst, uint8_t* mute) {
    const LiveSpec::Field& f = spec.f[at++];
    const bool have = f.param >= 0 && static_cast<size_t>(f.param) < n;
    if (!have || (f.hasSnap && fbits(params[f.param]) == f.snap)) {
      dst = f.exact;
      if (mute) *mute = f.mute;
    } else {
      dst = rnd(static_cast<double>(params[f.param]) * static_cast<double>(f.scale));
      if (mute) *mute = 0;
    }
  };
  value(out.mainVol, nullptr);
  value(out.modX, nullptr);
  value(out.modY, nullptr);
  for (int i = 0; i < 162; ++i) value(out.matrix[i], &out.mutes[i]);
  for (int u = 0; u < kOps; ++u) value(out.opVol[u], nullptr);
  for (int j = 0; j < kFilters; ++j) {
    out.flt[j][0] = out.flt[j][1] = 0;
    for (int i = 0; i < 8; ++i) value(out.flt[j][2 + i], nullptr);
  }
}

}  // namespace gl::packs::synths::mfmref
