// Gridline — FLP export: plugin state encoders for FlpFx2Space.cpp (Convolver, Delay, Phaser, Reeverb, LuxeVerb, Vintage Chorus, Vintage Phaser).
// The reverse of the decoders in those files; see FlpEncode.h for the model.
#include <algorithm>
#include <cmath>
#include "../../packs/spacefx/spacefx.h"
#include "../PluginParams.h"
#include "FlpEncode.h"

namespace gl::flp::enc {

namespace {

namespace sx = gl::packs::spacefx;

// One stored int per Gridline parameter: tag (index in the int block), param, stored range.
struct TagField {
  int tag, gl;
  double lo, hi;
};

// Decoded Gridline state of `b` (false when the decoder rejects it).
bool decoded(const std::string& flName, const Bytes& b, const EncodeContext& c, PluginState& out) {
  PluginMapping m;
  if (!decodeState(flName, b, m, c)) return false;
  out = m.state;
  return true;
}
float paramOf(const PluginState& s, int k) { return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : 0.0f; }

// ------------------------------------------------------------------ LuxeVerb → gl.hall
// u32 version, u32 sidechain input, 5 header bytes (pre-delay sync, feedback
// sync, 3 display flags), one i32 per tag from offset 13, v9: 2 feed-forward
// sync bytes.
constexpr size_t kLuxeTags = 13;
const std::vector<TagField>& luxeTable() {
  static const std::vector<TagField> t = {
      {0, sx::HvInGain, -600, 1500},     {1, sx::HvInHighCut, 40, 20000},  {2, sx::HvInLowCut, 20, 5000},
      {3, sx::HvOn, 0, 1},               {4, sx::HvDecay, 250, 20000},     {5, sx::HvPreDelay, 0, 5000},
      {6, sx::HvPreTicks, 1, 768},       {7, sx::HvBright, 0, 1000},       {8, sx::HvCharacter, 0, 100},
      {9, sx::HvSize, 1, 100},           {10, sx::HvModFreq, 0, 1000},     {11, sx::HvModDepth, 0, 100},
      {12, sx::HvDiffusion, 0, 1000},    {13, sx::HvFreeze, 0, 2},         {14, sx::HvHQ, 0, 1},
      {15, sx::HvFbOn, 0, 1},            {16, sx::HvFbPitch, -120, 120},   {17, sx::HvFbDelay, 10, 5000},
      {18, sx::HvFbTicks, 1, 768},       {19, sx::HvFbGain, 0, 1000},      {20, sx::HvFbHighCut, 20, 5000},
      {21, sx::HvFbLowCut, 20, 5000},    {22, sx::HvFbMix, 0, 1000},       {23, sx::HvEnvScale, -1000, 1000},
      {24, sx::HvEnvThresh, -6000, 0},   {25, sx::HvEnvAttack, 50, 1000},  {26, sx::HvEnvRelease, 50, 1000},
      {27, sx::HvEnvOffset, 0, 100},     {28, sx::HvEnvRoute, 0, 2},       {29, sx::HvEnvSidechain, 0, 1},
      {30, sx::HvEnvLowCut, 20, 2000},   {31, sx::HvEnvSmooth, 10, 1000},  {32, sx::HvDry, -600, 1500},
      {33, sx::HvWet, -600, 1500},       {34, sx::HvEqFreq, 40, 10000},    {35, sx::HvEqGain, -1800, 1800},
      {36, sx::HvEqQ, 25, 5000},         {37, sx::HvWidth, 0, 200},        {38, sx::HvLimiter, 0, 1},
      {39, sx::HvFbMode, 0, 1},          {40, sx::HvFfDirect, 0, 1000},    {41, sx::HvFfPitch1, -24, 24},
      {42, sx::HvFfPitch2, -24, 24},     {43, sx::HvFfAmp1, 0, 1000},      {44, sx::HvFfAmp2, 0, 1000},
      {45, sx::HvFfDelay1, 10, 5000},    {46, sx::HvFfTicks1, 1, 768},     {47, sx::HvFfDelay2, 0, 1000},
      {48, sx::HvFfTicks2, 1, 768},
  };
  return t;
}

Encoder luxeVerb() {
  Encoder e;
  e.flName = "LuxeVerb";
  e.glType = "gl.hall";
  e.nativeRank = 0;  // the only FL home of the Hall
  e.defaults = [] {
    // Version 9 with the plugin's default settings (values per tag).
    const int inGain = 721, inHighCut = 12991, inLowCut = 20, reverbOn = 1, decay = 11625, preDelay = 326,
              preTicks = 192, bright = 253, character = 40, size = 20, modFreq = 47, modDepth = 20, diffusion = 403,
              freeze = 0, hq = 1, fbOn = 0, fbPitch = 120, fbDelay = 2988, fbTicks = 192, fbGain = 0, fbHighCut = 1719,
              fbLowCut = 720, fbMix = 1000, envScale = 0, envThresh = -4000, envAttack = 50, envRelease = 400,
              envOffset = 0, envRoute = 0, envSidechain = 0, envLowCut = 301, envSmooth = 10, dry = 746, wet = 746,
              eqFreq = 1570, eqGain = 315, eqQ = 865, width = 120, limiter = 1, fbMode = 0, ffDirect = 0, ffPitch1 = 24,
              ffPitch2 = 20, ffAmp1 = 1000, ffAmp2 = 20, ffDelay1 = 1000, ffTicks1 = 192, ffDelay2 = 1000, ffTicks2 = 192;
    const int tags[49] = {inGain,    inHighCut, inLowCut,  reverbOn,  decay,      preDelay,     preTicks,  bright,   character, size,
                          modFreq,   modDepth,  diffusion, freeze,    hq,         fbOn,         fbPitch,   fbDelay,  fbTicks,   fbGain,
                          fbHighCut, fbLowCut,  fbMix,     envScale,  envThresh,  envAttack,    envRelease, envOffset, envRoute, envSidechain,
                          envLowCut, envSmooth, dry,       wet,       eqFreq,     eqGain,       eqQ,       width,    limiter,   fbMode,
                          ffDirect,  ffPitch1,  ffPitch2,  ffAmp1,    ffAmp2,     ffDelay1,     ffTicks1,  ffDelay2, ffTicks2};
    ByteWriter w;
    w.u32(9);  // version
    w.u32(0);  // sidechain input number
    w.u8(0);   // pre-delay tempo sync
    w.u8(0);   // feedback delay tempo sync
    w.u8(1);   // display flags (spectrum on, peaks off, envelope graph off)
    w.u8(0);
    w.u8(0);
    for (int v : tags) w.i32(v);
    w.u8(0);  // feed-forward delay 1 tempo sync
    w.u8(0);  // feed-forward delay 2 tempo sync
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    const uint32_t version = getU32(b, 0);
    const size_t nTags = version <= 7 ? 38 : version == 8 ? 39 : 49;
    for (const TagField& t : luxeTable())
      if (static_cast<size_t>(t.tag) < nTags) f.push_back(i32(t.gl, kLuxeTags + 4 * static_cast<size_t>(t.tag), t.lo, t.hi));
    f.push_back(field(sx::HvPreSync, 8, Type::U8, 0, 1));
    f.push_back(field(sx::HvFbSync, 9, Type::U8, 0, 1));
    if (nTags == 49 && b.size() >= kLuxeTags + 49 * 4 + 2) {
      f.push_back(field(sx::HvFfSync1, kLuxeTags + 49 * 4, Type::U8, 0, 1));
      f.push_back(field(sx::HvFfSync2, kLuxeTags + 49 * 4 + 1, Type::U8, 0, 1));
    }
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ Fruity Convolver → gl.convolver
// u32 version (low word 3, version in the high word for v14/15), dry, wet,
// input separation, wet separation (i32), u8 "file" flag, impulse path (u8
// length or 0xFF + u32), embedded RIFF when the flag is 0 (u32 size + data),
// 4 × f64 editor selection / view, f32 zoom, five point envelopes (u32 kind,
// u32 format, u32 count, count × {f64 Δt, f64 value, f32 tension, u32 flags},
// 20 marker bytes), 4 flag bytes ([3] = normalize), v15: i32 latency, then
// i32 editor tab, pre-delay, self-convolution, stretch, [v ≥ 11] EQ mix.
struct ConvLayout {
  bool ok = false;
  uint32_t version = 0;
  size_t pathEnd = 0;  // end of the path / embedded impulse section (starts at 20)
  size_t env[5] = {};
  size_t envEnd = 0;   // = the 4 flag bytes
  size_t latency = 0;  // 0: none
  size_t ints = 0;
  size_t nInts = 0;
};
ConvLayout convLayout(const Bytes& b) {
  ConvLayout L;
  if (b.size() < 22) return L;
  L.version = getU32(b, 0);
  if (L.version >= 0x10000) L.version >>= 16;
  const bool isFile = b[20] != 0;
  size_t p = 22, len = b[21];
  if (len == 0xFF) len = getU32(b, 22), p = 26;
  p += len;
  if (!isFile) p += 4 + static_cast<size_t>(getU32(b, p));
  L.pathEnd = p;
  p += 4 * 8 + 4;
  for (size_t& e : L.env) {
    if (p + 12 > b.size()) return L;
    e = p;
    const size_t n = getU32(b, p + 8);
    if (n > 100000) return L;
    p += 12 + n * 24 + 20;
  }
  if (p + 4 > b.size()) return L;
  L.envEnd = p;
  p += 4;
  if (L.version >= 15) L.latency = p, p += 4;
  L.ints = p;
  L.nInts = std::min<size_t>(L.version >= 11 ? 5 : 4, p <= b.size() ? (b.size() - p) / 4 : 0);
  L.ok = true;
  return L;
}

// UTF-8 → Latin-1 (FL stores the impulse path as 8-bit text).
std::string toLatin1(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xC0 && c < 0xE0 && i + 1 < s.size()) cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu), n = 2;
    else if (c >= 0xE0 && c < 0xF0) cp = '?', n = 3;
    else if (c >= 0xF0) cp = '?', n = 4;
    o += static_cast<char>(cp < 256 ? cp : '?');
    i += n;
  }
  return o;
}

// Replaces envelope `k` with the given points (empty: the stored points, flattened to unity).
void writeConvEnvelope(Bytes& b, const ConvLayout& L, int k, const std::vector<sx::EnvPoint>& pts) {
  const size_t at = L.env[k];
  const size_t n = getU32(b, at + 8);
  const size_t end = at + 12 + n * 24 + 20;
  if (pts.empty()) {
    for (size_t i = 0; i < n; ++i) putF64(b, at + 12 + i * 24 + 8, 0.5);
    return;
  }
  ByteWriter w;
  w.u32(getU32(b, at));      // kind
  w.u32(getU32(b, at + 4));  // format
  w.u32(static_cast<uint32_t>(pts.size()));
  double t = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    w.f64(i == 0 ? pts[i].t : pts[i].t - t);
    t = pts[i].t;
    w.f64(pts[i].v);
    w.f32(static_cast<float>(pts[i].k));
    // Point flags: the last point takes the stored end point's, the others the stored point's at that index.
    uint32_t flags = 0;
    if (n > 0 && i + 1 == pts.size()) flags = getU32(b, at + 12 + (n - 1) * 24 + 20);
    else if (i + 1 < n) flags = getU32(b, at + 12 + i * 24 + 20);
    flags = (flags & 0xFFFFFF00u) | static_cast<uint32_t>(std::clamp(pts[i].c, 0, 12));  // curve kind
    w.u32(flags);
  }
  w.bytes(b.data() + at + 12 + n * 24, 20);  // markers
  b.erase(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(end));
  b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), w.b.begin(), w.b.end());
}

