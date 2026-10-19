// Gridline — FLP import: κ3 decoders (Gross Beat → Time Gate; more κ3
// plugins below). Registered after the family files, so these win.
//
// Gross Beat state (plugin data event; established from the shipped presets
// and render test; the code is original):
//   u32 version (>= 0x10000: version >> 16)
//   i32 x 9 parameters by tag (8 before version 2): time slot, volume slot,
//       time mix, volume mix (0..256), attack, release (0..10000), volume
//       tension (-256..256), hold flags (bit 0), scratch offset (65536 = centre)
//   i32 editor page, u8 remove DC, u8 attack compensation, u8 HQ, u8 previews
//       (version >= 3), u8 click reduction (version >= 5)
//   72 slots (36 time, 36 volume): u8 name length, name, i32 trigger sync,
//       i32 position sync, i32 force hold, u8 link, u8 key-held, u8 one-shot,
//       envelope record { u32 3, u32 2|3, u32 n, n x { f64 dx beats, f64 y,
//       f32 tension, u32 mode | sign << 24 }, u32, i32 x 4 }
//   u8 trigger slots from MIDI (version >= 9; older states: on)
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include "../../engine/TimeGateSlots.h"
#include "../../engine/WaveCurve.h"
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/spacefx/spacefx.h"
#include "../../packs/creative/creative.h"
#include "../PluginParams.h"
#include "FlpFx.h"

namespace gl::flp::fx {

namespace {

namespace pp = gl::pp;

uint32_t le32(const std::vector<uint8_t>& b, size_t o) {
  return o + 4 <= b.size() ? static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) | (static_cast<uint32_t>(b[o + 2]) << 16) |
                                 (static_cast<uint32_t>(b[o + 3]) << 24)
                           : 0;
}
double lef64(const std::vector<uint8_t>& b, size_t o) {
  uint64_t u = static_cast<uint64_t>(le32(b, o)) | (static_cast<uint64_t>(le32(b, o + 4)) << 32);
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}
float lef32(const std::vector<uint8_t>& b, size_t o) {
  uint32_t u = le32(b, o);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
// Automation mapping plus raw recorded values (control ranges of the table).
void bindAll(PluginMapping& m, std::shared_ptr<const KnobTable> t) {
  bindTable(m, t);
  std::vector<RawRange> r;
  for (const Knob& k : *t) r.push_back({k.flTag, k.flMin, k.flMax});
  setRawRanges(m, std::move(r));
}

std::vector<uint8_t> payload(const std::vector<Event>& evs) {
  const Event* e = pluginState(evs);
  return e ? std::vector<uint8_t>(e->data, e->data + e->size) : std::vector<uint8_t>{};
}

// ============================================================ Gross Beat
struct GbPoint {
  double x;  // absolute beats
  double y;
  float t;
  uint32_t flags;
};
struct GbSlot {
  std::string name;
  int trig = 5, pos = 5, force = 0;
  bool link = false, keyHeld = false, oneShot = false;
  std::vector<GbPoint> pts;
};

// Parses one slot at o; returns the offset after it (0 on failure).
size_t gbSlotAt(const std::vector<uint8_t>& b, size_t o, GbSlot& s) {
  if (o >= b.size()) return 0;
  size_t len = b[o];
  size_t q = o + 1 + len;
  if (q + 15 + 12 > b.size()) return 0;
  s.name.clear();
  for (size_t i = 0; i < len; ++i) {
    unsigned char ch = b[o + 1 + i];
    s.name += ch >= 32 && ch < 127 ? static_cast<char>(ch) : '?';
  }
  s.trig = static_cast<int32_t>(le32(b, q)), s.pos = static_cast<int32_t>(le32(b, q + 4)), s.force = static_cast<int32_t>(le32(b, q + 8));
  s.link = b[q + 12] != 0, s.keyHeld = b[q + 13] != 0, s.oneShot = b[q + 14] != 0;
  if (s.trig < 0 || s.trig > 5 || s.pos < 0 || s.pos > 5 || s.force < 0 || s.force > 2) return 0;
  size_t r = q + 15;
  uint32_t a = le32(b, r), ver = le32(b, r + 4), n = le32(b, r + 8);
  if (a != 3 || (ver != 2 && ver != 3) || n < 1 || n > 8192) return 0;
  size_t p = r + 12;
  if (p + static_cast<size_t>(n) * 24 + 20 > b.size()) return 0;
  s.pts.clear();
  double x = 0;
  for (uint32_t i = 0; i < n; ++i, p += 24) {
    double dx = lef64(b, p), y = lef64(b, p + 8);
    float t = lef32(b, p + 16);
    if (!std::isfinite(dx) || !std::isfinite(y) || !std::isfinite(t) || dx < 0 || dx > 1024) return 0;
    x += dx;
    s.pts.push_back({x, y, t, le32(b, p + 20)});
  }
  return p + 20;
}

double gbAttack(double v) { return (std::pow(501.0, std::clamp(v, 0.0, 10000.0) / 10000.0) - 1.0) * 0.001; }        // s
double gbRelease(double v) { return 2.0 * (std::pow(501.0, std::clamp(v, 0.0, 10000.0) / 10000.0) - 1.0) * 0.001; }  // s

std::shared_ptr<const KnobTable> grossBeatTable() {
  static auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, tg::kSlots - 1, pp::TgTimeSlot, [](double v) { return std::clamp(std::round(v), 0.0, tg::kSlots - 1.0) + 1.0; }},
      {1, 0, tg::kSlots - 1, pp::TgVolSlot, [](double v) { return std::clamp(std::round(v), 0.0, tg::kSlots - 1.0) + 1.0; }},
      {2, 0, 256, pp::TgTimeMix, [](double v) { return std::clamp(v / 256.0, 0.0, 1.0); }},
      {3, 0, 256, pp::TgVolMix, [](double v) { return std::clamp(v / 256.0, 0.0, 1.0); }},
      {4, 0, 10000, pp::TgVolAttack, [](double v) { return std::clamp(gbAttack(v), 0.0001, 0.5); }},
      {5, 0, 10000, pp::TgVolRelease, [](double v) { return std::clamp(gbRelease(v), 0.0001, 1.0); }},
      {6, -256, 256, pp::TgTension, [](double v) { return std::clamp(v / 256.0, -1.0, 1.0); }},
      {7, 0, 1, pp::TgHold, [](double v) { return (static_cast<int>(std::lround(v)) & 1) ? 1.0 : 0.0; }},
      {8, 0, 131072, pp::TgScratch, [](double v) { return std::clamp(-8.0 * (v / 65536.0 - 1.0), -8.0, 8.0); }},
  });
  return t;
}

