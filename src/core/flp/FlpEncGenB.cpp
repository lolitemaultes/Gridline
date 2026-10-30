// Gridline — FLP export: plugin state encoders for FlpGen2Controllers/Pads/Slicers/SynthsA/SynthsB.cpp
// (the plugins of those files that no later decoder wave replaces): Keyboard
// Controller, Voltage Controller, FPC, DrumSynth Live, Fruity Slicer, Slicex,
// BeepMap, MiniSynth, Toxic Biohazard.
// The reverse of the decoders in those files; see FlpEncode.h for the model.
// Layouts are the ones documented in the decoders (offsets / types / ranges);
// value laws are never inverted here — the engine solves every field through
// the decoder.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include "../../engine/KerfGrid.h"
#include "../../engine/KerfMarks.h"
#include "../../engine/SlicexEngine.h"
#include "../../engine/Sample.h"
#include "../../engine/Slicing.h"
#include "../../packs/models/models.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/modulation/modulation_dsp.h"
#include "../../packs/synths/hex.h"
#include "../../packs/synths/pixel.h"
#include "../PluginParams.h"
#include "FlpEncode.h"
#include "FlpGen2.h"

namespace gl::packs::synths {
std::string pixelToneDefaultData();  // pixel_info.cpp
}

namespace gl::flp::enc {

namespace px = gl::packs::synths::pixel;

namespace {

namespace mod = gl::packs::modulation;
namespace md = gl::packs::models;

// ------------------------------------------------------------------ helpers

int32_t rdI32(const Bytes& b, size_t at) { return at + 4 <= b.size() ? static_cast<int32_t>(getU32(b, at)) : 0; }
uint32_t rdU32(const Bytes& b, size_t at) { return at + 4 <= b.size() ? getU32(b, at) : 0; }
void putBytes(ByteWriter& w, const char* s) { w.bytes(s, std::strlen(s)); }

float param(const PluginState& g, int k, float def = 0) {
  return k >= 0 && static_cast<size_t>(k) < g.params.size() ? g.params[static_cast<size_t>(k)] : def;
}

// Decodes `state` as `flName` (false when the decoder rejects it).
bool decodeAs(const std::string& flName, const Bytes& state, PluginMapping& m, const EncodeContext& c) {
  return decodeState(flName, state, m, c);
}

// Joint search for parameters the decoder derives from several stored
// integers (the engine solves one stored value at a time). Every value of
// the `outer` stored i32 is tried; for each, the `inner` stored values are
// bisected in turn (each against its parameter, through the decoder) and
// the combination is kept when every `check` parameter decodes to the
// target. The state is restored when nothing matches.
struct Stored {
  size_t at;
  int lo, hi;
  int gl = -1;  // inner: the parameter it is bisected against
};
struct Joint {
  const std::string& flName;
  const EncodeContext& c;
  const PluginState& g;
  Bytes& b;
  PluginMapping m;
  // J-51: optional: parameter k for the current bytes from the decoder's own
  // law, without decoding (false: decode). setVerifyLaws checks it.
  std::function<bool(const Bytes& b, int k, float& v)> law = {};
  bool lawValue(int k, float& v) {
    if (!law || !law(b, k, v)) return false;
    if (verifyLaws()) {
      PluginMapping d;
      const float got = decodeAs(flName, b, d, c) ? param(d.state, k, NAN) : NAN;
      if (!(got == v || (std::isnan(got) && std::isnan(v)))) {
        std::fprintf(stderr, "flp encoder: %s param %d joint law differs from the decoder (%.9g vs %.9g)\n", flName.c_str(), k, got, v);
        noteLawMismatch();
        return false;
      }
    }
    return true;
  }
  float value(int k) {
    float v;
    if (lawValue(k, v)) return v;
    return decodeAs(flName, b, m, c) ? param(m.state, k, NAN) : NAN;
  }
  bool matches(const std::vector<int>& ks) {
    bool viaLaw = static_cast<bool>(law);
    for (int k : ks) {
      float v;
      if (!viaLaw) break;
      if (!lawValue(k, v)) viaLaw = false;
      else if (v != param(g, k, NAN)) return false;
    }
    if (viaLaw) return true;
    if (!decodeAs(flName, b, m, c)) return false;
    for (int k : ks)
      if (param(m.state, k, NAN) != param(g, k, NAN)) return false;
    return true;
  }
  // Bisection of one stored i32 for parameter s.gl (monotonic law assumed).
  // A target held over a range of stored values (a clamped law) is taken at
  // the edge of that range, next to where the value starts to move.
  bool bisect(const Stored& s) {
    const float want = param(g, s.gl, NAN);
    auto at = [&](int r) {
      putI32(b, s.at, r);
      return value(s.gl);
    };
    int lo = s.lo, hi = s.hi;
    const float vlo = at(lo), vhi = at(hi);
    if (vlo == want && vhi == want) return true;
    if (vlo == want || vhi == want) {
      int in = vlo == want ? lo : hi, out = vlo == want ? hi : lo;  // value(in) == want, value(out) != want
      while (std::abs(out - in) > 1) {
        const int mid = in + (out - in) / 2;
        (at(mid) == want ? in : out) = mid;
      }
      at(in);
      return true;
    }
    if (!(vlo != vhi)) return false;
    const bool inc = vhi > vlo;
    while (hi - lo > 1) {
      const int mid = lo + (hi - lo) / 2;
      const float v = at(mid);
      if (v == want) return true;
      if ((v < want) == inc) lo = mid;
      else hi = mid;
    }
    return false;
  }
  // `preset`: stored values set first (offset, value).
  bool nested(const Stored& outer, const std::vector<Stored>& inner, const std::vector<int>& check,
              const std::vector<std::pair<size_t, int32_t>>& preset = {}) {
    if (matches(check)) return true;
    const Bytes keep = b;
    for (const auto& [at, val] : preset) putI32(b, at, val);
    for (int o = outer.lo; o <= outer.hi; ++o) {
      putI32(b, outer.at, o);
      for (const Stored& s : inner) bisect(s);
      if (matches(check)) return true;
    }
    b = keep;
    return false;
  }
};

// ------------------------------------------------------- Keyboard Controller
// u32 version 2, i32 attack, i32 release (0..256), f32 value[132] (keys
// 0..131), i32 zone low, i32 zone high, i32 0, u8 flag. Version-less states
// (FL 4) start with attack / release; the 13-byte tail is the same.
constexpr size_t kKcTail = 13;
constexpr int kKcKeys = 132;

bool kcVersioned(const Bytes& b) { return rdU32(b, 0) == 2 && b.size() >= 12 + kKcTail; }

Encoder keyboardController() {
  Encoder e;
  e.flName = "Fruity Keyboard Controller";
  e.glType = "gl.keyctl";
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = [] {
    constexpr uint32_t kVersion = 2;
    constexpr int kZoneLo = 48, kZoneHi = 71;  // selected zone: C3..B4
    ByteWriter w;
    w.u32(kVersion);
    w.i32(0);  // attack smoothing: instant
    w.i32(0);  // release smoothing: instant
    // Default key map: a ramp over the white keys of the zone (a black key
    // takes the value of the white key below it), 0 below and 1 above.
    auto white = [](int k) {
      const int n = k % 12;
      return n == 0 || n == 2 || n == 4 || n == 5 || n == 7 || n == 9 || n == 11;
    };
    int steps = -1;
    for (int k = kZoneLo; k <= kZoneHi; ++k) steps += white(k) ? 1 : 0;
    for (int k = 0; k < kKcKeys; ++k) {
      if (k < kZoneLo) {
        w.f32(0.0f);
      } else if (k > kZoneHi) {
        w.f32(1.0f);
      } else {
        int idx = -1;
        for (int j = kZoneLo; j <= k; ++j) idx += white(j) ? 1 : 0;
        w.f32(static_cast<float>(std::max(0, idx)) / static_cast<float>(steps));
      }
    }
    w.i32(kZoneLo);
    w.i32(kZoneHi);
    w.i32(0);  // reserved
    w.u8(0);   // flag
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    const size_t at = kcVersioned(b) ? 4 : 0;
    return std::vector<Field>{i32(mod::KcAttack, at, 0, 256), i32(mod::KcRelease, at + 4, 0, 256)};
  };
  // Key values (the data text): rewritten where they differ.
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext&) {
    if (b.size() < kKcTail + 8) return;
    const size_t keysAt = kcVersioned(b) ? 12 : 8, keysEnd = b.size() - kKcTail;
    const std::vector<float> want = mod::parseKeyValues(g.data);
    for (size_t k = 0; k < 128 && keysAt + 4 * k + 4 <= keysEnd; ++k) {
      const float cur = std::clamp(getF32(b, keysAt + 4 * k), 0.0f, 1.0f);
      const std::vector<float> one = mod::parseKeyValues(mod::formatKeyValues({cur}));
      if (std::isfinite(cur) && one[0] == want[k]) continue;
      putF32(b, keysAt + 4 * k, want[k]);
    }
  };
  return e;
}

// -------------------------------------------------------- Voltage Controller
// u32 version (2), i32 offset[8] (0..65536, 32768 = 0 V), i32 scale[8]
// (0..65536), u8 source[8], u8 enabled, 22 bytes of per-output options, i32
// clock PPQ at 0x63, i32 pulse length (samples) at 0x67, f64 pitch scaling
// factor at 0x6B (version 2).
Encoder voltageController() {
  Encoder e;
  e.flName = "Fruity Voltage Controller";
  e.glType = "gl.cvctl";
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = [] {
    constexpr uint32_t kVersion = 2;
    constexpr int32_t kOffsetZero = 32768, kScaleFull = 65536;
    // Sources per output (main L/R, out 2, 3, 4): pitch, gate, velocity,
    // panning, mod X, mod Y, none, none.
    constexpr uint8_t kSources[8] = {0, 1, 3, 5, 6, 7, 9, 9};
    constexpr int32_t kClockPpq = 24, kPulseLength = 15;
    constexpr double kPitchScale = 1.0;  // 1 V/oct
    ByteWriter w;
    w.u32(kVersion);
    for (int i = 0; i < 8; ++i) w.i32(kOffsetZero);
    for (int i = 0; i < 8; ++i) w.i32(kScaleFull);
    for (uint8_t s : kSources) w.u8(s);
    w.u8(0);       // output disabled
    w.zeros(22);   // per-output polarity / colour options
    w.i32(kClockPpq);
    w.i32(kPulseLength);
    w.f64(kPitchScale);
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> f = {
        i32(mod::CvLeftOfs, 4, 0, 65536),          i32(mod::CvRightOfs, 8, 0, 65536),
        i32(mod::CvLeftScale, 0x24, 0, 65536),     i32(mod::CvRightScale, 0x28, 0, 65536),
        field(mod::CvLeftSrc, 0x44, Type::U8, 0, 9), field(mod::CvRightSrc, 0x45, Type::U8, 0, 9),
        field(mod::CvEnable, 0x4C, Type::U8, 0, 1),
    };
    if (b.size() >= 0x67) f.push_back(i32(mod::CvClockPpq, 0x63, 2, 96));
    if (b.size() >= 0x6B) f.push_back(i32(mod::CvPulse, 0x67, 1, 8820));
    if (b.size() >= 0x73) {
      Field p = field(mod::CvPitchScale, 0x6B, Type::F64, 1.0, 1.2);
      p.inverse = [](double gl) { return gl >= 0.5 ? 1.2 : 1.0; };
      f.push_back(p);
    }
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ MiniSynth
// "SSLF", u32 size, then an FLS chunk file: magic, "HEAD" {u16 1, u16 1,
// u16 name length, UTF-16 name}, "DATA" {"0rev", 43 × f32 0..1 in the
// plugin's parameter order}.
enum MiniParam {
  MnVoicesMode, MnTranspose, MnSlide, MnMwDest, MnMwAmount, MnOscKind, MnOscMod, MnNoise,
  MnCutoff, MnPeak, MnKbd, MnVel, MnFAtk, MnFDec, MnFSus, MnFRel, MnFAmt,
  MnLfoShape, MnLfoDest, MnLfoRate, MnLfoAmt, MnAAtk, MnADec, MnASus, MnARel,
  MnOverdrive, MnDecimator, MnDist, MnDelay, MnDelayTime, MnDelayFb, MnDelayFilter, MnDelayMix,
  MnChorus, MnChorusRate, MnChorusDepth, MnChorusMix, MnPhaser, MnPhaserSpeed, MnPhaserWidth, MnPhaserFb, MnPhaserMix,
  MnPhaserFreq, MnCount
};

Bytes miniSynthDefaults() {
  float v[MnCount] = {};
  v[MnTranspose] = 0.5f;  // no transposition
  v[MnCutoff] = 1.0f;     // filter open
  v[MnFDec] = 1.0f;
  v[MnFSus] = 1.0f;
  v[MnFAmt] = 0.6796296f;
  v[MnLfoRate] = 0.3736363f;
  v[MnLfoAmt] = 0.5f;  // centre = no LFO
  v[MnAAtk] = 0.02f;
  v[MnADec] = 1.0f;
  v[MnASus] = 1.0f;
  v[MnARel] = 0.05f;
  v[MnDecimator] = 0.1555556f;
  v[MnDelayTime] = 0.6666667f;
  v[MnDelayMix] = 1.0f;
  v[MnChorusRate] = 0.4981481f;
  v[MnChorusDepth] = 0.5273076f;
  v[MnChorusMix] = 1.0f;
  v[MnPhaserSpeed] = 0.3736363f;
  v[MnPhaserMix] = 1.0f;
  ByteWriter data;
  putBytes(data, "0rev");
  for (float x : v) data.f32(x);
  const std::string name = "Default";
  ByteWriter head;
  head.u16(1);
  head.u16(1);
  head.u16(static_cast<uint32_t>(name.size()));
  for (char ch : name) head.u16(static_cast<uint8_t>(ch));
  ByteWriter fls;
  const uint8_t magic[8] = {0x99, 'F', 'L', 'S', '\r', '\n', 0x1A, '\n'};
  fls.bytes(magic, sizeof magic);
  putBytes(fls, "HEAD");
  fls.u32(static_cast<uint32_t>(head.b.size()));
  fls.bytes(head.b);
  putBytes(fls, "DATA");
  fls.u32(static_cast<uint32_t>(data.b.size()));
  fls.bytes(data.b);
  ByteWriter w;
  putBytes(w, "SSLF");
  w.u32(static_cast<uint32_t>(fls.b.size()));
  w.bytes(fls.b);
  return w.b;
}

Encoder miniSynth() {
  Encoder e;
  e.flName = "MiniSynth";
  e.glType = "gl.pocketsynth";  // J-33: the reference engine, parameters 1:1
  e.nativeRank = 0;             // J-39: its FL home (also without the origin)
  e.generator = true;
  e.defaults = miniSynthDefaults;
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    static const char kTag[] = "0rev";
    auto it = std::search(b.begin(), b.end(), kTag, kTag + 4);
    if (it == b.end()) return f;
    const size_t at = static_cast<size_t>(it - b.begin()) + 4;
    for (int i = 0; i < MnCount; ++i) {
      Field x = field(i, at + 4 * static_cast<size_t>(i), Type::F32, 0.0, 1.0);
      x.inverse = [](double gl) { return gl; };
      f.push_back(x);
    }
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ BeepMap
// i32 version 0, i32 per control tag 0..6 (range, pixel length, scale, use
// blue, grainy, loop, widen), 7 reserved words, i32 max bitmap height, u32
// bitmap size, then the bitmap (Windows BMP).
enum BeepTag { BmRange, BmLength, BmScale, BmUseBlue, BmGrainy, BmLoop, BmWiden, BmTags };

// The picture as a 24-bit bottom-up BMP.
Bytes pictureBmp(const px::Picture& p) {
  const int w = std::max(1, p.w), h = std::max(1, p.h);
  const uint32_t stride = static_cast<uint32_t>((w * 3 + 3) & ~3), pixBytes = stride * static_cast<uint32_t>(h), off = 54;
  ByteWriter b;
  putBytes(b, "BM");
  b.u32(off + pixBytes), b.u32(0), b.u32(off);
  b.u32(40), b.i32(w), b.i32(h), b.u16(1), b.u16(24), b.u32(0), b.u32(pixBytes), b.u32(0), b.u32(0), b.u32(0), b.u32(0);
  for (int y = h - 1; y >= 0; --y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(p.w) + static_cast<size_t>(x)) * 3;
      const bool in = y < p.h && x < p.w && i + 2 < p.rgb.size();
      b.u8(in ? p.rgb[i + 2] : 0), b.u8(in ? p.rgb[i + 1] : 0), b.u8(in ? p.rgb[i] : 0);
    }
    b.zeros(stride - static_cast<uint32_t>(w * 3));
  }
  return b.b;
}

Encoder beepMap() {
  Encoder e;
  e.flName = "BeepMap";
  e.glType = "gl.pixeltone";  // J-33: the reference engine
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = [] {
    constexpr int32_t kVersion = 0, kRange = 7200, kLength = 2048, kScale = 0, kUseBlue = 0, kGrainy = 0, kLoop = 1, kWiden = 0, kMaxHeight = 23;
    ByteWriter w;
    w.i32(kVersion);
    for (int32_t v : {kRange, kLength, kScale, kUseBlue, kGrainy, kLoop, kWiden}) w.i32(v);
    w.zeros(4 * 7);  // reserved
    w.i32(kMaxHeight);
    px::Picture pic;
    px::parsePicture(gl::packs::synths::pixelToneDefaultData(), pic);
    const Bytes bmp = pictureBmp(pic);
    w.u32(static_cast<uint32_t>(bmp.size()));
    w.bytes(bmp);
    return w.b;
  };
  e.fields = [](const Bytes&) {
    static const int lo[7] = {24, 0, 0, 0, 0, 0, 0}, hi[7] = {9600, 4096, 2, 1, 1, 1, 1};
    std::vector<Field> f;
    for (int t = 0; t < 7; ++t) f.push_back(i32(t, static_cast<size_t>(4 + 4 * t), lo[t], hi[t]));
    f.push_back(i32(px::PxMaxHeight, 60, 3, 999));
    return f;
  };
  // The picture (PluginState::data) and, in version 1 states, the channel
  // envelope / filter copy (PluginState::ins).
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginMapping m;
    if (b.size() < 68 || !decodeAs("BeepMap", b, m, c)) return;
    const int32_t ver = rdI32(b, 0);
    const size_t at = ver == 0 ? 64 : 428;
    if (ver == 1 && m.state.ins != g.ins) writeInsCopy(b, 64, 404, g.ins);
    if (m.state.data == g.data) return;
    px::Picture pic;
    if (!px::parsePicture(g.data, pic) || at + 4 > b.size()) return;
    const Bytes bmp = pictureBmp(pic);
    b.resize(at);
    ByteWriter n;
    n.u32(static_cast<uint32_t>(bmp.size()));
    b.insert(b.end(), n.b.begin(), n.b.end());
    b.insert(b.end(), bmp.begin(), bmp.end());
    if (rdI32(b, 60) < pic.h) putI32(b, 60, std::min(999, pic.h));
  };
  return e;
}

// ------------------------------------------------- Fruity DrumSynth Live
// u32 version (4), i32 selected key, 120 Pascal patch names (keys 0..119),
// 120 records of 38 × i32 (patch parameters in tag order), u32 1. Version 1
// (FL 3): 24 patches on keys 60..83, names as 24-byte fields. The decoder
// reads 16 pads from the patches of the keys the channel plays — the selected
// key when nothing is played (the export case), at most key 112.
constexpr int kDslKeys = 120, kDslParams = 38, kDslV1Patches = 24, kDslV1Key = 60, kDslMaxBase = 112;
constexpr size_t kDslRecord = 4 * kDslParams;

struct DslLayout {
  bool ok = false, v1 = false;
  size_t recAt = 0;
  int base = 0;
  // Offset of patch parameter i of key k (npos when the state has no such patch).
  size_t at(int key, int i) const {
    if (v1) {
      if (key < kDslV1Key || key >= kDslV1Key + kDslV1Patches) return std::string::npos;
      key -= kDslV1Key;
    }
    return recAt + static_cast<size_t>(key) * kDslRecord + 4 * static_cast<size_t>(i);
  }
};

DslLayout dslLayout(const Bytes& b) {
  DslLayout l;
  if (b.size() < 8) return l;
  if (rdU32(b, 0) == 1 && b.size() == 8 + kDslV1Patches * (24 + kDslRecord)) {
    l.ok = l.v1 = true;
    l.recAt = 8 + kDslV1Patches * 24;
    l.base = kDslV1Key;
    return l;
  }
  size_t p = 8;
  for (int k = 0; k < kDslKeys; ++k) {
    if (p >= b.size()) return l;
    p += 1 + b[p];
  }
  if (p + kDslKeys * kDslRecord > b.size()) return l;
  l.ok = true;
  l.recAt = p;
  l.base = std::min(std::clamp(rdI32(b, 4), 0, kDslKeys - 1), kDslMaxBase);
  return l;
}

// J-51: a pad parameter from the patch it reads, through the decoder's law
// (gen2::drumSynthLivePad), then as decodeState leaves it (float, clamped to
// the parameter's range). False when the pad has no patch here.
// (`over`: an offset whose stored value is taken as `overVal` instead.)
bool dslLawAt(const Bytes& b, int k, size_t over, int32_t overVal, float& v) {
  const int rel = k - md::DmCount;
  if (rel < 0 || rel >= md::kDrumPads * md::DpCount) return false;
  const int pad = rel / md::DpCount, p = rel % md::DpCount;
  const DslLayout l = dslLayout(b);
  const int key = l.base + pad;
  if (!l.ok || key >= kDslKeys || l.at(key, 0) == std::string::npos || l.at(key, kDslParams - 1) + 4 > b.size()) return false;
  int32_t patch[kDslParams];
  for (int i = 0; i < kDslParams; ++i) patch[i] = l.at(key, i) == over ? overVal : rdI32(b, l.at(key, i));
  const gen2::DslPadValues o = gen2::drumSynthLivePad(patch);
  const PluginInfo* info = findPlugin("gl.drumkit");
  if (!info || static_cast<size_t>(k) >= info->params.size()) return false;
  for (int i = o.n - 1; i >= 0; --i)  // (the last setting of p wins, as in the decoder)
    if (o.param[i] == p) {
      v = static_cast<float>(info->params[static_cast<size_t>(k)].clamp(static_cast<float>(o.value[i])));
      return true;
    }
  return false;
}

bool dslLaw(const Bytes& b, int k, float& v) { return dslLawAt(b, k, std::string::npos, 0, v); }

Encoder drumSynthLive() {
  Encoder e;
  e.flName = "Fruity DrumSynth Live";
  e.glType = "gl.drumkit";
  e.nativeRank = 20;  // lossy (one modelled drum per patch): a fallback home
  e.generator = true;
  e.defaults = [] {
    constexpr uint32_t kVersion = 4;
    constexpr int32_t kSelected = 60;
    // One neutral patch (the editor's defaults, tag order) on every key.
    int32_t p[kDslParams] = {};
    p[0] = 75;   // level 0..128
    p[4] = 500;  // osc 1 frequency 0..1000
    p[5] = 64;   // osc 1 bandwidth
    p[6] = 64;   // osc 1 sweep
    p[7] = 64;   // osc 1 decay
    p[8] = 64;   // osc 1 level
    p[9] = 64;   // osc 2 envelope shape
    p[10] = 500; // osc 2 frequency
    p[11] = 64;  // osc 2 bandwidth
    p[12] = 64;  // osc 2 attack
    p[13] = 64;  // osc 2 decay
    p[14] = 64;  // osc 2 level
    p[15] = 8;   // waveform
    p[17] = 64;  // noise <> wave mix
    p[19] = 64;  // retrigger rate
    p[20] = 64;  // retrigger decay
    p[21] = 64;  // release
    p[33] = 64;  // ring modulation
    p[35] = 50;  // send level
    p[37] = 64;  // morph amount
    ByteWriter w;
    w.u32(kVersion);
    w.i32(kSelected);
    for (int k = 0; k < kDslKeys; ++k) w.u8(0);  // unnamed patches
    for (int k = 0; k < kDslKeys; ++k)
      for (int32_t v : p) w.i32(v);
    w.u32(1);
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    const DslLayout l = dslLayout(b);
    if (!l.ok) return f;
    for (int pad = 0; pad < md::kDrumPads; ++pad) {
      const int key = l.base + pad;
      if (key >= kDslKeys || l.at(key, 0) == std::string::npos) continue;
      // (J-51: with the decoder's law for the solver, see dslLaw.)
      auto fp = [&](int p, int i, double lo, double hi) {
        const int k = md::drumPad(pad, p);
        const size_t at = l.at(key, i);
        f.push_back(lawField(i32(k, at, lo, hi), [k, at](double r, const Bytes& st) {
          float v;
          return dslLawAt(st, k, at, static_cast<int32_t>(r), v) ? static_cast<double>(v) : std::numeric_limits<double>::quiet_NaN();
        }));
      };
      // Shape and the oscillator levels first: they decide how the rest is read.
      fp(md::DpShape, 32, 0, 1);
      fp(md::DpNoise, 14, 0, 127);
      fp(md::DpNoise, 8, 0, 127);
      fp(md::DpNoise, 14, 0, 127);
      fp(md::DpLevel, 0, 0, 128);
      fp(md::DpPan, 1, -64, 64);
      fp(md::DpFreq, 4, 0, 1000);
      fp(md::DpDecay, 7, 0, 127);
      fp(md::DpMaterial, 3, -50, 50);
      fp(md::DpDrive, 3, -50, 50);
      fp(md::DpBend, 6, 0, 127);
      fp(md::DpBendTime, 6, 0, 127);
      fp(md::DpClick, 23, 0, 100);
      fp(md::DpNoiseDecay, 13, 0, 127);
      fp(md::DpNoiseTone, 10, 0, 1000);
      fp(md::DpNoiseTone, 2, -50, 50);
      fp(md::DpChoke, 24, 0, 9);
    }
    return f;
  };
  // The pads start at the selected key. Pad level, noise, shape and noise
  // tone are each read from two or three patch values: searched jointly.
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    DslLayout l = dslLayout(b);
    if (!l.ok) return;
    if (!l.v1) {
      const int want = std::clamp(static_cast<int>(std::lround(param(g, md::DmBaseNote, 60))), 0, kDslMaxBase);
      if (want != l.base) putI32(b, 4, want), l = dslLayout(b);
    }
    Joint j{"Fruity DrumSynth Live", c, g, b, {}, {}};
    j.law = dslLaw;
    for (int pad = 0; pad < md::kDrumPads; ++pad) {
      const int key = l.base + pad;
      if (key >= kDslKeys || l.at(key, 0) == std::string::npos) continue;
      auto v = [&](int i) { return l.at(key, i); };
      const int pLevel = md::drumPad(pad, md::DpLevel), pNoise = md::drumPad(pad, md::DpNoise);
      const int pShape = md::drumPad(pad, md::DpShape), pTone = md::drumPad(pad, md::DpNoiseTone);
      j.nested({v(2), -50, 50}, {{v(10), 0, 1000, pTone}}, {pTone});
      const bool plate = static_cast<int>(std::lround(param(g, pShape))) == md::DsPlate;
      const std::vector<int> check = {pShape, pNoise, pLevel};
      for (int osc1Noise : {0, 1}) {
        if (osc1Noise && !plate) break;
        putI32(b, v(32), osc1Noise);
        // (Noise held at its maximum by several osc 2 levels: searched the other way round.)
        if (!plate && (j.nested({v(8), 0, 127}, {{v(14), 0, 127, pNoise}, {v(0), 0, 128, pLevel}}, check) ||
                       j.nested({v(14), 0, 127}, {{v(8), 0, 127, pNoise}, {v(0), 0, 128, pLevel}}, check)))
          break;
        // (Plate: the level follows the louder oscillator; the other one is silenced.)
        if (plate && (j.nested({v(8), 0, 127}, {{v(0), 0, 128, pLevel}}, check) ||
                      j.nested({v(14), 0, 127}, {{v(0), 0, 128, pLevel}}, check) ||
                      j.nested({v(0), 0, 128}, {{v(14), 0, 127, pLevel}}, check, {{v(8), 0}}) ||
                      j.nested({v(0), 0, 128}, {{v(8), 0, 127, pLevel}}, check, {{v(14), 0}})))
          break;
      }
    }
  };
  return e;
}

