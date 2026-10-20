// Gridline — FLP export: Harmor state writer (J-34 fields, J-39 full writer).
//
// Layout (FlpGenHarmor.cpp): u32 version, u32 section mask, per part (A, B):
// i32 size + part (386 tag dwords — 385 before v16 —, 12 unit-order dwords,
// 13-dword part record, 2 filter shapes, 2 resonance shapes (100 +), u8
// enabled, 2 bytes, i32 (v11+), 43 articulators (42 before v16: envelope,
// LFO, 9 mapping curves each), 26 shaping curves, the image block and path),
// then the global block: 53 dwords (tags 10000.., 20000..), 6 fx-order
// dwords, 14 dwords (2 mono, 12 distortion, 13 compression), 5 bytes, info
// text, u8 (v19).
//
// The state is read into a structure that keeps every stored value (Patch:
// parse + serialize reproduce each of FL's shipped states byte for byte) and
// Gridline's values are put into it (apply): parameters through the inverse
// of the decoder's laws (searched over the stored range), the drawn data
// (articulator curves / scales / smoothing, shaping curves, image layer)
// from PluginState::data. Values that already decode to Gridline's are left
// as they are, so an imported state keeps its other bytes. Without a
// template the writer starts from a fresh version-19 patch (defaultPatch)
// whose values Gridline does not model are the ones FL's shipped version-19
// states hold (format constants: unit order, articulator headers, neutral
// curves, option words). The J-34 fields then solve whatever the inverse
// laws missed through the decoder itself.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include "FlpEncode.h"
#include "FlpGen.h"
#include "../PluginInfo.h"
#include "../../packs/models/harmor.h"