bool decodeGrossBeat(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = payload(evs);
  begin(m, "gl.timegate");
  auto table = grossBeatTable();
  bindAll(m, table);
  if (b.size() < 48) return true;
  uint32_t ver = le32(b, 0);
  if (ver >= 0x10000) ver >>= 16;
  const size_t nParams = ver < 2 ? 8 : 9;
  std::vector<double> fl(9, 0.0);
  fl[8] = 65536;
  for (size_t k = 0; k < nParams; ++k) fl[k] = static_cast<int32_t>(le32(b, 4 + 4 * k));
  size_t opt = 4 + 4 * nParams + 4;  // after the editor page
  // Slots: the computed start first, then a scan (unknown older layouts).
  std::vector<GbSlot> slots;
  auto tryAt = [&](size_t start) {
    std::vector<GbSlot> v;
    size_t o = start;
    for (int k = 0; k < 2 * tg::kSlots; ++k) {
      GbSlot s;
      size_t e = gbSlotAt(b, o, s);
      if (!e) return false;
      v.push_back(std::move(s));
      o = e;
    }
    slots = std::move(v);
    if (ver >= 9 && o < b.size()) set(m, pp::TgMidiTrigger, b[o] ? 1 : 0);
    return true;
  };
  size_t start = opt + (ver >= 3 ? 4 : 0) + (ver >= 5 ? 1 : 0);
  bool ok = tryAt(start);
  for (size_t p = 36; !ok && p < std::min<size_t>(b.size(), 96); ++p) ok = tryAt(p);
  if (!ok) c.report.warn("Gross Beat: slot banks not decoded");
  if (ver >= 3 && opt + 4 <= b.size()) {
    set(m, pp::TgDcRemove, b[opt] ? 1 : 0);
    set(m, pp::TgCompensate, b[opt + 1] ? 1 : 0);
    set(m, pp::TgHq, b[opt + 2] ? 1 : 0);
  }
  set(m, pp::TgDeclick, ver >= 5 && opt + 5 <= b.size() ? std::clamp<int>(b[opt + 4], 0, 2) : 1);
  if (ver < 9) set(m, pp::TgMidiTrigger, 1);
  applyTable(m, *table, fl);
  set(m, pp::TgMode, 0);
  set(m, pp::TgGateOn, 0);
  bool lossy = false;
  if (ok) {
    std::string data = "GB2\n";
    for (int k = 0; k < 2 * tg::kSlots; ++k) {
      const GbSlot& g = slots[static_cast<size_t>(k)];
      auto s = std::make_unique<tg::Slot>();
      s->setName(g.name.c_str());
      s->trig = g.trig, s->pos = g.pos, s->force = g.force, s->link = g.link, s->keyHeld = g.keyHeld, s->oneShot = g.oneShot;
      size_t n = g.pts.size();
      size_t step = n > static_cast<size_t>(tg::kMaxPts) ? (n + tg::kMaxPts - 1) / tg::kMaxPts : 1;
      if (step > 1) lossy = true;
      for (size_t i = 0; i < n; ++i) {
        if (i % step != 0 && i + 1 != n) continue;
        const GbPoint& p = g.pts[i];
        uint8_t mode = static_cast<uint8_t>(p.flags & 0xFF);
        int8_t sign = static_cast<int8_t>(p.flags >> 24);
        float t = std::clamp(p.t, -1.0f, 1.0f);
        if (mode >= menv::ModeCount) mode = menv::SingleCurve, lossy = true;
        if (sign == 2 && mode != menv::Hold) mode = menv::SingleCurve, t = 0;  // forced linear segment
        s->env.add(p.x, static_cast<float>(std::clamp(p.y, 0.0, 1.0)), t == 0.0f ? 0.0f : t, mode);
      }
      s->len = static_cast<float>(std::max(0.25, std::min(16.0, g.pts.empty() ? 4.0 : g.pts.back().x)));
      if (s->env.n == 0) s->flat(1.f);
      data += tg::serialize(k < tg::kSlots ? 'T' : 'V', k % tg::kSlots, *s) + "\n";
    }
    m.state.data = data;
  }
  m.exact = ok && !lossy;  // read-back interpolation and declick crossfade lengths are Gridline's own
  return true;
}

