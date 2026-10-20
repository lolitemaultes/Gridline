// Gridline — FLP export: plugin state encoders for FlpFxK3.cpp (Chorus, Fast
// Dist, Flangus, Love Philter, PanOMatic, WaveShaper, Gross Beat). FlpFxK3.cpp
// is registered after FlpFxEq / FlpFxTime, so its decoders are the ones these
// encoders reverse. See FlpEncode.h for the model.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include "FlpEncode.h"
#include "FlpFx.h"
#include "../Png.h"
#include "../PluginParams.h"
#include "../../engine/TimeGateSlots.h"
#include "../../engine/WaveCurve.h"
#include "../../packs/creative/creative.h"
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/spacefx/spacefx.h"

namespace gl::flp::enc {

namespace {

namespace dy = gl::packs::dynamics;
namespace sf = gl::packs::spacefx;

struct Tag {
  int gl;
  int tag;
  double lo, hi;
};
std::vector<Field> tagFields(size_t base, std::initializer_list<Tag> tags) {
  std::vector<Field> v;
  for (const Tag& t : tags) v.push_back(i32(t.gl, base + 4 * static_cast<size_t>(t.tag), t.lo, t.hi));
  return v;
}
bool decoded(const std::string& flName, const Bytes& b, const EncodeContext& c, PluginState& out) {
  PluginMapping m;
  if (!decodeState(flName, b, m, c)) return false;
  out = m.state;
  return true;
}
float param(const PluginState& s, int k) { return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : 0.f; }
uint32_t versionOf(const Bytes& b) {
  uint32_t v = b.size() >= 4 ? getU32(b, 0) : 0;
  return v >= 0x10000 ? v >> 16 : v;
}

// Point flags: segment mode in the low byte, the tension's direction in the
// top byte (1 up, -1 down, 2 straight curve; nothing on the first point).
uint32_t pointFlags(int i, float t, uint8_t mode) {
  const uint32_t dir = i == 0 ? 0u : t > 0 ? 1u : t < 0 ? 0xFFu : mode == menv::SingleCurve ? 2u : 0u;
  return mode | dir << 24;
}
// Envelope points: u32 format 3 (64-bit points), u32 n, n × {f64 dx, f64 y,
// f32 tension, u32 flags}; x positions are stored as distances.
template <class Env>
void putPoints(ByteWriter& w, const Env& e) {
  w.u32(3);
  w.u32(static_cast<uint32_t>(e.n));
  double prev = 0;
  for (int i = 0; i < e.n; ++i) {
    w.f64(i ? e.x[i] - prev : e.x[i]);
    prev = e.x[i];
    w.f64(static_cast<double>(e.y[i]));
    w.f32(e.t[i]);
    w.u32(pointFlags(i, e.t[i], e.mode[i]));
  }
}
// Envelope record shared by Gross Beat / WaveShaper: u32 kind 3, points,
// u32 0, i32 × 4 (loop points, -1 = none).
template <class Env>
void putEnvRecord(ByteWriter& w, const Env& e) {
  w.u32(3);
  putPoints(w, e);
  w.u32(0);
  for (int k = 0; k < 4; ++k) w.i32(-1);
}
// Size of the envelope record at o (0 when malformed).
size_t envRecordSize(const Bytes& b, size_t o) {
  if (o + 12 > b.size()) return 0;
  const uint32_t fmt = getU32(b, o + 4), n = getU32(b, o + 8);
  if (fmt > 3 || n > 1u << 16) return 0;
  const size_t sz = fmt == 0 ? 12 : fmt == 1 ? 16 : 24;
  const size_t end = o + 12 + n * sz + 20;
  return end <= b.size() ? end - o : 0;
}

// ============================================================ Gross Beat
// u32 version (0x00080003: version 8), i32 × 9 parameters, i32 editor page,
// u8 remove DC, u8 attack compensation, u8 HQ, u8 previews, u8 click
// reduction, 72 slots (36 time, 36 volume), u8 MIDI trigger (version >= 9).
constexpr uint32_t kGbVersion = 0x00080003;

// Offset of the slot banks.
size_t gbSlotStart(const Bytes& b) {
  const uint32_t ver = versionOf(b);
  const size_t nParams = ver < 2 ? 8 : 9;
  return 4 + 4 * nParams + 4 + (ver >= 3 ? 4 : 0) + (ver >= 5 ? 1 : 0);
}
// Slot: u8 name length, name, i32 trigger sync, i32 position sync, i32 force
// hold, u8 link, u8 key-held, u8 one-shot, envelope record.
void putGbSlot(ByteWriter& w, const tg::Slot& s) {
  std::string name;
  for (const char* p = s.name; *p && name.size() < 255; ++p) name += *p >= 32 && *p < 127 ? *p : '?';
  w.u8(static_cast<uint32_t>(name.size()));
  w.bytes(name.data(), name.size());
  w.i32(s.trig < 0 ? 5 : s.trig);  // plugin default: 4 beats
  w.i32(std::clamp(s.pos, 0, 5));
  w.i32(std::clamp(s.force, 0, 2));
  w.u8(s.link ? 1 : 0);
  w.u8(s.keyHeld ? 1 : 0);
  w.u8(s.oneShot ? 1 : 0);
  putEnvRecord(w, s.env);
}
// End of the slot banks starting at o (0 when malformed).
size_t gbSlotsEnd(const Bytes& b, size_t o) {
  for (int k = 0; k < 2 * tg::kSlots; ++k) {
    if (o >= b.size()) return 0;
    const size_t q = o + 1 + b[o] + 15;
    const size_t r = envRecordSize(b, q);
    if (!r) return 0;
    o = q + r;
  }
  return o;
}
Bytes gbSlotBytes(const std::string& data) {
  auto bank = std::make_unique<tg::Bank>();
  tg::factory(*bank);
  tg::parse(data.c_str(), *bank);
  ByteWriter w;
  for (int k = 0; k < tg::kSlots; ++k) putGbSlot(w, bank->time[k]);
  for (int k = 0; k < tg::kSlots; ++k) putGbSlot(w, bank->vol[k]);
  return w.b;
}

Encoder grossBeat() {
  Encoder e;
  e.flName = "Gross Beat";
  e.glType = "gl.timegate";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(kGbVersion);
    w.i32(0);      // 0 time slot
    w.i32(0);      // 1 volume slot
    w.i32(256);    // 2 time mix
    w.i32(256);    // 3 volume mix
    w.i32(1765);   // 4 attack
    w.i32(1674);   // 5 release
    w.i32(0);      // 6 volume tension
    w.i32(0);      // 7 hold flags
    w.i32(65536);  // 8 scratch offset (centre)
    w.i32(0);      // editor page
    w.u8(0);       // remove DC
    w.u8(0);       // attack compensation
    w.u8(1);       // HQ
    w.u8(1);       // previews
    w.u8(1);       // click reduction (normal)
    // Slots: Gridline's own factory envelopes (original designs).
    w.bytes(gbSlotBytes(""));
    return w.b;
  };
  // Slot envelopes: the banks are rewritten from the data when they differ.
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (!decoded("Gross Beat", b, c, cur) || cur.data == g.data) return;
    const size_t start = gbSlotStart(b), end = gbSlotsEnd(b, start);
    if (!end) return;
    Bytes out(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(start));
    const Bytes slots = gbSlotBytes(g.data);
    out.insert(out.end(), slots.begin(), slots.end());
    out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(end), b.end());
    b = std::move(out);
  };
  e.fields = [](const Bytes& b) {
    using namespace pp;
    const uint32_t ver = versionOf(b);
    std::vector<Field> v = tagFields(4, {{TgTimeSlot, 0, 0, tg::kSlots - 1}, {TgVolSlot, 1, 0, tg::kSlots - 1}, {TgTimeMix, 2, 0, 256},
                                         {TgVolMix, 3, 0, 256}, {TgVolAttack, 4, 0, 10000}, {TgVolRelease, 5, 0, 10000},
                                         {TgTension, 6, -256, 256}, {TgHold, 7, 0, 1}});
    if (ver >= 2) v.push_back(i32(TgScratch, 4 + 4 * 8, 0, 131072));
    const size_t opt = 4 + 4 * (ver < 2 ? 8 : 9) + 4;
    if (ver >= 3) {
      v.push_back(field(TgDcRemove, opt, Type::U8, 0, 1));
      v.push_back(field(TgCompensate, opt + 1, Type::U8, 0, 1));
      v.push_back(field(TgHq, opt + 2, Type::U8, 0, 1));
    }
    if (ver >= 5) v.push_back(field(TgDeclick, opt + 4, Type::U8, 0, 2));
    if (ver >= 9) {
      const size_t end = gbSlotsEnd(b, gbSlotStart(b));
      if (end && end < b.size()) v.push_back(field(TgMidiTrigger, end, Type::U8, 0, 1));
    }
    return v;
  };
  return e;
}