// ------------------------------------------------------------- sample files
// Frames of a sample file the way the slicer decoders count them: linear
// WAV = data size / block size, compressed WAV = "fact" frames, other
// formats through the sample cache.
uint64_t fileFrames(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return 0;
  Bytes b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (b.size() >= 12 && std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(b.data() + 8, "WAVE", 4) == 0) {
    const size_t end = std::min<size_t>(b.size(), static_cast<size_t>(getU32(b, 4)) + 8);
    uint32_t tag = 0, align = 0, data = 0, fact = 0;
    bool haveData = false;
    for (size_t q = 12; q + 8 <= end;) {
      const uint32_t len = getU32(b, q + 4);
      if (std::memcmp(b.data() + q, "fmt ", 4) == 0 && q + 22 <= b.size()) tag = getU16(b, q + 8), align = getU16(b, q + 20);
      else if (std::memcmp(b.data() + q, "fact", 4) == 0 && q + 12 <= b.size()) fact = getU32(b, q + 8);
      else if (std::memcmp(b.data() + q, "data", 4) == 0) data = len, haveData = true;
      if (len > end - q - 8) break;
      q += 8 + len + (len & 1);
    }
    const bool linear = tag == 1 || tag == 3 || tag == 0xFFFE;
    if (haveData && linear && align > 0) return data / align;
    if (!linear && fact > 0) return fact;
  }
  SamplePtr s = sampleCache().get(path);
  return s ? s->frames() : 0;
}