// ============================================================ Fruity Fast Dist
// i32 x 5 in tag order: pre amp 64..192, threshold 1..10, type 0 / 1, mix 0..128, post 0..128.
std::shared_ptr<const KnobTable> fastDistTable() {
  namespace dy = gl::packs::dynamics;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 64, 192, dy::FdPreK, [](double v) { return std::clamp(v / 128.0, 0.5, 1.5); }},
      {1, 1, 10, dy::FdDrive, [](double v) { return std::clamp(std::round(v), 1.0, 10.0); }},
      {2, 0, 1, dy::FdType, [](double v) { return v >= 0.5 ? 1.0 : 0.0; }},
      {3, 0, 128, dy::FdMixK, [](double v) { return std::clamp(v / 128.0, 0.0, 1.0); }},
      {4, 0, 128, dy::FdPostK, [](double v) { return std::clamp(v / 128.0, 0.0, 1.0); }},
  });
  return t;
}
bool decodeFastDist(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = payload(evs);
  if (b.size() < 20) return false;
  std::vector<double> v(5);
  for (size_t k = 0; k < 5; ++k) v[k] = static_cast<int32_t>(le32(b, 4 * k));
  begin(m, "gl.fastdist");
  applyTable(m, *fastDistTable(), v);
  bindAll(m, fastDistTable());
  set(m, gl::packs::dynamics::FdClassic, 1);
  m.exact = true;
  return true;
}

// ============================================================ Fruity WaveShaper
// i32 version (>= 0x10000: version >> 16), i32 pre (64..192), mix, post (0..128),
// u8 bipolar, u8 centre, i32 oversampling (0..4 = 2^n), curve record {u32 kind,
// u32 format (0: f32 dx, y, tension; 1: + u32 flags; 2 / 3: f64 dx, f64 y,
// f32 tension, u32 flags), u32 n, points, u32, i32 x 4}, then (version > 1)
// u32 panel shown, u8 HQ, u8 smooth. dx is the distance from the previous point;
// flags byte 0 = segment mode.
std::shared_ptr<const KnobTable> waveShaperTable() {
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 64, 192, pp::WsPreAmp, [](double v) { return std::clamp(v / 128.0, 0.5, 1.5); }},
      {1, 0, 128, pp::WsMix, [](double v) { return std::clamp(v / 128.0, 0.0, 1.0); }},
      {2, 0, 128, pp::WsPost, [](double v) { return std::clamp(v / 128.0, 0.0, 1.0); }},
  });
  return t;
}
bool decodeWaveShaper(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = payload(evs);
  if (b.size() < 34) return false;
  uint32_t ver = le32(b, 0);
  if (ver >= 0x10000) ver >>= 16;
  std::vector<double> g = {static_cast<double>(static_cast<int32_t>(le32(b, 4))), static_cast<double>(static_cast<int32_t>(le32(b, 8))),
                           static_cast<double>(static_cast<int32_t>(le32(b, 12)))};
  const bool bipolar = b[16] != 0, centre = b[17] != 0;
  const int os = std::clamp(static_cast<int32_t>(le32(b, 18)), 0, 4);
  begin(m, "gl.waveshaper");
  applyTable(m, *waveShaperTable(), g);
  bindAll(m, waveShaperTable());
  set(m, pp::WsClassic, 1);
  set(m, pp::WsBipolar, bipolar ? 0 : 1);
  set(m, pp::WsCenter, centre ? 1 : 0);
  set(m, pp::WsOversample, os);
  size_t o = 22;
  uint32_t fmt = le32(b, o + 4), n = le32(b, o + 8);
  size_t sz = fmt == 0 ? 12 : fmt == 1 ? 16 : 24;
  o += 12;
  if (fmt > 3 || n < 1 || n > 4096 || o + n * sz > b.size()) {
    c.report.warn("WaveShaper: curve not decoded");
    m.exact = false;
    return true;
  }
  wscurve::Curve cv;
  double x = 0;
  bool lossy = n > static_cast<uint32_t>(wscurve::kPts);
  size_t step = lossy ? (n + wscurve::kPts - 1) / wscurve::kPts : 1;
  for (uint32_t i = 0; i < n; ++i, o += sz) {
    double dx = fmt >= 2 ? lef64(b, o) : lef32(b, o), y = fmt >= 2 ? lef64(b, o + 8) : lef32(b, o + 4);
    float t = lef32(b, o + (fmt >= 2 ? 16 : 8));
    uint8_t mode = fmt >= 1 ? static_cast<uint8_t>(le32(b, o + (fmt >= 2 ? 20 : 12)) & 0xFF) : menv::SingleCurve;
    if (!std::isfinite(dx) || !std::isfinite(y)) break;
    x += std::max(0.0, dx);
    if (i % step != 0 && i + 1 != n) continue;
    if (mode >= menv::ModeCount) mode = menv::SingleCurve;
    if (!std::isfinite(t)) t = 0;
    cv.add(std::clamp(x, 0.0, 1.0), static_cast<float>(std::clamp(y, 0.0, 1.0)), std::clamp(t, -1.0f, 1.0f), mode);
  }
  o += 20;
  if (ver > 1 && o + 6 <= b.size()) {
    set(m, pp::WsHq, b[o + 4] ? 1 : 0);
    set(m, pp::WsSmooth, b[o + 5] ? 1 : 0);
  }
  cv.prepare();
  m.state.data = wscurve::serialize(cv);
  m.exact = !lossy && os < 4;  // 16x oversampling runs at 8x
  return true;
}

