// Gridline — FLP import: Sytrus → Matrix FM ("gl.matrixfm").
//
// State: dword header (version in the high word, or a plain version dword),
// then a zlib stream. Decompressed layout (worked out from the shipped presets
// and demo projects; every one of FL's 1 474 Sytrus states walks to its exact
// end), dwords unless noted:
//   main block (962 bytes): 0 master level (0..160, 128 = unity), 1 master LFO
//     level (/128), 2 master pitch (semitones), 3..83 the 9 × 9 frequency
//     matrix and 84..164 the ring-modulation matrix (row = destination unit:
//     operators 1..6, filters 1..3; columns 0..5 operators, 6 pan, 7 effects,
//     8 output; ±256), 165..172 volume / filter A D S R scales (128 = ×1,
//     sustain offset ±128), 173..179 unison order / pan / volume / pitch / sub /
//     phase / envelope, 180 effects pan, 181..187 chorus order / depth (/1024) /
//     speed (/1024) / delay / spread / cross / volume, 190 / 191 mod X / Y (/512),
//     193..196 envelope-accuracy / oversampling choices; bytes 800..961 one
//     mute byte per matrix cell (frequency layer, then ring layer).
//   soften dword (0..128), option bytes (the 3rd is copied into every
//     operator on load: ringing attenuation), three delays (23 bytes: feedback,
//     time (1/384 s, or 1/192 beat with tempo), stereo offset (/512), volume,
//     mode, then tempo / ? / on bytes), reverb (low cut Hz, high cut ×100 Hz,
//     predelay (1/384 s or 1/192 beat), room, diffusion, decay ×0.1 s, high
//     damping ×100 Hz, colour, wet; enable + tempo bytes), EQ target dword,
//     three EQ bands (gain cents-of-dB, frequency 0..65536, bandwidth 0..65536,
//     type), info text (RTF; UTF-16 from version 23).
//   ten units (6 operators, 3 filters, main): ten unit dwords (operator:
//     phase /256, volume /256, pitch range cents; filter: mode, cutoff
//     articulator amount, cutoff, resonance (/256), low/gain, band, high,
//     shaper pre-amp, shaper mix (/128), to-next (/256)), then the operator
//     block (ratio /20000, offset Hz ×100, noise, harmonic levels + phases as
//     floats, shape /256, tension, skew, sine, pre-filter (/128), option bytes)
//     or the filter block (12 dwords, type in the last) + waveshaper curve,
//     and the articulators (6 / 8 / 2): i32 version, 3 floats, then curves.
//     Each curve: kind, point format, count, points (f64 dx, f64 y, f32
//     tension, 4 bytes: segment type, …, tension sign), then the curve tail:
//     flags (bit 0 tempo, bit 1 global, bit 2 bipolar LFO tension, bit 3
//     frozen LFO), enabled, decay point, loop start, sustain point (= loop
//     end). Envelope curve + A / D / S / R dwords (/128); LFO curve + speed
//     (0..65536 → (10001^x − 1) / 625 Hz, at 120 BPM when tempo-synced),
//     tension, skew, pulse width (/128) and, from articulator version 3, the
//     start phase (u32 / 2^32); then six mapping curves (key, velocity, X, Y,
//     random, unison). Envelope run (from the plugin's behaviour): segments up
//     to the decay point use the attack scale (none without a decay point),
//     then the decay scale up to the sustain point, which holds or loops back
//     to the loop start; tempo envelopes are drawn at 120 BPM.
// The DSP is Gridline's own, so the mapping is approximate (exact = false).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include "../../packs/synths/matrixfm.h"
#include "../../packs/synths/matrixfm_data.h"
#include "FlpGen.h"