// Slice markers of a gl.slicer state (positions 0..1, until the first unused one).
std::vector<double> slicerMarkers(const PluginState& g) {
  std::vector<double> v;
  for (int k = 0; k < pp::kMaxSlices; ++k) {
    const double x = param(g, pp::slMarker(k), -1);
    if (x < 0) break;
    v.push_back(x);
  }
  return v;
}

bool sameMarkers(const PluginState& a, const PluginState& b) {
  for (int k = 0; k < pp::kMaxSlices; ++k)
    if (param(a, pp::slMarker(k), -1) != param(b, pp::slMarker(k), -1)) return false;
  return true;
}

// Pascal (one length byte) string of UTF-8 text, at most 255 bytes.
void putPascal(Bytes& out, const std::string& s) {
  const size_t n = std::min<size_t>(s.size(), 255);
  out.push_back(static_cast<uint8_t>(n));
  out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

// ----------------------------------------------------------- Fruity Slicer
// u32 version, f32 beats, f32 tempo, i32 pitch (cents), 4 × i32 options,
// Pascal sample path at 32, i32 slice count, slices {Pascal name, i32 start
// frame, i32 end (-1 = next slice), f32 (-1), u8}, then the tail: u8, u8 base
// note, … (version 16 adds articulator settings to the tail).
struct FsLayout {
  bool ok = false;
  size_t countAt = 0, tailAt = 0;
  std::vector<size_t> slice;  // offset of each slice record (its name)
};

FsLayout fsLayout(const Bytes& b) {
  FsLayout l;
  if (b.size() < 37) return l;
  l.countAt = 33 + static_cast<size_t>(b[32]);
  const int32_t count = rdI32(b, l.countAt);
  size_t p = l.countAt + 4;
  for (int32_t k = 0; k < count && k < 4096; ++k) {
    if (p >= b.size()) return l;
    l.slice.push_back(p);
    p += 1 + static_cast<size_t>(b[p]) + 13;
  }
  if (p > b.size()) return l;
  l.tailAt = p;
  l.ok = true;
  return l;
}

// J-182: Kerf's editor settings (snap, ruler, grid lines) and the classic
// engine's region names have no place in FL's states: written as defaults / the path alone.
void stripKerfEditor(PluginState& g) {
  // J-190: slices on the beat grid travel as the markers they are.
  if (std::lround(param(g, gl::pp::SlMode, 0)) == gl::pp::SlModeGrid && g.params.size() > static_cast<size_t>(gl::pp::slMarker(gl::pp::kMaxSlices - 1))) {
    const SamplePtr s = sampleCache().get(gl::kerfmarks::pathOf(g.data));
    const auto r = gl::kerfgrid::resultFor(g, s, true);
    if (r && s && s->frames() > 0) {
      for (int k = 0; k < gl::pp::kMaxSlices; ++k)
        g.params[static_cast<size_t>(gl::pp::slMarker(k))] =
            k < static_cast<int>(r->cuts.size()) ? static_cast<float>(static_cast<double>(r->cuts[static_cast<size_t>(k)]) / static_cast<double>(s->frames())) : -1.f;
      g.params[static_cast<size_t>(gl::pp::SlMode)] = static_cast<float>(gl::pp::SlModeManual);
    }
  }
  if (const PluginInfo* info = findPlugin("gl.slicer"))
    for (int p : {gl::pp::SlSnap, gl::pp::SlSnapZero, gl::pp::SlRuler, gl::pp::SlGridLines, gl::pp::SlGridDiv, gl::pp::SlGridMeter, gl::pp::SlGridHits,
                  gl::pp::SlGridZero, gl::pp::SlGridFollow})
      if (static_cast<size_t>(p) < g.params.size() && static_cast<size_t>(p) < info->params.size())
        g.params[static_cast<size_t>(p)] = static_cast<float>(info->params[static_cast<size_t>(p)].def);
  if (gl::kerfmarks::hasNames(g.data)) g.data = gl::kerfmarks::pathOf(g.data);
}

Encoder fruitySlicer() {
  Encoder e;
  e.flName = "Fruity Slicer";
  e.glType = "gl.slicer";
  e.nativeRank = 0;
  // Classic slicer and the processed engine (2); the region model is Slicex's.
  e.accepts = [](const PluginState& g) { return param(g, gl::pp::SlEngine, 0) < 0.5f || param(g, gl::pp::SlEngine, 0) >= 1.5f; };
  e.strip = stripKerfEditor;
  e.generator = true;
  e.defaults = [] {
    constexpr uint32_t kVersion = 15;  // the plugin database state's layout
    ByteWriter w;
    w.u32(kVersion);
    w.f32(0.0f);  // beats (0 = from the sample)
    w.f32(0.0f);  // tempo (0 = from the sample)
    w.i32(0);     // pitch (cents)
    w.i32(0);
    w.i32(4);     // option (FL writes 4)
    w.i32(0);
    w.i32(4);     // option (FL writes 4)
    w.u8(0);      // no sample
    w.i32(0);     // no slices
    // Tail: u8 1, base note 60 (C5), i32 0, i32 sample rate 44100, three
    // switches on, i32 0.
    w.u8(1);
    w.u8(60);
    w.i32(0);
    w.i32(44100);
    w.u8(1);
    w.u8(1);
    w.u8(1);
    w.i32(0);
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    // J-41: the processing settings (engine 2).
    std::vector<Field> f = {i32(pp::SlProcPitch, 12, -1200, 1200), i32(pp::SlProcFit, 16, -20000, 20000), i32(pp::SlProcMethod, 20, 0, 7),
                            i32(pp::SlProcFadeIn, 24, 0, 1000), i32(pp::SlProcFadeOut, 28, 0, 1000)};
    const FsLayout l = fsLayout(b);
    if (l.ok && l.tailAt + 2 <= b.size()) f.push_back(field(pp::SlBaseNote, l.tailAt + 1, Type::U8, 1, 127));
    if (l.ok && l.tailAt + 12 <= b.size()) f.push_back(field(pp::SlProcDeclick, l.tailAt + 11, Type::U8, 0, 1));
    for (size_t k = 0; l.ok && k < l.slice.size() && k < static_cast<size_t>(pp::kMaxSlices); ++k)
      f.push_back(field(pp::slParam(static_cast<int>(k), pp::SliceReverse), l.slice[k] + 1 + b[l.slice[k]] + 12, Type::U8, 0, 1));
    return f;
  };
  // Sample path and slice starts.
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    FsLayout l = fsLayout(b);
    if (!l.ok) return;
    PluginMapping m;
    if (!decodeAs("Fruity Slicer", b, m, c)) return;
    if (m.state.data != g.data) {
      Bytes out(b.begin(), b.begin() + 32);
      putPascal(out, flPath(g.data, c));
      out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(l.countAt), b.end());
      b.swap(out);
      l = fsLayout(b);
      if (!l.ok || !decodeAs("Fruity Slicer", b, m, c)) return;
    }
    if (sameMarkers(m.state, g)) return;
    std::vector<double> want = slicerMarkers(g);
    if (want.size() == 1 && want[0] == 0.0) want.clear();  // one slice at 0 = no slices
    const double frames = static_cast<double>(fileFrames(g.data));
    // Rebuild the slice list, keeping each record's other values.
    Bytes out(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(l.countAt));
    ByteWriter cnt;
    cnt.i32(static_cast<int32_t>(want.size()));
    out.insert(out.end(), cnt.b.begin(), cnt.b.end());
    std::vector<size_t> startAt;
    for (size_t k = 0; k < want.size(); ++k) {
      if (k < l.slice.size()) {
        const size_t s = l.slice[k], n = 1 + static_cast<size_t>(b[s]);
        out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(s), b.begin() + static_cast<std::ptrdiff_t>(s + n + 13));
        startAt.push_back(out.size() - 13);
      } else {
        out.push_back(0);  // unnamed
        startAt.push_back(out.size());
        ByteWriter r;
        r.i32(0);
        r.i32(-1);     // end: the next slice
        r.f32(-1.0f);  // (FL writes -1)
        r.u8(0);
        out.insert(out.end(), r.b.begin(), r.b.end());
      }
      putI32(out, startAt.back(), static_cast<int32_t>(std::llround(want[k] * frames)));
    }
    out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(l.tailAt), b.end());
    b.swap(out);
    // Starts that do not decode back exactly: the nearest frames that do.
    for (size_t k = 0; k < want.size() && frames > 0; ++k) {
      const int32_t s0 = rdI32(b, startAt[k]);
      for (int d : {0, 1, -1, 2, -2}) {
        putI32(b, startAt[k], s0 + d);
        if (!decodeAs("Fruity Slicer", b, m, c)) break;
        if (param(m.state, pp::slMarker(static_cast<int>(k)), -1) == static_cast<float>(want[k])) break;
        if (d == -2) putI32(b, startAt[k], s0);
      }
    }
  };
  return e;
}