// ============================================================ Fruity Flangus
// i32 x 8: order 1..9, depth 0..1024, speed 0..1024, delay 0..128, spread 0..128,
// cross / dry / wet -128..128 (knob positions; the engine applies the curves).
std::shared_ptr<const KnobTable> flangusTable() {
  namespace sf = gl::packs::spacefx;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 1, 9, sf::SwOrder, [](double v) { return std::clamp(std::round(v), 1.0, 9.0); }},
      {1, 0, 1024, sf::SwDepthK, [](double v) { return std::clamp(v / 1024.0, 0.0, 1.0); }},
      {2, 0, 1024, sf::SwSpeedK, [](double v) { return std::clamp(v / 1024.0, 0.0, 1.0); }},
      {3, 0, 128, sf::SwDelayK, [](double v) { return std::clamp(v / 128.0, 0.0, 1.0); }},
      {4, 0, 128, sf::SwSpreadK, [](double v) { return std::clamp(v / 128.0, 0.0, 1.0); }},
      {5, -128, 128, sf::SwCrossK, [](double v) { return std::clamp(v / 128.0, -1.0, 1.0); }},
      {6, -128, 128, sf::SwDryK, [](double v) { return std::clamp(v / 128.0, -1.0, 1.0); }},
      {7, -128, 128, sf::SwWetK, [](double v) { return std::clamp(v / 128.0, -1.0, 1.0); }},
  });
  return t;
}
bool decodeFlangus(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = payload(evs);
  if (b.size() < 32) return false;
  std::vector<double> v(8);
  for (size_t k = 0; k < 8; ++k) v[k] = static_cast<int32_t>(le32(b, 4 * k));
  begin(m, "gl.swirl");
  applyTable(m, *flangusTable(), v);
  bindAll(m, flangusTable());
  set(m, gl::packs::spacefx::SwModel, 1);
  m.exact = false;  // the per-voice sweep contour is approximated (parabolic)
  return true;
}

// ============================================================ Fruity PanOMatic
// i32 x 6 (no version): pan 0..256, volume 0..320, LFO shape (0 sine, 1 triangle,
// 2 square), target (0 off, 1 pan, 2 volume), amount 0..128, speed 0..55000.
std::shared_ptr<const KnobTable> panOMaticTable() {
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 256, pp::ApStaticPan, [](double v) { return std::clamp((v - 128.0) / 128.0, -1.0, 1.0); }},
      {1, 0, 320, pp::ApStaticVol, [](double v) { return std::clamp(v / 256.0, 0.0, 1.25); }},
      {2, 0, 2, pp::ApShape, [](double v) { return std::clamp(std::round(v), 0.0, 2.0); }},
      {3, 0, 2, pp::ApTarget, [](double v) { return std::clamp(std::round(v), 0.0, 2.0); }},
      {4, 0, 128, pp::ApAmount, [](double v) { return std::clamp(v / 128.0, 0.0, 1.0); }},
      {5, 0, 55000, pp::ApSpeedK, [](double v) { return std::clamp(v / 65536.0, 0.0, 55000.0 / 65536.0); }},
  });
  return t;
}
bool decodePanOMatic(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = payload(evs);
  if (b.size() < 24) return false;
  std::vector<double> v(6);
  for (size_t k = 0; k < 6; ++k) v[k] = static_cast<int32_t>(le32(b, 4 * k));
  begin(m, "gl.autopan");
  applyTable(m, *panOMaticTable(), v);
  bindAll(m, panOMaticTable());
  set(m, pp::ApClassic, 1);
  set(m, pp::ApSync, 0);
  m.exact = true;
  return true;
}

