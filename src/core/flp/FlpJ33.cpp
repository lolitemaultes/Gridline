// Gridline — FLP import, J-33 (instrument exactness tier D): registry and the
// six-oscillator synth decoder.
//
//   Toxic Biohazard → Hex Synth (gl.hexsynth), parameters in the reference's
//   own index order (automation maps one-to-one).
//
// State layout (worked out from the reference's load routine and checked on
// all 790 shipped presets and the project states):
//   [project states only: "SSLF", u32 size, then the payload below]
//   u32 magic 'TBi2' (current) / 'TBio' (older: modulation source codes
//       renumbered s → s + 3 + (s > 2)) / 'TBiD';
//   8 × i16 modulation slot source, 8 × i16 target, 8 × f32 amount (a copy of
//       parameters 316..339, which win on load);
//   8 wave slots × (128 f32 harmonic amplitudes, 128 f32 phases in cycles)
//       (oscillators 1..6, LFO 1, LFO 2: the custom waves);
//   340 × f32 parameters;
//   294 × i8 MIDI controller link per automatable parameter (−1 = none);
//   optional 128-byte program name (NUL-terminated).
// A state that does not walk to one of these ends is refused.
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include "../../packs/synths/hex.h"
#include "../../packs/synths/pixel.h"
#include "../../packs/synths/pocket.h"
#include "../../packs/synths/samplekeys.h"
#include "../../packs/samplers/dwave.h"
#include <filesystem>
#include "../../engine/ChannelIns.h"
#include "FlpGen.h"
#include "FlpJ22.h"

namespace gl::packs::synths {
bool pocketReferenceTables();  // pocket_dsp.cpp
}