// ------------------------------------------------------------------ FPC
// u32 magic, then chunks {u32 id, u64 size, payload}. Chunk 2 = one pad (32:
// banks A and B) holding 0x64 settings (16 × i32: volume, pan, flags, note,
// ?, cut group, cut-by group, tune, colour, …), 0x65 name, 0x66 trigger
// options (8 × i32, bit 2 = random layers), 0x69 layers (0x12C sample path,
// 0x12D settings 7 × i32: volume, pan, velocity low / high, ?, tune, ?;
// 0x12E), 0x67 {kind 0 volume / 1 pan, 0} + 0x68 envelope, 0x6A. After the
// pads: 0x01 MIDI loop name, 0x03 u8, 0x04 8 × i32. Older kits (magics
// 0x000F42xx) store text as 8-bit, the others as UTF-16 (no terminator).
constexpr uint32_t kFpcMagic = 0x1E84E403;
constexpr int kFpcPads = 32;
bool fpcMagic(uint32_t v) { return v == kFpcMagic || v == 0x001E848E || v == 0x000F424E || v == 0x000F424B; }

struct FpcChunk {
  uint32_t id = 0;
  Bytes data;
};

std::vector<FpcChunk> fpcSplit(const Bytes& b, size_t at, size_t end) {
  std::vector<FpcChunk> out;
  while (at + 12 <= end) {
    const uint32_t id = getU32(b, at);
    const uint64_t size = static_cast<uint64_t>(getU32(b, at + 4)) | (static_cast<uint64_t>(getU32(b, at + 8)) << 32);
    if (size > end - at - 12) break;
    out.push_back({id, Bytes(b.begin() + static_cast<std::ptrdiff_t>(at + 12), b.begin() + static_cast<std::ptrdiff_t>(at + 12 + size))});
    at += 12 + static_cast<size_t>(size);
  }
  return out;
}

