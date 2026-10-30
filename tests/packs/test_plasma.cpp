// Gridline — "plasma" pack tests: Plasma (gl.plasma). Creation, sound after a
// note, silence after release, finite output at parameter extremes, block-size
// invariance, no allocation on the audio thread, band-limiting at high pitch,
// unison detune, the modulation matrix, every effect module and splitter,
// factory presets, short state vectors, the data model round trip, and the
// Serum 2 preset import on synthetic presets written by this test (no Xfer
// content is used or shipped).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include "../../src/core/Json.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/plasma/plasma.h"
#include "../Check.h"

using namespace gl;
namespace pl = gl::packs::plasma;

namespace {
std::atomic<bool> gWatch{false};
std::atomic<long> gAllocs{0};

constexpr double kSr = 48000;

struct Ev {
  int at;
  bool on;
  uint32_t id;
  double pitch;
  float vel;
};
struct Out {
  std::vector<float> L, R;
  bool finite = true;
  float peak = 0;
  bool activeAtEnd = false;
  long allocs = 0;
};

Out render(const PluginState& st, const std::vector<Ev>& evs, int total, int block, bool watch = false) {
  Out o;
  o.L.assign(static_cast<size_t>(total), 0.f);
  o.R.assign(static_cast<size_t>(total), 0.f);
  auto g = packs::createGenerator(st.type);
  if (!g) {
    o.finite = false;
    return o;
  }
  ChannelAux aux;
  packs::prepareAux(st, aux);
  g->prepareHost(st, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = 120;
  ctx.ppq = 96;
  ctx.playing = true;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    ctx.tick = pos / kSr * 2.0 * ctx.ppq;
    for (const auto& e : evs)
      if (e.at >= pos && e.at < pos + n) {
        if (e.on) g->noteOn(e.id, e.pitch, e.vel, e.at - pos);
        else g->noteOff(e.id, e.at - pos);
      }
    if (watch && pos > 0) gWatch = true;
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
    gWatch = false;
  }
  o.allocs = gAllocs.exchange(0);
  for (size_t i = 0; i < o.L.size(); ++i) {
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) o.finite = false;
    o.peak = std::max({o.peak, std::fabs(o.L[i]), std::fabs(o.R[i])});
  }
  o.activeAtEnd = g->active();
  return o;
}
float peakIn(const Out& o, int a, int b) {
  float p = 0;
  for (int i = std::max(0, a); i < std::min(b, static_cast<int>(o.L.size())); ++i)
    p = std::max({p, std::fabs(o.L[static_cast<size_t>(i)]), std::fabs(o.R[static_cast<size_t>(i)])});
  return p;
}
double rms(const std::vector<float>& x, int a, int b) {
  double s = 0;
  for (int i = a; i < b; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(s / std::max(1, b - a));
}
// Magnitude of frequency f in x[a, b) (Hann window).
double mag(const std::vector<float>& x, int a, int b, double f) {
  const double w = 2 * 3.14159265358979 * f / kSr;
  double re = 0, im = 0;
  for (int i = a; i < b; ++i) {
    const double win = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * (i - a) / (b - a));
    re += x[static_cast<size_t>(i)] * win * std::cos(w * i);
    im -= x[static_cast<size_t>(i)] * win * std::sin(w * i);
  }
  return std::sqrt(re * re + im * im) / (b - a);
}

PluginState base() { return defaultPluginState(pl::kType); }
void setP(PluginState& s, int k, float v) {
  if (static_cast<int>(s.params.size()) <= k) s.params.resize(static_cast<size_t>(k + 1), 0.f);
  s.params[static_cast<size_t>(k)] = v;
}
int tableIndex(const char* name) {
  const auto& n = pl::factoryTableNames();
  for (size_t i = 0; i < n.size(); ++i)
    if (n[i] == name) return static_cast<int>(i) + 1;
  return 1;
}
const std::vector<Ev> kHold = {{0, true, 1, 60, 0.8f}};
const std::vector<Ev> kPress = {{0, true, 1, 60, 0.8f}, {12000, false, 1, 60, 0}};

// ------------------------------------------------------------------ synthetic Serum 2 presets
// Minimal CBOR writer + zstd "raw block" frame writer: enough to produce a
// container the importer reads, from values chosen here.
struct Cbor {
  std::vector<uint8_t> b;
  void head(int mt, uint64_t v) {
    if (v < 24) b.push_back(static_cast<uint8_t>(mt << 5 | v));
    else if (v < 256) b.push_back(static_cast<uint8_t>(mt << 5 | 24)), b.push_back(static_cast<uint8_t>(v));
    else if (v < 65536) b.push_back(static_cast<uint8_t>(mt << 5 | 25)), b.push_back(static_cast<uint8_t>(v >> 8)), b.push_back(static_cast<uint8_t>(v));
    else {
      b.push_back(static_cast<uint8_t>(mt << 5 | 26));
      for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
    }
  }
  void str(const std::string& s) {
    head(3, s.size());
    b.insert(b.end(), s.begin(), s.end());
  }
  void num(double d) {
    if (d == std::floor(d) && std::fabs(d) < 1e9) {
      if (d >= 0) head(0, static_cast<uint64_t>(d));
      else head(1, static_cast<uint64_t>(-d - 1));
      return;
    }
    b.push_back(0xFB);
    uint64_t u;
    std::memcpy(&u, &d, 8);
    for (int s = 56; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(u >> s));
  }
  void map(size_t n) { head(5, n); }
  void arr(size_t n) { head(4, n); }
};
// Writes a gl::Json value as CBOR.
void cborJson(Cbor& c, const Json& j) {
  switch (j.type) {
    case Json::Type::Object:
      c.map(j.object.size());
      for (const auto& kv : j.object) {
        c.str(kv.first);
        cborJson(c, kv.second);
      }
      break;
    case Json::Type::Array:
      c.arr(j.array.size());
      for (const auto& v : j.array) cborJson(c, v);
      break;
    case Json::Type::String: c.str(j.str); break;
    case Json::Type::Number: c.num(j.number); break;
    case Json::Type::Bool: c.b.push_back(j.boolean ? 0xF5 : 0xF4); break;
    default: c.b.push_back(0xF6); break;
  }
}
// zstd test vector: Gridline's own pseudo-random word text (generated below),
// compressed with the reference zstd tool at level 19 (Huffman literals, FSE
// sequences, repeat offsets).
const uint8_t kZstdVec[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x68, 0x05, 0x0a, 0x00, 0x22, 0x86, 0x11, 0x10, 0xa0, 0xed, 0xf0, 0x6f, 0xb5, 0x90, 0x3e,
    0x2c, 0x39, 0xe1, 0xff, 0xff, 0x57, 0x87, 0x9e, 0x21, 0xb6, 0x6d, 0xdb, 0xf6, 0x05, 0xba, 0x5b, 0x67, 0xd7, 0x90, 0xed,
    0x2f, 0x61, 0x1e, 0x96, 0xaa, 0x3c, 0xcc, 0x16, 0xa0, 0xb9, 0x4d, 0x5c, 0xb1, 0x68, 0xb6, 0x79, 0xdf, 0xd4, 0x10, 0xb0,
    0xa9, 0x0e, 0xe5, 0x16, 0x70, 0x95, 0xf4, 0x0f, 0x1d, 0xa4, 0x15, 0x79, 0xc0, 0x91, 0x51, 0x6f, 0x6a, 0xa9, 0x6a, 0x04,
    0x4b, 0x01, 0x80, 0x86, 0xa8, 0x71, 0x5b, 0x52, 0xa8, 0x4c, 0x7b, 0x07, 0x20, 0x42, 0xa3, 0x94, 0x74, 0x1e, 0x21, 0x0c,
    0x01, 0x8b, 0x68, 0x02, 0x4f, 0x4c, 0x4c, 0x54, 0x2b, 0x85, 0x65, 0x0c, 0xd1, 0x89, 0xd0, 0xb0, 0xc4, 0x09, 0xa7, 0x10,
    0x1a, 0xb2, 0x67, 0x9a, 0x73, 0xfa, 0x39, 0xaa, 0x31, 0x1b, 0xa0, 0x6b, 0xb1, 0x57, 0x4b, 0xd7, 0x6e, 0x3b, 0xca, 0x1c,
    0xfc, 0xfa, 0xd2, 0xcd, 0xcd, 0x08, 0x4c, 0x9d, 0xdc, 0x86, 0xb1, 0x6b, 0xe4, 0xda, 0x81, 0x44, 0xb8, 0xa5, 0xa9, 0x6d,
    0xd1, 0xc1, 0xf4, 0x87, 0x37, 0x44, 0x9b, 0xf9, 0xf4, 0x7a, 0x0b, 0x3e, 0xb1, 0x90, 0xdc, 0xde, 0x3f, 0x10, 0x8e, 0x34,
    0x94, 0xaf, 0xb2, 0xb4, 0xac, 0x6e, 0x67, 0xe0, 0xc5, 0x1d, 0x75, 0xca, 0x5d, 0x96, 0x1c, 0x92, 0x93, 0xc4, 0x14, 0x16,
    0x39, 0xba, 0xa4, 0x68, 0xbd, 0x0a, 0x7c, 0x74, 0xbb, 0xee, 0xdc, 0x76, 0xe9, 0xa9, 0x92, 0x52, 0x0d, 0xa9, 0x41, 0x37,
    0xd2, 0xa2, 0x1f, 0xc6, 0x14, 0xc9, 0x56, 0xae, 0x91, 0xe3, 0x70, 0xb1, 0xad, 0x25, 0x3d, 0xc1, 0xb1, 0xa8, 0x19, 0x88,
    0xce, 0x17, 0x05, 0x7b, 0x89, 0x78, 0xb2, 0x34, 0x93, 0x6e, 0x31, 0x4f, 0x22, 0xd1, 0xda, 0x00, 0x70, 0x31, 0x5f, 0xa8,
    0xd1, 0xdd, 0x01, 0x69, 0x66, 0xe5, 0x10, 0xda, 0x30, 0x26, 0x36, 0xb9, 0x01, 0xa5, 0x89, 0x84, 0xa4, 0x19, 0x4a, 0xe2,
    0xb5, 0x39, 0x30, 0x62, 0x5b, 0x54, 0x7c, 0x23, 0xda, 0x34, 0xea, 0x25, 0x02, 0x9b, 0xd2, 0xf0, 0x51, 0x59, 0xa3, 0xd1,
    0x95, 0x9e, 0xc6, 0x7e, 0x97, 0xba, 0xfe, 0x6f, 0xa3, 0x5c, 0xb5, 0x0a, 0x76, 0x39, 0xe5, 0x9d, 0xe4, 0x16, 0x82, 0xca,
    0xbd, 0x14, 0x75, 0xa1, 0x85, 0xcc, 0x60, 0xa9, 0x02, 0xe4, 0x80, 0xa2, 0x31,
};
std::string zstdVecText() {
  static const char* const w[] = {"plasma", "grid", "line", "wavetable", "oscillator", "filter", "envelope", "matrix", "macro", "sequence", "spectral", "granular"};
  std::string t;
  uint64_t seed = 7;
  for (int i = 0; t.size() < 1500; ++i) {
    seed = (seed * 1103515245u + 12345u) % 2147483648u;
    t += w[(seed >> 16) % 12];
    t += i % 9 ? ' ' : '\n';
  }
  return t;
}