// ============================================================ Fruity Fast Dist
// i32 × 5: pre amp, threshold, type, mix, post.
Encoder fastDist() {
  Encoder e;
  e.flName = "Fruity Fast Dist";
  e.glType = "gl.fastdist";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return param(s, dy::FdClassic) >= 0.5f; };
  e.defaults = [] {
    ByteWriter w;
    w.i32(128);  // pre amp (unity)
    w.i32(10);   // threshold
    w.i32(0);    // type A
    w.i32(128);  // mix
    w.i32(128);  // post
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return tagFields(0, {{dy::FdPreK, 0, 64, 192}, {dy::FdDrive, 1, 1, 10}, {dy::FdType, 2, 0, 1}, {dy::FdMixK, 3, 0, 128}, {dy::FdPostK, 4, 0, 128}});
  };
  return e;
}

// ============================================================ Fruity WaveShaper
// u32 version (0x00040003: version 4), i32 pre, mix, post, u8 bipolar, u8
// centre, i32 oversampling, curve record, u32 panel, u8 HQ, u8 smooth.
constexpr size_t kWsCurve = 22;
Bytes wsCurveBytes(const std::string& data) {
  wscurve::Curve cv;
  if (!wscurve::parse(data.c_str(), cv)) cv.add(0, 0.f), cv.add(1, 1.f), cv.prepare();  // straight line
  ByteWriter w;
  putEnvRecord(w, cv);
  return w.b;
}