Bytes fpcJoin(const std::vector<FpcChunk>& cs) {
  ByteWriter w;
  for (const auto& c : cs) {
    w.u32(c.id);
    w.u64(c.data.size());
    w.bytes(c.data);
  }
  return w.b;
}

FpcChunk fpcInts(uint32_t id, std::initializer_list<int32_t> v) {
  ByteWriter w;
  for (int32_t x : v) w.i32(x);
  return {id, w.b};
}

// Envelope chunk: i32 1, i32 3, i32 count, count × {f64 time since the
// previous point (s), f64 value, f32 tension, u32 flags}, i32 0, i32 enabled
// (-1 on / 0 off), i32 sustain point, i32 loop start, i32 loop end, 4 × i32.
struct FpcEnvPoint {
  double t, v;
  float ten;
};
Bytes fpcEnvelope(const std::vector<FpcEnvPoint>& pts, bool on, int sustain, int loopStart, int loopEnd, const Bytes* tailFrom) {
  ByteWriter w;
  w.i32(1);
  w.i32(3);
  w.i32(static_cast<int32_t>(pts.size()));
  for (size_t i = 0; i < pts.size(); ++i) {
    w.f64(pts[i].t);
    w.f64(pts[i].v);
    w.f32(pts[i].ten);
    w.u32(i == 0 ? 0 : 0x01000000);  // point flags (FL's value for curve points)
  }
  w.i32(0);
  w.i32(on ? -1 : 0);
  w.i32(sustain);
  w.i32(loopStart);
  w.i32(loopEnd);
  // Trailing settings: kept from the previous envelope, else FL's defaults.
  bool kept = false;
  if (tailFrom && tailFrom->size() >= 12) {
    const size_t n = static_cast<size_t>(std::max(0, rdI32(*tailFrom, 8)));
    const size_t at = 12 + 24 * n + 20;
    if (at <= tailFrom->size()) w.bytes(Bytes(tailFrom->begin() + static_cast<std::ptrdiff_t>(at), tailFrom->end())), kept = true;
  }
  if (!kept)
    for (int32_t x : {128, 128, 0, 128}) w.i32(x);
  return w.b;
}

// FL's default pad note map: bank A on the General MIDI drum notes, bank B from C5 up.
constexpr int32_t kFpcNotes[kFpcPads] = {37, 36, 42, 54, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53,
                                         60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75};

FpcChunk fpcFreshLayer() {
  FpcChunk l{0x69, {}};
  std::vector<FpcChunk> sub;
  sub.push_back({0x12C, {}});                                // no sample
  sub.push_back(fpcInts(0x12D, {127, 0, 0, 127, 1, 0, -1}));  // volume, pan, velocity 0..127, ?, tune, ?
  sub.push_back({0x12E, {}});
  l.data = fpcJoin(sub);
  return l;
}

std::vector<FpcChunk> fpcFreshPad(int k) {
  std::vector<FpcChunk> p;
  p.push_back(fpcInts(0x64, {99, 0, 0, kFpcNotes[k], 0, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0}));  // volume 99, no cut groups
  p.push_back({0x65, {}});                                                                    // no name
  p.push_back(fpcInts(0x66, {1, 0, 0, 0, 0, 0, 0, 0}));                                       // trigger options
  p.push_back(fpcFreshLayer());
  // Default volume envelope (off): rise 0.75 s, fall to half 1.5 s, release 0.25 s, sustain at point 1.
  p.push_back(fpcInts(0x67, {0, 0}));
  p.push_back({0x68, fpcEnvelope({{0, 0, 0}, {0.75, 1, 0.2f}, {1.5, 0.5, 0.2f}, {0.25, 0, 0.2f}}, false, 1, -1, 2, nullptr)});
  // Default pan envelope (off): centre.
  p.push_back(fpcInts(0x67, {1, 0}));
  p.push_back({0x68, fpcEnvelope({{0, 0.5, 0.2f}}, false, -1, -1, -1, nullptr)});
  p.push_back({0x6A, {}});
  return p;
}

Bytes fpcText(const std::string& utf8, bool narrow) {
  Bytes u = utf16z(utf8);
  u.resize(u.size() >= 2 ? u.size() - 2 : 0);  // no terminator
  if (!narrow) return u;
  Bytes out;
  for (size_t i = 0; i + 1 < u.size(); i += 2) out.push_back(u[i + 1] ? static_cast<uint8_t>('?') : u[i]);
  return out;
}

struct FpcPadView {
  size_t settings = std::string::npos, trigger = std::string::npos;  // payload offsets of 0x64 / 0x66
  std::vector<size_t> layers;                                        // 0x12D payload offsets of layers with a sample
};

// Offsets of the solver's values in a state (pads in order).
std::vector<FpcPadView> fpcView(const Bytes& b) {
  std::vector<FpcPadView> pads;
  if (b.size() < 4 || !fpcMagic(getU32(b, 0))) return pads;
  auto walk = [&](size_t at, size_t end, auto&& f) {
    while (at + 12 <= end) {
      const uint32_t id = getU32(b, at);
      const uint64_t size = static_cast<uint64_t>(getU32(b, at + 4)) | (static_cast<uint64_t>(getU32(b, at + 8)) << 32);
      if (size > end - at - 12) return;
      f(id, at + 12, static_cast<size_t>(size));
      at += 12 + static_cast<size_t>(size);
    }
  };
  walk(4, b.size(), [&](uint32_t id, size_t p, size_t n) {
    if (id != 2) return;
    FpcPadView v;
    walk(p, p + n, [&](uint32_t id2, size_t q, size_t m) {
      if (id2 == 0x64 && m >= 32) v.settings = q;
      else if (id2 == 0x66 && m >= 4) v.trigger = q;
      else if (id2 == 0x69) {
        size_t settings = std::string::npos;
        bool path = false;
        walk(q, q + m, [&](uint32_t id3, size_t r, size_t k) {
          if (id3 == 0x12C) path = k > 0;
          if (id3 == 0x12D && k >= 28) settings = r;
        });
        if (path && settings != std::string::npos) v.layers.push_back(settings);
      }
    });
    pads.push_back(v);
  });
  return pads;
}

// Direct inverses (fast path; the engine checks them through the decoder),
// and (J-51) the decoder's laws (FlpGen2Pads.cpp) for the solver.
enum class FpcLaw { Level, Pan, Tune, Int127 };
Field fpcField(int gl, size_t at, double lo, double hi, double scale, FpcLaw law) {
  Field f = i32(gl, at, lo, hi);
  f.inverse = [scale](double v) { return std::round(v * scale); };
  f.forward = [law](double r, const Bytes&) {
    switch (law) {
      case FpcLaw::Level: {
        const double v = r / 127.0;
        return v < 0 ? 0 : (v > 1 ? 1 : v);
      }
      case FpcLaw::Pan: return std::clamp(r / 128.0, -1.0, 1.0);
      case FpcLaw::Tune: return static_cast<int>(r) * 12.0 / 128.0;
      case FpcLaw::Int127: break;
    }
    return static_cast<double>(std::clamp(static_cast<int>(r), 0, 127));
  };
  return f;
}

std::vector<Field> fpcFields(const Bytes& b) {
  using namespace gl::pp;
  std::vector<Field> f;
  const std::vector<FpcPadView> pads = fpcView(b);
  constexpr double kLevel = 127, kPan = 128, kTune = 128.0 / 12.0;  // stored units per Gridline unit
  for (size_t k = 0; k < pads.size() && k < static_cast<size_t>(kPadsAll); ++k) {
    const int pk = static_cast<int>(k);
    const FpcPadView& v = pads[k];
    if (v.settings != std::string::npos) {
      f.push_back(fpcField(padP(pk, PadLevel), v.settings, 0, 127, kLevel, FpcLaw::Level));
      f.push_back(fpcField(padP(pk, PadPan), v.settings + 4, -128, 128, kPan, FpcLaw::Pan));
      f.push_back(fpcField(padP(pk, PadTune), v.settings + 28, -128, 128, kTune, FpcLaw::Tune));
      f.push_back(fpcField(padE(pk, PadNote), v.settings + 12, 0, 127, 1, FpcLaw::Int127));
    }
    if (v.trigger != std::string::npos) {
      Field t = i32(padE(pk, PadLayerMode), v.trigger, 0, 3);
      t.inverse = [](double m) { return static_cast<int>(std::lround(m)) == LayerRandom ? 3 : 1; };  // bit 2: random
      f.push_back(t);
    }
    for (size_t g = 0; g < v.layers.size() && g < static_cast<size_t>(kPadLayers); ++g) {
      const int lg = static_cast<int>(g);
      const size_t q = v.layers[g];
      f.push_back(fpcField(padL(pk, lg, LayLevel), q, 0, 127, kLevel, FpcLaw::Level));
      f.push_back(fpcField(padL(pk, lg, LayPan), q + 4, -128, 128, kPan, FpcLaw::Pan));
      f.push_back(fpcField(padL(pk, lg, LayVelLow), q + 8, 0, 127, 1, FpcLaw::Int127));
      f.push_back(fpcField(padL(pk, lg, LayVelHigh), q + 12, 0, 127, 1, FpcLaw::Int127));
      f.push_back(fpcField(padL(pk, lg, LayTune), q + 20, -128, 128, kTune, FpcLaw::Tune));
    }
  }
  return f;
}