std::vector<uint8_t> zstdRaw(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> f = {0x28, 0xB5, 0x2F, 0xFD};
  f.push_back(0xA0);  // frame header: FCS_flag = 2 (4-byte content size), single segment
  const uint32_t n = static_cast<uint32_t>(data.size());
  for (int s = 0; s < 32; s += 8) f.push_back(static_cast<uint8_t>(n >> s));
  size_t pos = 0;
  do {
    const size_t len = std::min<size_t>(data.size() - pos, 100000);
    const bool last = pos + len >= data.size();
    const uint32_t hdr = (static_cast<uint32_t>(len) << 3) | (0u << 1) | (last ? 1u : 0u);  // raw block
    f.push_back(static_cast<uint8_t>(hdr));
    f.push_back(static_cast<uint8_t>(hdr >> 8));
    f.push_back(static_cast<uint8_t>(hdr >> 16));
    f.insert(f.end(), data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + len));
    pos += len;
  } while (pos < data.size());
  return f;
}
std::vector<uint8_t> container(const Json& body, const std::string& name) {
  Cbor c;
  cborJson(c, body);
  std::string meta = "{\"fileType\":\"SerumPreset\",\"presetName\":\"" + name + "\",\"presetAuthor\":\"Test\",\"product\":\"Serum2\",\"version\":1}";
  std::vector<uint8_t> out = {'X', 'f', 'e', 'r', 'J', 's', 'o', 'n', 0};
  const uint64_t ml = meta.size();
  for (int s = 0; s < 64; s += 8) out.push_back(static_cast<uint8_t>(ml >> s));
  out.insert(out.end(), meta.begin(), meta.end());
  const uint32_t un = static_cast<uint32_t>(c.b.size());
  for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(un >> s));
  for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(2u >> s));
  auto z = zstdRaw(c.b);
  out.insert(out.end(), z.begin(), z.end());
  return out;
}
Json obj(std::initializer_list<std::pair<const char*, Json>> kv) {
  Json j = Json::makeObject();
  for (const auto& p : kv) j.object[p.first] = p.second;
  return j;
}
Json num(double d) { return Json::makeNumber(d); }
Json str(const char* s) { return Json::makeString(s); }