Encoder waveShaper() {
  Encoder e;
  e.flName = "Fruity WaveShaper";
  e.glType = "gl.waveshaper";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return param(s, pp::WsClassic) >= 0.5f; };
  e.defaults = [] {
    ByteWriter w;
    w.u32(0x00040003);  // version 4
    w.i32(128);         // pre amp (unity)
    w.i32(128);         // mix
    w.i32(128);         // post
    w.u8(0);            // bipolar off (unipolar, mirrored)
    w.u8(0);            // centre off
    w.i32(0);           // oversampling off
    w.bytes(wsCurveBytes(""));  // straight transfer line
    w.u32(0);           // panel
    w.u8(0);            // HQ
    w.u8(0);            // smooth
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (!decoded("Fruity WaveShaper", b, c, cur) || cur.data == g.data) return;
    const size_t n = envRecordSize(b, kWsCurve);
    if (!n) return;
    Bytes out(b.begin(), b.begin() + kWsCurve);
    const Bytes rec = wsCurveBytes(g.data);
    out.insert(out.end(), rec.begin(), rec.end());
    out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(kWsCurve + n), b.end());
    b = std::move(out);
  };
  e.fields = [](const Bytes& b) {
    using namespace pp;
    std::vector<Field> v = tagFields(4, {{WsPreAmp, 0, 64, 192}, {WsMix, 1, 0, 128}, {WsPost, 2, 0, 128}});
    v.push_back(field(WsBipolar, 16, Type::U8, 0, 1));
    v.push_back(field(WsCenter, 17, Type::U8, 0, 1));
    v.push_back(i32(WsOversample, 18, 0, 4));
    const size_t n = envRecordSize(b, kWsCurve);
    if (n && versionOf(b) > 1 && kWsCurve + n + 6 <= b.size()) {
      v.push_back(field(WsHq, kWsCurve + n + 4, Type::U8, 0, 1));
      v.push_back(field(WsSmooth, kWsCurve + n + 5, Type::U8, 0, 1));
    }
    return v;
  };
  return e;
}

// ============================================================ Fruity Flangus
// i32 × 8: order, depth, speed, delay, spread, cross, dry, wet.
Encoder flangus() {
  Encoder e;
  e.flName = "Fruity Flangus";
  e.glType = "gl.swirl";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return param(s, sf::SwModel) >= 0.5f; };
  e.defaults = [] {
    ByteWriter w;
    w.i32(4);    // order
    w.i32(512);  // depth
    w.i32(768);  // speed
    w.i32(8);    // delay
    w.i32(128);  // spread
    w.i32(-32);  // cross
    w.i32(96);   // dry
    w.i32(96);   // wet
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return tagFields(0, {{sf::SwOrder, 0, 1, 9},       {sf::SwDepthK, 1, 0, 1024},   {sf::SwSpeedK, 2, 0, 1024}, {sf::SwDelayK, 3, 0, 128},
                         {sf::SwSpreadK, 4, 0, 128},   {sf::SwCrossK, 5, -128, 128}, {sf::SwDryK, 6, -128, 128}, {sf::SwWetK, 7, -128, 128}});
  };
  return e;
}