// ============================================================ Fruity Chorus
// u32 version (1), i32 x 12 in tag order (older states: 12 normalised f32):
// delay 0..1024, depth 0..5000, stereo 0..1024, LFO 1-3 rate 0..5000,
// LFO 1-3 wave 0..1024, cross type 0..100, cross cutoff 0..5000, wet only 0..1024.
double chDelay(double v) { return 0.05 + 29.95 * std::clamp(v / 1024.0, 0.0, 1.0); }
double chRate(double v) {
  double p = std::clamp(v / 5000.0, 0.0, 1.0);
  return 5.0 * p * p;
}
double chWave(double v) { return std::min(3.0, std::floor(3.0 * std::clamp(v / 1024.0, 0.0, 1.0))); }
double chCut(double v) { return 440.0 * std::pow(2.0, (127.0 * std::clamp(v / 5000.0, 0.0, 1.0) - 69.0) / 12.0); }

std::shared_ptr<const KnobTable> chorusTable() {
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 1024, pp::ChoDelay, chDelay},
      {1, 0, 5000, pp::ChoDepthMs, [](double v) { return 5.0 * std::clamp(v / 5000.0, 0.0, 1.0); }},
      {2, 0, 1024, pp::ChoStereo, [](double v) { return std::clamp(v / 1024.0, 0.0, 1.0); }},
      {3, 0, 5000, pp::ChoRate, chRate},
      {4, 0, 5000, pp::ChoRate2, chRate},
      {5, 0, 5000, pp::ChoRate3, chRate},
      {6, 0, 1024, pp::ChoWave1, chWave},
      {7, 0, 1024, pp::ChoWave2, chWave},
      {8, 0, 1024, pp::ChoWave3, chWave},
      {9, 0, 100, pp::ChoCrossMode, [](double v) { return v > 25 ? 2.0 : 1.0; }},
      {10, 0, 5000, pp::ChoCrossFreq, chCut},
      {11, 0, 1024, pp::ChoWetOnly, [](double v) { return v > 256 ? 1.0 : 0.0; }},
  });
  return t;
}

bool decodeChorus(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = payload(evs);
  static const double kRange[12] = {1024, 5000, 1024, 5000, 5000, 5000, 1024, 1024, 1024, 100, 5000, 1024};
  if (b.size() < 48) return false;
  std::vector<double> v(12);
  if (b.size() < 52) {  // older float state
    for (size_t k = 0; k < 12; ++k) v[k] = std::clamp(static_cast<double>(lef32(b, 4 * k)), 0.0, 1.0) * kRange[k];
  } else {
    for (size_t k = 0; k < 12; ++k) v[k] = static_cast<int32_t>(le32(b, 4 + 4 * k));
  }
  begin(m, "gl.chorus");
  applyTable(m, *chorusTable(), v);
  bindAll(m, chorusTable());
  set(m, pp::ChoClassic, 1);
  set(m, pp::ChoVoices, 2);
  set(m, pp::ChoFeedback, 0);
  m.exact = true;
  return true;
}

// ============================================================ Fruity Love Philter
// u32 version (2 | 5 | 12...), then a zlib stream. Decompressed (sequential):
//   i32 x 8 globals (master, master LFO, follower level / attack / release,
//   pattern 1..10, mod X, mod Y), i32 0, f32 X/Y smoothing coefficient,
//   i32 solo unit (-1 none), i32 HQ envelopes, i32 oversampling, option
//   bytes (smooth, centre, sync patterns, ui; +1 from version 10, +1 from 11),
//   i32 ui, i32 n + n bytes info text (RTF), then 8 units:
//   i32 x 12 knobs, i32 order, i32 type, u8 alt, u8 enabled, u8 shaper on,
//   shaper curve record, u8 bipolar, then 8 targets (pan, vol, cut, res, low,
//   band, high, shaper mix): i32 1, f32 rest, f32 amount, f32 LFO amount,
//   10 x { pattern record, i32 x 4 }, LFO record, i32 x 4 (version 2) or 5
//   (speed, 3 shape knobs, phase), 3 map records (X, Y, follower).
// Records: u32 kind, u32 format (1 = f32 points, 2 / 3 = f64), u32 n, points
// { dx, y, f32 tension, u32 mode | sign << 24 }, u32, i32 x 4 (..., loop start, loop end).
namespace cr = gl::packs::creative;