// Pads, layers, sample paths, names, colours, cut groups, envelopes and
// layers 5..32 (data text) from the Gridline state; numbers of the first
// four layers and the pads are left to the solver.
void fpcWrite(const PluginState& g, Bytes& b, const EncodeContext& c) {
  using namespace gl::pp;
  if (b.size() < 4 || !fpcMagic(getU32(b, 0))) return;
  const bool narrow = (getU32(b, 0) >> 16) == 0x000F;
  std::vector<FpcChunk> top = fpcSplit(b, 4, b.size());
  std::vector<size_t> padIdx;
  for (size_t i = 0; i < top.size(); ++i)
    if (top[i].id == 2) padIdx.push_back(i);
  // Pads the Gridline state uses beyond the template's: fresh pads appended.
  int used = 0;
  for (int k = 0; k < kPadsAll; ++k) {
    const std::string pn = std::to_string(k + 1);
    bool any = !padDataGet(g.data, "N" + pn).empty() || !padDataGet(g.data, "C" + pn).empty();
    for (int l = 0; l < std::max(kPadLayers, padLayerCount(g.data, k)) && !any; ++l) {
      const std::string p = padSamplePath(g.data, k, l);
      any = !p.empty() && p != "none";
    }
    if (any) used = k + 1;
  }
  if (static_cast<int>(padIdx.size()) < used) {
    const size_t at = padIdx.empty() ? 0 : padIdx.back() + 1;
    std::vector<FpcChunk> add;
    for (int k = static_cast<int>(padIdx.size()); k < used; ++k) add.push_back({2, fpcJoin(fpcFreshPad(k))});
    top.insert(top.begin() + static_cast<std::ptrdiff_t>(at), add.begin(), add.end());
    padIdx.clear();
    for (size_t i = 0; i < top.size(); ++i)
      if (top[i].id == 2) padIdx.push_back(i);
  }
  // Cut groups: the decoder numbers FL groups 1..8 in order of appearance
  // (each pad's cut-by group, then its cut group); keep the template's FL ids.
  std::map<int, int> glToFl;
  std::set<int> flUsed;
  {
    int n = 0;
    for (size_t i : padIdx)
      for (const auto& ch : fpcSplit(top[i].data, 0, top[i].data.size()))
        if (ch.id == 0x64 && ch.data.size() >= 28)
          for (int flg : {rdI32(ch.data, 24), rdI32(ch.data, 20)})
            if (flg >= 0 && !flUsed.count(flg)) flUsed.insert(flg), glToFl.emplace(++n, flg);
  }
  auto flGroup = [&](int gl) {
    if (gl <= 0) return -1;
    auto it = glToFl.find(gl);
    if (it != glToFl.end()) return it->second;
    int id = 0;
    while (flUsed.count(id)) ++id;
    flUsed.insert(id);
    glToFl[gl] = id;
    return id;
  };
  for (size_t k = 0; k < padIdx.size() && k < static_cast<size_t>(kPadsAll); ++k) {
    const int pk = static_cast<int>(k);
    const std::string pn = std::to_string(pk + 1);
    std::vector<FpcChunk> sub = fpcSplit(top[padIdx[k]].data, 0, top[padIdx[k]].data.size());
    // Wanted layers: every layer with a sample (1..4 also switched on).
    struct Want {
      std::string path;
      int lg;
    };
    std::vector<Want> want;
    for (int l = 0; l < std::max(kPadLayers, padLayerCount(g.data, pk)) && l < kPadMaxLayers; ++l) {
      const std::string p = padSamplePath(g.data, pk, l);
      if (p.empty() || p == "none") continue;
      if (l < kPadLayers && param(g, padL(pk, l, LayEnabled), 0) < 0.5f) continue;
      want.push_back({p, static_cast<int>(want.size())});
    }
    std::vector<FpcChunk> haveLayers, emptyLayers;
    for (const auto& ch : sub)
      if (ch.id == 0x69) {
        bool path = false;
        for (const auto& l : fpcSplit(ch.data, 0, ch.data.size()))
          if (l.id == 0x12C) path = !l.data.empty();
        (path ? haveLayers : emptyLayers).push_back(ch);
      }
    std::vector<FpcChunk> layers;
    for (size_t j = 0; j < want.size(); ++j) {
      FpcChunk l = j < haveLayers.size() ? haveLayers[j] : (j - haveLayers.size() < emptyLayers.size() ? emptyLayers[j - haveLayers.size()] : fpcFreshLayer());
      std::vector<FpcChunk> ls = fpcSplit(l.data, 0, l.data.size());
      bool hasPath = false;
      for (auto& x : ls) {
        if (x.id == 0x12C) x.data = fpcText(flPath(want[j].path, c), narrow), hasPath = true;
        if (x.id == 0x12D && x.data.size() >= 28 && want[j].lg >= kPadLayers) {
          // Layers 5..32: settings from the data text.
          PadLayerX lx;
          parsePadLayer(padDataGet(g.data, "S" + pn + "." + std::to_string(want[j].lg + 1)), lx);
          putI32(x.data, 0, static_cast<int32_t>(std::lround(lx.level * 127.0)));
          putI32(x.data, 4, static_cast<int32_t>(std::lround(lx.pan * 128.0)));
          putI32(x.data, 8, lx.lo);
          putI32(x.data, 12, lx.hi);
          putI32(x.data, 20, static_cast<int32_t>(std::lround(lx.tune * 128.0 / 12.0)));
        }
      }
      if (!hasPath) ls.insert(ls.begin(), FpcChunk{0x12C, fpcText(flPath(want[j].path, c), narrow)});
      l.data = fpcJoin(ls);
      layers.push_back(l);
    }
    if (layers.empty()) {
      FpcChunk l = !emptyLayers.empty() ? emptyLayers[0] : fpcFreshLayer();
      if (emptyLayers.empty() && !haveLayers.empty()) {  // a used layer emptied
        std::vector<FpcChunk> ls = fpcSplit(haveLayers[0].data, 0, haveLayers[0].data.size());
        for (auto& x : ls)
          if (x.id == 0x12C) x.data.clear();
        l.data = fpcJoin(ls);
      }
      layers.push_back(l);
    }
    std::vector<FpcChunk> out;
    bool layersPlaced = false;
    int envKind = -1;
    const std::string colour = padDataGet(g.data, "C" + pn);
    for (auto& ch : sub) {
      if (ch.id == 0x69) {
        if (!layersPlaced) out.insert(out.end(), layers.begin(), layers.end()), layersPlaced = true;
        continue;
      }
      if (ch.id == 0x64 && ch.data.size() >= 28) {
        putI32(ch.data, 20, flGroup(static_cast<int>(std::lround(param(g, padE(pk, PadCutBy), 0)))));
        putI32(ch.data, 24, flGroup(static_cast<int>(std::lround(param(g, padP(pk, PadCut), 0)))));
        if (ch.data.size() >= 36) {
          uint32_t rgb = colour.empty() ? 0 : static_cast<uint32_t>(std::strtoul(colour.c_str(), nullptr, 16));
          putU32(ch.data, 32, ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF));  // stored as BGR
        }
      } else if (ch.id == 0x65) {
        ch.data = fpcText(padDataGet(g.data, "N" + pn), narrow);
      } else if (ch.id == 0x67 && ch.data.size() >= 4) {
        envKind = rdI32(ch.data, 0);
      } else if (ch.id == 0x68 && (envKind == 0 || envKind == 1)) {
        PadEnvelope pe;
        const std::string txt = padDataGet(g.data, "E" + pn + (envKind == 0 ? ".v" : ".p"));
        const bool on = parsePadEnvelope(txt, pe) && pe.on;
        if (on) {
          // Sustain / loop points as written (the text parser drops a loop end without a start).
          int onFlag = 0, sus = pe.sustain, ls = pe.loopStart, le = pe.loopEnd;
          std::sscanf(txt.c_str(), "%d %d %d %d", &onFlag, &sus, &ls, &le);
          std::vector<FpcEnvPoint> pts;
          for (int i = 0; i < pe.n; ++i) pts.push_back({pe.t[i], pe.v[i], pe.ten[i]});
          ch.data = fpcEnvelope(pts, true, sus, ls, le, &ch.data);
        } else if (ch.data.size() >= 12) {
          const size_t at = 12 + 24 * static_cast<size_t>(std::max(0, rdI32(ch.data, 8))) + 4;
          if (at + 4 <= ch.data.size()) putI32(ch.data, at, 0);  // switched off
        }
        envKind = -1;
      }
      out.push_back(ch);
      if (ch.id == 0x66 && !layersPlaced && std::none_of(sub.begin(), sub.end(), [](const FpcChunk& x) { return x.id == 0x69; }))
        out.insert(out.end(), layers.begin(), layers.end()), layersPlaced = true;
    }
    top[padIdx[k]].data = fpcJoin(out);
  }
  Bytes nb(b.begin(), b.begin() + 4);
  const Bytes body = fpcJoin(top);
  nb.insert(nb.end(), body.begin(), body.end());
  b.swap(nb);
  // Numbers that differ: their direct inverse in one go (one decode instead
  // of one solve per value; the solver still checks and refines each).
  PluginMapping m;
  if (!decodeAs("FPC", b, m, c)) return;
  for (const Field& f : fpcFields(b))
    if (param(m.state, f.gl, NAN) != param(g, f.gl, NAN) && f.inverse) writeField(b, f.offset, f.type, std::clamp(f.inverse(param(g, f.gl)), f.lo, f.hi));
}

Encoder fpc() {
  Encoder e;
  e.flName = "FPC";
  e.glType = "gl.pads";
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = [] {
    std::vector<FpcChunk> top;
    for (int k = 0; k < kFpcPads; ++k) top.push_back({2, fpcJoin(fpcFreshPad(k))});
    top.push_back({0x01, {}});                            // MIDI loop name: none
    top.push_back({0x03, {1}});                           // (FL writes 1)
    top.push_back(fpcInts(0x04, {0, 0, 0, 0, 0, 0, 0, 0}));  // reserved
    ByteWriter w;
    w.u32(kFpcMagic);
    w.bytes(fpcJoin(top));
    return w.b;
  };
  e.fields = fpcFields;
  e.write = fpcWrite;
  return e;
}

// ------------------------------------------------------------------ Slicex
// 0x60-byte header (version, knob integers at 4·(1 + tag): master level at
// 0x0C, pitch at 0x18, modulation X / Y at 0x1C / 0x20), byte 0x5F = 1 when
// the sample is referenced by path only, Pascal path at 0x60, then (embedded
// sample) u32 length + RIFF/WAVE whose "cue " chunk holds the slice starts
// and "LIST adtl" ltxt entries the region lengths. The settings block that
// follows holds the region count at +0x18C and one 68-byte region record
// (17 × i32: volume [3], pan [4], cutoff [6], resonance [7], …) per region
// from +0x190, then ~25 KB of articulator settings. That block is not built
// from scratch: Slicex is written only for instances imported from it.
constexpr size_t kSxRegionCount = 0x18C, kSxRegions = 0x190, kSxRegion = 68;
// A fresh region record: volume 8000 (unity), pan 0, cutoff / resonance 128
// (neutral), the articulator amounts 12800, the rest 0.
constexpr int32_t kSxRegionDefault[17] = {0, 0, 0, 8000, 0, 12800, 128, 128, 0, 12800, 0, 0, 12800, 0, 0, 0, 12800};