// ============================================================ Fruity PanOMatic
// i32 × 6: pan, volume, LFO shape, target, amount, speed.
Encoder panOMatic() {
  Encoder e;
  e.flName = "Fruity PanOMatic";
  e.glType = "gl.autopan";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return param(s, pp::ApClassic) >= 0.5f; };
  e.defaults = [] {
    ByteWriter w;
    w.i32(128);    // pan (centre)
    w.i32(256);    // volume (unity)
    w.i32(0);      // LFO shape (sine)
    w.i32(0);      // LFO target (off)
    w.i32(64);     // amount
    w.i32(27500);  // speed
    return w.b;
  };
  e.fields = [](const Bytes&) {
    using namespace pp;
    return tagFields(0, {{ApStaticPan, 0, 0, 256}, {ApStaticVol, 1, 0, 320}, {ApShape, 2, 0, 2}, {ApTarget, 3, 0, 2}, {ApAmount, 4, 0, 128},
                         {ApSpeedK, 5, 0, 55000}});
  };
  return e;
}

// ============================================================ Fruity Chorus
// u32 version 1, i32 × 12: delay, depth, stereo, LFO 1-3 rate, LFO 1-3 wave,
// cross type, cross cutoff, wet only.
Encoder chorus() {
  Encoder e;
  e.flName = "Fruity Chorus";
  e.glType = "gl.chorus";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return param(s, pp::ChoClassic) >= 0.5f; };
  e.defaults = [] {
    ByteWriter w;
    w.u32(1);      // version
    w.i32(512);    // 0 delay
    w.i32(2250);   // 1 depth
    w.i32(341);    // 2 stereo
    w.i32(1500);   // 3 LFO 1 rate
    w.i32(2500);   // 4 LFO 2 rate
    w.i32(3500);   // 5 LFO 3 rate
    w.i32(23);     // 6 LFO 1 wave
    w.i32(0);      // 7 LFO 2 wave
    w.i32(0);      // 8 LFO 3 wave
    w.i32(0);      // 9 cross type
    w.i32(2500);   // 10 cross cutoff
    w.i32(0);      // 11 wet only
    return w.b;
  };
  static const int kGl[12] = {pp::ChoDelay, pp::ChoDepthMs, pp::ChoStereo, pp::ChoRate, pp::ChoRate2, pp::ChoRate3,
                              pp::ChoWave1, pp::ChoWave2, pp::ChoWave3, pp::ChoCrossMode, pp::ChoCrossFreq, pp::ChoWetOnly};
  static const int kRange[12] = {1024, 5000, 1024, 5000, 5000, 5000, 1024, 1024, 1024, 100, 5000, 1024};
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    // J-44: the older float layout (no version, 12 × f32 0..1 of each range).
    if (b.size() == 48)
      for (size_t k = 0; k < 12; ++k) f.push_back(field(kGl[k], 4 * k, Type::F32, 0, 1));
    else
      for (size_t k = 0; k < 12; ++k) f.push_back(i32(kGl[k], 4 + 4 * k, 0, kRange[k]));
    return f;
  };
  // J-44: values between the integer steps (states FL saved in the float
  // layout: "Chorus send") are written in that layout, which FL still loads —
  // when it reaches more of the values than the integer one (the float grid
  // misses other values: an LFO rate of 2.45 Hz is only on the integer one).
  // Only the continuous knobs (delay, depth, stereo, LFO rates, cross cutoff)
  // can fall between steps; each is searched on its monotonic scale.
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    if (b.size() != 52) return;
    auto misses = [&](Bytes t, bool flt) {
      PluginState cur;
      int n = 0;
      for (size_t k : {0u, 1u, 2u, 3u, 4u, 5u, 10u}) {
        const float want = param(g, kGl[k]);
        auto at = [&](double v) {
          if (flt) {
            const float x = static_cast<float>(v);
            uint32_t u;
            std::memcpy(&u, &x, 4);
            putU32(t, 4 * k, u);
          } else {
            putI32(t, 4 + 4 * k, static_cast<int32_t>(v));
          }
          return decoded("Fruity Chorus", t, c, cur) ? param(cur, kGl[k]) : NAN;
        };
        double a = 0, z = flt ? 1.0 : kRange[k];
        if (!(at(a) <= want && want <= at(z))) continue;  // outside the knob: the solver clamps it
        bool hit = at(a) == want || at(z) == want;
        for (int it = 0; it < 64 && !hit; ++it) {
          const double m = flt ? static_cast<float>(0.5 * (a + z)) : std::floor(0.5 * (a + z));
          if (m == a || m == z) break;
          const float v = at(m);
          hit = v == want;
          (v < want ? a : z) = m;
        }
        for (double r : {a, z}) {  // neighbours in the stored precision
          float x = static_cast<float>(r);
          for (int d = 0; d < 4 && flt && !hit; ++d, x = std::nextafter(x, 2.0f)) hit = at(x) == want;
        }
        n += hit ? 0 : 1;
      }
      return n;
    };
    const int onInts = misses(b, false);
    if (onInts == 0) return;
    Bytes f(48);
    for (size_t k = 0; k < 12; ++k) {
      const float x = static_cast<float>(static_cast<int32_t>(getU32(b, 4 + 4 * k))) / static_cast<float>(kRange[k]);
      uint32_t u;
      std::memcpy(&u, &x, 4);
      putU32(f, 4 * k, u);
    }
    if (misses(f, true) >= onInts) return;
    b = std::move(f);
  };
  return e;
}