std::string pointsKey(const std::vector<sx::EnvPoint>& p) {
  sx::ConvData d;
  d.env = p;
  return sx::formatConvData(d);
}

Encoder convolver() {
  Encoder e;
  e.flName = "Fruity Convolver";
  e.glType = "gl.convolver";
  e.nativeRank = 0;  // the only FL home of the Convolver
  e.defaults = [] {
    const int dry = 1000, wet = 500, inSeparation = 0, wetSeparation = 0;
    const std::string path = "%FLStudioFactoryData%\\Data\\Patches\\Impulses\\Default.wav";
    const double irSeconds = 6.848;  // length of the default impulse (envelope span)
    ByteWriter w;
    w.u32((15u << 16) | 3u);  // version 15 (high word)
    w.i32(dry);
    w.i32(wet);
    w.i32(inSeparation);
    w.i32(wetSeparation);
    w.u8(1);  // impulse from a file
    w.u8(static_cast<uint32_t>(path.size()));
    w.bytes(path.data(), path.size());
    w.f64(-1);  // editor selection start / end (none)
    w.f64(-1);
    w.f64(0);  // editor view start / end
    w.f64(irSeconds);
    w.f32(0.02f);  // editor zoom
    auto envelope = [&](uint32_t kind, double span) {
      w.u32(kind);
      w.u32(3);  // point format
      w.u32(2);  // two flat points at unity
      for (int i = 0; i < 2; ++i) {
        w.f64(i ? span : 0.0);
        w.f64(0.5);
        w.f32(0);
        w.u32(i ? 0x02000000u : 0u);  // end point flag
      }
      w.u32(0);  // markers: none
      for (int i = 0; i < 4; ++i) w.i32(-1);
    };
    for (int k = 0; k < 4; ++k) envelope(6, irSeconds);  // time envelopes
    envelope(3, 1.0);                                     // EQ curve (0..1 log axis)
    w.u8(0);  // flags: [0..2] editor options (FL writes 0, 0, 1), [3] normalize
    w.u8(0);
    w.u8(1);
    w.u8(1);
    w.i32(0);  // latency
    const int tab = 0, preDelay = 0, selfConv = 0, stretch = 0, eqMix = 1000;
    for (int v : {tab, preDelay, selfConv, stretch, eqMix}) w.i32(v);
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> f = {i32(sx::CvDry, 4, 0, 1000), i32(sx::CvWet, 8, 0, 1000), i32(sx::CvInWidth, 12, -1000, 1000),
                            i32(sx::CvWidth, 16, -1000, 1000)};
    const ConvLayout L = convLayout(b);
    if (!L.ok) return f;
    f.push_back(field(sx::CvNormalize, L.envEnd + 3, Type::U8, 0, 1));
    if (L.latency) f.push_back(i32(sx::CvLatency, L.latency, 0, 65536));
    if (L.nInts >= 4) {
      f.push_back(i32(sx::CvPreDelay, L.ints + 4, 0, 1000));
      f.push_back(i32(sx::CvSelfConv, L.ints + 8, 0, 1000));
      f.push_back(i32(sx::CvStretch, L.ints + 12, -1000, 1000));
    }
    if (L.nInts >= 5) f.push_back(i32(sx::CvEqMix, L.ints + 16, 0, 1000));
    return f;
  };
  // Impulse path, IR envelope and EQ curve (PluginState::data).
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (!decoded("Fruity Convolver", b, c, cur)) return;
    const sx::ConvData want = sx::parseConvData(g.data), have = sx::parseConvData(cur.data);
    ConvLayout L = convLayout(b);
    if (!L.ok) return;
    if (want.path != have.path) {
      const std::string p = toLatin1(flPath(want.path, c));
      ByteWriter w;
      w.u8(1);  // from a file
      if (p.size() < 0xFF) {
        w.u8(static_cast<uint32_t>(p.size()));
      } else {
        w.u8(0xFF);
        w.u32(static_cast<uint32_t>(p.size()));
      }
      w.bytes(p.data(), p.size());
      b.erase(b.begin() + 20, b.begin() + static_cast<std::ptrdiff_t>(L.pathEnd));
      b.insert(b.begin() + 20, w.b.begin(), w.b.end());
      L = convLayout(b);
      if (!L.ok) return;
    }
    // Time envelopes 0 pan, 1 volume, 2 separation (J-42), then the EQ curve.
    const std::vector<sx::EnvPoint>* wantK[3] = {&want.pan, &want.env, &want.sep};
    const std::vector<sx::EnvPoint>* haveK[3] = {&have.pan, &have.env, &have.sep};
    for (int k = 0; k < 3; ++k)
      if (pointsKey(*wantK[k]) != pointsKey(*haveK[k])) {
        writeConvEnvelope(b, L, k, *wantK[k]);
        L = convLayout(b);
        if (!L.ok) return;
      }
    if (pointsKey(want.eq) != pointsKey(have.eq)) writeConvEnvelope(b, L, 4, want.eq);
  };
  return e;
}