namespace gl::flp {

namespace gen {

std::string channelInsText(const std::vector<Event>& events, Bytes fallbackEnv, Bytes fallbackLevels, bool* exact) {
  InsSettings s;
  bool haveEnv = false, haveLevels = false;
  auto readTarget = [](Bytes b, InsTarget& t) {
    t.flags = b.i32(0);
    t.on = b.i32(4) != 0;
    t.predelay = b.i32(8), t.attack = b.i32(12), t.hold = b.i32(16), t.decay = b.i32(20), t.sustain = std::clamp(b.i32(24), 0, 128);
    t.release = b.i32(28), t.amount = b.i32(32);
    t.lfoDelay = b.i32(36), t.lfoAttack = b.i32(40), t.lfoAmount = b.i32(44), t.lfoSpeed = b.i32(48);
    t.lfoShape = std::clamp(b.i32(52), 0, 2);
    if (b.has(56, 12)) {
      t.tensionA = b.i32(56), t.tensionD = b.i32(60), t.tensionR = b.i32(64);
    } else {  // legacy records: flag bits for the default tensions
      t.tensionA = (t.flags & 8) ? -101 : 0;
      t.tensionD = (t.flags & 64) ? -101 : 0;
      t.tensionR = 0;
      t.flags &= ~(8 | 64);
    }
  };
  int k = 0, tracks = 0;
  for (const auto& e : events) {
    if (e.id == 218 && k < InsTargets && e.size >= 56) readTarget(Bytes(e), s.t[k++]), haveEnv = true;
    if (e.id == 219 && e.size >= 24) {
      Bytes b(e);
      s.cutoff = std::clamp(b.i32(12), 0, 256), s.reso = std::clamp(b.i32(16), 0, 256), s.type = std::clamp(b.i32(20), 0, 7);
      haveLevels = true;
    }
    if (e.id == 228 && e.size >= 16 && tracks < 2) {  // J-72: velocity, then key tracking (middle, → pan, → mod X, → mod Y)
      Bytes b(e);
      for (int j = 0; j < 4; ++j) s.track[tracks][j] = b.i32(4 * static_cast<size_t>(j));
      ++tracks;
    }
  }
  if (!haveEnv && fallbackEnv.has(0, 68 * InsTargets))
    for (int t = 0; t < InsTargets; ++t) readTarget(fallbackEnv.sub(static_cast<size_t>(t) * 68, 68), s.t[t]);
  if (!haveLevels && fallbackLevels.has(0, 24)) {  // the state copy: (0, 0, cutoff, resonance, 0, type) (J-34)
    s.cutoff = std::clamp(fallbackLevels.i32(8), 0, 256), s.reso = std::clamp(fallbackLevels.i32(12), 0, 256);
    s.type = std::clamp(fallbackLevels.i32(20), 0, 7);
  }
  if (exact) {
    *exact = true;
    const InsTarget& pan = s.t[InsPan];
    if (pan.on && pan.amount != 0) *exact = false;  // the panning envelope's routing is not confirmed
  }
  return s.active() ? formatIns(s) : std::string();
}

}  // namespace gen

namespace {

namespace hx = gl::packs::synths::hex;
using gen::Bytes;

constexpr uint32_t kTBi2 = 0x54426932, kTBio = 0x5442696F, kTBiD = 0x54426944;
constexpr size_t kHeader = 4 + 64, kWaves = 16 * 128 * 4, kParams = 340 * 4, kLinks = 294, kName = 128;
constexpr size_t kCore = kHeader + kWaves + kParams;  // 0x2594

bool decodeHex(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  const Event* ev = pluginState(evs);
  if (!ev) return false;
  Bytes b(*ev);
  if (b.n >= 8 && b.u32(0) == 0x464C5353u) {  // "SSLF" wrapper of project states
    const uint32_t size = b.u32(4);
    if (size > b.n - 8) return false;
    b = Bytes(b.p + 8, size);
  }
  uint32_t magic = b.u32(0);
  if (b.n > 0x280 && magic != kTBi2 && magic != kTBio && magic != kTBiD) {  // outer wrapper the reference skips
    b = Bytes(b.p + 0x280, b.n - 0x280);
    magic = b.u32(0);
  }
  if (magic != kTBi2 && magic != kTBio && magic != kTBiD) return false;
  if (b.n != kCore && b.n != kCore + kLinks && b.n != kCore + kLinks + kName) return false;
  PluginState s = gen::base("gl.hexsynth");
  s.params.resize(hx::HxCount);
  for (int i = 0; i < hx::HxCount; ++i) {
    const float v = b.f32(kHeader + kWaves + 4 * static_cast<size_t>(i));
    if (!std::isfinite(v)) return false;
    s.params[static_cast<size_t>(i)] = v;
  }
  if (magic == kTBio)
    for (int k = 0; k < hx::kModSlots; ++k) {
      float& src = s.params[static_cast<size_t>(hx::HxModSource + k)];
      const int code = static_cast<int>(src);
      if (code != 0) src = static_cast<float>(code + 3 + (code > 2 ? 1 : 0));
    }
  hx::HexData d;
  for (int slot = 0; slot < hx::kSlots; ++slot)
    for (int h = 0; h < hx::kHarmonics; ++h) {
      const size_t at = kHeader + (static_cast<size_t>(slot) * 2 * 128 + static_cast<size_t>(h)) * 4;
      const float a = b.f32(at), p = b.f32(at + 128 * 4);
      d.amp[slot][h] = std::isfinite(a) ? a : 0.f;
      d.phase[slot][h] = std::isfinite(p) ? p : 0.f;
    }
  if (b.n >= kCore + kLinks)
    for (size_t i = 0; i < kLinks; ++i) {
      const int8_t l = static_cast<int8_t>(b.u8(kCore + i));
      d.link[i] = l >= 0 ? l : static_cast<int8_t>(-1);
    }
  if (b.n >= kCore + kLinks + kName) {
    std::string name;
    for (size_t i = 0; i < kName && b.u8(kCore + kLinks + i); ++i) name += static_cast<char>(b.u8(kCore + kLinks + i));
    if (name != "<noname>") d.name = name;
  }
  s.data = hx::formatHexData(d);
  m.glType = "gl.hexsynth";
  m.state = s;
  // Every parameter, custom spectrum, controller link and the name carry over.
  // Wave selectors keep their index; selectors 6..32 play Gridline's own wave
  // of the same family (hexwaves_dsp.cpp) instead of reading the reference's.
  m.exact = true;
  gen::ParamTable t;
  for (int i = 0; i < hx::kAutomatable; ++i) t.lin(i, i, 0.0, 1.0);
  t.install(m);
  return true;
}

// MiniSynth → Pocket Synth (gl.pocketsynth). State: ["SSLF", u32 size] then
// the chunk file (also a .flms preset): 8-byte magic, "HEAD" u32 length, u16 1,
// u16 1, u16 n, n UTF-16 name units, "DATA" u32 length L, 4-byte version tag,
// then up to 43 f32 parameters (parameter i is present when L − 4i ≥ 8; the
// reference keeps its defaults for absent ones).
bool decodePocket(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  namespace pk = gl::packs::synths::pocket;
  const Event* ev = pluginState(evs);
  if (!ev) return false;
  Bytes b(*ev);
  if (b.n >= 8 && b.u32(0) == 0x464C5353u) {
    const uint32_t size = b.u32(4);
    if (size == 0 || size > b.n - 8) return false;
    b = Bytes(b.p + 8, size);
  }
  if (!b.has(0, 22) || b.u32(8) != 0x44414548u) return false;  // "HEAD"
  const size_t n = b.u16(20);
  const size_t at = 22 + 2 * n;
  if (!b.has(at, 8) || b.u32(at) != 0x41544144u) return false;  // "DATA"
  const uint32_t L = b.u32(at + 4);
  if (!b.has(at + 8, L)) return false;
  PluginState s = gen::base("gl.pocketsynth");
  for (int i = 0; i < pk::PkCount; ++i) {
    if (L < 4 || static_cast<int64_t>(L) - 4 * i < 8) break;
    const float v = b.f32(at + 12 + 4 * static_cast<size_t>(i));
    if (!std::isfinite(v)) return false;
    s.params[static_cast<size_t>(i)] = std::clamp(v, 0.f, 1.f);
  }
  m.glType = "gl.pocketsynth";
  m.state = s;
  m.exact = at + 8 + L == b.n && gl::packs::synths::pocketReferenceTables();
  if (!gl::packs::synths::pocketReferenceTables()) gen::note(c, "MiniSynth", "wavetable data file not found (fitted spectrum used)");
  gen::ParamTable t;
  for (int i = 0; i < pk::PkCount; ++i) t.lin(i, i, 0.0, 1.0);
  t.install(m);
  return true;
}

// Windows BMP (1/4/8/16/24/32 bit, uncompressed or 16/32-bit bitfields; both
// row orders) → top-down RGB. `exactColours` is false for 16-bit pictures (the
// reference's 5-bit → 8-bit expansion is not confirmed).
bool decodeBmp(Bytes b, gl::packs::synths::pixel::Picture& out, bool& exactColours) {
  exactColours = true;
  if (!b.has(0, 54) || b.u8(0) != 'B' || b.u8(1) != 'M') return false;
  const uint32_t dataAt = b.u32(10), hdr = b.u32(14);
  const int32_t w = b.i32(18), hs = b.i32(22);
  const int bpp = b.u16(28);
  const uint32_t comp = b.u32(30);
  if (w <= 0 || hs == 0 || w > 100000 || std::abs(hs) > 100000 || (comp != 0 && comp != 3)) return false;
  const int h = std::abs(hs);
  const bool topDown = hs < 0;
  const size_t stride = ((static_cast<size_t>(w) * static_cast<size_t>(bpp) + 31) / 32) * 4;
  if (!b.has(dataAt, stride * static_cast<size_t>(h))) return false;
  uint32_t nColors = b.has(46, 4) ? b.u32(46) : 0;
  if (bpp <= 8 && nColors == 0) nColors = 1u << bpp;
  const size_t pal = 14 + hdr;
  uint32_t rm = 0x7C00, gm = 0x03E0, bm = 0x001F;
  if (bpp == 32) rm = 0x00FF0000, gm = 0x0000FF00, bm = 0x000000FF;
  if (comp == 3 && b.has(54, 12)) rm = b.u32(54), gm = b.u32(58), bm = b.u32(62);
  auto chan = [](uint32_t v, uint32_t mask) -> uint8_t {
    if (!mask) return 0;
    int sh = 0;
    while (!((mask >> sh) & 1)) ++sh;
    int bits = 0;
    while (sh + bits < 32 && ((mask >> (sh + bits)) & 1)) ++bits;
    const uint32_t x = (v & mask) >> sh;
    if (bits >= 8) return static_cast<uint8_t>(x >> (bits - 8));
    return static_cast<uint8_t>((x << (8 - bits)) | (x >> (bits - (8 - bits) > 0 ? bits - (8 - bits) : 0)));
  };
  out.w = w, out.h = h;
  out.rgb.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 0);
  for (int y = 0; y < h; ++y) {
    const size_t row = dataAt + stride * static_cast<size_t>(topDown ? y : h - 1 - y);
    for (int x = 0; x < w; ++x) {
      uint8_t r = 0, g = 0, bl = 0;
      if (bpp == 32 || bpp == 16) {
        const uint32_t v = bpp == 32 ? b.u32(row + 4 * static_cast<size_t>(x)) : b.u16(row + 2 * static_cast<size_t>(x));
        r = chan(v, rm), g = chan(v, gm), bl = chan(v, bm);
      } else if (bpp == 24) {
        const size_t at = row + 3 * static_cast<size_t>(x);
        bl = b.u8(at), g = b.u8(at + 1), r = b.u8(at + 2);
      } else if (bpp == 8 || bpp == 4 || bpp == 1) {
        const size_t bit = static_cast<size_t>(x) * static_cast<size_t>(bpp);
        const uint8_t byte = b.u8(row + bit / 8);
        const int idx = (byte >> (8 - bpp - static_cast<int>(bit % 8))) & ((1 << bpp) - 1);
        if (static_cast<uint32_t>(idx) < nColors) {
          const size_t e = pal + 4 * static_cast<size_t>(idx);
          bl = b.u8(e), g = b.u8(e + 1), r = b.u8(e + 2);
        }
      } else {
        return false;
      }
      uint8_t* px = &out.rgb[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3];
      px[0] = r, px[1] = g, px[2] = bl;
    }
  }
  if (bpp == 16) exactColours = false;
  return true;
}

// BeepMap → Pixel Tone (gl.pixeltone). State: i32 version (0 / 1), 7 i32
// controls (tags 0..6), 7 spare i32, i32 maximum picture height; version 1:
// a 364-byte copy of the channel envelope / filter settings; then i32 size and
// a BMP file of that size.
bool decodePixel(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  namespace px = gl::packs::synths::pixel;
  const Event* ev = pluginState(evs);
  if (!ev) return false;
  Bytes b(*ev);
  const int32_t ver = b.i32(0);
  if ((ver != 0 && ver != 1) || !b.has(0, 68)) return false;
  const size_t at = ver == 0 ? 64 : 428;
  if (!b.has(at, 4)) return false;
  const uint32_t S = b.u32(at);
  if (!b.has(at + 4, S)) return false;
  PluginState s = gen::base("gl.pixeltone");
  static const int lo[7] = {24, 0, 0, 0, 0, 0, 0}, hi[7] = {9600, 4096, 2, 1, 1, 1, 1};
  for (int i = 0; i < 7; ++i) s.params[static_cast<size_t>(i)] = static_cast<float>(std::clamp(b.i32(4 + 4 * static_cast<size_t>(i)), lo[i], hi[i]));
  const int maxH = std::clamp(b.i32(60), 3, 999);
  s.params[px::PxMaxHeight] = static_cast<float>(maxH);
  px::Picture pic;
  bool exactColours = true;
  if (!decodeBmp(Bytes(b.p + at + 4, S), pic, exactColours) || pic.h < 3) return false;
  bool exact = exactColours && at + 4 + S == b.n;
  if (pic.h > maxH) {  // squeezed to the height limit (area average; the reference's filter is not confirmed)
    px::Picture q;
    q.w = pic.w, q.h = maxH;
    q.rgb.assign(static_cast<size_t>(q.w) * static_cast<size_t>(q.h) * 3, 0);
    for (int y = 0; y < q.h; ++y)
      for (int x = 0; x < q.w; ++x)
        for (int ch = 0; ch < 3; ++ch) {
          const int y0 = y * pic.h / q.h, y1 = std::max(y0 + 1, (y + 1) * pic.h / q.h);
          int acc = 0;
          for (int k = y0; k < y1; ++k) acc += pic.rgb[(static_cast<size_t>(k) * static_cast<size_t>(pic.w) + static_cast<size_t>(x)) * 3 + static_cast<size_t>(ch)];
          q.rgb[(static_cast<size_t>(y) * static_cast<size_t>(q.w) + static_cast<size_t>(x)) * 3 + static_cast<size_t>(ch)] = static_cast<uint8_t>(acc / (y1 - y0));
        }
    pic = q;
    exact = false;
  }
  // Channel envelopes / filter (the channel's records, else the copy in a
  // version 1 state) run as the channel settings; the pitch envelope / LFO
  // would retune the picture's rows, which Pixel Tone does not do.
  {
    bool insExact = true;
    const bool copy = ver == 1 && b.has(64, 364);
    s.ins = gen::channelInsText(evs, copy ? b.sub(64, 340) : Bytes(), copy ? b.sub(404, 24) : Bytes(), &insExact);
    exact = exact && insExact;
    gl::InsSettings is;
    if (!s.ins.empty() && gl::parseIns(s.ins, is) && ((is.t[gl::InsPitch].on && is.t[gl::InsPitch].amount != 0) || is.t[gl::InsPitch].lfoAmount != 0)) {
      exact = false;
      gen::note(c, "BeepMap", "channel pitch envelope / LFO not applied");
    }
  }
  s.data = px::formatPicture(pic);
  m.glType = "gl.pixeltone";
  m.state = s;
  m.exact = exact;
  if (!exactColours) gen::note(c, "BeepMap", "16-bit picture: 5-bit colour expansion approximated");
  gen::ParamTable t;
  for (int i = 0; i < 7; ++i) {
    t.lin(i, i, lo[i], hi[i]);
    t.range(i, lo[i], hi[i]);
  }
  t.install(m);
  return true;
}

}  // namespace