struct SxLayout {
  bool ok = false, external = false;
  size_t pathAt = 0, pathEnd = 0, riffAt = 0, riffLen = 0, settings = 0;
};

// J-176: the path is one length byte, or 0xFF and a u32 length (255+ bytes).
void putSxPath(Bytes& out, const std::string& s) {
  if (s.size() < 0xFF) {
    out.push_back(static_cast<uint8_t>(s.size()));
  } else {
    out.push_back(0xFF);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(static_cast<uint32_t>(s.size()) >> (8 * i)));
  }
  out.insert(out.end(), s.begin(), s.end());
}

SxLayout sxLayout(const Bytes& b) {
  SxLayout l;
  if (b.size() < 0x62) return l;
  l.external = b[0x5F] == 1;
  l.pathAt = 0x61;
  size_t plen = b[0x60];
  if (plen == 0xFF) {
    if (b.size() < 0x65) return l;
    plen = getU32(b, 0x61);
    l.pathAt = 0x65;
  }
  if (plen > b.size() - l.pathAt) return l;
  l.pathEnd = l.pathAt + plen;
  size_t p = l.pathEnd;
  if (!l.external) {
    if (p + 4 > b.size()) return l;
    l.riffLen = getU32(b, p);
    l.riffAt = p + 4;
    if (l.riffLen > b.size() - l.riffAt) return l;
    p = l.riffAt + l.riffLen;
  }
  l.settings = p;
  l.ok = p + kSxRegions <= b.size();
  return l;
}

// RIFF chunk list of a WAV (offset, size of payload) by id.
struct RiffChunk {
  char id[4];
  size_t at, size;
};
std::vector<RiffChunk> riffChunks(const Bytes& r) {
  std::vector<RiffChunk> v;
  if (r.size() < 12 || std::memcmp(r.data(), "RIFF", 4) != 0 || std::memcmp(r.data() + 8, "WAVE", 4) != 0) return v;
  const size_t end = std::min<size_t>(r.size(), static_cast<size_t>(getU32(r, 4)) + 8);
  for (size_t q = 12; q + 8 <= end;) {
    const uint32_t len = getU32(r, q + 4);
    RiffChunk c{{}, q + 8, std::min<size_t>(len, end - q - 8)};
    std::memcpy(c.id, r.data() + q, 4);
    v.push_back(c);
    if (len > end - q - 8) break;
    q += 8 + len + (len & 1);
  }
  return v;
}

uint64_t riffFrames(const Bytes& r) {
  uint32_t tag = 0, align = 0, data = 0, fact = 0;
  bool haveData = false;
  for (const auto& c : riffChunks(r)) {
    if (std::memcmp(c.id, "fmt ", 4) == 0 && c.size >= 14) tag = getU16(r, c.at), align = getU16(r, c.at + 12);
    else if (std::memcmp(c.id, "fact", 4) == 0 && c.size >= 4) fact = getU32(r, c.at);
    else if (std::memcmp(c.id, "data", 4) == 0) data = static_cast<uint32_t>(c.size), haveData = true;
  }
  const bool linear = tag == 1 || tag == 3 || tag == 0xFFFE;
  if (haveData && linear && align > 0) return data / align;
  return !linear ? fact : 0;
}

// The RIFF with its cue points (and region lengths, 0 = to the next cue) replaced.
Bytes riffWithCues(const Bytes& r, const std::vector<uint32_t>& starts, const std::vector<uint32_t>& lens) {
  ByteWriter w;
  putBytes(w, "RIFF");
  w.u32(0);
  putBytes(w, "WAVE");
  for (const auto& c : riffChunks(r)) {
    const bool cue = std::memcmp(c.id, "cue ", 4) == 0;
    const bool adtl = std::memcmp(c.id, "LIST", 4) == 0 && c.size >= 4 && std::memcmp(r.data() + c.at, "adtl", 4) == 0;
    if (cue || adtl) continue;
    w.bytes(c.id, 4);
    w.u32(static_cast<uint32_t>(c.size));
    w.bytes(r.data() + c.at, c.size);
    if (c.size & 1) w.u8(0);
  }
  if (!starts.empty()) {
    putBytes(w, "cue ");
    w.u32(static_cast<uint32_t>(4 + 24 * starts.size()));
    w.u32(static_cast<uint32_t>(starts.size()));
    for (size_t k = 0; k < starts.size(); ++k) {
      w.u32(static_cast<uint32_t>(k + 1));  // cue id
      w.u32(starts[k]);                     // play position
      putBytes(w, "data");
      w.u32(0);
      w.u32(0);
      w.u32(starts[k]);  // sample offset
    }
    ByteWriter l;
    putBytes(l, "adtl");
    for (size_t k = 0; k < starts.size(); ++k) {
      if (k >= lens.size() || lens[k] == 0) continue;
      putBytes(l, "ltxt");
      l.u32(20);
      l.u32(static_cast<uint32_t>(k + 1));
      l.u32(lens[k]);
      putBytes(l, "rgn ");
      l.u16(0), l.u16(0), l.u16(0), l.u16(0);  // country, language, dialect, code page
    }
    if (l.b.size() > 4) {
      putBytes(w, "LIST");
      w.u32(static_cast<uint32_t>(l.b.size()));
      w.bytes(l.b);
    }
  }
  putU32(w.b, 4, static_cast<uint32_t>(w.b.size() - 8));
  return w.b;
}

Bytes readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Smallest frame whose position (frame / frames) reads back as `pos`.
uint32_t frameAt(double pos, uint64_t frames) {
  const double fr = static_cast<double>(frames);
  const int64_t s0 = std::llround(pos * fr);
  for (int64_t s = std::max<int64_t>(0, s0 - 2); s <= s0 + 2; ++s)
    if (static_cast<float>(static_cast<double>(s) / fr) == static_cast<float>(pos)) return static_cast<uint32_t>(s);
  return static_cast<uint32_t>(std::max<int64_t>(0, s0));
}

std::vector<Field> sxFields(const Bytes& b) {
  using namespace gl::pp;
  std::vector<Field> f = {i32(SlVolume, 0x0C, 0, 16000), i32(SlPitch, 0x18, -1200, 1200), i32(SlModX, 0x1C, 0, 512),
                          i32(SlModY, 0x20, 0, 512)};
  const SxLayout l = sxLayout(b);
  if (!l.ok) return f;
  const uint32_t n = getU32(b, l.settings + kSxRegionCount);
  for (uint32_t k = 0; k < n && k < static_cast<uint32_t>(kMaxSlices); ++k) {
    const size_t r = l.settings + kSxRegions + kSxRegion * k;
    if (r + kSxRegion > b.size()) break;
    const int ik = static_cast<int>(k);
    f.push_back(i32(slParam(ik, SliceGain), r + 12, 0, 16000));
    f.push_back(i32(slParam(ik, SlicePan), r + 8, -6400, 6400));
    f.push_back(i32(slParam(ik, SlicePitch), r + 40, -1200, 1200));
    f.push_back(i32(slParam2(ik, SliceCut), r + 0, 0, 99));
    f.push_back(i32(slParam2(ik, SliceDelay), r + 52, 0, 12800));
  }
  return f;
}

// Sample (path or embedded copy) and slice points; region records follow the slice count.
void sxWrite(const PluginState& g, Bytes& b, const EncodeContext& c) {
  using namespace gl::pp;
  SxLayout l = sxLayout(b);
  if (!l.ok) return;
  PluginMapping m;
  if (!decodeAs("Slicex", b, m, c)) return;
  const std::string rawPath(b.begin() + static_cast<std::ptrdiff_t>(l.pathAt), b.begin() + static_cast<std::ptrdiff_t>(l.pathEnd));
  auto replaceSample = [&](bool external, const std::string& path, const Bytes& riff) {
    Bytes out(b.begin(), b.begin() + 0x60);
    out[0x5F] = external ? 1 : 0;
    putSxPath(out, path);
    if (!external) {
      ByteWriter n;
      n.u32(static_cast<uint32_t>(riff.size()));
      out.insert(out.end(), n.b.begin(), n.b.end());
      out.insert(out.end(), riff.begin(), riff.end());
    }
    out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(l.settings), b.end());
    b.swap(out);
    l = sxLayout(b);
  };
  if (m.state.data != g.data) {
    replaceSample(true, flPath(g.data, c), {});  // another sample: referenced by its path
    if (!l.ok || !decodeAs("Slicex", b, m, c)) return;
  }
  // Slice points: written into an embedded copy of the sample when they differ.
  bool ends = true;
  for (int k = 0; k < kMaxSlices && ends; ++k) ends = param(m.state, slParam2(k, SliceEnd), -1) == param(g, slParam2(k, SliceEnd), -1);
  std::vector<double> want = slicerMarkers(g);
  if (!sameMarkers(m.state, g) || !ends) {
    Bytes riff = l.external ? readFile(g.data) : Bytes(b.begin() + static_cast<std::ptrdiff_t>(l.riffAt),
                                                       b.begin() + static_cast<std::ptrdiff_t>(l.riffAt + l.riffLen));
    const uint64_t frames = riffFrames(riff);
    if (frames > 0) {
      if (want.size() == 1 && want[0] == 0.0 && param(g, slParam2(0, SliceEnd), -1) < 0) want.clear();  // one region = no cues
      std::vector<uint32_t> starts, lens;
      for (size_t k = 0; k < want.size(); ++k) {
        starts.push_back(frameAt(want[k], frames));
        const double e = param(g, slParam2(static_cast<int>(k), SliceEnd), -1);
        lens.push_back(e >= 0 ? frameAt(e, frames) - std::min(starts.back(), frameAt(e, frames)) : 0);
      }
      replaceSample(false, rawPath, riffWithCues(riff, starts, lens));
      if (!l.ok) return;
    }
  }
  // Region records: one per slice.
  const uint32_t have = getU32(b, l.settings + kSxRegionCount);
  const uint32_t need = static_cast<uint32_t>(std::max<size_t>(1, want.size()));
  if (have != need && l.settings + kSxRegions + kSxRegion * have <= b.size()) {
    const auto at = b.begin() + static_cast<std::ptrdiff_t>(l.settings + kSxRegions);
    if (need < have) {
      b.erase(at + static_cast<std::ptrdiff_t>(kSxRegion * need), at + static_cast<std::ptrdiff_t>(kSxRegion * have));
    } else {
      ByteWriter r;
      for (uint32_t k = have; k < need; ++k)
        for (int32_t v : kSxRegionDefault) r.i32(v);
      b.insert(at + static_cast<std::ptrdiff_t>(kSxRegion * have), r.b.begin(), r.b.end());
    }
    putU32(b, l.settings + kSxRegionCount, need);
  }
}