// ------------------------------------------------------------------ Fruity Reeverb → gl.reverb (plate)
// version 1, then 10 i32 (tags 0..9).
Encoder reeverb() {
  Encoder e;
  e.flName = "Fruity Reeverb";
  e.glType = "gl.reverb";
  e.nativeRank = 0;  // the plate model (RvbModel = 1) is this plugin's
  e.accepts = [](const PluginState& s) { return paramOf(s, pp::RvbModel) == 1.0f; };
  e.defaults = [] {
    const int lowCut = 1250, highCut = 10800, preDelay = 0, roomSize = 26000, diffusion = 65536, colour = 20,
              decay = 4700, highDamping = 10800, dry = 65536, reverb = 13120;
    ByteWriter w;
    w.i32(1);  // version
    for (int v : {lowCut, highCut, preDelay, roomSize, diffusion, colour, decay, highDamping, dry, reverb}) w.i32(v);
    return w.b;
  };
  e.fields = [](const Bytes&) {
    const TagField t[] = {{0, pp::RvbLowCut, 0, 65536},     {1, pp::RvbHighCut, 0, 65536}, {2, pp::RvbPreDelay, 0, 250},
                          {3, pp::RvbSize, 0, 65536},       {4, pp::RvbDiffusion, 0, 65536}, {5, pp::RvbBassMult, 0, 40},
                          {6, pp::RvbDecayTime, 0, 65536},  {7, pp::RvbDampHz, 0, 65536},   {8, pp::RvbDry, 0, 65536},
                          {9, pp::RvbWet, 0, 65536}};
    std::vector<Field> f;
    for (const TagField& x : t) f.push_back(i32(x.gl, 4 + 4 * static_cast<size_t>(x.tag), x.lo, x.hi));
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ Fruity Delay → gl.delay
// version 1, input, feedback, cutoff, tempo, steps (1..16), mode.
Encoder delay() {
  Encoder e;
  e.flName = "Fruity Delay";
  e.glType = "gl.delay";
  e.nativeRank = 0;  // the classic delay model (DlyModel = 2) is this plugin's
  e.accepts = [](const PluginState& s) { return std::lround(paramOf(s, pp::DlyModel)) == 2; };
  e.defaults = [] {
    const int input = 338, feedback = 512, cutoff = 1024, tempo = 0, steps = 3, mode = 0;
    ByteWriter w;
    w.i32(1);  // version
    for (int v : {input, feedback, cutoff, tempo, steps, mode}) w.i32(v);
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(pp::DlyClsIn, 4, 0, 1024),  i32(pp::DlyClsFb, 8, 0, 1024), i32(pp::DlyClsCoef, 12, 0, 1024),
                              i32(pp::DlyClsBpm, 16, 0, 1024), i32(pp::DlyTicks, 20, 1, 16),  i32(pp::DlyClsMode, 24, 0, 1024)};
  };
  return e;
}

// ------------------------------------------------------------------ Fruity Phaser → gl.phaser
// version 1, sweep rate, min / max depth, frequency range, stereo, stages,
// feedback, dry-wet, output gain.
Encoder phaser() {
  Encoder e;
  e.flName = "Fruity Phaser";
  e.glType = "gl.phaser";
  e.nativeRank = 0;  // the Phaser's FL home
  e.defaults = [] {
    const int rate = 2500, minDepth = 100, maxDepth = 800, freqRange = 0, stereo = 512, stages = 7, feedback = 400,
              dryWet = 512, outGain = 4000;
    ByteWriter w;
    w.i32(1);  // version
    for (int v : {rate, minDepth, maxDepth, freqRange, stereo, stages, feedback, dryWet, outGain}) w.i32(v);
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(pp::PhsSweepN, 4, 0, 5000),    i32(pp::PhsDepthMin, 8, 0, 1000), i32(pp::PhsDepthMax, 12, 0, 1000),
                              i32(pp::PhsRangeLarge, 16, 0, 1024), i32(pp::PhsStereoN, 20, 0, 1024), i32(pp::PhsStagesN, 24, 0, 22),
                              i32(pp::PhsFeedbackN, 28, 0, 1000), i32(pp::PhsDryN, 32, 0, 1024),    i32(pp::PhsGainN, 36, 0, 5000)};
  };
  return e;
}