namespace gl::flp::gen {

namespace {

namespace mf = gl::packs::synths::mfm;

struct Reader {
  const uint8_t* p;
  size_t n, at = 0;
  bool bad = false;
  bool need(size_t k) {
    if (bad || k > n - std::min(n, at) || at > n) bad = true;
    return !bad;
  }
  int32_t i32() {
    if (!need(4)) return 0;
    int32_t v = static_cast<int32_t>(p[at] | (p[at + 1] << 8) | (p[at + 2] << 16) | (static_cast<uint32_t>(p[at + 3]) << 24));
    at += 4;
    return v;
  }
  uint8_t u8() { return need(1) ? p[at++] : 0; }
  float f32() {
    uint32_t u = static_cast<uint32_t>(i32());
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
  size_t skip(size_t k) {
    const size_t s = at;
    if (need(k)) at += k;
    return s;
  }
};

struct RawPt {
  double x = 0, y = 0;
  float tension = 0;
};
struct RawList {
  std::vector<RawPt> pts;
};

std::vector<RawPt> readPts(Reader& r, int n, int ver) {
  std::vector<RawPt> v;
  if (n < 0 || n > 4096) {
    r.bad = true;
    return v;
  }
  for (int i = 0; i < n && !r.bad; ++i) {
    RawPt p;
    if (ver <= 0) {
      p.x = r.f32(), p.y = r.f32(), p.tension = r.f32();
    } else if (ver == 1) {
      p.x = r.f32(), p.y = r.f32(), p.tension = r.f32();
      r.i32();
    } else {
      p.x = r.f64(), p.y = r.f64(), p.tension = r.f32();
      r.i32();
    }
    v.push_back(p);
  }
  return v;
}

// Short list (3-dword header: ?, point format, count).
RawList readList(Reader& r) {
  RawList l;
  r.i32();
  const int fmt = r.i32(), n = r.i32();
  l.pts = readPts(r, n, fmt);
  return l;
}

struct RawArt {
  RawList env, lfo;
  int envTail[4] = {0, 0, -1, -1}, lfoTail[4] = {0, 0, -1, -1};
  int p1[5] = {2, 128, 128, 0, 128}, p2[6] = {2, 40000, 0, 0, 0, 0};
  RawList maps[mf::kMaps];
  int h0 = 0;
};

RawArt readArt(Reader& r) {
  RawArt a;
  const int h0 = a.h0 = r.i32();
  r.f32(), r.f32(), r.f32(), r.i32();
  const int fmt = r.i32(), n = r.i32();
  a.env.pts = readPts(r, n, fmt);
  for (int& t : a.envTail) t = r.i32();
  for (int& t : a.p1) t = r.i32();
  a.lfo = readList(r);
  for (int& t : a.lfoTail) t = r.i32();
  const int np2 = h0 >= 3 ? 6 : 5;
  for (int i = 0; i < np2; ++i) a.p2[i] = r.i32();
  for (auto& m : a.maps) {
    m = readList(r);
    for (int i = 0; i < 5; ++i) r.i32();
  }
  return a;
}

struct RawUnit {
  int u[10] = {};
  // operator
  int ratio = 20000, freq = 0, noise = 0;
  std::vector<float> lv, ph;
  int tail[5] = {0, 0, 0, 0, 128};
  uint8_t flags[8] = {};
  // filter
  int fb[12] = {};
  RawList ws;
  uint8_t wsBip = 0;
  std::vector<RawArt> arts;
};

struct RawSytrus {
  int ver = 0;
  int32_t main[240] = {};
  int soften = 0;
  uint8_t opt[2] = {};  // global option bytes (meaning not confirmed)
  uint8_t gibbs = 0;    // copied into every operator on load: ringing attenuation
  uint8_t delays[3][23] = {};
  int32_t rev[9] = {75, 40, 0, 40, 100, 15, 40, 2, 25};
  uint8_t revOn = 0, revTempo = 0;
  int eqTarget = 0;
  int eqb[3][4] = {{0, 8950, 17500, 0}, {0, 33145, 17500, 1}, {0, 49305, 17500, 2}};
  RawUnit units[mf::kUnits];
  size_t end = 0, size = 0;
};

bool walk(const uint8_t* d, size_t n, int ver, RawSytrus& s) {
  Reader r{d, n};
  s.ver = ver;
  s.size = n;
  for (int i = 0; i < 240; ++i) s.main[i] = r.i32();
  r.skip(2);  // last two matrix-mute bytes
  if (ver > 3) s.soften = r.i32();
  if (ver > 5) s.opt[0] = r.u8();
  if (ver > 6) s.opt[1] = r.u8();
  if (ver > 12) s.gibbs = r.u8();
  int n507 = 0;
  if (ver > 13) {
    n507 = ver > 14 ? 5 : 1;
    if (ver > 17) n507 += 3;
    if (ver > 18) n507 += 1;
  }
  r.skip(static_cast<size_t>(n507));
  for (auto& dl : s.delays) {
    const size_t at = r.skip(23);
    if (!r.bad) std::memcpy(dl, d + at, 23);
  }
  if (ver >= 10) {
    for (int& v : s.rev) v = r.i32();
    s.revOn = r.u8();
    if (ver > 10) s.revTempo = r.u8();
  }
  if (ver > 2) {
    s.eqTarget = r.i32();
    for (auto& b : s.eqb) {
      b[0] = r.i32(), b[1] = r.i32(), b[2] = r.i32();
      if (ver > 13) b[3] = r.i32();
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
  for (int u = 0; u < mf::kUnits && !r.bad; ++u) {
    RawUnit& U = s.units[u];
    for (int& v : U.u) v = r.i32();
    int na = 2;
    if (u < mf::kOps) {
      na = 6;
      int nh = 0;
      if (ver < 8) {
        U.ratio = r.i32(), U.freq = r.i32(), nh = 24;
      } else {
        U.ratio = r.i32(), U.freq = r.i32(), U.noise = r.i32(), nh = ver < 0x11 ? 128 : 256;
      }
      U.lv.resize(static_cast<size_t>(nh)), U.ph.resize(static_cast<size_t>(nh));
      for (float& v : U.lv) v = r.f32();
      for (float& v : U.ph) v = r.f32();
      for (int& v : U.tail) v = r.i32();
      if (ver < 8) {
        U.noise = r.i32();
        for (int i = 0; i < 7; ++i) U.flags[i] = r.u8();
      } else {
        for (uint8_t& f : U.flags) f = r.u8();
      }
    } else if (u < mf::kOps + mf::kFilters) {
      na = 8;
      const int nf = ver < 9 ? 10 : 12;
      for (int i = 0; i < nf; ++i) U.fb[i] = r.i32();
      if (ver < 9) {
        r.u8();
        U.fb[11] = 1;  // older versions: low-pass
      }
      U.ws = readList(r);
      for (int i = 0; i < 5; ++i) r.i32();
      if (ver >= 12) U.wsBip = r.u8();
    }
    for (int a = 0; a < na && !r.bad; ++a) U.arts.push_back(readArt(r));
  }
  r.skip(static_cast<size_t>((ver > 0x17) + (ver > 0x18)));
  s.end = r.at;
  return !r.bad && r.at == n;
}

double clampd(double v, double lo, double hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : lo; }

// FL's segment tension bends the opposite way to Matrix FM's: positive FL
// tension starts fast (most shipped decays use it), positive Matrix FM tension
// starts slow — so the sign flips.
mf::Curve toCurve(const RawList& l) {
  mf::Curve c;
  for (const RawPt& p : l.pts)
    c.pts.push_back({static_cast<float>(clampd(p.x, 0, 1000)), static_cast<float>(clampd(p.y, -1, 2)), static_cast<float>(-clampd(p.tension, -1, 1))});
  return c;
}

// LFO speed word (0..65536) → cycles per second (at 120 BPM for tempo LFOs).
double sytrusLfoHz(int v) {
  const double x = std::clamp(v, 0, 65536) / 65536.0;
  return (std::pow(10001.0, x) - 1.0) / 625.0;
}

bool flatAt(const RawList& l, double y) {
  if (l.pts.empty()) return true;
  for (const RawPt& p : l.pts)
    if (std::fabs(p.y - y) > 1e-4) return false;
  return true;
}

}  // namespace

bool decodeSytrusMatrix(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  if (!b.has(0, 8)) return false;
  const uint32_t hdr = b.u32(0);
  const int ver = static_cast<int>(hdr > 0xffff ? hdr >> 16 : hdr);
  std::vector<uint8_t> raw;
  if (!inflateZlib(b.sub(4, b.n - 4), raw, 4u << 20) || raw.size() < 1000) return false;
  RawSytrus S;
  if (!walk(raw.data(), raw.size(), ver, S)) return false;
  std::string name;
  for (const auto& e : events)
    if (e.id == 203) name = e.text();

  PluginState s = base("gl.matrixfm");
  mf::PatchData pd;
  const int32_t* M = S.main;
  setp(s, mf::MVolume, M[0] / 128.0);
  setp(s, mf::MLfoLevel, clampd(M[1] / 128.0, -1, 1));
  setp(s, mf::MPitch, clampd(M[2], -24, 24));
  // Matrix cells; a cell can be muted (one byte per cell from byte 800 of the
  // main block: frequency layer, then ring layer; v2+).
  auto muted = [&](int layer, int r, int col) {
    if (ver < 2) return false;
    const int byte = 800 + layer * 81 + r * 9 + col;
    return ((static_cast<uint32_t>(M[byte / 4]) >> (8 * (byte % 4))) & 0xff) != 0;
  };
  for (int r = 0; r < mf::kRows; ++r)
    for (int col = 0; col < mf::kCols; ++col) {
      setp(s, mf::fm(r, col), muted(0, r, col) ? 0.0 : clampd(M[3 + r * 9 + col] / 256.0, -1, 1));
      if (r < mf::kOps && col < mf::kOps) setp(s, mf::rm(r, col), muted(1, r, col) ? 0.0 : clampd(M[84 + r * 9 + col] / 256.0, -1, 1));
    }
  const int adsr[8] = {mf::MVolA, mf::MVolD, mf::MVolS, mf::MVolR, mf::MFltA, mf::MFltD, mf::MFltS, mf::MFltR};
  for (int i = 0; i < 8; ++i) setp(s, adsr[i], clampd(M[165 + i] / 128.0, i % 4 == 2 ? -1 : 0, i % 4 == 2 ? 1 : 2));
  setp(s, mf::MUniOrder, clampd(M[173], 1, 9));
  setp(s, mf::MUniPan, clampd(M[174] / 128.0, 0, 1));
  setp(s, mf::MUniVol, clampd(M[175] / 128.0, 0.125, 1));
  setp(s, mf::MUniPitch, clampd(M[176] / 256.0, 0, 1));
  setp(s, mf::MUniSub, clampd(M[177] / 128.0, 0, 0.75));
  setp(s, mf::MUniPhase, clampd(M[178] / 128.0, 0, 1));
  setp(s, mf::MUniEnv, clampd(M[179] / 128.0, 0, 1.5));
  setp(s, mf::FxPan, clampd(M[180] / 128.0, -1, 1));
  setp(s, mf::ChOrder, clampd(M[181], 0, 9));
  setp(s, mf::ChDepth, clampd(M[182] / 1024.0, 0, 1));
  setp(s, mf::ChSpeed, clampd(M[183] / 1024.0, 0, 1));
  setp(s, mf::ChDelay, clampd(M[184] / 128.0, 0, 1));
  setp(s, mf::ChSpread, clampd(M[185] / 128.0, 0, 1));
  setp(s, mf::ChCross, clampd(M[186] / 128.0, -1, 1));
  setp(s, mf::ChVol, clampd(M[187] / 128.0, -1, 1));
  setp(s, mf::MModX, clampd(M[190] / 512.0, 0, 1));
  setp(s, mf::MModY, clampd(M[191] / 512.0, 0, 1));
  setp(s, mf::MSoften, clampd(S.soften / 128.0, 0, 1));
  setp(s, mf::MGibbs, S.gibbs ? 1 : 0);
  // Delays.
  for (int i = 0; i < 3; ++i) {
    Reader r{S.delays[i], 23};
    const int fb = r.i32(), time = r.i32(), ofs = r.i32(), vol = r.i32(), mode = r.i32();
    const uint8_t tempo = r.u8(), mid = r.u8(), on = r.u8();
    (void)mid;
    setp(s, mf::dly(i, 0), on ? 1 : 0);
    setp(s, mf::dly(i, 1), clampd(mode, 0, 2));
    setp(s, mf::dly(i, 2), clampd(fb / 128.0, 0, 0.99));
    setp(s, mf::dly(i, 3), clampd(time / 192.0, 0.01, 4));
    setp(s, mf::dly(i, 4), clampd(ofs / 512.0, -1, 1));
    setp(s, mf::dly(i, 5), clampd(vol / 128.0, -1, 1));
    setp(s, mf::dly(i, 6), tempo ? 1 : 0);
  }
  // Reverb.
  setp(s, mf::RvLowCut, clampd(S.rev[0], 19, 3000));
  setp(s, mf::RvHighCut, clampd(S.rev[1] * 100.0, 500, 22000));
  // Predelay: 1/384 s, or 1/192 beat when tempo-synced.
  setp(s, mf::RvPreDelay, clampd(S.rev[2] / (S.revTempo ? 192.0 : 384.0), 0, 0.5));
  setp(s, mf::RvRoom, clampd(S.rev[3] / 100.0, 0.01, 1));
  setp(s, mf::RvDiff, clampd(S.rev[4] / 100.0, 0, 1));
  setp(s, mf::RvDecay, clampd(S.rev[5] / 10.0, 0.1, 20));
  setp(s, mf::RvHighDamp, clampd(S.rev[6] * 100.0, 500, 22000));
  setp(s, mf::RvColor, clampd(S.rev[7], 0, 4));
  setp(s, mf::RvVol, clampd(S.rev[8] / 128.0, -1, 1));
  setp(s, mf::RvOn, S.revOn ? 1 : 0);
  setp(s, mf::RvTempo, S.revTempo ? 1 : 0);
  // EQ.
  setp(s, mf::MEqTarget, clampd(S.eqTarget, 0, 3));
  for (int i = 0; i < 3; ++i) {
    setp(s, mf::eq(i, 0), clampd(S.eqb[i][3], 0, 2));
    setp(s, mf::eq(i, 1), clampd(20.0 * std::pow(1000.0, S.eqb[i][1] / 65536.0), 20, 20000));
    setp(s, mf::eq(i, 2), clampd(S.eqb[i][2] / 65536.0 * 4.0, 0.1, 4));
    setp(s, mf::eq(i, 3), clampd(S.eqb[i][0] / 100.0, -18, 18));
  }
  // Units.
  int envs = 0, lfos = 0, maps = 0;
  for (int u = 0; u < mf::kUnits; ++u) {
    const RawUnit& U = S.units[u];
    if (u < mf::kOps) {
      setp(s, mf::op(u, mf::OpPhase), clampd(U.u[0] / 256.0, 0, 1));
      setp(s, mf::op(u, mf::OpVol), clampd(U.u[1] / 256.0, -1, 1));
      setp(s, mf::op(u, mf::OpPitchRange), clampd(U.u[2] / 100.0, 0, 48));
      setp(s, mf::op(u, mf::OpRatio), clampd(U.ratio / 20000.0, 0, 32));
      setp(s, mf::op(u, mf::OpFreq), clampd(U.freq / 100.0, -5000, 5000));
      setp(s, mf::op(u, mf::OpNoise), clampd(U.noise / 128.0, 0, 1));
      setp(s, mf::op(u, mf::OpShape), clampd(U.tail[0] / 256.0, 0, 2));
      setp(s, mf::op(u, mf::OpTension), clampd(U.tail[1] / 128.0, -1, 1));
      setp(s, mf::op(u, mf::OpSkew), clampd(U.tail[2] / 128.0, 0, 1));
      setp(s, mf::op(u, mf::OpSine), clampd(U.tail[3] / 128.0, 0, 1));
      setp(s, mf::op(u, mf::OpFilter), clampd(U.tail[4] / 128.0, 0, 1));
      const int flagParams[7] = {mf::OpHalf, mf::OpEven, mf::OpAbs, mf::OpGlobal, mf::OpCenter, mf::OpDeclick, mf::OpPluck};
      for (int i = 0; i < 7; ++i) setp(s, mf::op(u, flagParams[i]), U.flags[i] ? 1 : 0);
      auto& lv = pd.level[static_cast<size_t>(u)];
      auto& ph = pd.phase[static_cast<size_t>(u)];
      lv.fill(0.f), ph.fill(0.f);
      for (size_t h = 0; h < U.lv.size() && h < lv.size(); ++h) {
        lv[h] = static_cast<float>(clampd(U.lv[h], 0, 1));
        const double p = std::isfinite(U.ph[h]) ? U.ph[h] : 0.0;
        ph[h] = static_cast<float>(p - std::floor(p));
      }
    } else if (u < mf::kOps + mf::kFilters) {
      const int j = u - mf::kOps;
      setp(s, mf::flt(j, mf::FlMode), clampd(U.u[0], 0, mf::kFltModes - 1));
      setp(s, mf::flt(j, mf::FlEnv), clampd(U.u[1] / 128.0, 0, 2));
      setp(s, mf::flt(j, mf::FlCut), clampd(U.u[2] / 256.0, 0, 1));
      setp(s, mf::flt(j, mf::FlRes), clampd(U.u[3] / 256.0, 0, 1));
      setp(s, mf::flt(j, mf::FlLow), clampd(U.u[4] / 128.0, 0, 1));
      setp(s, mf::flt(j, mf::FlBand), clampd(U.u[5] / 128.0, 0, 1));
      setp(s, mf::flt(j, mf::FlHigh), clampd(U.u[6] / 128.0, 0, 1));
      setp(s, mf::flt(j, mf::FlWsAmp), clampd(U.u[7] / 128.0, 0.5, 1.5));
      setp(s, mf::flt(j, mf::FlWsMix), clampd(U.u[8] / 128.0, 0, 1));
      setp(s, mf::flt(j, mf::FlNext), clampd(U.u[9] / 256.0, -1, 1));
      setp(s, mf::flt(j, mf::FlType), clampd(U.fb[11], 0, mf::kFltTypes - 1));
      setp(s, mf::flt(j, mf::FlWsOn), U.fb[10] ? 1 : 0);
      setp(s, mf::flt(j, mf::FlHq), (U.fb[0] & 0xff) ? 1 : 0);
      setp(s, mf::flt(j, mf::FlFlat), ((U.fb[0] >> 8) & 0xff) ? 1 : 0);
      setp(s, mf::flt(j, mf::FlWsBipolar), U.wsBip ? 1 : 0);
      bool straight = U.ws.pts.size() == 2 && std::fabs(U.ws.pts[0].y) < 1e-4 && std::fabs(U.ws.pts[1].y - 1) < 1e-4;
      if (!straight && !U.ws.pts.empty()) pd.ws[static_cast<size_t>(j)] = toCurve(U.ws);
    }
    for (int a = 0; a < static_cast<int>(U.arts.size()) && a < mf::unitArts(u); ++a) {
      const RawArt& A = U.arts[static_cast<size_t>(a)];
      mf::ArtData& ad = pd.arts[static_cast<size_t>(u)][static_cast<size_t>(a)];
      const bool envOn = A.envTail[1] != 0 && A.env.pts.size() >= 2;
      const bool lfoOn = A.lfoTail[1] != 0 && !A.lfo.pts.empty();
      // Curve tails: flags (bit 0 tempo, bit 1 global), enabled, decay point,
      // loop start, sustain point (= loop end).
      if (A.env.pts.size() >= 2) {
        ad.env = toCurve(A.env);
        const int n = static_cast<int>(ad.env.pts.size());
        const int sus = A.p1[0], dec = A.envTail[2], loop = A.envTail[3];
        ad.env.sustain = sus >= 0 && sus < n ? sus : -1;
        // No decay point: FL starts in the decay stage (no attack part).
        ad.env.decay = dec >= 0 && dec < n ? dec : 0;
        ad.env.loopStart = loop >= 0 && loop < ad.env.sustain ? loop : -1;
        ad.env.loopEnd = ad.env.loopStart >= 0 ? ad.env.sustain : -1;
        ad.env.tempo = (A.envTail[0] & 1) != 0;
      }
      if (!A.lfo.pts.empty()) ad.lfo = toCurve(A.lfo);
      const bool lfoTempo = (A.lfoTail[0] & 1) != 0;
      setp(s, mf::art(u, a, mf::AfEnvOn), envOn ? 1 : 0);
      setp(s, mf::art(u, a, mf::AfLfoOn), lfoOn ? 1 : 0);
      setp(s, mf::art(u, a, mf::AfAtt), clampd(A.p1[1] / 128.0, 0, 2));
      setp(s, mf::art(u, a, mf::AfDec), clampd(A.p1[2] / 128.0, 0, 2));
      setp(s, mf::art(u, a, mf::AfSus), clampd(A.p1[3] / 128.0, -1, 1));
      setp(s, mf::art(u, a, mf::AfRel), clampd(A.p1[4] / 128.0, 0, 2));
      // LFO record: sustain, speed, tension, skew, pulse width (/128), phase (u32, v3+).
      const double hz = sytrusLfoHz(A.p2[1]);
      setp(s, mf::art(u, a, mf::AfLfoSpeed), clampd(lfoTempo ? hz * 0.5 : hz, 0.01, 50));  // tempo: cycles per beat (drawn at 120 BPM)
      setp(s, mf::art(u, a, mf::AfTempo), lfoTempo ? 1 : 0);
      setp(s, mf::art(u, a, mf::AfLfoGlobal), (A.lfoTail[0] & 2) ? 1 : 0);
      setp(s, mf::art(u, a, mf::AfLfoSkew), clampd(A.p2[3] / 128.0, -1, 1));
      ad.lfoTension = static_cast<float>(clampd(A.p2[2] / 128.0, -1, 1));
      ad.lfoPw = static_cast<float>(clampd(A.p2[4] / 128.0, -1, 1));
      ad.lfoPhase = A.h0 >= 3 ? static_cast<float>(static_cast<uint32_t>(A.p2[5]) / 4294967296.0) : 0.f;
      envs += envOn, lfos += lfoOn;
      const bool bip = mf::artBipolar(u, a);
      for (int k = 0; k < mf::kMaps; ++k) {
        const RawList& L = A.maps[k];
        if (flatAt(L, bip ? 0.5 : 1.0)) continue;
        // Operator damping's default key ramp is neutral here.
        if (u < mf::kOps && a == mf::OaDamp && k == mf::MapKey && L.pts.size() == 2 && std::fabs(L.pts[0].y - 0.0625) < 0.002 &&
            std::fabs(L.pts[1].y - 0.9375) < 0.002)
          continue;
        ad.maps[static_cast<size_t>(k)] = toCurve(L);
        ++maps;
      }
    }
  }
  if (!c.paramsOnly) s.data = mf::serializePatch(pd);
  if (s.data == "mfm 1\n") s.data.clear();  // J-39: Matrix FM's defaults are no data (as a fresh instance)
  m.glType = "gl.matrixfm";
  m.state = s;
  m.exact = false;
  note(c, "Sytrus", "\"" + name + "\" (v" + std::to_string(ver) + ") → Matrix FM: matrix, operators (harmonics, shapes), filters, " +
                        std::to_string(envs) + " envelopes, " + std::to_string(lfos) + " LFOs, " + std::to_string(maps) +
                        " mapping curves, unison and effects decoded; Gridline's own DSP (approximate)");
  // Automation: mod X / Y (18 / 19), filter cutoff / resonance (74 parameters per unit from 326).
  ParamTable t;
  t.lin(18, mf::MModX, 0, 1);
  t.lin(19, mf::MModY, 0, 1);
  t.range(18, 0, 512), t.range(19, 0, 512);
  for (int j = 0; j < mf::kFilters; ++j) {
    t.lin(326 + 74 * j + 2, mf::flt(j, mf::FlCut), 0, 1);
    t.lin(326 + 74 * j + 3, mf::flt(j, mf::FlRes), 0, 1);
    t.range(326 + 74 * j + 2, 0, 256), t.range(326 + 74 * j + 3, 0, 256);
  }
  t.install(m);
  return true;
}

}  // namespace gl::flp::gen