// J-41: the Slicex region model (SlEngine = 1) is written whole: the model
// text (decks, region records, articulator groups, options) with the
// slicer's parameters folded back in (master knobs, deck A region gain /
// pan / pitch / cut group / delay); embedded deck audio comes from the
// cached copy the decoder stored (byte for byte).
bool sxRegionWrite(const PluginState& g, Bytes& b) {
  using namespace gl::pp;
  namespace sx = gl::slicex;
  if (param(g, SlEngine, 0) < 0.5f) return false;
  sx::Model md;
  if (!sx::modelOf(g.data, md)) return false;
  std::array<Bytes, 2> riff;
  for (int d = 0; d < 2; ++d) {
    sx::Deck& dk = md.decks[static_cast<size_t>(d)];
    // J-166: the regions as they play (model starts / names, the key parameter
    // and end point of deck A regions); when they differ from the ones the deck
    // audio carries, its region chunks are rebuilt (a path-only deck is then
    // embedded so its chops travel with the project).
    const Bytes audio = !dk.riff.empty() ? dk.riff : (dk.file.empty() ? Bytes{} : readFile(dk.file));
    const sx::WavRegions have = sx::readWavRegions(audio.data(), audio.size());
    std::vector<sx::Region> want = dk.regions;
    for (size_t k = 0; k < want.size(); ++k) {
      const int ik = static_cast<int>(k);
      want[k].key = sx::explicitKey(md, &g, d, ik);
      if (d == 0 && ik < kMaxSlices && have.frames > 0) {
        const double e = param(g, slParam2(ik, SliceEnd), -1);
        if (e >= 0) want[k].length = std::max<int64_t>(1, std::llround(e * static_cast<double>(have.frames)) - want[k].start);
      }
    }
    auto same = [](const sx::Region& a, const sx::Region& b) { return a.start == b.start && a.length == b.length && a.key == b.key && a.name == b.name; };
    bool changed = want.size() != std::max<size_t>(1, have.regions.size());
    for (size_t k = 0; !changed && k < want.size(); ++k)
      changed = have.regions.empty() ? !same(want[k], sx::Region{}) : !same(want[k], have.regions[k]);
    if (have.riff && have.frames > 0 && changed) {
      riff[static_cast<size_t>(d)] = sx::writeWavRegions(audio, want);
      dk.flag = 0, dk.embedded = true;
      continue;
    }
    if (dk.flag != 0) continue;  // referenced by path
    riff[static_cast<size_t>(d)] = audio;
  }
  md.knobs[2] = static_cast<int32_t>(std::lround(12800.0 * std::log(10.0 * std::max(0.0, static_cast<double>(param(g, SlMaster, 1))) + 1.0) / std::log(11.0)));
  md.knobs[5] = static_cast<int32_t>(std::lround(param(g, SlPitch, 0) * 100.0));
  md.knobs[6] = static_cast<int32_t>(std::lround(param(g, SlModX, 0.5f) * 512.0));
  md.knobs[7] = static_cast<int32_t>(std::lround(param(g, SlModY, 0.5f) * 512.0));
  auto& regs = md.decks[0].regions;
  for (size_t k = 0; k < regs.size() && k < static_cast<size_t>(kMaxSlices); ++k) {
    auto& rec = regs[k].rec;
    const int ik = static_cast<int>(k);
    const double db = param(g, slParam(ik, SliceGain), 0);
    const double gain = db <= -24.0 + 1e-9 ? 0.0 : std::pow(10.0, db / 20.0);
    rec[sx::RVolume] = static_cast<int32_t>(std::lround(8000.0 * std::log(2.0 * gain + 1.0) / std::log(3.0)));
    rec[sx::RPan] = static_cast<int32_t>(std::lround(param(g, slParam(ik, SlicePan), 0) * 6400.0));
    rec[sx::RPitch] = static_cast<int32_t>(std::lround(param(g, slParam(ik, SlicePitch), 0) * 100.0));
    rec[sx::RCut] = static_cast<int32_t>(std::lround(param(g, slParam2(ik, SliceCut), 0)));
    rec[sx::RDelay] = static_cast<int32_t>(std::lround(param(g, slParam2(ik, SliceDelay), 0) / 0.5 * 12800.0));
  }
  b = sx::writeState(md, riff);
  return true;
}

// An empty deck's audio: a header-only 16-bit stereo WAV.
Bytes emptyWav() {
  ByteWriter w;
  w.bytes("RIFF", 4), w.u32(36), w.bytes("WAVE", 4);
  w.bytes("fmt ", 4), w.u32(16), w.u16(1), w.u16(2), w.u32(44100), w.u32(44100 * 4), w.u16(4), w.u16(16);
  w.bytes("data", 4), w.u32(0);
  return w.b;
}

Encoder slicex() {
  Encoder e;
  e.flName = "Slicex";
  e.glType = "gl.slicer";
  e.generator = true;
  // Region-model states (J-41) are written whole, from Gridline values alone.
  e.nativeRank = 0;
  e.accepts = [](const PluginState& g) {
    return param(g, gl::pp::SlEngine, 0) >= 0.5f && g.data.find('\n') != std::string::npos && !gl::kerfmarks::hasNames(g.data);
  };
  e.strip = stripKerfEditor;
  e.defaults = [] {
    gl::slicex::Model md = gl::slicex::defaultModel();
    return gl::slicex::writeState(md, {emptyWav(), emptyWav()});
  };
  e.fields = sxFields;
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    if (!sxRegionWrite(g, b)) sxWrite(g, b, c);
  };
  return e;
}

// ---------------------------------------------------------- Toxic Biohazard
// "SSLF" "TBi2" engine block → Hex Synth (J-33, 340 parameters in reference
// order). J-39: written whole from Gridline's state: "SSLF", u32 size, magic
// "TBi2", the 64-byte engine header (the template's; FL's default header for
// a fresh state: 16 words then 8 floats, not read by the decoder), the 8 wave
// slots (128 harmonic amplitudes, then 128 phases, f32), the 340 parameters
// (f32), 294 modulation-link bytes and the 128-byte patch name ("<noname>").
namespace toxic {

namespace hx = gl::packs::synths::hex;
constexpr size_t kHeader = 64, kName = 128, kLinks = 294;

Bytes build(const PluginState& g, const Bytes& header) {
  hx::HexData d;
  if (!hx::parseHexData(g.data, d)) d = hx::HexData{};
  ByteWriter in;
  in.u32(0x54426932u);  // "2iBT" as stored
  in.bytes(header);
  for (int s = 0; s < hx::kSlots; ++s) {
    for (int h = 0; h < 128; ++h) in.f32(h < hx::kHarmonics ? d.amp[s][h] : 0.f);
    for (int h = 0; h < 128; ++h) in.f32(h < hx::kHarmonics ? d.phase[s][h] : 0.f);
  }
  for (int i = 0; i < 340; ++i) in.f32(i < static_cast<int>(g.params.size()) && i < hx::HxCount ? g.params[static_cast<size_t>(i)] : 0.f);
  for (size_t i = 0; i < kLinks; ++i) in.u8(static_cast<uint8_t>(i < static_cast<size_t>(hx::kAutomatable) ? d.link[i] : -1));
  std::string name = d.name.empty() ? "<noname>" : d.name;
  name.resize(kName, '\0');
  in.bytes(name.data(), kName);
  ByteWriter w;
  putBytes(w, "SSLF");
  w.u32(static_cast<uint32_t>(in.b.size()));
  w.bytes(in.b);
  return w.b;
}
Bytes defaultHeader() {
  ByteWriter h;
  for (int v : {8, 8, 8, 0, 0, 0, 0, 0, 12, 150, 244, 3, 3, 3, 3, 3}) h.u16(static_cast<uint32_t>(v));
  for (float v : {0.64f, -0.99f, 1.0f, 0.f, 0.f, 0.f, 0.f, 0.f}) h.f32(v);
  return h.b;
}

Encoder encoder() {
  Encoder e;
  e.flName = "Toxic Biohazard";
  e.glType = "gl.hexsynth";
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = [] { return build(defaultPluginState("gl.hexsynth"), defaultHeader()); };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginMapping m;
    if (decodeState("Toxic Biohazard", b, m, c) && m.state.params == g.params && m.state.data == g.data) return;
    // The template's engine header when it is the plain "SSLF" "TBi2" form.
    Bytes header = defaultHeader();
    if (b.size() >= 12 + kHeader && getU32(b, 0) == 0x464C5353u && getU32(b, 8) == 0x54426932u)
      header.assign(b.begin() + 12, b.begin() + 12 + static_cast<std::ptrdiff_t>(kHeader));
    b = build(g, header);
  };
  return e;
}

}  // namespace toxic

}  // namespace

void registerGenB(std::vector<Encoder>& v) {
  v.push_back(keyboardController());
  v.push_back(voltageController());
  v.push_back(fpc());
  v.push_back(drumSynthLive());
  v.push_back(fruitySlicer());
  v.push_back(slicex());
  v.push_back(miniSynth());
  v.push_back(beepMap());
  v.push_back(toxic::encoder());
}

}  // namespace gl::flp::enc