struct LpReader {
  const std::vector<uint8_t>& b;
  size_t o = 0;
  bool ok = true;
  int32_t i32() {
    if (o + 4 > b.size()) return ok = false, 0;
    int32_t v = static_cast<int32_t>(le32(b, o));
    o += 4;
    return v;
  }
  float f32() {
    if (o + 4 > b.size()) return ok = false, 0.f;
    float v = lef32(b, o);
    o += 4;
    return v;
  }
  uint8_t u8() {
    if (o + 1 > b.size()) return ok = false, uint8_t{0};
    return b[o++];
  }
  struct Rec {
    std::vector<GbPoint> pts;
    int loopS = -1, loopE = -1;
  };
  Rec rec() {
    Rec r;
    if (o + 12 > b.size()) return ok = false, r;
    uint32_t fmt = le32(b, o + 4), n = le32(b, o + 8);
    o += 12;
    if (fmt < 1 || fmt > 3 || n > 5000) return ok = false, r;
    size_t sz = fmt >= 2 ? 24 : 16;
    if (o + n * sz + 20 > b.size()) return ok = false, r;
    double x = 0;
    for (uint32_t i = 0; i < n; ++i, o += sz) {
      double dx = fmt >= 2 ? lef64(b, o) : lef32(b, o), y = fmt >= 2 ? lef64(b, o + 8) : lef32(b, o + 4);
      float t = lef32(b, o + (fmt >= 2 ? 16 : 8));
      uint32_t fl = le32(b, o + (fmt >= 2 ? 20 : 12));
      if (!std::isfinite(dx) || !std::isfinite(y) || !std::isfinite(t)) return ok = false, r;
      x += std::max(0.0, dx);
      r.pts.push_back({x, y, t, fl});
    }
    r.loopS = static_cast<int32_t>(le32(b, o + 12)), r.loopE = static_cast<int32_t>(le32(b, o + 16));
    o += 20;
    return r;
  }
};

bool recFlat(const LpReader::Rec& r) {
  for (const auto& p : r.pts)
    if (std::fabs(p.y - r.pts.front().y) > 1e-6) return false;
  return true;
}
// Record → envelope (points beyond the pool size are thinned).
cr::OEnv toEnv(const LpReader::Rec& r, bool& lossy) {
  cr::OEnv e;
  size_t n = r.pts.size(), step = n > static_cast<size_t>(cr::kEnvPts) ? (n + cr::kEnvPts - 1) / cr::kEnvPts : 1;
  if (step > 1) lossy = true;
  for (size_t i = 0; i < n; ++i) {
    if (i % step != 0 && i + 1 != n) continue;
    const GbPoint& p = r.pts[i];
    uint8_t mode = static_cast<uint8_t>(p.flags & 0xFF);
    int8_t sign = static_cast<int8_t>(p.flags >> 24);
    float t = std::clamp(p.t, -1.0f, 1.0f);
    if (mode >= menv::ModeCount) mode = menv::SingleCurve, lossy = true;
    if (sign == 2 && mode != menv::Hold) mode = menv::SingleCurve, t = 0;
    e.add(p.x, static_cast<float>(std::clamp(p.y, 0.0, 1.0)), t == 0.0f ? 0.0f : t, mode);
  }
  e.prepare();
  return e;
}