// ============================================================ Fruity Love Philter
// u32 version (0x000C0003: version 12), then a zlib stream holding (see the
// decoder): 8 global ints, i32 0, f32 X/Y smoothing coefficient, i32 solo,
// i32 HQ envelopes, i32 oversampling, 6 option bytes, i32 ui, varint +
// UTF-16 info text, then 8 units { 12 knobs, order, type, u8 alt / enabled /
// shaper on, shaper record, u8 bipolar, 8 targets { i32 1, f32 rest, f32 1,
// f32 1, 10 × {pattern record, i32 × 4}, LFO record, i32 × 5, 3 map records } },
// u8 0. Records: u32 kind, points, u32, i32 × 4 (…, loop start, loop end).
// Everything lives in the compressed stream, so the encoder writes it whole
// (write) instead of through fields.
namespace cr = gl::packs::creative;
constexpr uint32_t kLpVersion = 0x000C0003;
struct LpKnobDef {
  int f;
  double div;  // stored value per Gridline unit
};
const LpKnobDef kLpUnitKnobs[12] = {{cr::UEnv, 128},  {cr::UCut, 256},   {cr::URes, 256}, {cr::ULow, 128}, {cr::UBand, 128}, {cr::UHigh, 128},
                                    {cr::UWsPre, 128}, {cr::UWsMix, 128}, {cr::UPan, 128}, {cr::UOut, 128}, {cr::UIn, 128},  {cr::UNext, 128}};
// Follower attack / release law (as the decoder reads it), for the inverse.
double lpFollowerSeconds(double v) { return std::max(0.0005, 0.0125 * (std::pow(81.0, std::clamp(v, 0.0, 16384.0) / 16384.0) - 1.0)); }
int32_t lpFollowerStored(float s) {
  const double v = 16384.0 * std::log1p(std::max(0.0, static_cast<double>(s)) / 0.0125) / std::log(81.0);
  int32_t best = static_cast<int32_t>(std::clamp(std::round(v), 0.0, 16384.0));
  for (int32_t d = -3; d <= 3; ++d) {
    const int32_t c = std::clamp(best + d, 0, 16384);
    if (static_cast<float>(lpFollowerSeconds(c)) == s) return c;
  }
  return best;
}
int32_t scaled(float v, double k) { return static_cast<int32_t>(std::lround(static_cast<double>(v) * k)); }

cr::OEnv flatEnv(float y, bool twoPoints) {
  cr::OEnv e;
  e.add(0, y);
  if (twoPoints) e.add(1, y);
  e.prepare();
  return e;
}

// Info text: 7-bit varint character count + UTF-16LE (line breaks as CR).
void putLpInfo(ByteWriter& w, const std::string& text) {
  std::vector<uint16_t> u;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    uint32_t cp = c, len = 1;
    if (c >= 0xF0) cp = c & 0x07, len = 4;
    else if (c >= 0xE0) cp = c & 0x0F, len = 3;
    else if (c >= 0xC0) cp = c & 0x1F, len = 2;
    for (uint32_t k = 1; k < len && i + k < text.size(); ++k) cp = cp << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    i += len;
    u.push_back(cp == '\n' ? uint16_t{'\r'} : cp < 0x10000 ? static_cast<uint16_t>(cp) : uint16_t{'?'});
  }
  size_t n = u.size();
  do {
    w.u8(static_cast<uint32_t>((n & 0x7F) | (n > 0x7F ? 0x80 : 0)));
    n >>= 7;
  } while (n);
  for (uint16_t x : u) w.u16(x);
}