// ------------------------------------------------------------------ parameter coverage
// Every parameter changes the output under at least one setting that a user
// would make (a source routed through the matrix, an oscillator type with a
// synthetic sample, an effect module, a legato note...). The param audit's
// VERIFIED entries for gl.plasma (engine/ParamAudit.cpp) rest on this test.
namespace cov {

struct Scene {
  PluginState st;
  pl::Model m;
  bool mpe = false;  // send per-channel bend / slide on channel 2
};
struct Cand {
  std::function<void(Scene&)> f;
  std::vector<float> extra;  // values tried before the generic ones
  int group = 0;             // oscillator settings: tried first for the parameters of that group
};

std::string gWav, gShort, gSfz;

void writeWav(const std::string& path, int frames, uint32_t seed) {
  std::vector<int16_t> pcm(static_cast<size_t>(frames));
  double ph = 0;
  for (int i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / frames;
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    const double nz = static_cast<double>(static_cast<int32_t>(seed)) / 2147483648.0;
    ph += (110.0 + 330.0 * t) / kSr;
    const double saw = 2.0 * (ph - std::floor(ph)) - 1.0;
    const double gate = std::fmod(t * 12, 1.0) < 0.5 ? 1.0 : 0.1;  // bursts: onsets for the spectral / granular modes
    const double env = std::sin(3.14159265358979 * t) * (0.6 + 0.4 * std::sin(40 * t)) * gate;
    pcm[static_cast<size_t>(i)] = static_cast<int16_t>(std::lround(std::clamp((0.7 * saw + 0.3 * nz) * env, -1.0, 1.0) * 30000));
  }
  std::ofstream f(path, std::ios::binary);
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  f.write("RIFF", 4), u32(36 + static_cast<uint32_t>(frames) * 2), f.write("WAVE", 4);
  f.write("fmt ", 4), u32(16), u16(1), u16(1), u32(48000), u32(96000), u16(2), u16(16);
  f.write("data", 4), u32(static_cast<uint32_t>(frames) * 2);
  f.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
}

constexpr int kTotal = 26000;

Out renderScene(const Scene& sc) {
  PluginState st = sc.st;
  st.data = pl::modelToData(sc.m);
  Out o;
  o.L.assign(kTotal, 0.f);
  o.R.assign(kTotal, 0.f);
  auto g = packs::createGenerator(st.type);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  g->prepareHost(st, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = 100;  // not 120: tempo-synced times differ from their free values
  ctx.ppq = 96;
  ctx.playing = true;
  NoteProps np;
  np.pan = 0.3f, np.modX = 0.3f, np.modY = 0.7f, np.color = 2;
  constexpr int kBlockC = 256;
  for (int pos = 0; pos < kTotal; pos += kBlockC) {
    const int n = std::min(kBlockC, kTotal - pos);
    ctx.tick = pos / kSr * (ctx.tempo / 60.0) * ctx.ppq;
    auto at = [&](int s) { return s >= pos && s < pos + n; };
    if (at(0)) g->noteOn(1, 60, 0.8f, 0, np);
    if (at(6000)) g->noteOn(2, 67, 0.6f, 6000 - pos);
    if (at(3000) && sc.mpe) {
      g->midi(0xE2, 0, 100, 3000 - pos);  // channel 2 bend up
      g->midi(0xB2, 74, 110, 3000 - pos); // channel 2 slide
    }
    if (at(14000)) g->noteOff(1, 14000 - pos);
    if (at(20000)) g->noteOff(2, 20000 - pos);
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
  }
  for (size_t i = 0; i < o.L.size(); ++i) {
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) o.finite = false;
    o.peak = std::max({o.peak, std::fabs(o.L[i]), std::fabs(o.R[i])});
  }
  return o;
}
double maxDiff(const Out& a, const Out& b) {
  double d = 0;
  for (size_t i = 0; i < a.L.size(); ++i) d = std::max({d, static_cast<double>(std::fabs(a.L[i] - b.L[i])), static_cast<double>(std::fabs(a.R[i] - b.R[i]))});
  return d;
}

void P(Scene& s, int k, float v) { setP(s.st, k, v); }
// Shows a modulation source as pitch: matrix slot -> osc A coarse.
void observe(Scene& s, int slot, int src, float amt = 0.5f) {
  P(s, pl::modP(slot, pl::M_Source), static_cast<float>(src));
  P(s, pl::modP(slot, pl::M_Amount), amt);
  P(s, pl::modP(slot, pl::M_Dest), static_cast<float>(pl::oscP(0, pl::O_Coarse) + 1));
}
void lfoBase(Scene& s, int l) {
  P(s, pl::lfoP(l, pl::L_Mode), pl::LfoRetrig);
  P(s, pl::lfoP(l, pl::L_Bpm), 0);
  P(s, pl::lfoP(l, pl::L_Rate), 0.4f);
}
int filterIndex(const char* name) {
  const auto& n = pl::filterNames();
  for (size_t i = 0; i < n.size(); ++i)
    if (n[i] == name) return static_cast<int>(i);
  return 0;
}
void twoFilters(Scene& s) {
  P(s, pl::filtP(0, pl::F_Enable), 1), P(s, pl::filtP(0, pl::F_Type), static_cast<float>(filterIndex("Low 24"))), P(s, pl::filtP(0, pl::F_Freq), 0.3f);
  P(s, pl::filtP(1, pl::F_Enable), 1), P(s, pl::filtP(1, pl::F_Type), static_cast<float>(filterIndex("High 24"))), P(s, pl::filtP(1, pl::F_Freq), 0.6f);
}
void oscOn(Scene& s, int o) {
  P(s, pl::oscP(o, pl::O_Enable), 1);
  P(s, pl::oscP(o, pl::O_Level), 0.8f);
}
void oscType(Scene& s, int o, int type) {
  oscOn(s, o);
  P(s, pl::oscP(o, pl::O_Type), static_cast<float>(type));
  if (type == pl::OscMultisample) s.m.osc[static_cast<size_t>(o)].sfz = gSfz;
  else if (type != pl::OscWavetable) s.m.osc[static_cast<size_t>(o)].sample = gWav;
}
void arpOn(Scene& s, int variant) {
  P(s, pl::G_ArpOn, 1);
  auto& a = s.m.arp[0];
  a.shape = pl::ArpUp, a.shift = 2, a.range = 2, a.velo = true, a.retrigRate = true, a.retrigDiv = 3;
  P(s, pl::G_Swing, 0.7f);
  observe(s, 0, pl::SrcVelo);  // arp velocities show as pitch
  if (variant >= 1) {
    a.shape = pl::ArpPattern, a.wrap = variant == 1 ? 1 : 3, a.rangeWrap = 1, a.range = 1;
    a.steps.clear();
    for (int i = 0; i < 8; ++i) a.steps.push_back({i * 0.25f, 0.2f, i % 5, 0.5f + 0.06f * i, i == 3, 0});
    a.patternBeats = 2;
  }
}
void clipOn(Scene& s) {
  P(s, pl::G_ClipOn, 1);
  auto& c = s.m.clip[0];
  c.notes = {{0, 0.2f, 60, 0.8f, false}, {0.25f, 0.2f, 64, 0.7f, false}, {0.5f, 0.2f, 67, 0.8f, false}, {0.75f, 0.2f, 72, 0.6f, false}};
  c.lengthBeats = 1;
  c.span = 2;
}
void legato(Scene& s) { P(s, pl::G_Mono, 1), P(s, pl::G_Legato, 1); }

// Candidate settings of each section (tried in order until the parameter acts).
std::vector<Cand> candsFor(int k) {
  using namespace pl;
  std::vector<Cand> c;
  int grp = 0;
  auto add = [&](std::function<void(Scene&)> f, std::vector<float> extra = {}) { c.push_back({std::move(f), std::move(extra), grp}); };
  if (k == tailP(T_NoiseTone)) {  // the noise tone filter: a built-in noise, heard
    add([](Scene& s) { P(s, noiseP(N_Enable), 1), P(s, noiseP(N_Level), 0.8f), P(s, noiseP(N_Type), 1); });
    return c;
  }
  if (k == tailP(T_UniMix)) {  // the unison voice mix: a detuned unison on oscillator A
    add([](Scene& s) { oscOn(s, 0), P(s, oscP(0, O_Unison), 5), P(s, oscP(0, O_UniDetune), 0.5f); });
    return c;
  }
  if (k == tailP(T_DriveMode)) {  // the filter drive mode: a driven filter 1 on oscillator A
    add([](Scene& s) {
      P(s, filtP(0, F_Enable), 1), P(s, filtP(0, F_Freq), 0.5f), P(s, filtP(0, F_Drive), 0.6f);
      P(s, routeP(RtA, R_Dest), RouteFilter), P(s, routeP(RtA, R_Balance), 0.f);
    });
    return c;
  }
  if (k < G_Count) {
    add([](Scene&) {});
    add([](Scene& s) { P(s, G_ArpOn, 1); });
    add([](Scene& s) { P(s, G_PitchWheel, 0.5f); });
    add([](Scene& s) { P(s, G_Poly, 1); });
    add([](Scene& s) { twoFilters(s), P(s, routeP(RtA, R_Dest), RouteFilter), P(s, G_RndCutoff, 1); });
    add([](Scene& s) { legato(s), P(s, G_PortaAlways, 1), P(s, G_PortaTime, 0.6f); });
    add([](Scene& s) { P(s, G_PortaAlways, 1), P(s, G_PortaTime, 0.6f); });
    add([](Scene& s) { P(s, G_Mono, 1), P(s, G_PortaTime, 0.6f); });
    add([](Scene& s) { P(s, G_PitchWheel, -0.5f); });
    add([](Scene& s) { observe(s, 0, SrcModWheel); });
    add([](Scene& s) { arpOn(s, 0); });
    add([](Scene& s) { arpOn(s, 1); });
    add([](Scene& s) { arpOn(s, 2); });
    add([](Scene& s) { clipOn(s); });
    add([](Scene& s) { P(s, routeP(RtA, R_Dest), RouteDirect); });
    add([](Scene& s) { P(s, routeP(RtA, R_Bus1), 0.7f), P(s, routeP(RtA, R_Bus2), 0.7f), P(s, G_MasterVol, 0.4f), P(s, G_DirectVol, 0.8f); });
    add([](Scene& s) { P(s, oscP(0, O_PitchMode), 1), P(s, oscP(0, O_Semi), 3); });
    add([](Scene& s) { s.mpe = true, P(s, G_MpeOn, 1), observe(s, 0, SrcExprY); });
    add([](Scene& s) { lfoBase(s, 0), observe(s, 0, SrcLfo1), P(s, G_LfoScale, 2); });
    add([](Scene& s) {
      for (int o = 0; o < kOscs; ++o) oscOn(s, o);
      P(s, subP(S_Enable), 1), P(s, subP(S_Level), 0.8f), P(s, noiseP(N_Enable), 1), P(s, noiseP(N_Level), 0.8f);
      P(s, G_RndPan, 1), P(s, G_RndDetune, 20);
      for (int i = 0; i < 8; ++i) s.m.voiceSeq[static_cast<size_t>(i)] = {i & 1 ? 0.8f : -0.8f, 0.1f * i, 0, 0, 0, 0};
    });
    add([](Scene& s) { legato(s); });
    return c;
  }
  if (k < kSubBase) {
    const int o = (k - kOscBase) / O_Count;
    add([o](Scene& s) { oscOn(s, o); });
    add([o](Scene& s) { oscOn(s, o), P(s, oscP(o, O_Unison), 4), P(s, oscP(o, O_UniDetune), 0.3f); });
    add([o](Scene& s) { oscOn(s, o), P(s, oscP(o, O_Semi), 3), P(s, oscP(o, O_Ratio), 2.6f), P(s, oscP(o, O_PitchMode), 3), P(s, G_Scale, 2); });
    add([o](Scene& s) { oscOn(s, o), P(s, oscP(o, O_Table), static_cast<float>(tableIndex("Analog Morph"))), P(s, oscP(o, O_WtPos), 0.37f); });
    grp = 1;
    for (int mode : {1, 2, 3, 4, 6, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80, 90, 100})
      add([o, mode](Scene& s) {
        oscOn(s, o), P(s, oscP(o, O_Unison), 4), P(s, oscP(o, O_UniDetune), 0.3f);
        P(s, oscP(o, O_Warp1Mode), static_cast<float>(mode)), P(s, oscP(o, O_Warp1), 0.5f);
        P(s, oscP(o, O_Warp2Mode), static_cast<float>(mode)), P(s, oscP(o, O_Warp2), 0.5f);
        oscOn(s, (o + 1) % kOscs);  // FM / PD / AM / RM sources
      });
    grp = 2;
    add([o](Scene& s) {
      oscType(s, o, OscSample), P(s, oscP(o, O_LoopMode), 1);
      s.m.osc[static_cast<size_t>(o)].slices = {0, 0.25f, 0.5f, 0.75f};
    });
    add([o](Scene& s) { oscType(s, o, OscSample), P(s, oscP(o, O_ScanSync), 1); });
    add([o](Scene& s) { oscType(s, o, OscSample), P(s, oscP(o, O_MEnvOn), 1); });
    add([o](Scene& s) {
      oscType(s, o, OscSample), P(s, oscP(o, O_MEnvOn), 1), P(s, oscP(o, O_MSustain), 0.3f), P(s, oscP(o, O_MDecay), 0.3f), P(s, oscP(o, O_MAttack), 0.2f);
    });
    add([o](Scene& s) {
      oscType(s, o, OscSample), P(s, oscP(o, O_LoopMode), 1), P(s, oscP(o, O_Start), 0.3f), P(s, oscP(o, O_LoopStart), 0.1f), P(s, oscP(o, O_LoopEnd), 0.2f);
    });
    grp = 3;
    for (int t : {OscGranular, OscSpectral})
      for (int v = 0; v < 9; ++v)
        add([o, t, v](Scene& s) {
          oscType(s, o, t);
          if (v == 1) P(s, oscP(o, O_Unison), 4), P(s, oscP(o, O_UniDetune), 0.3f);
          if (v == 2) P(s, oscP(o, O_Warp1Mode), 20), P(s, oscP(o, O_Warp1), 0.5f), P(s, oscP(o, O_Warp2Mode), 21), P(s, oscP(o, O_Warp2), 0.5f);
          if (v == 3) P(s, oscP(o, O_GDensMode), 1), P(s, oscP(o, O_GLenMode), 1);
          if (v == 4) P(s, oscP(o, O_GDensMode), 2), P(s, oscP(o, O_GLenMode), 2);
          if (v == 5) P(s, oscP(o, O_ScanSync), 1), P(s, oscP(o, O_ScanRate), 1);
          if (v >= 6) P(s, oscP(o, O_GWinShape), static_cast<float>(v - 4)), P(s, oscP(o, O_Unison), 3), P(s, oscP(o, O_SCut), 0.4f);
          if (v >= 6) P(s, oscP(o, O_SLo), 0.02f), P(s, oscP(o, O_SHi), 0.1f);
        });
    grp = 4;
    for (int wm : {58, 59, 64, 65})  // key zone: the second note (G4) is outside; another oscillator warps from this one
      add([o, wm](Scene& s) {
        oscOn(s, o), P(s, oscP(o, O_KeyMax), 63);
        const int w = (o + 1) % kOscs;
        oscOn(s, w), P(s, oscP(w, O_Warp1Mode), static_cast<float>(wm)), P(s, oscP(w, O_Warp1), 1);
        oscOn(s, (o + 2) % kOscs), P(s, oscP((o + 2) % kOscs, O_Warp2Mode), static_cast<float>(wm)), P(s, oscP((o + 2) % kOscs, O_Warp2), 1);
      });
    grp = 5;
    add([o](Scene& s) { oscType(s, o, OscMultisample), P(s, oscP(o, O_MEnvOn), 1); });
    add([o](Scene& s) { oscType(s, o, OscMultisample), P(s, oscP(o, O_MEnvOn), 1), P(s, oscP(o, O_Unison), 4); });
    const int p = (k - kOscBase) % O_Count;
    const int want = (p >= O_UniWarp1 && p <= O_UniWarp2) || (p >= O_Warp1Mode && p <= O_Warp2Var) ? 1
                     : p >= O_Start && p <= O_SliceMode                                          ? 2
                     : p >= O_GDensity && p <= O_STransients                                     ? 3
                     : p >= O_KeyMin                                                             ? 4
                     : p >= O_MTimbre                                                            ? 5
                                                                                                 : 0;
    std::stable_partition(c.begin(), c.end(), [want](const Cand& x) { return x.group == want; });
    return c;
  }
  if (k < kNoiseBase) {
    add([](Scene& s) { P(s, subP(S_Enable), 1), P(s, subP(S_Level), 0.8f); });
    add([](Scene& s) { P(s, subP(S_Enable), 1), P(s, subP(S_Level), 0.8f), legato(s); });
    add([](Scene& s) { P(s, subP(S_Enable), 1), P(s, subP(S_Level), 0.8f), P(s, subP(S_Phase), 0.3f); });
    return c;
  }
  if (k < kFiltBase) {
    add([](Scene& s) { P(s, noiseP(N_Enable), 1), P(s, noiseP(N_Level), 0.8f); });
    add([](Scene& s) { P(s, noiseP(N_Enable), 1), P(s, noiseP(N_Level), 0.8f), P(s, noiseP(N_Type), 0), s.m.noiseFile = gShort; });
    for (int t = 2; t < 20; ++t) add([t](Scene& s) { P(s, noiseP(N_Enable), 1), P(s, noiseP(N_Level), 0.8f), P(s, noiseP(N_Type), static_cast<float>(t)); });
    return c;
  }
  if (k < kRouteBase) {
    const int f = (k - kFiltBase) / F_Count;
    for (int t = 0; t < static_cast<int>(filterNames().size()); ++t)
      add([f, t](Scene& s) {
        P(s, filtP(f, F_Enable), 1), P(s, filtP(f, F_Type), static_cast<float>(t)), P(s, filtP(f, F_Freq), 0.5f), P(s, filtP(f, F_Res), 0.5f);
        P(s, routeP(RtA, R_Dest), RouteFilter), P(s, routeP(RtA, R_Balance), f == 0 ? 0.f : 1.f);
        P(s, filtP(f, F_X), 0.3f), P(s, filtP(f, F_Y), 0.7f);
      });
    return c;
  }
  if (k < kEnvBase) {
    const int r = (k - kRouteBase) / R_Count;
    auto src = [r](Scene& s) {
      twoFilters(s);
      if (r <= RtC) oscOn(s, r);
      if (r == RtNoise) P(s, noiseP(N_Enable), 1), P(s, noiseP(N_Level), 0.8f);
      if (r == RtSub) P(s, subP(S_Enable), 1), P(s, subP(S_Level), 0.8f);
      if (r >= RtF1) P(s, routeP(RtA, R_Dest), RouteFilter), P(s, routeP(RtA, R_Balance), r == RtF1 ? 0.f : 1.f);
      else P(s, routeP(r, R_Dest), RouteFilter), P(s, routeP(r, R_Balance), 0.5f);
    };
    add(src);
    add([src, r](Scene& s) { src(s), P(s, routeP(r, R_Bus1), 0.7f), P(s, routeP(r, R_Bus2), 0.7f); });
    add([src, r](Scene& s) { src(s), P(s, routeP(r, R_Dest), RouteMain); });
    return c;
  }
  if (k < kLfoBase) {
    const int e = (k - kEnvBase) / E_Count;
    auto env = [e](Scene& s) {
      observe(s, 0, SrcEnv1 + e);
      P(s, envP(e, E_Attack), 0.4f), P(s, envP(e, E_Hold), 0.3f), P(s, envP(e, E_Decay), 0.4f), P(s, envP(e, E_Sustain), 0.3f), P(s, envP(e, E_Release), 0.4f);
    };
    add(env);
    add([env, e](Scene& s) { env(s), P(s, envP(e, E_Bpm), 1); });
    add([env](Scene& s) { env(s), legato(s); });
    add([env](Scene& s) { env(s), P(s, G_Poly, 1); });
    add([env](Scene& s) { env(s), P(s, G_Mono, 1); });
    return c;
  }
  if (k < kMacroBase) {
    const int l = (k - kLfoBase) / L_Count;
    auto lfo = [l](Scene& s) { lfoBase(s, l), observe(s, 0, SrcLfo1 + l); };
    add(lfo);
    add([lfo, l](Scene& s) { lfo(s), P(s, lfoP(l, L_Bpm), 1); });
    add([lfo, l](Scene& s) { lfo(s), P(s, lfoP(l, L_Phase), 0.3f), P(s, lfoP(l, L_PhaseSnap), 1), P(s, lfoP(l, L_GridX), 4); });
    add([lfo, l](Scene& s) { lfo(s), P(s, lfoP(l, L_Swing), 1), P(s, G_Swing, 0.75f); });
    add([lfo, l](Scene& s) { lfo(s), P(s, lfoP(l, L_Mode), LfoEnvelope); });
    add([lfo, l](Scene& s) { lfo(s), P(s, lfoP(l, L_Delay), 0.3f), P(s, lfoP(l, L_Rise), 0.3f); });
    add([lfo, l](Scene& s) { lfo(s), P(s, lfoP(l, L_Mode), LfoFree); });
    for (int sh = 1; sh < static_cast<int>(lfoShapeNames().size()); sh += 4) add([lfo, l, sh](Scene& s) { lfo(s), P(s, lfoP(l, L_Shape), static_cast<float>(sh)); });
    for (int t = 1; t < LfoTypeCount; ++t) add([lfo, l, t](Scene& s) { lfo(s), P(s, lfoP(l, L_Type), static_cast<float>(t)); });
    return c;
  }
  if (k < kBusBase) {
    const int m = k - kMacroBase;
    add([m](Scene& s) { observe(s, 0, SrcMacro1 + m); });
    return c;
  }
  if (k < kRetrigBase) {
    const int b = k - kBusBase;
    add([b](Scene& s) {
      lfoBase(s, 0), observe(s, 0, SrcLfo1), P(s, lfoP(0, L_Shape), 0);
      s.m.lfo[0].pts = {{0, 0, 0}, {0.5f, 0.5f, 0}, {1, 0, 0}};
      s.m.busLinks = {{0, 1, 1, b, 1.f}};
    });
    return c;
  }
  if (k < kModBase) {
    add([](Scene& s) {
      legato(s);
      for (int o = 0; o < kOscs; ++o) oscOn(s, o), P(s, oscP(o, O_Phase), 0.25f), P(s, oscP(o, O_PhaseMemory), 1);
      P(s, subP(S_Enable), 1), P(s, subP(S_Level), 0.8f), P(s, subP(S_Phase), 0.25f), P(s, subP(S_ContPhase), 1);
      P(s, noiseP(N_Enable), 1), P(s, noiseP(N_Level), 0.8f), P(s, noiseP(N_Type), 0), s.m.noiseFile = gShort;
      for (int e = 1; e < kEnvs; ++e) observe(s, e - 1, SrcEnv1 + e, 0.3f), P(s, envP(e, E_Attack), 0.2f), P(s, envP(e, E_Decay), 0.4f), P(s, envP(e, E_Sustain), 0.3f);
      for (int l = 0; l < kLfos; ++l) lfoBase(s, l), observe(s, 3 + l, SrcLfo1 + l, 0.3f), P(s, lfoP(l, L_Phase), 0.25f);
    });
    return c;
  }
  if (k < kFxBase) {
    const int sl = (k - kModBase) / M_Count, p = (k - kModBase) % M_Count;
    std::vector<float> extra;
    if (p == M_Source) extra = {static_cast<float>(SrcLfo1)};
    if (p == M_Dest) extra = {static_cast<float>(oscP(0, O_Coarse) + 1)};
    if (p == M_Aux) extra = {static_cast<float>(SrcMacro1)};
    add([sl](Scene& s) {
      lfoBase(s, 0), observe(s, sl, SrcLfo1), P(s, lfoP(0, L_Shape), 5);  // square: slew shows
      P(s, modP(sl, M_Aux), SrcMacro1), P(s, macroP(0), 0.4f);
    }, extra);
    add([sl](Scene& s) {
      lfoBase(s, 0), observe(s, sl, SrcLfo1), P(s, lfoP(0, L_Shape), 1);  // triangle: the curve shows
      P(s, modP(sl, M_Aux), SrcMacro1), P(s, macroP(0), 0.4f);
    }, extra);
    return c;
  }
  const int sl = (k - kFxBase) / X_Count, p = (k - kFxBase) % X_Count;
  for (int t = 1; t < FxSplitLH; ++t) {
    if (p >= X_P0 && p - X_P0 >= static_cast<int>(fxParams(t).size())) continue;
    for (int sw = 0; sw < (t == FxComp ? 4 : 3); ++sw)  // defaults, every switch on, every switch off (+ a hard multiband compressor)
      add([sl, t, sw](Scene& s) {
        P(s, fxP(sl, X_Type), static_cast<float>(t)), P(s, fxP(sl, X_On), 1);
        for (int q = 0; q < static_cast<int>(fxParams(t).size()); ++q) {
          float v = fxDefaultNorm(t, q);
          if (sw > 0 && fxParams(t)[static_cast<size_t>(q)].kind == 1) v = sw == 1 ? 1.f : 0.f;
          if (sw == 1 && fxParams(t)[static_cast<size_t>(q)].kind == 0 && std::string(fxParams(t)[static_cast<size_t>(q)].unit) == "beats") v = 0.3f;
          if (sw == 3) {
            const std::string key = fxParams(t)[static_cast<size_t>(q)].key;
            if (key == "kParamMultiband") v = 1;
            if (key == "kParamThresh") v = 0.8f;  // -48 dB
            if (key.rfind("kParamDeadband", 0) == 0) v = 0;
          }
          P(s, fxP(sl, X_P0 + q), v);
        }
      });
  }
  return c;
}

std::vector<float> valuesFor(const ParamDesc& d, const std::vector<float>& extra) {
  std::vector<float> v = {static_cast<float>(d.def)};
  for (float e : extra) v.push_back(e);
  const double hi = d.kind == ParamKind::Choice && !d.choices.empty() ? static_cast<double>(d.choices.size() - 1) : d.max;
  for (double x : {d.max, d.min, (d.min + hi) * 0.5, d.min + (hi - d.min) * 0.25, d.min + (hi - d.min) * 0.75, d.min + (hi - d.min) * 0.56}) v.push_back(static_cast<float>(x));
  if (d.kind == ParamKind::Choice)
    for (int i = 0; i <= std::min(24, static_cast<int>(hi)); ++i) v.push_back(static_cast<float>(i));
  std::vector<float> u;
  for (float x : v) {
    x = static_cast<float>(d.clamp(x));
    if (std::find(u.begin(), u.end(), x) == u.end()) u.push_back(x);
  }
  return u;
}

// Reserved or editor-only parameters (documented as inert in engine/ParamAudit.cpp).
bool reserved(const std::string& name) {
  return name == "Legacy Warp Mode" || name == "MIDI Out" || name == "Filter 1 Filter Balance" || name == "Filter 2 Filter Balance" || (name.rfind("LFO ", 0) == 0 && name.size() > 7 && name.compare(name.size() - 7, 7, " Grid Y") == 0) ||
         (name.rfind("FX ", 0) == 0 && name.size() > 9 && name.compare(name.size() - 9, 9, " Param 28") == 0);
}

// Repeated slots run the same code: oscillator A, the first and last slot of the effects, the
// matrix and the LFOs are checked in full, the others on their main controls (PLASMA_COV_FULL=1:
// every one, about three minutes).
bool representative(int k) {
  using namespace pl;
  if (k >= kTailBase) return true;
  if (k >= kFxBase) {
    const int sl = (k - kFxBase) / X_Count, p = (k - kFxBase) % X_Count;
    return sl == 0 || sl == kFxSlots - 1 || p <= X_P0;
  }
  if (k >= kModBase) {
    const int sl = (k - kModBase) / M_Count, p = (k - kModBase) % M_Count;
    return sl == 0 || sl == kModSlots - 1 || p == M_Source || p == M_Amount || p == M_Dest;
  }
  if (k >= kOscBase && k < kSubBase) {  // osc A in full; B and C: their own switches and the zone
    const int o = (k - kOscBase) / O_Count, p = (k - kOscBase) % O_Count;
    return o == 0 || p == O_Enable || p == O_Type || p == O_Level || p == O_Warp1Mode || p == O_Warp2Mode || p >= O_KeyMin;
  }
  if (k >= kLfoBase && k < kMacroBase) {
    const int l = (k - kLfoBase) / L_Count, p = (k - kLfoBase) % L_Count;
    return l == 0 || l == kLfos - 1 || p == L_Rate || p == L_Shape || p == L_Mode;
  }
  return true;
}

void run(const PluginInfo& info) {
  const bool full = std::getenv("PLASMA_COV_FULL") != nullptr;
  const auto dir = std::filesystem::temp_directory_path() / ("gl_plasma_cov_" + std::to_string(reinterpret_cast<uintptr_t>(&info) & 0xFFFFFF));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  gWav = (dir / "tone.wav").string(), gShort = (dir / "short.wav").string(), gSfz = (dir / "keys.sfz").string();
  writeWav(gWav, 48000, 0x1234567u);
  writeWav(gShort, 4800, 0x7654321u);
  {
    std::ofstream f(gSfz);
    f << "<region> sample=tone.wav lokey=0 hikey=63 pitch_keycenter=60\n<region> sample=short.wav lokey=64 hikey=127 pitch_keycenter=67\n";
  }
  Scene base0;
  base0.st = base();
  base0.m = pl::defaultModel();
  setP(base0.st, pl::G_Quality, 0);  // Draft: the coverage renders are many
  {  // the renders repeat exactly (random sources are seeded), so a difference is the parameter's
    int same = 0;
    for (const auto& cand : {candsFor(pl::oscP(0, pl::O_GDensity))[1], candsFor(pl::G_RndPan)[19], candsFor(pl::noiseP(pl::N_Type))[1]}) {
      Scene s = base0;
      cand.f(s);
      same += maxDiff(renderScene(s), renderScene(s)) == 0;
    }
    expect(same == 3, "coverage renders repeat exactly");
  }
  const int n = static_cast<int>(info.params.size());
  std::vector<int> result(static_cast<size_t>(n), -2);  // -2 untested, -1 no effect, else the candidate that showed it
  std::atomic<int> next{0};
  std::atomic<long> renders{0};
  auto work = [&] {
    for (int k; (k = next.fetch_add(1)) < n;) {
      const ParamDesc& d = info.params[static_cast<size_t>(k)];
      if (reserved(d.name) || (!full && !representative(k))) continue;
      if (const char* only = std::getenv("PLASMA_COV_ONLY"); only && d.name.find(only) == std::string::npos) continue;
      result[static_cast<size_t>(k)] = -1;
      const auto cands = candsFor(k);
      const long r0 = renders.load();
      for (size_t ci = 0; ci < cands.size() && result[static_cast<size_t>(k)] < 0; ++ci) {
        Scene s = base0;
        cands[ci].f(s);
        const auto vals = valuesFor(d, cands[ci].extra);
        P(s, k, vals[0]);
        const Out ref = renderScene(s);
        ++renders;
        for (size_t vi = 1; vi < vals.size(); ++vi) {
          P(s, k, vals[vi]);
          ++renders;
          if (maxDiff(ref, renderScene(s)) > 1e-5) {
            result[static_cast<size_t>(k)] = static_cast<int>(ci);
            break;
          }
        }
      }
      if (std::getenv("PLASMA_COV_STATS")) std::printf("# stat %d\t%s\t%d\t%ld\n", k, d.name.c_str(), result[static_cast<size_t>(k)], renders.load() - r0);
    }
  };
  const unsigned nt = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < nt; ++t) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
  int dead = 0, tested = 0;
  for (int k = 0; k < n; ++k) {
    if (result[static_cast<size_t>(k)] == -2) continue;
    ++tested;
    if (result[static_cast<size_t>(k)] >= 0) continue;
    if (dead++ < (std::getenv("PLASMA_COV_ALL") ? 100000 : 40)) std::printf("# no effect: #%d %s\n", k, info.params[static_cast<size_t>(k)].name.c_str());
  }
  std::printf("# parameter coverage: %d of %d tested, %d without effect, %ld renders\n", tested, n, dead, renders.load());
  expect(dead == 0, "every parameter changes the sound under a setting that uses it");
  std::filesystem::remove_all(dir, ec);
}

}  // namespace cov

}  // namespace