// ------------------------------------------------------------------ Vintage Chorus → gl.bbdchorus
// version 1, then 17 i32 in tag order.
Encoder vintageChorus() {
  Encoder e;
  e.flName = "Vintage Chorus";
  e.glType = "gl.bbdchorus";
  e.nativeRank = 0;  // the Bucket Chorus's FL home
  e.defaults = [] {
    const int mode = 0, link = 0, speed = 80, lfoType = 0, phaseLR = 3000, time1 = 60, time2 = 3000, mix = 3000,
              monoIn = 0, invertWet = 0, feedback = 0, highPass = 3000, gain = 0, noise = 3000, gate = 0, sync = 0,
              startPhase = 0;
    ByteWriter w;
    w.i32(1);  // version
    for (int v : {mode, link, speed, lfoType, phaseLR, time1, time2, mix, monoIn, invertWet, feedback, highPass, gain, noise, gate,
                  sync, startPhase})
      w.i32(v);
    return w.b;
  };
  e.fields = [](const Bytes&) {
    auto at = [](int tag) { return 4 + 4 * static_cast<size_t>(tag); };
    return std::vector<Field>{
        i32(sx::BcRate, at(2), 1, 245760),      i32(sx::BcSyncTicks, at(2), 1, 245760), i32(sx::BcPhaseLR, at(4), 0, 6000),
        i32(sx::BcTime1, at(5), 0, 6000),       i32(sx::BcTime2, at(6), 0, 6000),       i32(sx::BcMix, at(7), 0, 6000),
        i32(sx::BcFeedback, at(10), 0, 6000),   i32(sx::BcHighPass, at(11), 0, 6000),   i32(sx::BcGain, at(12), 0, 6000),
        i32(sx::BcHiss, at(13), 0, 6000),       i32(sx::BcStartPhase, at(16), 0, 6000), i32(sx::BcWave, at(3), 0, 1),
        i32(sx::BcMonoIn, at(8), 0, 1),         i32(sx::BcInvertWet, at(9), 0, 1),      i32(sx::BcNoiseGate, at(14), 0, 1),
        i32(sx::BcSync, at(15), 0, 1)};
  };
  // Mode: chorus I / II (tag 0 = 0 / 1), I+II (tag 1 = link), custom (tag 0 = 2).
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (!decoded("Vintage Chorus", b, c, cur) || paramOf(cur, sx::BcMode) == paramOf(g, sx::BcMode)) return;
    const int mode = static_cast<int>(std::lround(paramOf(g, sx::BcMode)));
    const int model = getU32(b, 4) == 1 ? 1 : 0;
    putI32(b, 4, mode == 3 ? 2 : mode == 2 ? model : mode);
    putI32(b, 8, mode == 2 ? 1 : 0);
  };
  return e;
}