// Writes a record: kind, points, tail (u32, i32, i32 as given), loop points.
void putLpRecord(ByteWriter& w, uint32_t kind, const cr::OEnv& e, uint32_t t0, int32_t t1, int32_t t2, int32_t ls, int32_t le) {
  w.u32(kind);
  putPoints(w, e);
  w.u32(t0);
  w.i32(t1);
  w.i32(t2);
  w.i32(ls);
  w.i32(le);
}

// Fresh decompressed state (FL 25 layout, plugin defaults).
Bytes lpDefaultRaw() {
  ByteWriter w;
  w.i32(128);    // master level
  w.i32(128);    // master LFO
  w.i32(128);    // follower level
  w.i32(4096);   // follower attack
  w.i32(8192);   // follower release
  w.i32(1);      // pattern 1
  w.i32(512);    // mod X
  w.i32(512);    // mod Y
  w.i32(0);      // reserved
  w.f32(0.9685255885124207f);  // X / Y smoothing coefficient
  w.i32(-1);     // solo: none
  w.i32(0);      // HQ envelopes
  w.i32(1);      // oversampling
  w.u8(1);       // smooth
  w.u8(0);       // centre
  w.u8(1);       // sync patterns
  w.u8(1);       // ui
  w.u8(1);       // (version 10)
  w.u8(0);       // (version 11)
  w.i32(0);      // ui
  putLpInfo(w, "");
  cr::OEnv ws;
  cr::defaultWs(ws);
  const cr::OEnv one = flatEnv(1.f, false);
  for (int u = 0; u < cr::kUnits; ++u) {
    for (int j = 0; j < 12; ++j) {
      const int f = kLpUnitKnobs[j].f;
      w.i32(f == cr::UBand || f == cr::UHigh || f == cr::UPan || f == cr::UNext ? 0 : 128);  // unity / centre / off
    }
    w.i32(1);              // order: single
    w.i32(0);              // type: state variable
    w.u8(0);               // alt
    w.u8(u == 0 ? 1 : 0);  // enabled (first unit)
    w.u8(0);               // shaper on
    putLpRecord(w, 3, ws, 0, -1, -1, -1, -1);
    w.u8(0);  // bipolar shaper
    for (int t = 0; t < cr::kTargets; ++t) {
      const bool add = cr::additiveTarget(t);
      w.i32(1);
      w.f32(add ? 0.f : 1.f);  // rest value
      w.f32(1.f);
      w.f32(1.f);
      for (int k = 0; k < cr::kPatterns; ++k) {
        putLpRecord(w, 1, one, 1, 0, -1, -1, -1);
        w.i32(128), w.i32(128), w.i32(0), w.i32(128);  // pattern options
      }
      putLpRecord(w, 2, one, 1, 0, -1, -1, -1);  // LFO shape
      w.i32(40000), w.i32(0), w.i32(0), w.i32(0), w.i32(0);  // LFO speed, tension, skew, pulse, phase
      const cr::OEnv map = flatEnv(add ? 0.5f : 1.f, true);
      for (int q = 0; q < 3; ++q) putLpRecord(w, 3, map, 0, -1, -1, -1, -1);  // X / Y / follower maps
    }
  }
  w.u8(0);
  return w.b;
}

Bytes lpPack(const Bytes& head, const Bytes& raw) {
  Bytes out = head;
  const Bytes z = png::deflateZlib(raw.data(), raw.size());
  out.insert(out.end(), z.begin(), z.end());
  return out;
}