void* operator new(std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
  // ---- catalogue
  const PluginInfo* info = findPlugin(pl::kType);
  if (argc > 1 && std::string(argv[1]) == "bench") {  // local aid: CPU per preset / quality / notes (real-time %)
    for (size_t pi = 0; pi < info->presets.size(); ++pi) {
      if (argc > 2 && info->presets[pi].name != argv[2]) continue;
      for (int q = 0; q < 3; ++q)
        for (int notes : {1, 8}) {
          PluginState s = presetState(*info, pi);
          setP(s, pl::G_Quality, static_cast<float>(q));
          std::vector<Ev> ev;
          for (int i = 0; i < notes; ++i) ev.push_back({0, true, static_cast<uint32_t>(i + 1), 48.0 + i * 3, 0.8f});
          const int total = 48000 * 4;
          const auto t0 = std::chrono::steady_clock::now();
          Out o = render(s, ev, total, 256);
          const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
          std::printf("%s\tquality %d\tnotes %d\t%.2f %% of real time (one core)\n", info->presets[pi].name.c_str(), q, notes, 100.0 * secs / 4.0);
          (void)o;
        }
    }
    return 0;
  }
  if (argc > 2 && std::string(argv[1]) == "import") {  // local aid: import the user's own presets, one line each
    for (int a = 2; a < argc; ++a) {
      PluginState s;
      pl::ImportReport rep;
      const bool ok = pl::importSerumPreset(argv[a], s, rep);
      float peak = 0;
      bool finite = true;
      if (ok) {
        Out o = render(s, {{0, true, 1, 60, 0.8f}, {30000, false, 1, 60, 0}}, 48000, 256);
        peak = o.peak, finite = o.finite;
      }
      std::printf("%s\t%s\tmapped=%d\tskipped=%d\tapprox=%zu\tmissing=%zu\tpeak=%.4f\t%s\t%s\n", ok ? "OK" : "FAIL", argv[a], rep.mapped,
                  rep.skipped, rep.approximated.size(), rep.missingFiles.size(), peak, finite ? "finite" : "NONFINITE", rep.error.c_str());
      if (std::getenv("PLASMA_IMPORT_DEBUG")) {
        pl::Model m;
        pl::modelFromData(s.data, m);
        for (int o = 0; o < pl::kOscs; ++o)
          std::printf("#osc%d on=%.0f type=%.0f level=%.2f table='%s' sample='%s' sfz='%s'\n", o, s.params[static_cast<size_t>(pl::oscP(o, pl::O_Enable))],
                      s.params[static_cast<size_t>(pl::oscP(o, pl::O_Type))], s.params[static_cast<size_t>(pl::oscP(o, pl::O_Level))],
                      m.osc[static_cast<size_t>(o)].table.c_str(), m.osc[static_cast<size_t>(o)].sample.c_str(), m.osc[static_cast<size_t>(o)].sfz.c_str());
        for (const auto& nd : m.fx) {
          PluginState one = s;
          const int ft = static_cast<int>(s.params[static_cast<size_t>(pl::fxP(nd.slot, pl::X_Type))]);
          one.params[static_cast<size_t>(pl::fxP(nd.slot, pl::X_On))] = 0;
          Out so = render(one, {{0, true, 1, 60, 0.8f}, {30000, false, 1, 60, 0}}, 48000, 256);
          std::printf("#fx slot %d type %d rack %d parent %d band %d on=%.0f: without it peak %.4f\n", nd.slot, ft, nd.rack, nd.parent, nd.band,
                      s.params[static_cast<size_t>(pl::fxP(nd.slot, pl::X_On))], so.peak);
        }
        for (int o = 0; o < pl::kOscs; ++o) {
          PluginState solo = s;
          for (int q = 0; q < pl::kOscs; ++q)
            if (q != o) solo.params[static_cast<size_t>(pl::oscP(q, pl::O_Enable))] = 0;
          solo.params[static_cast<size_t>(pl::noiseP(pl::N_Enable))] = 0;
          solo.params[static_cast<size_t>(pl::subP(pl::S_Enable))] = 0;
          Out so = render(solo, {{0, true, 1, 60, 0.8f}, {30000, false, 1, 60, 0}}, 48000, 256);
          for (int f = 0; f < pl::kFxSlots; ++f) solo.params[static_cast<size_t>(pl::fxP(f, pl::X_Type))] = 0;
          Out sd = render(solo, {{0, true, 1, 60, 0.8f}, {30000, false, 1, 60, 0}}, 48000, 256);
          solo.params[static_cast<size_t>(pl::filtP(0, pl::F_Enable))] = 0;
          solo.params[static_cast<size_t>(pl::filtP(1, pl::F_Enable))] = 0;
          Out sf = render(solo, {{0, true, 1, 60, 0.8f}, {30000, false, 1, 60, 0}}, 48000, 256);
          std::printf("#solo osc%d peak %.4f  dry %.4f  dry+nofilter %.4f\n", o, so.peak, sd.peak, sf.peak);
        }
      }
      for (const auto& n : rep.approximated) std::printf("#approx\t%s\n", n.c_str());
      for (const auto& n : rep.missingFiles) std::printf("#missing\t%s\n", n.c_str());
    }
    return 0;
  }
  if (argc > 2 && std::string(argv[1]) == "zstd") {  // local aid: decompress a raw .zst file to stdout size
    std::ifstream f(argv[2], std::ios::binary);
    std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), out;
    std::string err;
    const bool ok = pl::zstdDecompress(in.data(), in.size(), out, &err);
    if (argc > 3) std::ofstream(argv[3], std::ios::binary).write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    std::printf("%s %zu %s\n", ok ? "OK" : "FAIL", out.size(), err.c_str());
    return ok ? 0 : 1;
  }
  if (argc > 2 && std::string(argv[1]) == "preset" && info) {  // debugging aid: level trace of one preset
    for (const auto& pr : info->presets) {
      if (pr.name != argv[2]) continue;
      PluginState s = base();
      const int drop = argc > 3 ? std::atoi(argv[3]) : -1;  // leave out override #drop (negative: keep the first -drop)
      for (size_t i = 0; i < pr.values.size(); ++i)
        if (drop >= 0 ? static_cast<int>(i) != drop : static_cast<int>(i) < -drop) setP(s, pr.values[i].first, pr.values[i].second);
      Out o = render(s, {{0, true, 1, 60, 0.8f}, {20000, false, 1, 60, 0}}, 36000, 256);
      std::printf("finite %d peak %.4f\n", o.finite, o.peak);
      for (int i = 0; i < 36000; i += 3000) std::printf("  %5d %.4f\n", i, peakIn(o, i, i + 3000));
      for (size_t i = 0; i < pr.values.size(); ++i) std::printf("  #%zu %s = %.3f\n", i, info->params[static_cast<size_t>(pr.values[i].first)].name.c_str(), pr.values[i].second);
    }
    return 0;
  }
  expect(info != nullptr, "gl.plasma is registered");
  if (!info) return finish();
  expect(static_cast<int>(info->params.size()) == pl::kParamCount, "parameter count matches the layout");
  expect(info->presets.size() >= 20 && info->presets.size() <= 40, "20-40 factory presets");
  expect(pl::factoryTableNames().size() >= 16, "factory wavetables");
  expect(pl::warpCount() == 104 && pl::filterNames().size() >= 100, "warp and filter lists");
  expect(packs::createGenerator(pl::kType) != nullptr, "generator is created");

  // ---- sound / release
  {
    PluginState s = base();
    Out o = render(s, kPress, 48000, 256);
    expect(o.finite, "finite output");
    expect(peakIn(o, 100, 12000) > 0.02f, "sound after note on");
    expect(peakIn(o, 30000, 48000) < 1e-4f, "silence after release");
    expect(!o.activeAtEnd, "voice ends after release");
  }
  // ---- block-size invariance
  {
    PluginState s = base();
    setP(s, pl::oscP(0, pl::O_Unison), 5);
    setP(s, pl::oscP(0, pl::O_UniDetune), 0.4f);
    setP(s, pl::filtP(0, pl::F_Enable), 1);
    setP(s, pl::filtP(0, pl::F_Freq), 0.6f);
    setP(s, pl::modP(0, pl::M_Source), pl::SrcLfo1);
    setP(s, pl::modP(0, pl::M_Dest), static_cast<float>(pl::filtP(0, pl::F_Freq) + 1));
    setP(s, pl::modP(0, pl::M_Amount), 0.4f);
    setP(s, pl::fxP(0, pl::X_Type), pl::FxDelay);
    setP(s, pl::fxP(0, pl::X_On), 1);
    for (int q = 0; q < static_cast<int>(pl::fxParams(pl::FxDelay).size()); ++q) setP(s, pl::fxP(0, pl::X_P0 + q), pl::fxDefaultNorm(pl::FxDelay, q));
    Out a = render(s, kPress, 30000, 64), b = render(s, kPress, 30000, 37), c = render(s, kPress, 30000, 512);
    double d = 0;
    for (size_t i = 0; i < a.L.size(); ++i) d = std::max({d, static_cast<double>(std::fabs(a.L[i] - b.L[i])), static_cast<double>(std::fabs(a.L[i] - c.L[i]))});
    std::printf("# block-size max diff %.3g\n", d);
    expect(a.peak > 0.01f && d < 1e-4, "block-size invariant");
  }
  // ---- allocation
  {
    PluginState s = base();
    setP(s, pl::oscP(0, pl::O_Unison), 8);
    setP(s, pl::oscP(1, pl::O_Enable), 1);
    setP(s, pl::oscP(1, pl::O_Type), pl::OscGranular);
    setP(s, pl::oscP(2, pl::O_Enable), 1);
    setP(s, pl::oscP(2, pl::O_Type), pl::OscSpectral);
    setP(s, pl::subP(pl::S_Enable), 1);
    setP(s, pl::noiseP(pl::N_Enable), 1);
    setP(s, pl::filtP(0, pl::F_Enable), 1);
    setP(s, pl::filtP(1, pl::F_Enable), 1);
    setP(s, pl::G_ArpOn, 1);
    const int types[] = {pl::FxDistortion, pl::FxChorus, pl::FxReverb, pl::FxComp, pl::FxConvolve, pl::FxHyper};
    for (int i = 0; i < 6; ++i) {
      setP(s, pl::fxP(i, pl::X_Type), static_cast<float>(types[i]));
      setP(s, pl::fxP(i, pl::X_On), 1);
      for (int q = 0; q < static_cast<int>(pl::fxParams(types[i]).size()); ++q) setP(s, pl::fxP(i, pl::X_P0 + q), pl::fxDefaultNorm(types[i], q));
    }
    std::vector<Ev> ev = {{0, true, 1, 60, 0.8f}, {3000, true, 2, 64, 0.8f}, {6000, true, 3, 67, 0.8f}, {12000, false, 1, 60, 0}, {14000, false, 2, 64, 0}};
    Out o = render(s, ev, 24000, 256, true);
    std::printf("# audio-thread allocations: %ld\n", o.allocs);
    expect(o.finite && o.allocs == 0, "no allocation on the audio thread");
  }
  // ---- band-limiting at high pitch
  {
    PluginState s = base();
    setP(s, pl::G_Quality, 0);  // 1x: the tables alone must keep aliasing away
    setP(s, pl::oscP(0, pl::O_Table), static_cast<float>(tableIndex("Basic Shapes")));
    setP(s, pl::oscP(0, pl::O_WtPos), 0);
    const double f0 = 440.0 * std::pow(2.0, (100 - 69) / 12.0);
    Out o = render(s, {{0, true, 1, 100, 0.9f}}, 24000, 256);
    const int a = 8000, b = 8000 + 8192;
    const double fund = mag(o.L, a, b, f0);
    double worst = 0;
    for (double f = 50; f < kSr / 2 - 50; f += 37) {
      const double k = f / f0;
      if (std::fabs(k - std::round(k)) * f0 < 80) continue;
      worst = std::max(worst, mag(o.L, a, b, f));
    }
    const double db = 20 * std::log10(std::max(1e-12, worst / std::max(1e-12, fund)));
    std::printf("# alias level %.1f dB\n", db);
    expect(fund > 1e-3 && db < -50, "no aliasing at high pitch");
  }
  // ---- unison detune
  {
    PluginState s = base();
    setP(s, pl::oscP(0, pl::O_Unison), 1);
    Out one = render(s, kHold, 48000, 256);
    setP(s, pl::oscP(0, pl::O_Unison), 7);
    setP(s, pl::oscP(0, pl::O_UniDetune), 0.5f);
    Out uni = render(s, kHold, 48000, 256);
    auto spread = [](const Out& o) {  // variation of the short-term level (beating)
      std::vector<double> lv;
      for (int i = 4800; i + 960 <= 48000; i += 960) lv.push_back(rms(o.L, i, i + 960));
      double mn = 1e9, mx = 0;
      for (double v : lv) mn = std::min(mn, v), mx = std::max(mx, v);
      return mx / std::max(1e-9, mn);
    };
    const double a = spread(one), b = spread(uni);
    std::printf("# level spread: unison 1 %.3f, unison 7 %.3f\n", a, b);
    expect(a < 1.15 && b > 1.5, "unison detune beats");
    const double f0 = 261.63;
    expect(mag(uni.L, 24000, 40000, f0) < mag(one.L, 24000, 40000, f0), "detuned voices spread away from the fundamental");
  }
  // ---- matrix
  {
    PluginState s = base();
    setP(s, pl::modP(3, pl::M_Source), pl::SrcMacro1);
    setP(s, pl::modP(3, pl::M_Dest), static_cast<float>(pl::oscP(0, pl::O_Level) + 1));
    setP(s, pl::modP(3, pl::M_Amount), -1);
    setP(s, pl::macroP(0), 0);
    Out a = render(s, kHold, 12000, 256);
    setP(s, pl::macroP(0), 1);
    Out b = render(s, kHold, 12000, 256);
    expect(a.peak > 0.02f && b.peak < 1e-3f, "macro -> osc level through the matrix");
    // LFO -> pitch: the zero-crossing rate moves
    PluginState p = base();
    setP(p, pl::modP(0, pl::M_Source), pl::SrcEnv1 + 1);
    setP(p, pl::modP(0, pl::M_Dest), static_cast<float>(pl::oscP(0, pl::O_Coarse) + 1));
    setP(p, pl::modP(0, pl::M_Amount), 0.5f);  // +64 st range * 0.5 at full envelope
    setP(p, pl::envP(1, pl::E_Sustain), 1);
    Out c = render(p, kHold, 24000, 256);
    Out d = render(base(), kHold, 24000, 256);
    int zc = 0, zd = 0;
    for (int i = 12001; i < 24000; ++i) {
      if (c.L[static_cast<size_t>(i - 1)] <= 0 && c.L[static_cast<size_t>(i)] > 0) ++zc;
      if (d.L[static_cast<size_t>(i - 1)] <= 0 && d.L[static_cast<size_t>(i)] > 0) ++zd;
    }
    std::printf("# zero crossings: modulated %d, dry %d\n", zc, zd);
    expect(zc > zd * 3, "envelope -> coarse pitch through the matrix");
  }
  // ---- every effect module
  {
    PluginState dry = base();
    Out d = render(dry, kPress, 30000, 256);
    for (int t = pl::FxDistortion; t < pl::FxSplitLH; ++t) {
      PluginState s = base();
      setP(s, pl::fxP(2, pl::X_Type), static_cast<float>(t));
      setP(s, pl::fxP(2, pl::X_On), 1);
      for (int q = 0; q < static_cast<int>(pl::fxParams(t).size()); ++q) setP(s, pl::fxP(2, pl::X_P0 + q), pl::fxDefaultNorm(t, q));
      // modules that are neutral at their defaults: move one control
      const std::pair<const char*, double> nudge[] = {{"kParamGain1", 12}, {"kParamHPF", 400}, {"kParamThresh", 0.5}};
      for (const auto& [key, plain] : nudge) {
        const int q = pl::fxParamIndex(t, key);
        if (q >= 0 && (t == pl::FxEq || t == pl::FxUtility || t == pl::FxComp)) setP(s, pl::fxP(2, pl::X_P0 + q), static_cast<float>(pl::fxNorm(t, q, plain)));
      }
      Out o = render(s, kPress, 30000, 256);
      double diff = 0;
      for (size_t i = 0; i < o.L.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(o.L[i] - d.L[i]) + std::fabs(o.R[i] - d.R[i])));
      char name[96];
      std::snprintf(name, sizeof name, "effect %s: sound, changed, finite", pl::fxTypeNames()[static_cast<size_t>(t)].c_str());
      std::printf("# %s peak %.3f diff %.3f\n", pl::fxTypeNames()[static_cast<size_t>(t)].c_str(), o.peak, diff);
      expect(o.finite && o.peak > 0.005f && diff > 1e-3, name);
    }
    // splitters: a distortion in one band
    for (int t = pl::FxSplitLH; t <= pl::FxSplitMS; ++t) {
      PluginState s = base();
      setP(s, pl::fxP(0, pl::X_Type), static_cast<float>(t));
      setP(s, pl::fxP(0, pl::X_On), 1);
      for (int q = 0; q < static_cast<int>(pl::fxParams(t).size()); ++q) setP(s, pl::fxP(0, pl::X_P0 + q), pl::fxDefaultNorm(t, q));
      setP(s, pl::fxP(1, pl::X_Type), pl::FxDistortion);
      setP(s, pl::fxP(1, pl::X_On), 1);
      for (int q = 0; q < static_cast<int>(pl::fxParams(pl::FxDistortion).size()); ++q)
        setP(s, pl::fxP(1, pl::X_P0 + q), pl::fxDefaultNorm(pl::FxDistortion, q));
      pl::Model m = pl::defaultModel();
      m.fx = {{0, 0, -1, 0}, {1, 0, 0, t == pl::FxSplitMS ? 0 : 1}};  // M/S: the mid band of a wide unison
      if (t == pl::FxSplitMS) {
        setP(s, pl::oscP(0, pl::O_Unison), 4);
        setP(s, pl::oscP(0, pl::O_UniDetune), 0.4f);
        setP(s, pl::oscP(0, pl::O_UniWidth), 1);
      }
      s.data = pl::modelToData(m);
      Out o = render(s, kPress, 30000, 256);
      // the same chain without the split (distortion over everything) differs
      PluginState u = s;
      m.fx = {{1, 0, -1, 0}};
      setP(u, pl::fxP(0, pl::X_Type), 0);
      u.data = pl::modelToData(m);
      Out w = render(u, kPress, 30000, 256);
      double diff = 0, dd = 0;
      for (size_t i = 0; i < o.L.size(); ++i) {
        diff = std::max(diff, static_cast<double>(std::fabs(o.L[i] - w.L[i])));
        dd = std::max(dd, static_cast<double>(std::fabs(o.L[i] - d.L[i])));
      }
      char name[96];
      std::snprintf(name, sizeof name, "splitter %s hosts a band chain", pl::fxTypeNames()[static_cast<size_t>(t)].c_str());
      expect(o.finite && o.peak > 0.005f && diff > 1e-3 && dd > 1e-3, name);
    }
  }
  // ---- oscillator types
  {
    for (int t = pl::OscWavetable; t < pl::OscTypeCount; ++t) {
      PluginState s = base();
      setP(s, pl::oscP(0, pl::O_Type), static_cast<float>(t));
      Out o = render(s, kPress, 24000, 256);
      char name[64];
      std::snprintf(name, sizeof name, "oscillator type %d sounds", t);
      std::printf("# osc type %d peak %.3f\n", t, o.peak);
      expect(o.finite && (t == pl::OscMultisample || t == pl::OscSample ? true : o.peak > 0.01f), name);
    }
  }
  // ---- every factory table plays at a few positions
  {
    int bad = 0;
    for (size_t t = 0; t < pl::factoryTableNames().size(); ++t)
      for (float pos : {0.f, 0.5f, 1.f}) {
        PluginState s = base();
        setP(s, pl::oscP(0, pl::O_Table), static_cast<float>(t + 1));
        setP(s, pl::oscP(0, pl::O_WtPos), pos);
        Out o = render(s, kHold, 4800, 256);
        if (!o.finite || o.peak < 0.01f || o.peak > 2.f) {
          ++bad;
          std::printf("# table %s @%.1f: peak %.3f\n", pl::factoryTableNames()[t].c_str(), pos, o.peak);
        }
      }
    expect(bad == 0, "every factory table plays");
  }
  // ---- warps: every mode is finite, most change the sound
  {
    PluginState d0 = base();
    Out dry = render(d0, kHold, 9600, 256);
    int changed = 0, finite = 0, total = 0;
    for (int w = 1; w < pl::warpCount(); ++w) {
      const int type = w >= 70 ? pl::OscSpectral : pl::OscWavetable;
      PluginState s = base();
      setP(s, pl::oscP(0, pl::O_Type), static_cast<float>(type));
      setP(s, pl::oscP(0, pl::O_Warp1Mode), static_cast<float>(w));
      setP(s, pl::oscP(0, pl::O_Warp1), 0.7f);
      setP(s, pl::oscP(1, pl::O_Enable), 1);
      setP(s, pl::oscP(1, pl::O_Level), 0.3f);
      setP(s, pl::subP(pl::S_Enable), 1);
      setP(s, pl::noiseP(pl::N_Enable), 1);
      setP(s, pl::noiseP(pl::N_Level), 0.2f);
      Out o = render(s, kHold, 9600, 256);
      ++total;
      if (o.finite && o.peak < 8) ++finite;
      double diff = 0;
      for (size_t i = 0; i < o.L.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(o.L[i] - dry.L[i])));
      if (diff > 1e-3) ++changed;
      else std::printf("# warp %s: no change\n", pl::warpNames()[static_cast<size_t>(w)].c_str());
    }
    std::printf("# warps: %d/%d finite, %d changed\n", finite, total, changed);
    expect(finite == total && changed == total, "every warp mode is finite and audible");
  }
  // ---- filters: every type finite and audible
  {
    PluginState d0 = base();
    Out dry = render(d0, kHold, 9600, 256);
    int bad = 0;
    for (int t = 0; t < static_cast<int>(pl::filterNames().size()); ++t) {
      PluginState s = base();
      setP(s, pl::filtP(0, pl::F_Enable), 1);
      setP(s, pl::filtP(0, pl::F_Type), static_cast<float>(t));
      setP(s, pl::filtP(0, pl::F_Freq), 0.45f);
      setP(s, pl::filtP(0, pl::F_Res), 0.6f);
      setP(s, pl::filtP(0, pl::F_Var), 0.7f);
      Out o = render(s, kHold, 9600, 256);
      double diff = 0;
      for (size_t i = 0; i < o.L.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(o.L[i] - dry.L[i])));
      if (!o.finite || o.peak > 8 || diff < 1e-3 || o.peak < 1e-3) {
        ++bad;
        std::printf("# filter %s: peak %.3f diff %.4f\n", pl::filterNames()[static_cast<size_t>(t)].c_str(), o.peak, diff);
      }
    }
    expect(bad == 0, "every filter type is finite and audible");
  }
  // ---- extremes
  {
    for (int pass = 0; pass < 3; ++pass) {
      PluginState s = base();
      uint32_t r = 12345u + static_cast<uint32_t>(pass);
      for (int k = 0; k < pl::kParamCount; ++k) {
        const ParamDesc& d = info->params[static_cast<size_t>(k)];
        if (k >= pl::kModBase && k < pl::kFxBase) continue;  // matrix: keep a few slots below
        r = r * 1664525u + 1013904223u;
        const double hi = d.kind == ParamKind::Choice ? static_cast<double>(d.choices.size() - 1) : d.max;
        double v = pass == 0 ? d.min : pass == 1 ? hi : d.min + (hi - d.min) * ((r >> 8) / 16777216.0);
        if (d.kind != ParamKind::Continuous) v = std::round(v);
        s.params[static_cast<size_t>(k)] = static_cast<float>(v);
      }
      setP(s, pl::G_Poly, 4);
      setP(s, pl::G_MasterVol, 0.5f);
      for (int sl = 0; sl < 6; ++sl) {
        setP(s, pl::modP(sl, pl::M_Source), static_cast<float>(1 + sl * 7));
        setP(s, pl::modP(sl, pl::M_Dest), static_cast<float>(pl::oscP(sl % 3, pl::O_WtPos + sl) + 1));
        setP(s, pl::modP(sl, pl::M_Amount), 1);
      }
      std::vector<Ev> ev = {{0, true, 1, 24, 1.f}, {100, true, 2, 127, 1.f}, {200, true, 3, 0, 0.01f}, {9000, false, 1, 0, 0}};
      Out o = render(s, ev, 19200, 256);
      char name[64];
      std::snprintf(name, sizeof name, "finite output at extremes (%s)", pass == 0 ? "minimum" : pass == 1 ? "maximum" : "random");
      std::printf("# extremes pass %d peak %.3f\n", pass, o.peak);
      expect(o.finite, name);
    }
  }
  // ---- presets
  {
    int silent = 0;
    for (const auto& pr : info->presets) {
      PluginState s = base();
      for (const auto& kv : pr.values) setP(s, kv.first, kv.second);
      Out o = render(s, {{0, true, 1, 60, 0.8f}, {20000, false, 1, 60, 0}}, 36000, 256);
      if (!o.finite || o.peak < 1e-3f || o.peak > 4.f) {
        ++silent;
        std::printf("# preset %s: peak %.4f\n", pr.name.c_str(), o.peak);
      }
    }
    expect(silent == 0, "every factory preset loads and sounds");
    for (const auto& dp : pl::factoryDataPresets()) {
      PluginState s = base();
      for (const auto& kv : dp.values) setP(s, kv.first, kv.second);
      s.data = dp.data;
      pl::Model m;
      expect(pl::modelFromData(dp.data, m), ("data preset parses: " + dp.name).c_str());
      Out o = render(s, {{0, true, 1, 60, 0.8f}, {20000, false, 1, 60, 0}}, 36000, 256);
      if (!o.finite || o.peak < 1e-3f || o.peak > 4.f) std::printf("# data preset %s: peak %.4f\n", dp.name.c_str(), o.peak);
      expect(o.finite && o.peak >= 1e-3f && o.peak <= 4.f, ("data preset sounds: " + dp.name).c_str());
    }
  }
  // ---- short / old state vectors
  {
    PluginState s = base();
    s.params.resize(10);
    normalizePluginState(s);
    expect(static_cast<int>(s.params.size()) == pl::kParamCount, "short vectors are padded");
    Out o = render(s, kPress, 12000, 256);
    expect(o.finite, "short vector renders");
  }
  // ---- model round trip
  {
    pl::Model m = pl::defaultModel();
    m.osc[0].table = "custom";
    m.osc[0].frames = {std::vector<float>(2048, 0.f), std::vector<float>(2048, 0.f)};
    for (int i = 0; i < 2048; ++i) m.osc[0].frames[0][static_cast<size_t>(i)] = std::sin(6.2831853f * i / 2048.f), m.osc[0].frames[1][static_cast<size_t>(i)] = i < 1024 ? 0.8f : -0.8f;
    m.lfo[2].pts = {{0, 0, 0}, {0.3f, 1, 0.2f}, {1, 0, -0.4f}};
    m.lfo[2].loop = 1;
    m.fx = {{3, 1, -1, 0}};
    m.arp[2].shape = pl::ArpConverge;
    m.clip[1].notes = {{0, 0.5f, 62, 0.7f, false}};
    m.macroName[0] = "Grit";
    pl::Model back;
    const bool ok = pl::modelFromData(pl::modelToData(m), back);
    expect(ok && back.osc[0].table == "custom" && back.osc[0].frames.size() == 2 && std::fabs(back.osc[0].frames[0][512] - 1.f) < 1e-3f &&
               back.lfo[2].pts.size() == 3 && back.lfo[2].loop == 1 && back.fx.size() == 1 && back.fx[0].rack == 1 &&
               back.arp[2].shape == pl::ArpConverge && back.clip[1].notes.size() == 1 && back.macroName[0] == "Grit",
           "model data round trip");
    pl::Model junk;
    expect(!pl::modelFromData("{not json", junk), "bad data rejected");
    // a custom table plays
    PluginState s = base();
    setP(s, pl::oscP(0, pl::O_Table), 0);
    s.data = pl::modelToData(m);
    Out o = render(s, kPress, 12000, 256);
    expect(o.finite && o.peak > 0.02f, "custom table plays");
  }
  // ---- arp / clip
  {
    PluginState s = base();
    setP(s, pl::G_ArpOn, 1);
    pl::Model m = pl::defaultModel();
    m.arp[0].shape = pl::ArpUp;
    s.data = pl::modelToData(m);
    std::vector<Ev> ev = {{0, true, 1, 60, 0.8f}, {0, true, 2, 64, 0.8f}, {0, true, 3, 67, 0.8f}};
    Out o = render(s, ev, 96000, 256);
    // pitch per 1/16 step (125 ms at 120 BPM): zero crossings in the middle of each step
    int changes = 0, prevZ = -1;
    for (int st = 0; st < 15; ++st) {
      const int a = st * 6000 + 1500, b = a + 3000;
      int z = 0;
      for (int i = a + 1; i < b; ++i) z += o.L[static_cast<size_t>(i - 1)] <= 0 && o.L[static_cast<size_t>(i)] > 0;
      if (prevZ >= 0 && std::abs(z - prevZ) >= 2) ++changes;
      prevZ = z;
    }
    std::printf("# arp pitch changes over 15 steps: %d\n", changes);
    expect(o.finite && changes >= 10, "arpeggiator steps through held notes");
    // releasing the keys stops the arpeggio (note-offs arrive by note id)
    {
      std::vector<Ev> rel = {{0, true, 1, 60, 0.8f}, {0, true, 2, 64, 0.8f}, {24000, false, 1, 60, 0}, {24000, false, 2, 64, 0}};
      Out r = render(s, rel, 96000, 256);
      expect(r.finite && peakIn(r, 4000, 20000) > 0.02f && peakIn(r, 80000, 96000) < 1e-3f, "arp stops when the keys are released");
    }
    // trigger octave: a key in it picks the arp slot and does not sound
    {
      PluginState t = s;
      pl::Model mt = pl::defaultModel();
      mt.arp[0].shape = pl::ArpUp;
      mt.arp[0].rate = 7;
      mt.arp[3].shape = pl::ArpUp;
      mt.arp[3].rate = 3;  // a slower division
      t.data = pl::modelToData(mt);
      setP(t, pl::G_TrigOctave, 4);  // C1: keys 36..47
      std::vector<Ev> only = {{0, true, 9, 39, 0.8f}};
      Out a = render(t, only, 24000, 256);
      expect(a.finite && a.peak < 1e-4f, "trigger-octave key is silent");
      std::vector<Ev> pick = {{0, true, 9, 39, 0.8f}, {10, true, 1, 60, 0.8f}, {10, true, 2, 67, 0.8f}};
      std::vector<Ev> nopick = {{10, true, 1, 60, 0.8f}, {10, true, 2, 67, 0.8f}};
      Out b = render(t, pick, 96000, 256), d = render(t, nopick, 96000, 256);
      double diff = 0;
      for (size_t i = 0; i < b.L.size(); ++i) diff += std::fabs(b.L[i] - d.L[i]);
      expect(b.finite && diff > 1.0, "trigger-octave key switches the arp slot");
    }
    PluginState c = base();
    setP(c, pl::G_ClipOn, 1);
    pl::Model mc = pl::defaultModel();
    mc.clip[0].notes = {{0, 0.25f, 60, 0.8f, false}, {1, 0.25f, 64, 0.8f, false}, {2, 0.25f, 67, 0.8f, false}};
    mc.clip[0].lengthBeats = 4;
    c.data = pl::modelToData(mc);
    Out q = render(c, {{0, true, 1, 60, 0.8f}}, 96000, 256);
    expect(q.finite && peakIn(q, 24000, 30000) > 0.01f && peakIn(q, 20000, 23000) < peakIn(q, 24000, 30000), "clip player plays its notes");
  }
  // ---- MIDI: pitch bend
  {
    PluginState s = base();
    auto g = packs::createGenerator(pl::kType);
    ChannelAux aux;
    packs::prepareAux(s, aux);
    g->prepareHost(s, kSr);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    std::vector<float> L(24000), R(24000);
    g->noteOn(1, 60, 0.8f, 0);
    g->process(s, aux, ctx, L.data(), R.data(), 12000);
    g->midi(0xE0, 0x7F, 0x7F, 0);  // full bend up (+2 st)
    g->process(s, aux, ctx, L.data() + 12000, R.data() + 12000, 12000);
    int z1 = 0, z2 = 0;
    for (int i = 2001; i < 12000; ++i) z1 += L[static_cast<size_t>(i - 1)] <= 0 && L[static_cast<size_t>(i)] > 0;
    for (int i = 14001; i < 24000; ++i) z2 += L[static_cast<size_t>(i - 1)] <= 0 && L[static_cast<size_t>(i)] > 0;
    std::printf("# bend: %d -> %d crossings\n", z1, z2);
    expect(z2 > z1 * 1.08 && z2 < z1 * 1.18, "pitch bend +2 semitones");
  }
  // ---- clip recording: notes sound directly and are reported with their timing
  {
    PluginState s = base();
    setP(s, pl::G_ClipOn, 1);  // even with the clip player on, recorded notes play through
    auto g = packs::createGenerator(pl::kType);
    ChannelAux aux;
    packs::prepareAux(s, aux);
    g->prepareHost(s, kSr);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    ctx.tempo = 120;
    pl::setClipRecording(g.get(), true);
    std::vector<float> L(48000), R(48000);
    g->noteOn(7, 64, 0.6f, 0);
    g->process(s, aux, ctx, L.data(), R.data(), 24000);  // half a second = one beat at 120 BPM
    g->noteOff(7, 0);
    g->process(s, aux, ctx, L.data() + 24000, R.data() + 24000, 24000);
    float pk = 0;
    for (int i = 2000; i < 20000; ++i) pk = std::max(pk, std::fabs(L[static_cast<size_t>(i)]));
    uint32_t rd = 0;
    pl::RecordedEvent ev[8];
    const int n = pl::recordedEvents(g.get(), rd, ev, 8);
    const bool timing = n == 2 && ev[0].on && !ev[1].on && ev[0].key == 64 && ev[1].key == 64 && std::fabs((ev[1].clock - ev[0].clock) - 1.0) < 0.02;
    expect(pk > 0.01f && timing, "clip recording plays and reports the notes with their length");
    pl::setClipRecording(g.get(), false);
    g->noteOn(8, 60, 0.6f, 0);
    g->process(s, aux, ctx, L.data(), R.data(), 2400);
    expect(pl::recordedEvents(g.get(), rd, ev, 8) == 0, "nothing is reported when not recording");
  }
  // ---- import (synthetic presets written above)
  {
    std::vector<uint8_t> raw(5000);
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<uint8_t>(i * 7 + (i >> 5));
    auto z = zstdRaw(raw);
    std::vector<uint8_t> back;
    expect(pl::zstdDecompress(z.data(), z.size(), back) && back == raw, "zstd raw blocks decode");
    const std::string vt = zstdVecText();
    expect(pl::zstdDecompress(kZstdVec, sizeof(kZstdVec), back) && std::string(back.begin(), back.end()) == vt, "zstd compressed blocks decode");
    uint32_t fz = 99;  // damaged data: must fail (or decode garbage) without crashing or hanging
    for (int round = 0; round < 400; ++round) {
      std::vector<uint8_t> zbad(kZstdVec, kZstdVec + sizeof(kZstdVec));
      for (int k = 0; k < 1 + round % 4; ++k) {
        fz = fz * 1664525u + 1013904223u;
        zbad[4 + (fz >> 8) % (zbad.size() - 4)] ^= static_cast<uint8_t>(1u << ((fz >> 4) & 7));
      }
      if (round % 7 == 0) zbad.resize(zbad.size() - 1 - (fz >> 3) % 40);
      pl::zstdDecompress(zbad.data(), zbad.size(), back);
      Json jj;
      pl::cborDecode(zbad.data(), zbad.size(), jj);
    }
    expect(true, "damaged zstd data is rejected safely");
    Cbor c;
    cborJson(c, obj({{"a", num(1.5)}, {"b", str("x")}, {"c", num(-3)}}));
    Json j;
    expect(pl::cborDecode(c.b.data(), c.b.size(), j) && j.find("a") && std::fabs(j.num("a") - 1.5) < 1e-9 && j.num("c") == -3,
           "CBOR map decodes");
    // a preset: osc A wavetable with unison, filter 1 low-pass, env 1, an LFO -> cutoff slot, a reverb
    Json osc0 = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamVolume", num(0.75)}, {"kParamUnison", num(5)},
                                          {"kParamDetune", num(0.3)}, {"kParamOctave", num(-1)}})},
                     {"WTOsc0", obj({{"plainParams", obj({{"kParamTablePos", num(1)}, {"kParamWarpMenu", str("kSync")}, {"kParamWarp", num(0.4)}})}})}});
    Json filt = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamType", str("L24")}, {"kParamFreq", num(0.4)}, {"kParamReso", num(30)}})}});
    Json env0 = obj({{"plainParams", obj({{"kParamAttack", num(0.05)}, {"kParamDecay", num(0.5)}, {"kParamSustain", num(0.5)}, {"kParamRelease", num(0.3)}})}});
    Json slot = obj({{"plainParams", obj({{"kParamAmount", num(40)}})},
                     {"source", [] { Json a = Json::makeArray(); a.array.push_back(num(6)); a.array.push_back(num(0)); return a; }()},
                     {"destModuleTypeString", str("VoiceFilter")},
                     {"destModuleID", num(0)},
                     {"destModuleParamName", str("kParamFreq")}});
    Json fxlist = Json::makeArray();
    fxlist.array.push_back(obj({{"FXReverb", obj({{"plainParams", obj({{"kParamWet", num(30)}, {"kParamSize", num(70)}, {"kParamType", str("kHall")}})}})}, {"type", num(6)}}));
    Json rack = obj({{"FX", fxlist}});
    Json glob = obj({{"plainParams", obj({{"kParamMasterVolume", num(0.7)}, {"kParamPolyCount", num(6)}, {"kParamMonoToggle", num(0)}})}});
    Json body = obj({{"Oscillator0", osc0}, {"VoiceFilter0", filt}, {"Env0", env0}, {"ModSlot0", slot}, {"FXRack0", rack}, {"Global0", glob}});
    auto bytes = container(body, "Synthetic Lead");
    PluginState st;
    pl::ImportReport rep;
    const bool ok = pl::importSerumBytes(bytes, st, rep);
    std::printf("# import: %s mapped %d skipped %d approx %zu err '%s'\n", ok ? "ok" : "fail", rep.mapped, rep.skipped, rep.approximated.size(), rep.error.c_str());
    expect(ok && st.type == pl::kType && rep.presetName == "Synthetic Lead", "synthetic Serum 2 preset imports");
    if (ok) {
      auto P = [&](int k) { return st.params[static_cast<size_t>(k)]; };
      expect(std::lround(P(pl::oscP(0, pl::O_Unison))) == 5 && std::lround(P(pl::oscP(0, pl::O_Octave))) == -1, "osc unison / octave mapped");
      expect(std::fabs(P(pl::oscP(0, pl::O_Level)) - std::sqrt(0.75f)) < 1e-3f && std::lround(P(pl::oscP(0, pl::O_Warp1Mode))) == 1, "osc level / warp mapped");
      expect(P(pl::filtP(0, pl::F_Enable)) > 0.5f && pl::filterNames()[static_cast<size_t>(std::lround(P(pl::filtP(0, pl::F_Type))))] == "Low 24",
             "filter type mapped");
      expect(std::fabs(pl::envTime(P(pl::envP(0, pl::E_Attack))) - 0.05) < 1e-3, "envelope time mapped (seconds)");
      expect(std::lround(P(pl::modP(0, pl::M_Source))) == pl::SrcLfo1 &&
                 std::lround(P(pl::modP(0, pl::M_Dest))) == pl::filtP(0, pl::F_Freq) + 1 && std::fabs(P(pl::modP(0, pl::M_Amount)) - 0.4f) < 1e-3f,
             "matrix slot mapped");
      bool reverb = false;
      for (int sl = 0; sl < pl::kFxSlots; ++sl)
        if (std::lround(P(pl::fxP(sl, pl::X_Type))) == pl::FxReverb) reverb = true;
      expect(reverb, "effect rack mapped");
      Out o = render(st, kPress, 24000, 256);
      expect(o.finite && o.peak > 0.01f, "imported preset sounds");
    }
    pl::ImportReport bad;
    PluginState st2;
    std::vector<uint8_t> junk = {'X', 'f', 'e', 'r', 1, 2, 3};
    expect(!pl::importSerumBytes(junk, st2, bad) && !bad.error.empty(), "damaged file is rejected with a message");
  }
  // ---- source meters: the live display channel follows an LFO, an envelope and the velocity
  {
    PluginState s = base();
    setP(s, pl::lfoP(0, pl::L_Mode), pl::LfoRetrig);
    setP(s, pl::lfoP(0, pl::L_Bpm), 0);
    setP(s, pl::lfoP(0, pl::L_Rate), 0.4f);
    setP(s, pl::envP(0, pl::E_Attack), static_cast<float>(pl::envTimeInv(0.3)));
    setP(s, pl::envP(0, pl::E_Decay), static_cast<float>(pl::envTimeInv(0.3)));
    setP(s, pl::envP(0, pl::E_Sustain), 0.5f);
    setP(s, pl::envP(0, pl::E_Release), static_cast<float>(pl::envTimeInv(0.3)));
    auto g = packs::createGenerator(s.type);
    ChannelAux aux;
    packs::prepareAux(s, aux);
    g->prepareHost(s, kSr);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    ctx.tempo = 120;
    ctx.ppq = 96;
    std::vector<float> L(256), R(256);
    pl::LiveView lv;
    float lfoMin = 2, lfoMax = -2, envPeak = 0, envAtOff = 0, envMinRel = 2, envEarlyRel = 0, envEnd = 1, velHeld = 0;
    bool lfoBip = false, envUni = false, envRises = true;
    float prevEnv = 0;
    const int total = static_cast<int>(kSr * 3), offAt = static_cast<int>(kSr * 2);
    for (int pos = 0; pos < total; pos += 256) {
      if (pos == 0) g->noteOn(1, 60, 0.6f, 0);
      if (pos == offAt) g->noteOff(1, 0);
      g->process(s, aux, ctx, L.data(), R.data(), 256);
      if (!pl::liveView(g.get(), lv)) break;
      bool bip = false;
      const float lf = pl::sourceMeterValue(pl::SrcLfo1, lv.source[pl::SrcLfo1], bip);
      lfoBip = bip;
      const float ev = pl::sourceMeterValue(pl::SrcEnv1, lv.source[pl::SrcEnv1], bip);
      envUni = !bip;
      if (pos < offAt) lfoMin = std::min(lfoMin, lf), lfoMax = std::max(lfoMax, lf);
      if (pos < static_cast<int>(kSr * 0.25)) envRises = envRises && ev >= prevEnv - 1e-6f;
      envPeak = std::max(envPeak, ev);
      if (pos + 256 >= offAt && pos < offAt) envAtOff = ev;
      if (pos > offAt + static_cast<int>(kSr * 0.1) && pos < offAt + static_cast<int>(kSr * 0.3)) envMinRel = std::min(envMinRel, ev);
      if (pos >= offAt + static_cast<int>(kSr * 0.04) && pos < offAt + static_cast<int>(kSr * 0.04) + 256) envEarlyRel = ev;
      envEnd = ev;
      prevEnv = ev;
      velHeld = lv.source[pl::SrcVelo];
    }
    std::printf("# meters: lfo %.2f..%.2f env peak %.2f at off %.2f release %.2f / %.2f end %.3f velo %.2f\n", lfoMin, lfoMax, envPeak, envAtOff, envEarlyRel, envMinRel, envEnd,
                velHeld);
    expect(lfoBip && lfoMin < -0.8f && lfoMax > 0.8f, "LFO meter swings both ways around zero");
    expect(envUni && envRises && envPeak > 0.95f, "envelope meter rises to the peak in the attack");
    expect(envAtOff > 0.2f && envEarlyRel > 0.02f && envEarlyRel < envAtOff && envMinRel < envAtOff * 0.5f, "envelope meter falls in the release");
    expect(envEnd < 0.01f, "envelope meter rests at zero after the release");
    expect(std::fabs(velHeld - 0.6f) < 0.05f, "velocity meter keeps the last note's velocity");
  }
  // ---- every parameter acts (the audit's VERIFIED entries)
  cov::run(*info);
  return finish();
}