namespace gl::flp::enc {

namespace {

namespace hm = gl::packs::models::hm;

float pv(const PluginState& s, int k) {
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : std::numeric_limits<float>::quiet_NaN();
}
int32_t geti(const Bytes& b, size_t at) { return static_cast<int32_t>(getU32(b, at)); }
double clampd(double v, double lo, double hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : lo; }

// ------------------------------------------------------------ J-34 fields

struct Layout {
  int ver = 0;
  size_t part[2] = {0, 0};  // offsets of the parts' tag dwords (0 = absent)
  size_t global = 0;        // offset of the global dwords (0 = absent)
};

Layout layout(const Bytes& b) {
  Layout l;
  if (b.size() < 12) return l;
  const uint32_t raw = getU32(b, 0);
  l.ver = raw >= 0x10000 ? static_cast<int>(raw >> 16) : static_cast<int>(raw);
  const int sections = geti(b, 4);
  const int parts = (sections & 3) == 3 ? 2 : (sections & 3) ? 1 : 0;
  size_t at = 8;
  for (int k = 0; k < parts; ++k) {
    if (at + 4 > b.size()) return Layout{};
    const int32_t size = geti(b, at);
    if (size < 0 || at + 4 + static_cast<size_t>(size) > b.size()) return Layout{};
    l.part[k] = at + 4;
    at += 4 + static_cast<size_t>(size);
  }
  if ((sections & 4) && at + 53 * 4 + 24 + 14 * 4 <= b.size()) l.global = at;
  return l;
}

struct T {
  int field, tag, lo, hi;
};
const std::vector<T>& partTable() {
  static const std::vector<T> t = {
      {hm::PfPhase, 0, 0, 1000},          {hm::PfPhaseRand, 1, -1000, 1000},   {hm::PfPredelay, 2, -384, 384},
      {hm::PfRelMul, 3, 0, 1000},         {hm::PfTimbreMix, 4, 0, 1000},       {hm::PfSub1, 5, 0, 2048},
      {hm::PfSub3, 6, 0, 2048},           {hm::PfSub4, 7, 0, 2048},            {hm::PfProtect, 8, 0, 1024},
      {hm::PfClip, 9, 0, 1024},           {hm::PfFxMix, 10, -1000, 1000},      {hm::PfVolume, 11, -2000, 2000},
      {hm::PfEnvVolume, 12, 0, 2000},     {hm::PfAutoGain, 13, 0, 1000},       {hm::PfVelAttTime, 14, 0, 1},
      {hm::PfVelAttLen, 15, 0, 1},        {hm::PfVelRelLen, 16, 0, 1},         {hm::PfTremolo, 17, 0, 1000},
      {hm::PfTremHz, 18, 0, 10000},       {hm::PfTremGap, 19, 0, 999},         {hm::PfBlurMix, 20, 0, 1000},
      {hm::PfTBlurAtt, 21, 0, 1600},      {hm::PfTBlurDec, 22, 0, 1600},       {hm::PfHBlur, 23, 0, 10000},
      {hm::PfHBlurLo, 24, 0, 1000},       {hm::PfHBlurHi, 25, 0, 1000},        {hm::PfPrism, 26, -1000, 1000},
      {hm::PfHzMix, 27, 0, 1000},         {hm::PfHzWidth, 28, 0, 1000},        {hm::PfHzStrength, 29, 0, 1024},
      {hm::PfHzShift, 30, -2, 7},         {hm::PfHzShiftStep, 31, -7, 8},      {hm::PfHzGap, 32, -1, 8},
      {hm::PfHzGapStep, 33, -7, 8},       {hm::PfUniOrder, 34, 1, 9},          {hm::PfUniType, 35, 0, 4},
      {hm::PfUniAlt, 36, 0, 1},           {hm::PfUniPan, 37, 0, 256},          {hm::PfUniPitch, 38, 0, 256},
      {hm::PfUniPhase, 39, -1000, 1000},  {hm::PfFreqMul, 40, 10000, 160000},  {hm::PfFreqDiv, 41, 1, 9},
      {hm::PfDetMul, 42, 10000, 160000},  {hm::PfDetDiv, 43, 1, 9},            {hm::PfPitchArt, 44, -6000, 6000},
      {hm::PfVibrato, 45, 0, 1000},       {hm::PfVibHz, 46, 0, 1000},          {hm::PfPortaLog, 47, 0, 1},
      {hm::PfPortaVar, 48, 0, 1},         {hm::PfPortaTime, 49, -384, 384},    {hm::PfPortaLimit, 50, 0, 1200},
      {hm::PfFMix, 74, -1000, 1000},      {hm::PfPluck, 75, 0, 1000},          {hm::PfPhMix, 76, 0, 1000},
      {hm::PfPhScale, 77, 0, 2},          {hm::PfPhWidth, 78, 0, 10000},       {hm::PfPhOfs, 79, 0, 999},
      {hm::PfPhMotion, 80, -8000, 8000},  {hm::PfPhKey, 81, -2000, 2000},      {hm::PfImgTimeSmooth, 82, -1000, 1000},
      {hm::PfImgTime, 83, 0, 1048575},    {hm::PfImgSpeed, 84, -20000, 20000}, {hm::PfImgSpeedOct, 85, -60000, 60000},
      {hm::PfImgSharp, 86, -1000, 1000},  {hm::PfImgGainScale, 88, 0, 1000},   {hm::PfImgGainMix, 89, 0, 1000},
      {hm::PfImgFreqMode, 90, 0, 3},      {hm::PfImgFreqScale, 92, -2000, 2000}, {hm::PfImgFormant, 93, -600, 600},
      {hm::PfImgFormantMix, 94, 0, 1000}};
  return t;
}
const std::vector<T>& filterTable() {  // per filter, tag + 12 × filter
  static const std::vector<T> t = {{hm::PfF1Adapt, 51, 0, 1},     {hm::PfF1Cut, 52, 0, 8192},     {hm::PfF1Width, 54, 0, 1000},
                                   {hm::PfF1Key, 55, -2000, 2000}, {hm::PfF1Env, 56, -1000, 1000}, {hm::PfF1Res, 57, 0, 1000},
                                   {hm::PfF1ResWidth, 59, 0, 1000}, {hm::PfF1ResOfs, 60, -2400, 2400}, {hm::PfF1ResAdapt, 61, 0, 1}};
  return t;
}
const std::vector<T>& globalTable() {  // field = Gridline param index
  static const std::vector<T> t = {
      {hm::GmModX, 10000, 0, 512},        {hm::GmModY, 10001, 0, 512},          {hm::GmModZ, 10002, 0, 512},
      {hm::GmLegatoLimit, 10003, 0, 2400}, {hm::GmLegato, 10004, 0, 1},         {hm::GmPorta, 10005, 0, 1},
      {hm::GmVelPorta, 10006, 0, 1},      {hm::GmStrumMode, 10007, 0, 3},       {hm::GmStrumTime, 10008, 0, 1000},
      {hm::GmStrumTension, 10009, -1000, 1000}, {hm::GmPartMix, 10010, 0, 1000}, {hm::GmVolume, 10011, 0, 2000},
      {hm::GmPostVolume, 10012, 0, 2000}, {hm::GmVelVol, 10013, 0, 1},          {hm::GmPitch, 10014, -2400, 2400},
      {hm::GmLfoAmt, 10015, -1000, 1000}, {hm::FxDistDrive, 20000, 0, 1000},    {hm::FxDistAsym, 20001, -500, 500},
      {hm::FxDistOut, 20002, 0, 2000},    {hm::FxDistMix, 20003, -1000, 1000},  {hm::FxDistTone, 20004, 0, 1100},
      {hm::FxRvOn, 20005, 0, 1},          {hm::FxRvLowCut, 20006, 0, 1000},     {hm::FxRvHighCut, 20007, 0, 1100},
      {hm::FxRvPredelay, 20008, -384, 384}, {hm::FxRvRoom, 20009, 0, 100},      {hm::FxRvDiffusion, 20010, 0, 100},
      {hm::FxRvDecay, 20011, 0, 900},     {hm::FxRvDamp, 20012, 0, 1100},       {hm::FxRvColor, 20013, 0, 4},
      {hm::FxRvMix, 20014, -1000, 1000},  {hm::FxChOrder, 20015, 0, 9},         {hm::FxChDepth, 20016, 0, 1024},
      {hm::FxChSpeed, 20017, 0, 1024},    {hm::FxChDelay, 20018, 0, 128},       {hm::FxChSpread, 20019, 0, 128},
      {hm::FxChCross, 20020, -128, 128},  {hm::FxChMix, 20021, -1000, 1000},    {hm::FxDlOn, 20022, 0, 1},
      {hm::FxDlMode, 20023, 0, 2},        {hm::FxDlFeedback, 20024, 0, 1000},   {hm::FxDlTime, 20025, -384, 384},
      {hm::FxDlOffset, 20026, -512, 512}, {hm::FxDlVol, 20027, -1000, 1000},    {hm::FxDlPan, 20028, -128, 128},
      {hm::FxDlLowCut, 20029, 0, 1000},   {hm::FxDlHighCut, 20030, 0, 1100},    {hm::FxDlDamp, 20031, 0, 1},
      {hm::FxCompAmount, 20032, 0, 1000}, {hm::FxCompLow, 20033, 0, 2000},      {hm::FxCompMid, 20034, 0, 2000},
      {hm::FxCompHigh, 20035, 0, 2000}};
  return t;
}
int gIndex(int tag) { return tag >= 20000 ? 17 + tag - 20000 : tag - 10000; }


// J-51: the value laws below (partLaw / globalLaw, the decoder's) as the
// fields' forward laws for the solver. (The delay time also depends on the
// song tempo, which a law does not see: searched through the decoder.)
double partLaw(int tag, int32_t t);
double globalLaw(int tag, int32_t g, double tempo);
std::function<double(double, const Bytes&)> partFieldLaw(int tag) {
  return [tag](double r, const Bytes&) { return partLaw(tag, static_cast<int32_t>(r)); };
}
std::function<double(double, const Bytes&)> globalFieldLaw(int tag) {
  if (tag == 20025) return {};
  return [tag](double r, const Bytes&) { return globalLaw(tag, static_cast<int32_t>(r), 120.0); };
}

std::vector<Field> fields(const Bytes& b) {
  std::vector<Field> f;
  const Layout l = layout(b);
  const int np = l.ver >= 16 ? 386 : 385;
  for (int p = 0; p < 2; ++p) {
    if (!l.part[p]) continue;
    const size_t t0 = l.part[p], rec = t0 + static_cast<size_t>(np) * 4 + 12 * 4;
    auto tag = [&](int gl, int t, int lo, int hi) { f.push_back(lawField(i32(gl, t0 + static_cast<size_t>(t) * 4, lo, hi), partFieldLaw(t))); };
    for (const T& t : partTable()) tag(hm::pf(p, t.field), t.tag, t.lo, t.hi);
    for (int k = 0; k < 2; ++k) {
      for (const T& t : filterTable()) tag(hm::flt(p, k, t.field), t.tag + 12 * k, t.lo, t.hi);
      f.push_back(i32(hm::flt(p, k, hm::PfF1Type), rec + 52 + static_cast<size_t>(k) * 4, 0, hm::kFltShapes));
      f.push_back(i32(hm::flt(p, k, hm::PfF1ResType), rec + 60 + static_cast<size_t>(k) * 4, 100, 100 + hm::kResShapes - 1));
    }
    tag(hm::flt(p, 0, hm::PfF1Osc), 62, 0, 2048);
    f.push_back(i32(hm::pf(p, hm::PfBlend), rec, 0, 5));
    f.push_back(i32(hm::pf(p, hm::PfPrismMul), rec + 12, 0, 1));
    f.push_back(i32(hm::pf(p, hm::PfClipMode), rec + 20, 0, 4));
    f.push_back(i32(hm::pf(p, hm::PfSubMode), rec + 24, 0, 1));
    f.push_back(i32(hm::pf(p, hm::PfPhType), rec + 40, 200, 210));
    f.push_back(field(hm::pf(p, hm::PfEnabled), rec + 68, Type::U8, 0, 1));
    if (p == 0) f.push_back(field(hm::FxEqMix, rec + 48, Type::F32, 0, 1));
  }
  if (l.global) {
    for (const T& t : globalTable())
      f.push_back(lawField(i32(t.field, l.global + static_cast<size_t>(gIndex(t.tag)) * 4, t.lo, t.hi), globalFieldLaw(t.tag)));
    const size_t rec = l.global + 53 * 4 + 6 * 4;
    f.push_back(i32(hm::GmMono, rec + 8, 0, 3));
    f.push_back(i32(hm::FxDistType, rec + 48, 0, 11));
    f.push_back(i32(hm::FxCompType, rec + 52, 0, 4));
  }
  return f;
}

// ------------------------------------------------------------ the patch structure

struct HPt {
  double x = 0, y = 0;
  float t = 0;
  uint8_t tail[4] = {0, 0, 0, 0};
};
struct HCurve {
  int32_t kind = 3, fmt = 3;
  std::vector<HPt> pts;
  int32_t tail[5] = {0, -1, -1, -1, -1};  // flags, enabled, decay, loop start, sustain
};
struct HArt {
  bool present = true;
  int32_t h = 3;
  float hdr[3] = {1, 1, 1};
  HCurve env, lfo;
  int32_t adsr[4] = {128, 128, 0, 128};
  int32_t lfop[5] = {40898, 0, 0, 0, 0};
  HCurve maps[hm::kMaps];
};
struct HImg {
  int32_t v = 0, src = 0, loop = 1;
  uint8_t f[4] = {};
  uint8_t d[24] = {};
  int32_t cols = 0, rows = 0;
  int32_t counts[3] = {0, 0, 0};
  std::vector<float> planes[3];
  Bytes path;
  bool longLen = false;
};
struct HPart {
  std::vector<int32_t> params;
  int32_t order[12] = {};
  int32_t rec[13] = {};
  int32_t ft[4] = {4, 18, 108, 111};
  uint8_t b3[3] = {1, 0, 0};
  int32_t agm = 0;
  std::vector<HArt> arts;
  HCurve shapes[hm::kShapes];
  HImg img;
};
struct HGlobal {
  int32_t g[53] = {};
  int32_t order[6] = {0, 1, 2, 3, 4, 5};
  int32_t rec[14] = {};
  uint8_t b5[5] = {0, 1, 1, 1, 0};
  Bytes info;
  bool longLen = false;
  uint8_t last = 1;
};
struct HPatch {
  uint32_t raw = 0x00130003;
  int ver = 19;
  int32_t sections = 7;
  std::vector<HPart> parts;
  bool hasGlobal = true;
  HGlobal g;
};

struct Rd {
  const Bytes& b;
  size_t at = 0, end = 0;
  bool bad = false;
  bool need(size_t k) {
    if (bad || at > end || k > end - at) bad = true;
    return !bad;
  }
  int32_t i32() {
    if (!need(4)) return 0;
    const int32_t v = geti(b, at);
    at += 4;
    return v;
  }
  uint8_t u8() { return need(1) ? b[at++] : 0; }
  float f32() {
    if (!need(4)) return 0;
    const float v = getF32(b, at);
    at += 4;
    return v;
  }
  double f64() {
    if (!need(8)) return 0;
    const double v = getF64(b, at);
    at += 8;
    return v;
  }
  void into(uint8_t* p, size_t n) {
    if (need(n)) std::memcpy(p, b.data() + at, n), at += n;
  }
  Bytes raw(size_t n) {
    if (!need(n)) return {};
    Bytes v(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
    at += n;
    return v;
  }
  // u8 length, or 0xFF + i32
  int32_t len(bool& longLen) {
    int32_t n = u8();
    longLen = n == 0xFF;
    if (longLen) n = i32();
    if (n < 0 || n > (1 << 24)) bad = true;
    return bad ? 0 : n;
  }
};
void putLen(ByteWriter& w, size_t n, bool longLen) {
  if (n < 0xFF && !longLen) w.u8(static_cast<uint32_t>(n));
  else w.u8(0xFF), w.i32(static_cast<int32_t>(n));
}

bool readCurve(Rd& r, int kind, HCurve& c) {
  c.kind = r.i32(), c.fmt = r.i32();
  const int n = r.i32();
  if (r.bad || c.kind != kind || c.fmt < 0 || c.fmt > 3 || n < 0 || n > 100000) return r.bad = true, false;
  c.pts.resize(static_cast<size_t>(n));
  for (HPt& p : c.pts) {
    if (c.fmt >= 2) p.x = r.f64(), p.y = r.f64();
    else p.x = kind == 6 ? r.f64() : r.f32(), p.y = r.f32();
    p.t = r.f32();
    if (c.fmt >= 1) r.into(p.tail, 4);
    if (r.bad) return false;
  }
  for (int32_t& v : c.tail) v = r.i32();
  return !r.bad;
}
void writeCurve(ByteWriter& w, const HCurve& c) {
  w.i32(c.kind), w.i32(c.fmt), w.i32(static_cast<int32_t>(c.pts.size()));
  for (const HPt& p : c.pts) {
    if (c.fmt >= 2) w.f64(p.x), w.f64(p.y);
    else if (c.kind == 6) w.f64(p.x), w.f32(static_cast<float>(p.y));
    else w.f32(static_cast<float>(p.x)), w.f32(static_cast<float>(p.y));
    w.f32(p.t);
    if (c.fmt >= 1) w.bytes(p.tail, 4);
  }
  for (int32_t v : c.tail) w.i32(v);
}

bool readPart(Rd& r, int ver, HPart& P) {
  P.params.resize(ver >= 16 ? 386 : 385);
  for (int32_t& v : P.params) v = r.i32();
  for (int32_t& v : P.order) v = r.i32();
  for (int32_t& v : P.rec) v = r.i32();
  for (int32_t& v : P.ft) v = r.i32();
  r.into(P.b3, 3);
  if (ver >= 11) P.agm = r.i32();
  P.arts.assign(hm::kArts, HArt{});
  for (int a = 0; a < hm::kArts && !r.bad; ++a) {
    HArt& A = P.arts[static_cast<size_t>(a)];
    if (ver < 16 && a == hm::ArGAttack) {
      A.present = false;
      continue;
    }
    A.h = r.i32();
    if (A.h != 3) return r.bad = true, false;
    for (float& v : A.hdr) v = r.f32();
    if (!readCurve(r, 1, A.env)) return false;
    for (int32_t& v : A.adsr) v = r.i32();
    if (!readCurve(r, 2, A.lfo)) return false;
    for (int32_t& v : A.lfop) v = r.i32();
    for (HCurve& m : A.maps)
      if (!readCurve(r, 3, m)) return false;
  }
  for (HCurve& s : P.shapes)
    if (!readCurve(r, 3, s)) return false;
  HImg& I = P.img;
  I.v = r.i32();
  if (I.v > 0) return r.bad = true, false;
  I.src = r.i32(), I.loop = r.i32();
  r.into(I.f, 4);
  r.into(I.d, 24);
  I.cols = r.i32(), I.rows = r.i32();
  if (I.src == 0)
    for (int k = 0; k < 3 && !r.bad; ++k) {
      I.counts[k] = r.i32();
      if (I.counts[k] > 0) {
        if (!r.need(static_cast<size_t>(I.counts[k]) * 4)) return false;
        I.planes[k].resize(static_cast<size_t>(I.counts[k]));
        for (float& v : I.planes[k]) v = r.f32();
      }
    }
  const int32_t n = r.len(I.longLen);
  I.path = r.raw(static_cast<size_t>(n));
  return !r.bad;
}
void writePart(ByteWriter& w, int ver, const HPart& P) {
  for (int32_t v : P.params) w.i32(v);
  for (int32_t v : P.order) w.i32(v);
  for (int32_t v : P.rec) w.i32(v);
  for (int32_t v : P.ft) w.i32(v);
  w.bytes(P.b3, 3);
  if (ver >= 11) w.i32(P.agm);
  for (const HArt& A : P.arts) {
    if (!A.present) continue;
    w.i32(A.h);
    for (float v : A.hdr) w.f32(v);
    writeCurve(w, A.env);
    for (int32_t v : A.adsr) w.i32(v);
    writeCurve(w, A.lfo);
    for (int32_t v : A.lfop) w.i32(v);
    for (const HCurve& m : A.maps) writeCurve(w, m);
  }
  for (const HCurve& s : P.shapes) writeCurve(w, s);
  const HImg& I = P.img;
  w.i32(I.v), w.i32(I.src), w.i32(I.loop);
  w.bytes(I.f, 4), w.bytes(I.d, 24);
  w.i32(I.cols), w.i32(I.rows);
  if (I.src == 0)
    for (int k = 0; k < 3; ++k) {
      w.i32(I.counts[k]);
      for (float v : I.planes[k]) w.f32(v);
    }
  putLen(w, I.path.size(), I.longLen);
  w.bytes(I.path);
}

bool parse(const Bytes& b, HPatch& s) {
  if (b.size() < 8) return false;
  Rd r{b, 0, b.size()};
  s.raw = static_cast<uint32_t>(r.i32());
  s.ver = s.raw >= 0x10000 ? static_cast<int>(s.raw >> 16) : static_cast<int>(s.raw);
  if (s.raw == 0xFFFF || s.raw > 0x130003 || s.ver < 1) return false;
  s.sections = r.i32();
  const int nParts = (s.sections & 3) == 3 ? 2 : (s.sections & 3) ? 1 : 0;
  s.parts.assign(static_cast<size_t>(nParts), HPart{});
  for (HPart& P : s.parts) {
    const int32_t size = r.i32();
    if (r.bad || size < 0 || !r.need(static_cast<size_t>(size))) return false;
    Rd pr{b, r.at, r.at + static_cast<size_t>(size)};
    if (!readPart(pr, s.ver, P) || pr.at != pr.end) return false;
    r.at += static_cast<size_t>(size);
  }
  s.hasGlobal = (s.sections & 4) != 0;
  if (s.hasGlobal) {
    HGlobal& G = s.g;
    for (int32_t& v : G.g) v = r.i32();
    for (int32_t& v : G.order) v = r.i32();
    for (int32_t& v : G.rec) v = r.i32();
    r.into(G.b5, 5);
    if (s.ver < 18) {
      const int32_t n = r.i32();
      if (n < 0 || n > (1 << 24)) return false;
      G.info = r.raw(static_cast<size_t>(n));
    } else {
      const int32_t n = r.len(G.longLen);
      G.info = r.raw(static_cast<size_t>(n) * 2);
    }
    if (s.ver > 18) G.last = r.u8();
  }
  return !r.bad && r.at == b.size() && nParts > 0;
}

Bytes serialize(const HPatch& s) {
  ByteWriter w;
  w.u32(s.raw);
  w.i32(s.sections);
  for (const HPart& P : s.parts) {
    ByteWriter pw;
    writePart(pw, s.ver, P);
    w.i32(static_cast<int32_t>(pw.b.size()));
    w.bytes(pw.b);
  }
  if (s.hasGlobal) {
    const HGlobal& G = s.g;
    for (int32_t v : G.g) w.i32(v);
    for (int32_t v : G.order) w.i32(v);
    for (int32_t v : G.rec) w.i32(v);
    w.bytes(G.b5, 5);
    if (s.ver < 18) {
      w.i32(static_cast<int32_t>(G.info.size()));
      w.bytes(G.info);
    } else {
      putLen(w, G.info.size() / 2, G.longLen);
      w.bytes(G.info.data(), G.info.size() / 2 * 2);
    }
    if (s.ver > 18) w.u8(G.last);
  }
  return w.b;
}

// ------------------------------------------------------------ fresh patch

HPt hpt(double x, double y, float t, uint8_t kind, bool first) {
  HPt p;
  p.x = x, p.y = y, p.t = t;
  p.tail[0] = kind;
  p.tail[3] = first ? 0 : t < 0 ? 255 : t > 0 ? 1 : 2;  // FL: tension sign of every point after the first (2 = straight)
  return p;
}
HCurve flat(int kind, double n) {
  HCurve c;
  c.kind = kind;
  c.pts = {hpt(0, n, 0, 0, true), hpt(1, n, 0, 0, false)};
  return c;
}
HCurve single(int kind, double y) {
  HCurve c;
  c.kind = kind;
  c.pts = {hpt(0, y, 0, 0, true)};
  c.tail[1] = 0;
  return c;
}

HPart defaultPart() {
  HPart P;
  P.params.assign(386, 0);
  P.params[87] = 4, P.params[91] = 4;  // tags Gridline does not use (FL's stored defaults)
  for (int i = 0; i < 12; ++i) P.order[i] = i;
  const int32_t rec[13] = {0, 0, 1000, 0, 0, 0, 0, 1, 0, 250, 200, 1, 0};
  std::copy(std::begin(rec), std::end(rec), P.rec);
  const float one = 1.0f;
  std::memcpy(&P.rec[12], &one, 4);
  P.arts.assign(hm::kArts, HArt{});
  for (int a = 0; a < hm::kArts; ++a) {
    HArt& A = P.arts[static_cast<size_t>(a)];
    const bool bip = hm::artBipolarDefault(a);
    const double n = bip ? 0.5 : 1.0;
    A.hdr[0] = bip ? 0.f : 1.f;
    A.env = single(1, n);
    A.lfo = single(2, 0.75);
    for (HCurve& m : A.maps) m = flat(3, n);
  }
  for (int s = 0; s < hm::kShapes; ++s) P.shapes[s] = flat(3, hm::neutralShape(s));
  const double d[3] = {-1.0, -1.0, 0.0};
  std::memcpy(P.img.d, d, 24);
  return P;
}

HPatch defaultPatch() {
  HPatch s;
  s.raw = 0x00130003, s.ver = 19, s.sections = 7;
  s.parts = {defaultPart(), defaultPart()};
  const int32_t rec[14] = {0, 0, 0, 1, 10800, 75, 200, 0, 0, 1, 0, 0, 0, 2};
  std::copy(std::begin(rec), std::end(rec), s.g.rec);
  return s;
}

// ------------------------------------------------------------ Gridline values → patch

// Stored integer in lo..hi whose law value is `want` (the current one when it
// already is). Bisection for monotonic laws, a scan for small ranges.
int32_t solveInt(int32_t cur, double lo, double hi, double want, const std::function<double(int32_t)>& law) {
  if (!std::isfinite(want)) return cur;
  const float w = static_cast<float>(want);
  if (static_cast<float>(law(cur)) == w) return cur;
  const int64_t a0 = static_cast<int64_t>(lo), z0 = static_cast<int64_t>(hi);
  int64_t best = cur;
  double be = std::fabs(law(cur) - want);
  auto consider = [&](int64_t r) {
    const double v = law(static_cast<int32_t>(r));
    if (static_cast<float>(v) == w) {
      best = r, be = 0;
      return true;
    }
    if (std::fabs(v - want) < be) be = std::fabs(v - want), best = r;
    return false;
  };
  if (z0 - a0 <= 4096) {
    for (int64_t r = a0; r <= z0; ++r)
      if (consider(r)) break;
    return static_cast<int32_t>(best);
  }
  const double vlo = law(static_cast<int32_t>(a0)), vhi = law(static_cast<int32_t>(z0));
  if (consider(a0) || consider(z0)) return static_cast<int32_t>(best);
  const bool inc = vhi > vlo;
  int64_t a = a0, z = z0;
  while (z - a > 1) {
    const int64_t m = a + (z - a) / 2;
    const double v = law(static_cast<int32_t>(m));
    if (consider(m)) return static_cast<int32_t>(best);
    if ((v < want) == inc) a = m;
    else z = m;
  }
  for (int64_t r = std::max(a0, a - 3); r <= std::min(z0, z + 3); ++r)
    if (consider(r)) break;
  return static_cast<int32_t>(best);
}

double l3(double y) { return 0.5 * (std::pow(3.0, clampd(y, 0, 2)) - 1.0); }
double l11(double y) { return 0.1 * (std::pow(11.0, clampd(y, 0, 1)) - 1.0); }
double fxHz(double v) { return clampd(16.3516 * std::pow(2.0, v / 100.0), 16, 20000); }

// Value laws of FlpGenHarmor.cpp: part tag → Gridline value.
double partLaw(int tag, int32_t t) {
  const double x = t;
  switch (tag) {
    case 0: return clampd(x / 1000.0, 0, 1);
    case 1: return clampd((t < 0 ? -1 : 1) * (x / 1000.0) * (x / 1000.0), -1, 1);
    case 2: return t < 0 ? clampd((std::pow(2.0, -x / 48.0) - 1.0) / 255.0, 0, 1) : 0.0;
    case 3: case 4: case 17: case 19: case 20: case 24: case 25: case 27: case 28: case 75: case 76: case 79: case 88: case 89: case 94:
      return clampd(x / 1000.0, 0, 1);
    case 5: case 6: case 7: return clampd(x / 1024.0, 0, 2);
    case 8: return t >= 1024 ? 1.0 : l11(x / 1024.0);
    case 9: case 29: return clampd(x / 1024.0, 0, 1);
    case 10: case 26: case 39: case 74: case 86: return clampd(x / 1000.0, -1, 1);
    case 11: return clampd(x / 1000.0, -2, 2);
    case 12: return clampd(l3(x / 1000.0), 0, 4);
    case 13: return clampd((x / 1000.0) * (x / 1000.0), 0, 1);
    case 14: case 15: case 16: case 36: case 47: case 48: return t ? 1 : 0;
    case 18: return clampd(std::pow(2.0, 6.0 - x / 1000.0), 0.0156, 64);
    case 21: case 22: return clampd((std::pow(2.0, x / 100.0) - 1.0) / 1000.0, 0, 10);
    case 23: return clampd(1.0 + 15.9375 * (std::pow(17.0, x / 10000.0) - 1.0), 1, 256);
    case 30: return std::clamp(t, -2, 7);
    case 31: case 33: return std::clamp(t, -7, 8);
    case 32: return std::clamp(t, -1, 8);
    case 34: return std::clamp(t, 1, 9);
    case 35: return std::clamp(t, 0, 4);
    case 37: return t <= 192 ? clampd(x / 192.0, 0, 1) : clampd(1.0 + (x - 192) * 0.046875, 0, 4);
    case 38: return clampd(1.875 * (std::exp(x / 256.0 * 3.71357) - 1.0), 0, 75);
    case 40: case 42: return clampd(x / 10000.0, 1, 16);
    case 41: case 43: return std::clamp(t, 1, 9);
    case 44: return clampd(x, -6000, 6000);
    case 45: return clampd(x * 0.1, 0, 100);
    case 46: return clampd(std::pow(2.0, 3.0 - x / 1000.0), 0.125, 8);
    case 49: return t < 0 ? clampd((std::pow(2.0, -x / 64.0) - 1.0) / 42.0, 0, 1.5) : clampd(x / 192.0 * 0.5, 0, 1.5);
    case 50: return clampd(x, 0, 1200);
    case 77: return std::clamp(t, 0, 2);
    case 78: return clampd(x / 10000.0, 0, 1);
    case 80: return clampd(x / 1000.0, -8, 8);
    case 81: case 92: return clampd(x / 1000.0, -2, 2);
    case 82: return clampd((x / 1000.0) * (x / 1000.0) * 2.0, 0, 2);
    case 83: return clampd(x / 1048576.0, 0, 1);
    case 84: return clampd(x / 10000.0, -2, 2);
    case 85: return clampd(x / 10000.0, -6, 6);
    case 90: return std::clamp(t, 0, 3);
    case 93: return clampd(x / 100.0, -6, 6);
    default: break;
  }
  // Filters (k = 0 / 1, tags 51.. / 63..).
  const int k = tag >= 63 ? 1 : 0, b = tag - 12 * k;
  switch (b) {
    case 51: case 61: return t ? 1 : 0;
    case 52: return clampd(x / 8192.0, 0, 1);
    case 54: case 57: case 59: return clampd(x / 1000.0, 0, 1);
    case 55: return clampd(x / 1000.0, -2, 2);
    case 56: return clampd(x / 1000.0, -1, 1);
    case 60: return clampd(x, -2400, 2400);
    case 62: return k == 0 ? clampd(std::pow(x / 1024.0, 2.0) / 4.0, 0, 1) : 0.0;
    default: return std::numeric_limits<double>::quiet_NaN();
  }
}
double globalLaw(int tag, int32_t g, double tempo) {
  const double x = g;
  switch (tag) {
    case 10000: case 10001: case 10002: return clampd(x / 512.0, 0, 1);
    case 10003: return clampd(x, 0, 2400);
    case 10004: case 10005: case 10006: case 10013: case 20005: case 20022: case 20031: return g ? 1 : 0;
    case 10007: return std::clamp(g, 0, 3);
    case 10008: case 10010: case 20000: case 20024: case 20032: return clampd(x / 1000.0, 0, 1);
    case 10009: case 10015: case 20003: case 20014: case 20021: case 20027: return clampd(x / 1000.0, -1, 1);
    case 10011: case 10012: return clampd(l3(x / 1000.0), 0, 4);
    case 10014: return clampd(x, -2400, 2400);
    case 20001: return clampd(x / 1000.0, -0.5, 0.5);
    case 20002: case 20033: case 20034: case 20035: return clampd(x / 1000.0, 0, 2);
    case 20004: return clampd(fxHz(x), 500, 20000);
    case 20006: case 20029: return fxHz(x);
    case 20007: case 20012: case 20030: return clampd(fxHz(x), 300, 20000);
    case 20008: return g < 0 ? clampd((std::pow(2.0, -x / 64.0) - 1.0) * 15.873 / 1000.0, 0, 0.5) : 0.0;
    case 20009: case 20010: return clampd(x / 100.0, 0, 1);
    case 20011: return clampd(x / 100.0, 0.1, 9);
    case 20013: return clampd(x, 0, 4);
    case 20015: return std::clamp(g, 0, 9);
    case 20016: case 20017: return clampd(x / 1024.0, 0, 1);
    case 20018: case 20019: return clampd(x / 128.0, 0, 1);
    case 20020: case 20028: return clampd(x / 128.0, -1, 1);
    case 20023: return std::clamp(g, 0, 2);
    case 20025: return g < 0 ? clampd((std::pow(2.0, -x / 96.0) - 1.0) * 66.67 / 1000.0 * tempo / 60.0, 0.01, 4) : clampd(x / 192.0, 0.01, 4);
    case 20026: return clampd(x / 512.0, -1, 1);
    default: return std::numeric_limits<double>::quiet_NaN();
  }
}

// Curves as the decoder reads them (FlpGenHarmor.cpp readCurve).
hm::Curve curveOf(const HCurve& c) {
  hm::Curve o;
  for (const HPt& p : c.pts) {
    hm::Pt q;
    q.tension = -std::clamp(std::isfinite(p.t) ? p.t : 0.0f, -1.0f, 1.0f);
    q.kind = c.fmt >= 1 ? static_cast<uint8_t>(std::min<int>(p.tail[0], 12)) : 0;
    q.dt = static_cast<float>(std::max(0.0, std::isfinite(p.x) ? p.x : 0.0));
    q.y = static_cast<float>(clampd(std::isfinite(p.y) ? p.y : 0.0, -4, 4));
    if (o.pts.size() < 4096) o.pts.push_back(q);
  }
  const int np = static_cast<int>(o.pts.size());
  const int flags = c.tail[0], en = c.tail[1], dec = c.tail[2], loop = c.tail[3], sus = c.tail[4];
  o.enabled = en != 0;
  o.tempo = (flags & 1) != 0;
  o.global = (flags & 2) != 0;
  o.decay = dec >= 0 && dec < np ? dec : -1;
  o.loopStart = loop >= 0 && loop < np ? loop : -1;
  o.sustain = sus >= 0 && sus < np ? sus : -1;
  return o;
}
bool sameCurve(const hm::Curve& a, const hm::Curve& b) {
  if (a.pts.size() != b.pts.size() || a.sustain != b.sustain || a.loopStart != b.loopStart || a.decay != b.decay || a.enabled != b.enabled ||
      a.tempo != b.tempo || a.global != b.global)
    return false;
  for (size_t i = 0; i < a.pts.size(); ++i)
    if (a.pts[i].dt != b.pts[i].dt || a.pts[i].y != b.pts[i].y || a.pts[i].tension != b.pts[i].tension || a.pts[i].kind != b.pts[i].kind)
      return false;
  return true;
}
bool flatAt(const hm::Curve& c, float v) {
  for (const auto& p : c.pts)
    if (std::fabs(p.y - v) > 1e-4f || p.kind == 6) return false;
  return true;
}
// Writes curve `want` into `c` (points, tail), keeping c's kind / format.
void putCurve(HCurve& c, const hm::Curve& want) {
  c.fmt = std::max(c.fmt, 2);
  c.pts.clear();
  for (size_t i = 0; i < want.pts.size(); ++i)
    c.pts.push_back(hpt(want.pts[i].dt, want.pts[i].y, -want.pts[i].tension, want.pts[i].kind, i == 0));
  c.tail[0] = (c.tail[0] & ~3) | (want.tempo ? 1 : 0) | (want.global ? 2 : 0);
  c.tail[1] = want.enabled ? -1 : 0;
  c.tail[2] = want.decay, c.tail[3] = want.loopStart, c.tail[4] = want.sustain;
}

void applyPart(const PluginState& g, const hm::PartData& want, int p, int ver, HPart& P, const EncodeContext& ec) {
  (void)ec;
  int32_t* t = P.params.data();
  // Tag dwords.
  for (const T& e : partTable()) t[e.tag] = solveInt(t[e.tag], e.lo, e.hi, pv(g, hm::pf(p, e.field)), [&](int32_t r) { return partLaw(e.tag, r); });
  for (int k = 0; k < 2; ++k)
    for (const T& e : filterTable()) {
      const int tag = e.tag + 12 * k;
      t[tag] = solveInt(t[tag], e.lo, e.hi, pv(g, hm::flt(p, k, e.field)), [&](int32_t r) { return partLaw(tag, r); });
    }
  t[62] = solveInt(t[62], 0, 2048, pv(g, hm::flt(p, 0, hm::PfF1Osc)), [](int32_t r) { return partLaw(62, r); });
  // Part record.
  P.rec[0] = solveInt(P.rec[0], 0, 5, pv(g, hm::pf(p, hm::PfBlend)), [](int32_t r) { return std::clamp(r, 0, 5); });
  P.rec[3] = solveInt(P.rec[3], 0, 1, pv(g, hm::pf(p, hm::PfPrismMul)), [](int32_t r) { return r ? 1 : 0; });
  P.rec[5] = solveInt(P.rec[5], 0, 4, pv(g, hm::pf(p, hm::PfClipMode)), [](int32_t r) { return std::clamp(r, 0, 4); });
  P.rec[6] = solveInt(P.rec[6], 0, 1, pv(g, hm::pf(p, hm::PfSubMode)), [](int32_t r) { return std::clamp(r, 0, 1); });
  P.rec[10] = solveInt(P.rec[10], 200, 210, pv(g, hm::pf(p, hm::PfPhType)), [](int32_t r) { return std::clamp(r - 200, 0, 10); });
  if (p == 0) {
    float eq;
    std::memcpy(&eq, &P.rec[12], 4);
    const float want = pv(g, hm::FxEqMix);
    if (std::isfinite(want) && static_cast<float>(clampd(std::isfinite(eq) ? eq : 1.0, 0, 1)) != want) std::memcpy(&P.rec[12], &want, 4);
  }
  for (int k = 0; k < 2; ++k) {
    P.ft[k] = solveInt(P.ft[k], 0, 19, pv(g, hm::flt(p, k, hm::PfF1Type)), [](int32_t r) { return std::clamp(r >= 17 ? r + 1 : r, 0, hm::kFltShapes - 1); });
    P.ft[2 + k] = solveInt(P.ft[2 + k], 100, 100 + hm::kResShapes - 1, pv(g, hm::flt(p, k, hm::PfF1ResType)),
                           [](int32_t r) { return std::clamp(r - 100, 0, hm::kResShapes - 1); });
  }
  const float en = pv(g, hm::pf(p, hm::PfEnabled));
  if (std::isfinite(en) && (P.b3[0] != 0) != (en >= 0.5f)) P.b3[0] = en >= 0.5f ? 1 : 0;
  // Articulators.
  for (int a = 0; a < hm::kArts && a < static_cast<int>(P.arts.size()); ++a) {
    HArt& A = P.arts[static_cast<size_t>(a)];
    if (!A.present) continue;
    const hm::ArtData& W = want.arts[static_cast<size_t>(a)];
    if ((A.hdr[0] < 0.5f) != W.bipolar) A.hdr[0] = W.bipolar ? 0.f : 1.f;
    if (!sameCurve(curveOf(A.env), W.env)) putCurve(A.env, W.env);
    auto scale = [](int32_t& raw, float w, float lo, float hi) {
      raw = solveInt(raw, -2147483647.0, 2147483647.0, w, [=](int32_t r) { return std::clamp(r / 128.0f, lo, hi); });
    };
    scale(A.adsr[0], W.att, 0.f, 4.f), scale(A.adsr[1], W.dec, 0.f, 4.f), scale(A.adsr[2], W.sus, -1.f, 1.f), scale(A.adsr[3], W.rel, 0.f, 4.f);
    if (!sameCurve(curveOf(A.lfo), W.lfo)) putCurve(A.lfo, W.lfo);
    A.lfop[0] = solveInt(A.lfop[0], 0, 65536, W.lfoSpeed, [](int32_t r) {
      return static_cast<float>(clampd((std::pow(10001.0, clampd(r, 0, 65536) / 65536.0) - 1.0) / 625.0, 0, 100));
    });
    scale(A.lfop[2], W.lfoSkew, -1.f, 1.f), scale(A.lfop[3], W.lfoPw, -1.f, 1.f);
    {
      const auto law = [](int32_t r) { return static_cast<float>(static_cast<uint32_t>(r) / 4294967296.0); };
      if (law(A.lfop[4]) != W.lfoPhase) {
        const uint32_t g0 = static_cast<uint32_t>(std::llround(std::clamp(static_cast<double>(W.lfoPhase), 0.0, 1.0) * 4294967296.0) & 0xFFFFFFFFll);
        uint32_t best = g0;
        for (int d = -256; d <= 256; ++d)
          if (law(static_cast<int32_t>(g0 + static_cast<uint32_t>(d))) == W.lfoPhase) {
            best = g0 + static_cast<uint32_t>(d);
            break;
          }
        A.lfop[4] = static_cast<int32_t>(best);
      }
    }
    const float neutral = W.bipolar ? 0.5f : 1.0f;
    for (int m = 0; m < hm::kMaps; ++m) {
      const hm::Curve& wm = W.maps[static_cast<size_t>(m)];
      hm::Curve have = curveOf(A.maps[m]);
      const bool haveEmpty = flatAt(have, neutral);
      have.enabled = true;
      if (haveEmpty && wm.empty()) continue;
      if (!haveEmpty && !wm.empty() && sameCurve(have, wm)) continue;
      if (wm.empty()) {
        const int32_t kind = A.maps[m].kind;
        A.maps[m] = flat(kind, neutral);
      } else {
        putCurve(A.maps[m], wm);
      }
    }
    // Output smoothing (tags 343..).
    // Before v16 there is no tag 376 (attack articulator): later tags sit one lower.
    const int idx = ver < 16 && 343 + a > 376 ? 342 + a : 343 + a;
    if (static_cast<size_t>(idx) < P.params.size()) {
      t[idx] = solveInt(t[idx], -2000, 2000, W.smooth,
                        [](int32_t v) { return static_cast<float>(clampd((std::pow(2.0, v * 0.009) - 1.0) / 0.0511 / 1000.0, 0, 10)); });
    }
  }
  // Shaping curves.
  for (int s = 0; s < hm::kShapes; ++s) {
    const hm::Curve& ws = want.shapes[static_cast<size_t>(s)];
    const hm::Curve have = curveOf(P.shapes[s]);
    const bool haveEmpty = flatAt(have, hm::neutralShape(s));
    if (haveEmpty && ws.empty()) continue;
    if (!haveEmpty && !ws.empty() && sameCurve(have, ws)) continue;
    if (ws.empty()) P.shapes[s] = flat(3, hm::neutralShape(s));
    else putCurve(P.shapes[s], ws);
  }
  // Image layer: embedded planes (gain 0..255, frequency offset ×1/64 octave) or a source file.
  {
    HImg& I = P.img;
    const hm::ImageData& wi = want.image;
    const float loopWant = pv(g, hm::pf(p, hm::PfImgLoop));
    I.loop = solveInt(I.loop, 0, 2, loopWant, [](int32_t r) { return std::clamp(r, 0, 2); });
    // What the decoder makes of the current block.
    hm::ImageData hi;
    if (I.src == 0 && (!I.planes[0].empty() || !I.planes[1].empty()) && I.cols <= 20000 && I.rows <= 1024 && I.cols > 0 && I.rows > 0) {
      hi.cols = I.cols, hi.rows = I.rows;
      hi.gain.resize(static_cast<size_t>(I.cols) * static_cast<size_t>(I.rows), 255);
      if (!I.planes[0].empty())
        for (size_t i = 0; i < hi.gain.size() && i < I.planes[0].size(); ++i)
          hi.gain[i] = static_cast<uint8_t>(std::clamp(std::lround(I.planes[0][i]), 0L, 255L));
      if (!I.planes[1].empty()) {
        hi.freq.resize(hi.gain.size());
        for (size_t i = 0; i < hi.freq.size() && i < I.planes[1].size(); ++i)
          hi.freq[i] = static_cast<int8_t>(std::clamp(std::lround(I.planes[1][i] * 64.0f), -127L, 127L));
      }
    }
    const bool sameImg = hi.cols == wi.cols && hi.rows == wi.rows && hi.gain == wi.gain && hi.freq == wi.freq;
    if (!sameImg && wi.source.empty()) {
      I.src = 0;
      I.cols = wi.gain.empty() ? 0 : wi.cols, I.rows = wi.gain.empty() ? 0 : wi.rows;
      for (auto& pl : I.planes) pl.clear();
      I.planes[0].assign(wi.gain.begin(), wi.gain.end());
      for (int8_t f : wi.freq) I.planes[1].push_back(f / 64.0f);
      for (int k = 0; k < 3; ++k) I.counts[k] = static_cast<int32_t>(I.planes[k].size());
    }
  }
}

void applySource(const hm::PartData& want, HPart& P, const EncodeContext& ec) {
  HImg& I = P.img;
  const std::string& src = want.image.source;
  if (src.empty() || !want.image.gain.empty()) {
    if (I.src == 1 && src.empty()) I.src = 0, I.path.clear();  // decoded as no source
    return;
  }
  const std::string fl = flPath(src, ec);
  if (I.src == 1 && std::string(I.path.begin(), I.path.end()) == fl) return;
  I.src = 1;
  for (auto& pl : I.planes) pl.clear();
  for (int32_t& c : I.counts) c = 0;
  I.path.assign(fl.begin(), fl.end());
}

void applyGlobal(const PluginState& g, HGlobal& G, double tempo) {
  for (const T& e : globalTable()) {
    const int i = gIndex(e.tag);
    G.g[i] = solveInt(G.g[i], e.lo, e.hi, pv(g, e.field), [&](int32_t r) { return globalLaw(e.tag, r, tempo); });
  }
  G.rec[2] = solveInt(G.rec[2], 0, 3, pv(g, hm::GmMono), [](int32_t r) { return std::clamp(r, 0, 3); });
  G.rec[12] = solveInt(G.rec[12], 0, 11, pv(g, hm::FxDistType), [](int32_t r) { return std::clamp(r, 0, 11); });
  G.rec[13] = solveInt(G.rec[13], 0, 4, pv(g, hm::FxCompType), [](int32_t r) { return std::clamp(r, 0, 4); });
}

void apply(const PluginState& g, HPatch& s, const EncodeContext& ec) {
  hm::PatchData want;
  if (!hm::parsePatch(g.data, want)) want = hm::PatchData{};
  for (size_t p = 0; p < s.parts.size() && p < 2; ++p) {
    applyPart(g, want.part[p], static_cast<int>(p), s.ver, s.parts[p], ec);
    applySource(want.part[p], s.parts[p], ec);
  }
  if (s.hasGlobal) applyGlobal(g, s.g, ec.tempo > 0 ? ec.tempo : 120.0);
}

// Before version 16 there is no attack articulator (33): an instance imported
// from such a state has none (an empty curve there), which only that layout
// reproduces — a version-16+ template is turned into version 12 (385 tag
// dwords: the one at 376 goes, 42 articulators, ANSI info text).
void toPre16(HPatch& p) {
  if (p.ver < 16) return;
  p.raw = 12, p.ver = 12;
  for (HPart& P : p.parts) {
    if (P.params.size() > 376) P.params.erase(P.params.begin() + 376);
    if (P.arts.size() > static_cast<size_t>(hm::ArGAttack)) P.arts[static_cast<size_t>(hm::ArGAttack)].present = false;
  }
  p.g.info.clear();
}

void write(const PluginState& g, Bytes& b, const EncodeContext& ec) {
  HPatch p;
  if (!parse(b, p)) return;
  hm::PatchData want;
  if (hm::parsePatch(g.data, want)) {
    bool noAttack = !p.parts.empty();
    for (size_t k = 0; k < p.parts.size() && k < 2; ++k) noAttack = noAttack && want.part[k].arts[static_cast<size_t>(hm::ArGAttack)].env.pts.empty();
    if (noAttack) toPre16(p);
  }
  apply(g, p, ec);
  b = serialize(p);
}

Bytes defaults() {
  static const Bytes d = [] {
    HPatch p = defaultPatch();
    EncodeContext ec;
    apply(defaultPluginState("gl.harmor"), p, ec);
    return serialize(p);
  }();
  return d;
}

}  // namespace

// A Harmor state without its info text (comparisons; empty when it does not read).
Bytes harmorComparable(const Bytes& state) {
  HPatch p;
  if (!parse(state, p)) return {};
  p.g.info.clear();
  return serialize(p);
}

bool harmorStructureRoundTrip(const Bytes& state, Bytes* again) {
  HPatch p;
  if (!parse(state, p)) return false;
  const Bytes b = serialize(p);
  if (again) *again = b;
  return b == state;
}

Encoder harmorEncoder() {
  Encoder e;
  e.flName = "Harmor";
  e.glType = "gl.harmor";
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = defaults;
  e.fields = fields;
  e.write = write;
  return e;
}

}  // namespace gl::flp::enc