// Plain text of an RTF document (control words and groups removed, \par → newline).
std::string rtfText(const std::string& rtf) {
  if (rtf.rfind("{\\rtf", 0) != 0) return rtf;
  std::string out;
  int depth = 0, skipDepth = -1;
  for (size_t i = 0; i < rtf.size(); ++i) {
    char c = rtf[i];
    if (c == '{') {
      ++depth;
      bool dest = i + 2 < rtf.size() && rtf[i + 1] == '\\' &&
                  (rtf[i + 2] == '*' || rtf.compare(i + 1, 8, "\\fonttbl") == 0 || rtf.compare(i + 1, 9, "\\colortbl") == 0 ||
                   rtf.compare(i + 1, 11, "\\stylesheet") == 0);
      if (dest && skipDepth < 0) skipDepth = depth;
      continue;
    }
    if (c == '}') {
      if (depth == skipDepth) skipDepth = -1;
      --depth;
      continue;
    }
    if (c == '\\') {
      size_t j = i + 1;
      if (j < rtf.size() && (rtf[j] == '\\' || rtf[j] == '{' || rtf[j] == '}')) {
        if (skipDepth < 0) out += rtf[j];
        i = j;
        continue;
      }
      std::string word;
      while (j < rtf.size() && std::isalpha(static_cast<unsigned char>(rtf[j]))) word += rtf[j++];
      while (j < rtf.size() && (std::isdigit(static_cast<unsigned char>(rtf[j])) || rtf[j] == '-')) ++j;
      if (j < rtf.size() && rtf[j] == ' ') ++j;
      if (skipDepth < 0 && (word == "par" || word == "line")) out += '\n';
      i = j - 1;
      continue;
    }
    if (c == '\r' || c == '\n' || c == 0) continue;
    if (skipDepth < 0) out += c;
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

double lpFollowerSeconds(double v) { return 0.0125 * (std::pow(81.0, std::clamp(v, 0.0, 16384.0) / 16384.0) - 1.0); }

struct LpKnob {
  int f;
  double lo, hi, div;
};
const LpKnob kLpKnobs[12] = {{cr::UEnv, 0, 256, 128},   {cr::UCut, 0, 256, 256},   {cr::URes, 0, 256, 256},    {cr::ULow, 0, 128, 128},
                             {cr::UBand, 0, 128, 128},  {cr::UHigh, 0, 128, 128},  {cr::UWsPre, 64, 192, 128}, {cr::UWsMix, 0, 128, 128},
                             {cr::UPan, -128, 128, 128}, {cr::UOut, 0, 160, 128},  {cr::UIn, -160, 160, 128},  {cr::UNext, -160, 160, 128}};

std::shared_ptr<const KnobTable> lovePhilterTable() {
  static auto t = [] {
    auto k = std::make_shared<KnobTable>(KnobTable{
        {0, 0, 160, cr::OfMaster, [](double v) { return v / 128.0; }},
        {1, -128, 128, cr::OfMasterLfo, [](double v) { return v / 128.0; }},
        {2, 0, 160, cr::OfIefLevel, [](double v) { return v / 128.0; }},
        {3, 0, 16384, cr::OfIefAttack, [](double v) { return std::max(0.0005, lpFollowerSeconds(v)); }},
        {4, 0, 16384, cr::OfIefRelease, [](double v) { return std::max(0.0005, lpFollowerSeconds(v)); }},
        {5, 1, 10, cr::OfPattern, [](double v) { return std::clamp(std::round(v), 1.0, 10.0) - 1.0; }},
        {6, 0, 1024, cr::OfModX, [](double v) { return v / 1024.0; }},
        {7, 0, 1024, cr::OfModY, [](double v) { return v / 1024.0; }},
    });
    // Unit knobs: FL parameter 8 + 12 x unit + knob (exact for unit 1; units 2-8 provisional).
    for (int u = 0; u < cr::kUnits; ++u)
      for (int j = 0; j < 12; ++j) {
        double div = kLpKnobs[j].div;
        k->push_back({8 + 12 * u + j, kLpKnobs[j].lo, kLpKnobs[j].hi, cr::unitParam(u, kLpKnobs[j].f), [div](double v) { return v / div; }});
      }
    return std::shared_ptr<const KnobTable>(k);
  }();
  return t;
}

bool decodeLovePhilter(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> st = payload(evs);
  begin(m, "gl.octafilter");
  bindAll(m, lovePhilterTable());
  if (st.size() < 8) return true;
  uint32_t ver = le32(st, 0);
  if (ver >= 0x10000) ver >>= 16;
  std::vector<uint8_t> b;
  for (size_t p = 4; p + 2 < st.size() && p < 20 && b.empty(); ++p)
    if (st[p] == 0x78) b = inflateZlib(st.data() + p, st.size() - p);
  if (b.size() < 64) {
    c.report.warn("Love Philter: state could not be decompressed");
    return true;
  }
  LpReader r{b};
  std::vector<double> g(8);
  for (double& x : g) x = r.i32();
  r.i32();
  r.f32();
  int solo = r.i32(), hqEnv = r.i32(), over = r.i32();
  uint8_t smooth = r.u8(), center = r.u8(), sync = r.u8();
  r.u8();
  if (ver >= 10) r.u8();
  if (ver >= 11) r.u8();
  r.i32();
  int32_t infoLen = r.i32();
  std::string info;
  if (infoLen > 0 && r.o + static_cast<size_t>(infoLen) <= b.size()) {
    info.assign(reinterpret_cast<const char*>(b.data() + r.o), static_cast<size_t>(infoLen));
    r.o += static_cast<size_t>(infoLen);
  } else if (infoLen != 0) {
    r.ok = false;
  }
  while (!info.empty() && info.back() == 0) info.pop_back();
  applyTable(m, *lovePhilterTable(), g);
  set(m, cr::OfSolo, solo >= 0 && solo < cr::kUnits ? solo + 1 : 0);
  set(m, cr::OfHqEnvelopes, hqEnv > 0 ? 1 : 0);
  set(m, cr::OfOversample, std::clamp(over, 0, 4));
  set(m, cr::OfSmooth, smooth ? 1 : 0);
  set(m, cr::OfCenter, center ? 1 : 0);
  set(m, cr::OfSyncPatterns, sync ? 1 : 0);
  std::string data = "OF1\n";
  bool lossy = false;
  int unitsRead = 0;
  for (int u = 0; u < cr::kUnits && r.ok; ++u) {
    for (int j = 0; j < 12; ++j) set(m, cr::unitParam(u, kLpKnobs[j].f), r.i32() / kLpKnobs[j].div);
    set(m, cr::unitParam(u, cr::UOrder), std::clamp(r.i32(), 0, 5));
    set(m, cr::unitParam(u, cr::UType), std::clamp(r.i32(), 0, cr::FilterTypeCount - 1));
    set(m, cr::unitParam(u, cr::UAlt), r.u8() ? 1 : 0);
    set(m, cr::unitParam(u, cr::UEnabled), r.u8() ? 1 : 0);
    set(m, cr::unitParam(u, cr::UWsOn), r.u8() ? 1 : 0);
    LpReader::Rec ws = r.rec();
    set(m, cr::unitParam(u, cr::UWsBipolar), r.u8() ? 1 : 0);
    if (!r.ok) break;
    {  // shaper curve: kept unless it is the default bend
      cr::OEnv e = toEnv(ws, lossy), d;
      cr::defaultWs(d);
      bool same = e.n == d.n;
      for (int i = 0; same && i < e.n; ++i)
        same = std::fabs(e.x[i] - d.x[i]) < 1e-6 && std::fabs(e.y[i] - d.y[i]) < 1e-6 && std::fabs(e.t[i] - d.t[i]) < 1e-4;
      if (!same && e.n > 0) data += cr::wsLine(u, e) + "\n";
    }
    for (int t = 0; t < cr::kTargets && r.ok; ++t) {
      r.i32(), r.f32(), r.f32(), r.f32();
      const bool add = cr::additiveTarget(t);
      for (int k = 0; k < cr::kPatterns; ++k) {
        LpReader::Rec pr = r.rec();
        for (int q = 0; q < 4; ++q) r.i32();
        if (!r.ok) break;
        if (pr.pts.size() >= 2) {  // a single point is the unused default
          cr::OEnv e = toEnv(pr, lossy);
          int ls = pr.loopS >= 0 && pr.loopS < e.n ? pr.loopS : -1, le = pr.loopE >= 0 && pr.loopE < e.n ? pr.loopE : -1;
          data += cr::envLine(u, t, k, e, ls, le) + "\n";
        }
      }
      LpReader::Rec lfo = r.rec();
      int lp[5] = {40000, 0, 0, 0, 0};
      for (int q = 0; q < (ver >= 5 ? 5 : 4); ++q) lp[q] = r.i32();
      if (!r.ok) break;
      set(m, cr::lfoParam(u, t, cr::LSpeed), std::clamp(lp[0] / 65536.0, 0.0, 1.0));
      set(m, cr::lfoParam(u, t, cr::LTension), std::clamp(lp[1] / 128.0, -1.0, 1.0));
      set(m, cr::lfoParam(u, t, cr::LSkew), std::clamp(lp[2] / 128.0, -1.0, 1.0));
      set(m, cr::lfoParam(u, t, cr::LPulse), std::clamp(lp[3] / 128.0, -1.0, 1.0));
      set(m, cr::lfoParam(u, t, cr::LPhase), std::clamp(lp[4] / 65536.0, 0.0, 1.0));
      if (lfo.pts.size() >= 2 || (lfo.pts.size() == 1 && std::fabs(lfo.pts[0].y - 1.0) > 1e-6)) {
        cr::OEnv e = toEnv(lfo, lossy);
        if (e.n == 1) e.add(1.0, e.y[0], 0, menv::SingleCurve), e.prepare();
        data += cr::envLine(u, t, cr::KLfo, e) + "\n";
      }
      for (int q = 0; q < 3; ++q) {
        LpReader::Rec mp = r.rec();
        if (!r.ok) break;
        bool used = mp.pts.size() >= 2 ? !recFlat(mp) : (mp.pts.size() == 1 && std::fabs(mp.pts[0].y - (add ? 0.5 : 1.0)) > 1e-6);
        if (used) data += cr::envLine(u, t, cr::KMapX + q, toEnv(mp, lossy)) + "\n";
      }
    }
    if (r.ok) ++unitsRead;
  }
  if (unitsRead < cr::kUnits) c.report.warn("Love Philter: not all filter units decoded");
  std::string text = rtfText(info);
  m.state.data = text.empty() ? data : cr::withInfo(data, text);
  // LFO speed curve, oversampling and the 2-pole / ladder filter cores are Gridline's own.
  m.exact = false;
  (void)lossy;
  return true;
}

}  // namespace

void registerFxK3(std::map<std::string, PluginDecoder>& d) {
  d["Gross Beat"] = decodeGrossBeat;
  d["Fruity Love Philter"] = decodeLovePhilter;
  d["Fruity Chorus"] = decodeChorus;
  d["Fruity PanOMatic"] = decodePanOMatic;
  d["Fruity Flangus"] = decodeFlangus;
  d["Fruity Fast Dist"] = decodeFastDist;
  d["Fruity WaveShaper"] = decodeWaveShaper;
}

}  // namespace gl::flp::fx