// ------------------------------------------------------------------ Vintage Phaser → gl.vphaser
// version 3: 36 i32 in tag order + a UI byte; version 2: 36 i32; version 1:
// 32 i32 in an older order (no engine / drive / inverted feedback).
Encoder vintagePhaser() {
  Encoder e;
  e.flName = "Vintage Phaser";
  e.glType = "gl.vphaser";
  e.nativeRank = 0;  // the Retro Phaser's FL home
  e.defaults = [] {
    const int freqMin = 896070, freqMax = 1588996, feedback = 3000, manual = 0, manualFreq = 125, hq = 0, speed = 1, sync = 1,
              lfoType = 0, tension = 0, startPhase = 0, phaseLR = 0, delayOn = 0, delayKeep = 0, delayMix = 0,
              delayStereo = 3000, delayTime = 1, delaySync = 0, delayFb = 0, delayLp = 3200, delayHp = 2000, mix = 50,
              monoIn = 0, noise = 0, noiseGate = 0, colorGain = 0, colorFreq = 5000, inputHp = 2000, fbHp = 2000,
              rateLo = 25, rateHi = 150000, depthScale = 6000, depthPull = 0, engine = 1, drive = 5000, invertFb = 0;
    ByteWriter w;
    w.i32(3);  // version
    for (int v : {freqMin, freqMax, feedback, manual, manualFreq, hq, speed, sync, lfoType, tension, startPhase, phaseLR,
                  delayOn, delayKeep, delayMix, delayStereo, delayTime, delaySync, delayFb, delayLp, delayHp, mix, monoIn,
                  noise, noiseGate, colorGain, colorFreq, inputHp, fbHp, rateLo, rateHi, depthScale, depthPull, engine, drive,
                  invertFb})
      w.i32(v);
    w.u8(0);  // editor page
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    const int version = static_cast<int32_t>(getU32(b, 0));
    // Offset of each tag (0: not stored).
    size_t at[36] = {};
    if (version == 1) {
      static const int remap[32] = {0, 1, 2, 3, 4, 6, 8, 11, 7, 10, 21, 22, 23, 5, 12, 13, 14, 15, 16, 17, 18, 19, 20, 25, 26, 27, 28, 29, 30, 31, 32, 9};
      for (int i = 0; i < 32; ++i) at[remap[i]] = 4 + 4 * static_cast<size_t>(i);
    } else {
      for (int t = 0; t < (version >= 3 ? 36 : 33); ++t) at[t] = 4 + 4 * static_cast<size_t>(t);
    }
    const TagField t[] = {
        {29, sx::VpRateLo, 25, 150000},     {30, sx::VpRateHi, 25, 150000},   {0, sx::VpFreqMin, 125, 2048000},
        {1, sx::VpFreqMax, 125, 2048000},   {2, sx::VpFeedback, 0, 6000},     {4, sx::VpManualFreq, 125, 2048000},
        {9, sx::VpTension, 0, 6000},        {10, sx::VpStartPhase, 0, 6000},  {11, sx::VpPhaseLR, 0, 6000},
        {14, sx::VpDelayMix, 0, 6000},      {15, sx::VpDelayStereo, 0, 6000}, {16, sx::VpDelayTime, 1, 768},
        {16, sx::VpDelayTicks, 1, 768},     {18, sx::VpDelayFb, 0, 6000},     {19, sx::VpDelayLp, 3200, 204800},
        {20, sx::VpDelayHp, 2000, 204800},  {21, sx::VpMix, 0, 100},          {23, sx::VpNoise, 0, 6000},
        {25, sx::VpColorGain, 0, 6000},     {26, sx::VpColorFreq, 5000, 650000}, {27, sx::VpInputHp, 2000, 80000},
        {28, sx::VpFbHp, 2000, 80000},      {31, sx::VpDepthScale, 0, 6000},  {32, sx::VpDepthPull, 0, 6000},
        {34, sx::VpDrive, 5000, 650000},    {6, sx::VpRate, 1, 245760},       {6, sx::VpSyncTicks, 1, 245760},
        {35, sx::VpInvertFb, 0, 1},         {3, sx::VpManual, 0, 1},          {5, sx::VpHq, 0, 1},
        {7, sx::VpSync, 0, 1},              {8, sx::VpWave, 0, 1},            {12, sx::VpDelayOn, 0, 1},
        {13, sx::VpDelayKeep, 0, 1},        {17, sx::VpDelaySync, 0, 1},      {22, sx::VpMonoIn, 0, 1},
        {24, sx::VpNoiseGate, 0, 1},        {33, sx::VpEngine, 0, 1},
    };
    std::vector<Field> f;
    for (const TagField& x : t)
      if (at[x.tag]) f.push_back(i32(x.gl, at[x.tag], x.lo, x.hi));
    return f;
  };
  return e;
}

}  // namespace

void registerFx2Space(std::vector<Encoder>& v) {
  v.push_back(luxeVerb());
  v.push_back(convolver());
  v.push_back(reeverb());
  v.push_back(delay());
  v.push_back(phaser());
  v.push_back(vintageChorus());
  v.push_back(vintagePhaser());
}

}  // namespace gl::flp::enc