namespace {

// FL Keys → Sample Keys: i32 version (1; newer versions are ignored by the
// reference, which then keeps its defaults), u8-prefixed sound set name, 15
// raw knob values in tag order. The set is looked up by name in the installed
// reference content; unknown names play the default set, as in the reference.
bool decodeSampleKeys(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  namespace sk = gl::packs::synths::skeys;
  const Event* ev = pluginState(evs);
  if (!ev) return false;
  Bytes b(*ev);
  if (!b.has(0, 5)) return false;
  PluginState s = gen::base("gl.samplekeys");
  const int32_t ver = b.i32(0);
  if (ver <= 1) {
    size_t at = 0;
    const std::string name = b.pstr(4, &at);
    if (!b.has(at, 60)) return false;
    for (int i = 0; i < sk::SkCount; ++i)
      s.params[static_cast<size_t>(i)] = static_cast<float>(std::clamp(b.i32(at + 4 * static_cast<size_t>(i)), sk::kSkMin[i], sk::kSkMax[i]));
    s.data = name;
  }
  m.glType = "gl.samplekeys";
  m.state = s;
  m.exact = !sk::keySetNames().empty();
  if (!m.exact) gen::note(c, "FL Keys", "reference sound sets not installed (silent)");
  gen::ParamTable t;
  for (int i = 0; i < sk::SkCount; ++i) {
    t.lin(i, i, sk::kSkMin[i], sk::kSkMax[i]);
    t.range(i, sk::kSkMin[i], sk::kSkMax[i]);
  }
  t.install(m);
  return true;
}

}  // namespace