// Walks a decompressed template, copying it with the instance's values.
struct LpWalk {
  explicit LpWalk(const Bytes& src) : s(src) {}
  const Bytes& s;
  size_t o = 0;
  ByteWriter w;
  bool ok = true;
  bool has(size_t n) const { return ok && o + n <= s.size(); }
  void copy(size_t n) {
    if (!has(n)) return void(ok = false);
    w.bytes(s.data() + o, n);
    o += n;
  }
  int32_t peek() const { return has(4) ? static_cast<int32_t>(getU32(s, o)) : 0; }
  void i32(int32_t v) {
    if (!has(4)) return void(ok = false);
    w.i32(v);
    o += 4;
  }
  void u8(bool v) {
    if (!has(1)) return void(ok = false);
    w.u8(v ? 1 : 0);
    o += 1;
  }
  // Record at o: size (0 = malformed) and its points (x, y, tension; tension 0 for forced-straight points).
  size_t recSize() const {
    if (!has(12)) return 0;
    const uint32_t fmt = getU32(s, o + 4), n = getU32(s, o + 8);
    if (fmt < 1 || fmt > 3 || n > 5000) return 0;
    const size_t size = 12 + static_cast<size_t>(n) * (fmt >= 2 ? 24 : 16) + 20;
    return has(size) ? size : 0;
  }
  struct Pt {
    double x, y, t;
  };
  std::vector<Pt> points() const {
    std::vector<Pt> v;
    if (!recSize()) return v;
    const uint32_t fmt = getU32(s, o + 4), n = getU32(s, o + 8);
    size_t p = o + 12;
    double x = 0;
    for (uint32_t i = 0; i < n; ++i, p += fmt >= 2 ? 24 : 16) {
      const double dx = fmt >= 2 ? getF64(s, p) : getF32(s, p), y = fmt >= 2 ? getF64(s, p + 8) : getF32(s, p + 4);
      double t = getF32(s, p + (fmt >= 2 ? 16 : 8));
      const uint32_t fl = getU32(s, p + (fmt >= 2 ? 20 : 12));
      if (static_cast<int8_t>(fl >> 24) == 2 && (fl & 0xFF) != menv::Hold) t = 0;
      x += std::max(0.0, dx);
      v.push_back({x, std::clamp(y, 0.0, 1.0), std::clamp(t, -1.0, 1.0)});
    }
    return v;
  }
  // Rewrites the record with `e` (null: copied as it is); loops < -1 keep the template's.
  void rec(const cr::OEnv* e, int32_t ls = -2, int32_t le = -2) {
    const size_t n = recSize();
    if (!n) return void(ok = false);
    if (!e) return copy(n);
    const size_t tail = o + n - 20;
    putLpRecord(w, getU32(s, o), *e, getU32(s, tail), static_cast<int32_t>(getU32(s, tail + 4)), static_cast<int32_t>(getU32(s, tail + 8)),
                ls < -1 ? static_cast<int32_t>(getU32(s, tail + 12)) : ls, le < -1 ? static_cast<int32_t>(getU32(s, tail + 16)) : le);
    o += n;
  }
};