namespace {

// DirectWave → Wave Program: version 36..38 chunked bank (records u32 id,
// u64 size, payload; programs and zones are containers). The program that
// sounds is the one MIDI channel 0 selects.
struct DwRec {
  uint32_t id;
  Bytes d;
};
std::vector<DwRec> dwRecords(Bytes b, size_t at) {
  std::vector<DwRec> out;
  while (b.has(at, 12)) {
    const uint32_t id = b.u32(at);
    const uint64_t size = b.u64(at + 4);
    if (size > b.n - at - 12) break;
    out.push_back({id, b.sub(at + 12, static_cast<size_t>(size))});
    at += 12 + static_cast<size_t>(size);
  }
  return out;
}
std::string dwText(Bytes b) {
  std::string s;
  for (size_t i = 0; i < b.n && b.p[i]; ++i) s += static_cast<char>(b.p[i]);
  return s;
}
std::string dwPath(Context& c, std::string p) {
  for (char& ch : p)
    if (ch == '\\') ch = '/';
  const std::string tag = "%FLStudioData%";
  if (p.rfind(tag, 0) == 0) p = c.factoryData + "/Data" + p.substr(tag.size());
  p = c.resolvePath(p);
  std::error_code ec;
  if (!p.empty() && !std::filesystem::exists(p, ec) && !c.projectDir.empty()) {
    const std::string alt = c.projectDir + "/" + std::filesystem::path(p).filename().string();
    if (std::filesystem::exists(alt, ec)) return alt;
  }
  return p;
}

bool decodeWaveProgram(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  namespace w = gl::packs::samplers::dw;
  const Event* ev = pluginState(evs);
  if (!ev) return false;
  Bytes b(*ev);
  if (!b.has(0, 4)) return false;
  const uint32_t ver = b.u32(0);
  if (ver < 36 || ver > 38) return false;
  w::DwProgram prog;
  PluginState s = gen::base("gl.waveprogram");
  bool exact = true, monolithic = false;
  std::vector<std::string> why;
  int chanProgram = 0;
  std::vector<DwRec> progs;
  for (const DwRec& r : dwRecords(b, 4)) {
    if (r.id == 6 && r.d.has(0, 4)) {
      const uint32_t f = r.d.u32(0);
      prog.clipVolume = f & 1, prog.compatVelocity = f & 2;
      if (f & 8) return false;  // demo-mode state: the reference refuses it
      monolithic = (f & 0x30) != 0;
    } else if (r.id == 518 && r.d.has(0, 6)) {
      prog.sinc = r.d.i32(1) != 0;
      prog.hq = r.d.u8(5) != 0;
    } else if (r.id == 5 && r.d.has(0, 0x248)) {
      if (r.d.u32(0) != 0) continue;
      chanProgram = r.d.u8(0x208);
      prog.bendRange = r.d.i32(0x214);
      prog.cc1 = r.d.f32(0x238), prog.cc7 = r.d.f32(0x23C), prog.cc10 = r.d.f32(0x240), prog.cc11 = r.d.f32(0x244);
    } else if (r.id == 1) {
      progs.push_back(r);
    }
  }
  const DwRec* pr = nullptr;
  for (const DwRec& r : progs) {
    for (const DwRec& h : dwRecords(r.d, 0))
      if (!pr && h.id == 100 && h.d.has(0, 4) && static_cast<int>(h.d.u32(0)) == chanProgram) pr = &r;
  }
  if (!pr) return false;
  auto setT = [&](int tag, float v) { s.params[static_cast<size_t>(tag)] = std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f; };
  int lfoK = 0, modK = 0;
  for (const DwRec& r : dwRecords(pr->d, 0)) {
    Bytes d = r.d;
    switch (r.id) {
      case 100:
        if (d.has(0, 18)) {
          prog.playMode = std::clamp<int>(d.u8(4), 0, 2), prog.glideAuto = d.u8(5) != 0;
          setT(w::DwMaster, d.f32(6)), setT(w::DwGlide, d.f32(10));
        }
        break;
      case 102: prog.name = dwText(d); break;
      case 104:
        if (d.has(0, 10)) prog.driveOn[0] = d.u8(0), prog.driveOn[1] = d.u8(1), setT(w::DwDriveA, d.f32(2)), setT(w::DwDriveB, d.f32(6));
        break;
      case 105:
        if (d.has(0, 18)) {
          prog.delayOn = d.u8(0), prog.pingPong = d.u8(1) != 0;
          for (int k = 0; k < 4; ++k) setT(w::DwDelayTime + k, d.f32(2 + 4 * static_cast<size_t>(k)));
        }
        break;
      case 106:
        if (d.has(0, 17)) {
          prog.revOn = d.u8(0);
          for (int k = 0; k < 4; ++k) setT(w::DwRevRoom + k, d.f32(1 + 4 * static_cast<size_t>(k)));
        }
        break;
      case 107:
        if (d.has(0, 17)) {
          prog.choOn = d.u8(0);
          for (int k = 0; k < 4; ++k) setT(w::DwChoDelay + k, d.f32(1 + 4 * static_cast<size_t>(k)));
        }
        break;
      case 108:
        if (d.has(0, 20) && lfoK < 2) {
          prog.plfo[lfoK].wave = std::clamp<int>(d.u8(0), 0, 7), prog.plfo[lfoK].sync = d.u8(1) != 0;
          setT(lfoK ? w::DwLfo2Rate : w::DwLfo1Rate, d.f32(4));
          setT(lfoK ? w::DwLfo2Phase : w::DwLfo1Phase, d.f32(8));
          setT(lfoK ? w::DwLfo2Attack : w::DwLfo1Attack, d.f32(12));
          ++lfoK;
        }
        break;
      case 109:
        if (d.n == 4 && modK < 4) setT(w::DwModVal1 + modK++, d.f32(0));
        break;
      case 110:
        if (d.has(0, 13)) {
          const uint32_t g = d.u32(0);
          if (g >= 1 && g <= 99) {
            w::DwTrigger& t = prog.trig[g];
            const int ty = d.u8(4);
            if (ty <= 3) t.type = ty;
            t.prob = d.f32(5), t.overlap = d.f32(9);
          }
        }
        break;
      case 3: {
        w::DwZone z;
        bool embedded = false;
        int f507 = 0, f514 = 0, f515 = 0, f516 = 0;
        for (const DwRec& zr : dwRecords(d, 0)) {
          Bytes q = zr.d;
          switch (zr.id) {
            case 500:
              if (q.has(0, 25)) {
                z.root = q.u8(0), z.lo = q.u8(1), z.hi = q.u8(2), z.vlo = q.u8(3), z.vhi = q.u8(4), z.muteGroup = q.u8(5);
                z.parallel = q.u8(8) != 0, z.gain = q.f32(9), z.pan = q.f32(13), z.trigGroup = std::clamp(q.i32(17), 0, 99);
              }
              break;
            case 501: z.name = dwText(q); break;
            case 502: z.path = dwPath(c, dwText(q)); break;
            case 503:
              if (q.has(0, 32)) {
                z.ticks = q.i32(12), z.rate = q.f32(16), z.loopMode = std::clamp(q.i32(20), 0, 4);
                z.loopStart = q.i32(24), z.loopEnd = q.i32(28), z.sampleStart = q.has(32, 4) ? q.i32(32) : 0;
              }
              break;
            case 504:
              if (q.has(0, 8)) z.tune = q.f32(0), z.transpose = static_cast<int8_t>(q.u8(4)), z.fine = static_cast<int8_t>(q.u8(5)), z.keytrack = static_cast<int16_t>(q.u16(6));
              break;
            case 505:
              if (q.has(0, 14)) z.ts = q.u8(0), z.tsSync = q.u8(1), z.tsTime = q.f32(2), z.tsGrain = q.f32(6), z.tsSmooth = q.f32(10);
              break;
            case 506:
              if (q.has(0, 16))
                for (int k = 0; k < 4; ++k) z.send[k] = q.f32(4 * static_cast<size_t>(k));
              break;
            case 507:
              if (q.has(0, 16) && f507 < 2) {
                w::DwFilter& f = z.f[f507++];
                f.type = std::clamp<int>(q.u8(0), 0, 7), f.cut = q.f32(4), f.res = q.f32(8), f.shape = q.f32(12);
              }
              break;
            case 508:
              if (q.has(0, 2)) z.beatSync = q.u8(0), z.waveSlice = q.u8(1);
              break;
            case 509:
              if (q.has(0, 16))
                for (int k = 0; k < 4; ++k) z.amp[k] = q.f32(4 * static_cast<size_t>(k));
              break;
            case 510: case 511: case 512: case 513:
              if (q.has(0, 9)) {
                const int k = static_cast<int>(zr.id) - 510;
                z.fxOn[k] = q.u8(0), z.fxA[k] = q.f32(1), z.fxB[k] = q.f32(5);
              }
              break;
            case 514:
              if (q.has(0, 16) && f514 < 2) {
                for (int k = 0; k < 4; ++k) z.menv[f514][k] = q.f32(4 * static_cast<size_t>(k));
                ++f514;
              }
              break;
            case 515:
              if (q.has(0, 20) && f515 < 2) {
                w::DwLfo& l = z.lfo[f515++];
                l.wave = std::clamp<int>(q.u8(0), 0, 7), l.sync = q.u8(1), l.rate = q.f32(4), l.phase = q.f32(8), l.attack = q.f32(12);
              }
              break;
            case 516:
              if (q.has(0, 8) && f516 < 16) {
                w::DwSlot& sl = z.slot[f516++];
                sl.src = q.u16(0), sl.dst = q.u16(2), sl.amt = q.f32(4);
              }
              break;
            case 517: case 518: embedded = true; break;
            default: break;
          }
        }
        std::error_code ec;
        if (embedded || monolithic) exact = false, why.push_back("embedded sample data");
        else if (z.path.empty() || !std::filesystem::exists(z.path, ec)) exact = false, why.push_back("sample not found: " + z.path);
        if (z.ts) exact = false, why.push_back("time stretch");
        if (z.f[0].type > 5 || z.f[1].type > 5) exact = false, why.push_back("ladder / vowel filter");
        if (z.fxOn[3]) exact = false, why.push_back("phaser constants");
        prog.zones.push_back(std::move(z));
        break;
      }
      default: break;
    }
  }
  if (prog.zones.empty()) return false;
  if (prog.pingPong && prog.delayOn) exact = false, why.push_back("ping-pong delay input");
  if (prog.sinc) exact = false, why.push_back("sinc interpolation");
  s.data = w::formatProgram(prog);
  m.glType = "gl.waveprogram";
  m.state = s;
  m.exact = exact;
  if (!exact) gen::note(c, "DirectWave", why.front());
  gen::ParamTable t;
  for (int i = 0; i < w::DwCount; ++i) t.lin(i, i, 0.0, 1.0);
  t.install(m);
  return true;
}

}  // namespace

void registerDecodersJ33(std::map<std::string, PluginDecoder>& d) {
  j22::set(d, "DirectWave", decodeWaveProgram);
  j22::set(d, "FL Keys", decodeSampleKeys);
  j22::set(d, "Toxic Biohazard", decodeHex);
  j22::set(d, "MiniSynth", decodePocket);
  j22::set(d, "BeepMap", decodePixel);
}

}  // namespace gl::flp