void lpWrite(const PluginState& g, Bytes& st, const EncodeContext& c) {
  PluginState cur;
  if (!decoded("Fruity Love Philter", st, c, cur) || (cur.params == g.params && cur.data == g.data)) return;
  const uint32_t ver = versionOf(st);
  size_t z = 0;
  for (size_t p = 4; p + 2 < st.size() && p < 20 && !z; ++p)
    if (st[p] == 0x78) z = p;
  if (!z) return;
  const Bytes raw = fx::inflateZlib(st.data() + z, st.size() - z);
  if (raw.size() < 64) return;
  auto bank = std::make_unique<cr::OctaBank>();
  cr::parse(g.data.c_str(), *bank);
  auto P = [&](int k) { return param(g, k); };
  LpWalk r(raw);
  r.i32(scaled(P(cr::OfMaster), 128));
  r.i32(scaled(P(cr::OfMasterLfo), 128));
  r.i32(scaled(P(cr::OfIefLevel), 128));
  r.i32(lpFollowerStored(P(cr::OfIefAttack)));
  r.i32(lpFollowerStored(P(cr::OfIefRelease)));
  r.i32(scaled(P(cr::OfPattern), 1) + 1);
  r.i32(scaled(P(cr::OfModX), 1024));
  r.i32(scaled(P(cr::OfModY), 1024));
  r.copy(8);  // reserved, smoothing coefficient
  r.i32(P(cr::OfSolo) >= 0.5f ? scaled(P(cr::OfSolo), 1) - 1 : -1);
  const int32_t hq = r.peek();
  r.i32(P(cr::OfHqEnvelopes) >= 0.5f ? std::max(1, hq) : 0);
  r.i32(scaled(P(cr::OfOversample), 1));
  r.u8(P(cr::OfSmooth) >= 0.5f);
  r.u8(P(cr::OfCenter) >= 0.5f);
  r.u8(P(cr::OfSyncPatterns) >= 0.5f);
  r.copy(1 + (ver >= 10 ? 1 : 0) + (ver >= 11 ? 1 : 0));
  r.copy(4);  // ui
  {           // info text
    size_t chars = 0;
    for (int shift = 0; r.has(1) && shift < 28; shift += 7) {
      const uint8_t v = raw[r.o++];
      chars |= static_cast<size_t>(v & 0x7F) << shift;
      if (!(v & 0x80)) break;
    }
    if (!r.has(2 * chars)) return;
    r.o += 2 * chars;
    putLpInfo(r.w, cr::infoText(g.data));
  }
  cr::OEnv defWs;
  cr::defaultWs(defWs);
  const cr::OEnv one = flatEnv(1.f, false);
  for (int u = 0; u < cr::kUnits && r.ok; ++u) {
    for (const LpKnobDef& k : kLpUnitKnobs) r.i32(scaled(P(cr::unitParam(u, k.f)), k.div));
    r.i32(scaled(P(cr::unitParam(u, cr::UOrder)), 1));
    r.i32(scaled(P(cr::unitParam(u, cr::UType)), 1));
    r.u8(P(cr::unitParam(u, cr::UAlt)) >= 0.5f);
    r.u8(P(cr::unitParam(u, cr::UEnabled)) >= 0.5f);
    r.u8(P(cr::unitParam(u, cr::UWsOn)) >= 0.5f);
    {  // shaper curve: the default bend when the data has none
      const cr::OEnv* e = bank->wsCurve(u);
      if (!e) {
        const auto pts = r.points();
        bool same = static_cast<int>(pts.size()) == defWs.n;
        for (int i = 0; same && i < defWs.n; ++i)
          same = std::fabs(pts[static_cast<size_t>(i)].x - defWs.x[i]) < 1e-6 && std::fabs(pts[static_cast<size_t>(i)].y - defWs.y[i]) < 1e-6 &&
                 std::fabs(pts[static_cast<size_t>(i)].t - defWs.t[i]) < 1e-4;
        if (!same) e = &defWs;
      }
      r.rec(e);
    }
    r.u8(P(cr::unitParam(u, cr::UWsBipolar)) >= 0.5f);
    for (int t = 0; t < cr::kTargets && r.ok; ++t) {
      const bool add = cr::additiveTarget(t);
      r.copy(16);
      for (int k = 0; k < cr::kPatterns; ++k) {
        const cr::OEnv* e = bank->env(u, t, k);
        if (e) r.rec(e, bank->loopS[u][t][k], bank->loopE[u][t][k]);
        else r.rec(r.points().size() >= 2 ? &one : nullptr, -1, -1);  // a single point = unused
        r.copy(16);
      }
      {  // LFO shape: unused = one point at 1
        const cr::OEnv* e = bank->env(u, t, cr::KLfo);
        const auto pts = r.points();
        const bool unused = pts.empty() || (pts.size() == 1 && std::fabs(pts[0].y - 1.0) <= 1e-6);
        r.rec(e ? e : unused ? nullptr : &one);
      }
      r.i32(scaled(P(cr::lfoParam(u, t, cr::LSpeed)), 65536));
      r.i32(scaled(P(cr::lfoParam(u, t, cr::LTension)), 128));
      r.i32(scaled(P(cr::lfoParam(u, t, cr::LSkew)), 128));
      r.i32(scaled(P(cr::lfoParam(u, t, cr::LPulse)), 128));
      if (ver >= 5) r.i32(scaled(P(cr::lfoParam(u, t, cr::LPhase)), 65536));
      const float def = add ? 0.5f : 1.f;
      const cr::OEnv flat = flatEnv(def, true);
      for (int q = 0; q < 3; ++q) {  // maps: unused = flat at the target's rest value
        const cr::OEnv* e = bank->env(u, t, cr::KMapX + q);
        const auto pts = r.points();
        bool unused = true;
        if (pts.size() >= 2) {
          for (const auto& p : pts) unused = unused && std::fabs(p.y - pts.front().y) <= 1e-6;
        } else if (pts.size() == 1) {
          unused = std::fabs(pts[0].y - def) <= 1e-6;
        }
        r.rec(e ? e : unused ? nullptr : &flat);
      }
    }
  }
  if (!r.ok) return;
  r.copy(raw.size() - r.o);
  st = lpPack(Bytes(st.begin(), st.begin() + static_cast<std::ptrdiff_t>(z)), r.w.b);
}

Encoder lovePhilter() {
  Encoder e;
  e.flName = "Fruity Love Philter";
  e.glType = "gl.octafilter";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter h;
    h.u32(kLpVersion);
    return lpPack(h.b, lpDefaultRaw());
  };
  e.write = lpWrite;  // no fields: every value is inside the compressed stream
  return e;
}

}  // namespace

void registerFxK3(std::vector<Encoder>& v) {
  v.push_back(lovePhilter());
  v.push_back(grossBeat());
  v.push_back(fastDist());
  v.push_back(waveShaper());
  v.push_back(flangus());
  v.push_back(panOMatic());
  v.push_back(chorus());
}

}  // namespace gl::flp::enc
