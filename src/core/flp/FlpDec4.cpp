// Gridline — FLP import, fourth wave (J-18 coverage sweep): the stock plugins
// that still had no decoder, mapped to their Gridline equivalents.
//
//   Fruity Big Clock          → gl.clock     state: 1 byte, display colour scheme
//                                            (styling only → exact)
//   VFX Color Mapper          → gl.colormap  i32 version, 16 × i32 note colour →
//                                            output port, 16 × i32 MIDI channel map
//   VFX Level Scaler          → gl.velscale  (settings not converted: defaults)
//   VFX Sequencer             → gl.notesq    (pattern bank not converted: defaults)
//   Dashboard                 → gl.surface   i32 version, …, @28 u32 N, N × u32
//                                            values (0..65536; value 0 belongs to the
//                                            panel), u32 control count, control
//                                            records: 32-byte Pascal name, 16 bytes,
//                                            32-byte Pascal caption, …
//   Soundfont Player          → gl.multisampler (the .sf2 path found in the state)
//   FL Studio Mobile Rack FX  → gl.pocketrack (J-42: up to four modules, reference engines)
//                                (chunked "SSLF" state; "RMOd" chunk: u32 size,
//                                u32 module type, u32 instance)
//   Fruity Mute 2             → gl.mute       (no shipped state: defaults)
//   Fruity Phase Inverter     → gl.utility    State knob 0..1024 (default 1024 = inverted)
//   Wasp, Wasp XT             → gl.analog     (no shipped state: defaults)
//   FL Slayer                 → gl.string     (no shipped state: defaults)
//   Fruity LSD                → gl.midiout    (General MIDI on the system synth → a MIDI Out channel)
//
// Layouts were read from FL Studio's own plugin database and preset files
// (analysis/demos/COVERAGE.md lists the evidence); no Image-Line code is used.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/utility/utility.h"
#include "../PluginParams.h"
#include "FlpGen2.h"

namespace gl::flp {

namespace {

using gen2::begin;
using gen2::Blob;
using gen2::set;

// Plugins whose state Gridline cannot read (none shipped to verify against):
// a default instance of the equivalent, flagged as approximate.
PluginDecoder defaults(const char* glType, const char* what) {
  return [glType, what](Context& c, const std::vector<Event>&, PluginMapping& m) {
    begin(m, glType);
    m.exact = false;
    c.report.warn(std::string(what) + ": settings not converted (Gridline default used)");
    return true;
  };
}

bool decodeBigClock(Context&, const std::vector<Event>&, PluginMapping& m) {
  begin(m, "gl.clock");
  set(m, packs::utility::CkDisplay, 0);  // song position + clock time, like FL's two displays
  m.exact = true;                         // the only stored setting is the colour scheme
  return true;
}

bool decodeColorMapper(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  namespace md = packs::modulation;
  begin(m, "gl.colormap");
  set(m, md::CmSource, 0);  // group by note colour
  bool exact = true;
  if (b.size() >= 4 + 16 * 4) {
    for (int k = 0; k < md::kColorGroups && k < 16; ++k) {
      int port = b.i32(4 + 4 * static_cast<size_t>(k));
      // FL port p → Gridline target p + 1 (targets 1..8); higher ports have no target.
      if (port < 0 || port > 7) exact = false;
      set(m, md::CmMap + k, std::clamp(port, 0, 7) + 1);
    }
    for (int k = 0; k < 16 && b.has(68 + 4 * static_cast<size_t>(k), 4); ++k)
      if (b.i32(68 + 4 * static_cast<size_t>(k)) != k) exact = false;  // MIDI channel remapping: not modelled
  } else {
    exact = false;
  }
  m.exact = exact;
  return true;
}

bool decodeDashboard(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  namespace md = packs::modulation;
  begin(m, "gl.surface");
  m.exact = false;
  if (b.size() < 32) return true;
  const DashPanel d = parseDashboard(b.p, b.size());
  if (!d.ok) {
    c.report.warn("Dashboard: panel layout not recognised; controls not converted");
    return true;
  }
  // J-45: each controller link is one Gridline control (value lo..hi → 0..1),
  // its kind from the panel control it belongs to.
  std::string data;
  bool exact = d.links.size() <= static_cast<size_t>(md::kSurfaceControls);
  for (size_t k = 0; k < d.links.size() && k < static_cast<size_t>(md::kSurfaceControls); ++k) {
    const DashLink& l = d.links[k];
    const double v = l.valueAt ? static_cast<double>(b.u32(l.valueAt)) : 0.0;
    const double span = static_cast<double>(l.hi) - static_cast<double>(l.lo);
    set(m, static_cast<int>(k), span > 0 ? std::clamp((v - l.lo) / span, 0.0, 1.0) : 0.0);
    const int kind = k < d.kinds.size() ? d.kinds[k] : 2;
    const int gk = kind == 1 ? md::SkSlider : kind == 4 ? md::SkToggle : md::SkKnob;
    std::string name = l.name;
    for (char& ch : name)
      if (ch == '\n' || ch == '\r') ch = ' ';
    data += std::to_string(gk) + " " + name + "\n";
    exact = exact && l.type == 4 && l.lo == 0 && l.hi == 65536 && l.valueAt;  // MIDI CC links send to a device instead
  }
  m.state.data = data;
  // The panel's own artwork is not reproduced (display only); its controls and values are.
  m.exact = exact;
  if (d.links.size() > static_cast<size_t>(md::kSurfaceControls)) c.report.warn("Dashboard: more than 16 controls, extra ones dropped");
  return true;
}

}  // namespace

namespace {
uint32_t le32at(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24; }
// Tagged chunks {4-char tag, u32 length} in [at, end): the payload offset of each tag's k-th chunk.
template <class F>
void eachTag(const uint8_t* p, size_t at, size_t end, F&& f) {
  while (at + 8 <= end) {
    const size_t len = le32at(p + at + 4);
    if (len > end - at - 8) return;
    f(std::string(reinterpret_cast<const char*>(p + at), 4), at + 8, len);
    at += 8 + len;
  }
}
}  // namespace

SeqBank parseSeqBank(const uint8_t* p, size_t n) {
  SeqBank s;
  if (n < 36 || le32at(p) != 2 || le32at(p + 4) != 1) return s;
  const size_t bankEnd = std::min(n, static_cast<size_t>(16) + le32at(p + 8));
  if (le32at(p + 16) != 1 || std::memcmp(p + 20, "bbnk", 4) != 0) return s;
  const size_t bb = 28, bbEnd = std::min(bankEnd, bb + le32at(p + 24));
  std::vector<std::pair<size_t, size_t>> progs;
  eachTag(p, bb, bbEnd, [&](const std::string& tag, size_t at, size_t len) {
    if (tag == "fixp" && len >= 20) s.fixp = at;
    if (tag == "fpgp" && len >= 42) s.fpgp = at;
    if (tag == "prog") progs.push_back({at, len});
  });
  if (!s.fixp || !s.fpgp || progs.empty()) return s;
  s.program = static_cast<int8_t>(p[s.fixp + 8]);
  if (s.program < 0 || static_cast<size_t>(s.program) >= progs.size()) return s;
  const auto [pa, pl] = progs[static_cast<size_t>(s.program)];
  int lanes = 0;
  eachTag(p, pa, pa + pl, [&](const std::string& tag, size_t at, size_t len) {
    if (tag == "prgp" && len >= 42) s.prgp = at;
    if (tag.compare(0, 3, "stp") == 0 && tag[3] >= '0' && tag[3] <= '5' && len >= 64) s.lanes[tag[3] - '0'] = at, ++lanes;
  });
  s.ok = s.prgp && lanes == 6;
  return s;
}

namespace {

// FL step-sync index (beats per step 1/16, 1/12, 1/8, 1/6, 1/4, 1/3, 1/2, 2/3,
// 1, 3/16, 3/8, 3/4, 3/2) → the Gridline rate choice (-1: none).
int seqRateOf(int sync) {
  static const int map[13] = {-1, -1, 5, 8, 4, 7, 3, 6, 2, -1, 11, 10, 9};
  return sync >= 0 && sync < 13 ? map[sync] : -1;
}

// VFX Sequencer (J-45): the current program of the bank becomes the step
// pattern (first 16 steps): step type 0 rest, 2 tie, 4 chord (all held keys),
// others a note; key bits b >= 2 = held key b - 2 (sorted by pitch), octave bit
// b = octave b - 3, velocity lane / 127, gate = gate % × 2^lane / 100.
bool decodeSequencer(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  namespace md = packs::modulation;
  begin(m, "gl.notesq");
  m.exact = false;  // key substitution, note order, swing and the randomisers are not modelled
  const SeqBank s = parseSeqBank(b.p, b.size());
  if (!s.ok) {
    c.report.warn("VFX Sequencer: bank not recognised (defaults used)");
    return true;
  }
  const uint8_t* g = b.p + s.prgp;
  const int steps = g[1], gate = std::max<int>(1, g[2]);
  set(m, md::SqThru, steps == 0 ? 1 : 0);
  set(m, md::SqLength, std::clamp(steps, 1, md::kSeqSteps));
  const int rate = seqRateOf(g[3]);
  set(m, md::SqRate, rate < 0 ? 4 : rate);
  set(m, md::SqTranspose, std::clamp(12 * static_cast<int8_t>(g[12]) + static_cast<int8_t>(b.p[s.fpgp + 14]), -24, 24));
  for (int i = 0; i < md::kSeqSteps; ++i) {
    auto lane = [&](int k) { return b.p[s.lanes[k] + static_cast<size_t>(i)]; };
    const int type = lane(2), keys = lane(3), oct = lane(1);
    set(m, md::seqParam(i, md::SqOn), type != 0 && type != 2 ? 1 : 0);
    set(m, md::seqParam(i, md::SqTie), type == 2 ? 1 : 0);
    int note = 0;
    for (int bit = 2; bit < 8; ++bit)
      if (keys & (1 << bit)) {
        note = bit - 2;
        break;
      }
    set(m, md::seqParam(i, md::SqNote), type == 4 ? 8 : std::min(note, 5));
    int o = 0;
    for (int bit = 0; bit < 8; ++bit)
      if (oct & (1 << bit)) {
        o = bit - 3;
        break;
      }
    set(m, md::seqParam(i, md::SqOctave), std::clamp(o, -2, 2));
    set(m, md::seqParam(i, md::SqVelocity), std::clamp(lane(4) / 127.0, 0.0, 1.0));
    const int ge = static_cast<int8_t>(lane(5));
    set(m, md::seqParam(i, md::SqGate), std::clamp(gate * std::pow(2.0, ge) / 100.0, 0.05, 1.0));
  }
  return true;
}

// VFX Level Scaler (J-45): u32 version 1, then chunks {u32 id, u64 length}:
// 1002 = u8 editor tab, 1001 = 6 targets (velocity, release, pitch, pan,
// mod X, mod Y) × 6 i32: centre 0..8192, multiply ±16384 (8192 = ×1),
// offset ±8192, tension ±8192, humanize 0..16384, humanize mode 0..2.
bool decodeLevelScaler(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  namespace md = packs::modulation;
  begin(m, "gl.velscale");
  set(m, md::VsModel, 1);
  size_t at = 4, data = 0;
  while (b.has(at, 12)) {
    const uint32_t id = b.u32(at);
    const uint64_t len = static_cast<uint64_t>(b.u32(at + 4)) | static_cast<uint64_t>(b.u32(at + 8)) << 32;
    at += 12;
    if (len > b.size() - at) break;
    if (id == 1001 && len >= 144) data = at;
    at += static_cast<size_t>(len);
  }
  if (b.u32(0) != 1 || !data) {
    m.exact = false;
    c.report.warn("VFX Level Scaler: settings not recognised (defaults used)");
    return true;
  }
  bool humanized = false;
  for (int t = 0; t < md::kVsTargets; ++t) {
    auto v = [&](int f) { return static_cast<double>(b.i32(data + 24 * static_cast<size_t>(t) + 4 * static_cast<size_t>(f))); };
    set(m, md::vsRef(t, md::VfCenter), std::clamp(v(0) / 8192.0, 0.0, 1.0));
    set(m, md::vsRef(t, md::VfMultiply), std::clamp(v(1) / 8192.0, -2.0, 2.0));
    set(m, md::vsRef(t, md::VfOffset), std::clamp(v(2) / 8192.0, -1.0, 1.0));
    set(m, md::vsRef(t, md::VfTension), std::clamp(v(3) / 8192.0, -1.0, 1.0));
    set(m, md::vsRef(t, md::VfHumanize), std::clamp(v(4) / 16384.0, 0.0, 1.0));
    set(m, md::vsRef(t, md::VfMode), std::clamp(v(5), 0.0, 2.0));
    humanized = humanized || v(4) != 0;
  }
  // Every law is the reference's; humanize draws from the player's own random numbers.
  m.exact = !humanized;
  return true;
}

bool decodeSoundfont(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  begin(m, "gl.multisampler");
  m.exact = false;  // bank / program, envelope, LFO, reverb and chorus sends are not converted
  // The SoundFont path: ASCII or UTF-16 text ending in ".sf2".
  std::string path;
  for (size_t at = 0; at + 4 <= b.size() && path.empty(); ++at) {
    auto lower = [&](size_t i) { return static_cast<char>(std::tolower(b.u8(i))); };
    bool ascii = lower(at) == '.' && lower(at + 1) == 's' && lower(at + 2) == 'f' && b.u8(at + 3) == '2';
    bool wide = b.has(at, 8) && lower(at) == '.' && b.u8(at + 1) == 0 && lower(at + 2) == 's' && b.u8(at + 3) == 0 &&
                lower(at + 4) == 'f' && b.u8(at + 5) == 0 && b.u8(at + 6) == '2' && b.u8(at + 7) == 0;
    if (!ascii && !wide) continue;
    const size_t step = wide ? 2 : 1, end = at + 4 * step;
    size_t s = at;
    while (s >= step && b.u8(s - step) >= 0x20 && (!wide || b.u8(s - step + 1) == 0)) s -= step;
    for (size_t i = s; i < end; i += step) path += static_cast<char>(b.u8(i));
  }
  if (!path.empty()) {
    path = c.resolvePath(path);
    m.state.data = "#gl-zones 1\nsf2 0 0 - | " + path + "\n";
  } else {
    c.report.warn("Soundfont Player: no SoundFont file in the saved state");
  }
  return true;
}


// One rack module: type, PRMS (index 0 = enable) and SMPR (post gain, mix, wet pan).
struct MobileModule {
  uint32_t type = 0;
  std::vector<float> prms, smpr;
};
std::vector<MobileModule> mobileModules(const Blob& b) {
  std::vector<MobileModule> out;
  for (size_t at = b.find("RMOd"); at != std::string::npos && b.has(at, 16); at = b.find("RMOd", at + 4)) {
    MobileModule mm;
    mm.type = b.u32(at + 8);
    const size_t end = std::min(b.size(), at + 8 + static_cast<size_t>(b.u32(at + 4)));
    for (size_t q = at + 16; q + 8 <= end;) {
      const uint32_t sz = b.u32(q + 4);
      const bool prms = b.u8(q) == 'P' && b.u8(q + 1) == 'R' && b.u8(q + 2) == 'M' && b.u8(q + 3) == 'S';
      const bool smpr = b.u8(q) == 'S' && b.u8(q + 1) == 'M' && b.u8(q + 2) == 'P' && b.u8(q + 3) == 'R';
      if (prms || smpr)
        for (size_t k = 0; k + 4 <= sz && q + 8 + k + 4 <= end; k += 4) (prms ? mm.prms : mm.smpr).push_back(b.f32(q + 8 + k));
      q += 8 + sz;
    }
    out.push_back(mm);
  }
  return out;
}

// J-42: the rack's modules → gl.pocketrack slots (reference module engines,
// pocket_mods.h). Up to four modules; each keeps its stored parameters (indices
// past the stored ones take the module's defaults) and its post gain / mix /
// wet balance. Exact when every module's engine is a reference model.
bool decodeMobileRackFx(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  namespace dy = packs::dynamics;
  Blob b = gen2::stateBlob(evs);
  std::vector<MobileModule> modules = mobileModules(b);
  begin(m, "gl.pocketrack");
  bool exact = modules.size() <= static_cast<size_t>(dy::kPkSlots);
  for (int s = 0; s < dy::kPkSlots; ++s) {
    const bool have = s < static_cast<int>(modules.size());
    const int type = have ? static_cast<int>(modules[static_cast<size_t>(s)].type) : 0;
    const bool known = type > 0 && type < dy::kPkTypes && *dy::pocketModuleName(type);
    set(m, dy::pkParam(s, dy::PkType), known ? type : 0);
    float def[dy::kPkParams];
    dy::pocketModuleDefaults(known ? type : 0, def);
    float p[dy::kPkParams];
    for (int i = 0; i < dy::kPkParams; ++i) {
      const auto& pr = have ? modules[static_cast<size_t>(s)].prms : std::vector<float>{};
      p[i] = i < static_cast<int>(pr.size()) && std::isfinite(pr[static_cast<size_t>(i)]) ? pr[static_cast<size_t>(i)] : def[i];
      set(m, dy::pkParam(s, dy::PkP0 + i), p[i]);
    }
    const auto& sm = have ? modules[static_cast<size_t>(s)].smpr : std::vector<float>{};
    set(m, dy::pkParam(s, dy::PkPost), sm.size() > 0 ? sm[0] : 0.5f);
    set(m, dy::pkParam(s, dy::PkMix), sm.size() > 1 ? sm[1] : 1.f);
    set(m, dy::pkParam(s, dy::PkPan), sm.size() > 2 ? sm[2] : 0.5f);
    if (have && !known) {
      exact = false;
      c.report.warn("FL Studio Mobile rack: module type " + std::to_string(type) + " not available");
    }
    if (have && known && p[0] >= 0.5f && !dy::pocketModuleExact(type, p)) exact = false;
  }
  if (modules.size() > static_cast<size_t>(dy::kPkSlots))
    c.report.warn("FL Studio Mobile rack: only the first " + std::to_string(dy::kPkSlots) + " of " + std::to_string(modules.size()) + " modules imported");
  m.exact = exact;
  return true;
}

bool decodePhaseInverter(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  begin(m, "gl.utility");
  // Native plugin state: i32 version, then one i32 per parameter (State 0..1024).
  int32_t state = b.size() >= 8 ? b.i32(4) : 1024;
  set(m, pp::UtlInvert, state >= 512 ? 1 : 0);
  m.exact = b.size() >= 8;
  return true;
}

}  // namespace

namespace {
struct DashReader {
  const uint8_t* p;
  size_t n, at = 0;
  bool bad = false;
  bool has(size_t k) const { return !bad && at + k <= n; }
  uint32_t u32() {
    if (!has(4)) return bad = true, 0;
    uint32_t v = static_cast<uint32_t>(p[at]) | static_cast<uint32_t>(p[at + 1]) << 8 | static_cast<uint32_t>(p[at + 2]) << 16 | static_cast<uint32_t>(p[at + 3]) << 24;
    at += 4;
    return v;
  }
  uint8_t u8() {
    if (!has(1)) return bad = true, 0;
    return p[at++];
  }
  std::string pstr() {
    const size_t len = u8();
    if (!has(len)) return bad = true, std::string();
    std::string s(reinterpret_cast<const char*>(p + at), len);
    at += len;
    return s;
  }
  std::string fixed(size_t size) {  // length byte + text, padded to size
    if (!has(size)) return bad = true, std::string();
    const size_t len = std::min<size_t>(p[at], size - 1);
    std::string s(reinterpret_cast<const char*>(p + at + 1), len);
    at += size;
    return s;
  }
};
}  // namespace

DashPanel parseDashboard(const uint8_t* p, size_t n) {
  DashPanel d;
  DashReader r{p, n};
  r.at = 28;
  const uint32_t nv = r.u32();
  if (r.bad || nv > 4096 || !r.has(4 * static_cast<size_t>(nv))) return d;
  const size_t values = r.at;
  r.at += 4 * static_cast<size_t>(nv);
  const uint32_t nl = r.u32();
  if (r.bad || nl > 4096 || !r.has(81 * static_cast<size_t>(nl))) return d;
  for (uint32_t i = 0; i < nl; ++i) {
    DashLink l;
    l.name = r.fixed(48);
    r.fixed(16);
    l.target = r.u32(), l.type = r.u32(), l.lo = r.u32(), l.hi = r.u32();
    r.u8();
    // An internal link's value sits at its target index; a MIDI link's (target = CC) at its own.
    const uint32_t idx = l.type == 4 ? (l.target & 0x7FFFFFFFu) : i;
    if (1 + static_cast<size_t>(idx) < nv) l.valueAt = values + 4 * (1 + static_cast<size_t>(idx));
    d.links.push_back(l);
  }
  const uint32_t pver = r.u32(), psize = r.u32();
  if (r.bad || !r.has(psize)) return d;
  const size_t pend = r.at + psize;
  if (pver >= 4) {
    r.pstr(), r.pstr();
    r.at += 2 + 4 + 4 + 4 + 4;  // grid flags, grid colour, width, height, constant
    r.pstr();
    r.at += 4 + 4 + 8 + 4 + 64;  // label size, colour, flags, -1, reserved
    const uint32_t nc = r.u32();
    for (uint32_t k = 0; k < nc && !r.bad && nc < 4096; ++k) {
      const int kind = r.u8();
      r.pstr();
      const uint32_t body = r.u32();
      if (!r.has(static_cast<size_t>(body) + 16)) return d;
      r.at += body + 16;
      if (kind == 0 || kind == 1 || kind == 2 || kind == 4 || kind == 7) d.kinds.push_back(kind);
    }
    if (r.bad || r.at != pend) return d;
  }
  d.ok = true;
  return d;
}

std::vector<uint8_t> buildDashboard(const std::vector<std::pair<int, std::string>>& controls, const std::vector<double>& values) {
  std::vector<uint8_t> o;
  auto u32 = [](std::vector<uint8_t>& b, int64_t v) {
    const uint32_t x = static_cast<uint32_t>(v);
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(x >> (8 * k)));
  };
  auto pstr = [](std::vector<uint8_t>& b, const std::string& s) {
    const std::string t = s.substr(0, 255);
    b.push_back(static_cast<uint8_t>(t.size()));
    b.insert(b.end(), t.begin(), t.end());
  };
  auto fixed = [](std::vector<uint8_t>& b, const std::string& s, size_t size) {
    const std::string t = s.substr(0, size - 1);
    b.push_back(static_cast<uint8_t>(t.size()));
    b.insert(b.end(), t.begin(), t.end());
    b.insert(b.end(), size - 1 - t.size(), 0);
  };
  auto hex = [](std::vector<uint8_t>& b, const char* h) {
    for (; h[0] && h[1]; h += 2) b.push_back(static_cast<uint8_t>(std::stoi(std::string(h, 2), nullptr, 16)));
  };
  const size_t n = controls.size();
  for (int64_t v : {2, 0, -1, -1, -1, 0, 0}) u32(o, v);
  u32(o, static_cast<int64_t>(n) + 1);
  u32(o, 0);
  for (size_t k = 0; k < n; ++k)
    u32(o, std::lround(std::clamp(k < values.size() ? values[k] : 0.0, 0.0, 1.0) * 65536.0));
  u32(o, static_cast<int64_t>(n));
  for (size_t k = 0; k < n; ++k) {
    fixed(o, controls[k].second, 48);
    fixed(o, controls[k].second, 16);
    u32(o, static_cast<int64_t>(k));
    u32(o, 4);
    u32(o, 0);
    u32(o, 65536);
    o.push_back(0);
  }
  // Panel: default artwork; controls in rows (sliders 35 px apart, others 40).
  struct Look {
    const char* ini;
    int w, h;
    const char* common;
    const char* spec;
  };
  static const Look slider{"Gray (22x100)", 22, 100, "ffff00000000", "0100000000200000"};
  static const Look wheel{"Gray dull (30x30)", 30, 30, "ffff01000000", "01000000dae3e900ff8000000020000000"};
  static const Look sw{"Button switch", 29, 21, "ffff00000000", "000000000055004000"};
  bool allSliders = n > 0;
  for (const auto& c : controls) allSliders = allSliders && c.first == 1;
  const size_t perRow = allSliders ? 10 : 8;
  std::vector<uint8_t> recs;
  int x = 10, y = 10, rowH = 0, right = 0;
  for (size_t k = 0; k < n; ++k) {
    if (k > 0 && k % perRow == 0) y += rowH + 25, x = 10, rowH = 0;
    const Look& L = controls[k].first == 1 ? slider : controls[k].first == 4 ? sw : wheel;
    std::vector<uint8_t> body;
    pstr(body, controls[k].second);
    pstr(body, controls[k].second);
    for (int v : {x, y, L.w, L.h}) u32(body, v);
    hex(body, L.common);
    hex(body, L.spec);
    recs.push_back(static_cast<uint8_t>(controls[k].first == 1 ? 1 : controls[k].first == 4 ? 4 : 2));
    pstr(recs, L.ini);
    u32(recs, static_cast<int64_t>(body.size()));
    recs.insert(recs.end(), body.begin(), body.end());
    recs.insert(recs.end(), 16, 0);
    right = std::max(right, x + L.w);
    rowH = std::max(rowH, L.h);
    x += controls[k].first == 1 ? 35 : 40;
  }
  const int pw = std::max(340, right + 13), ph = n ? y + rowH + 25 : 200;
  std::vector<uint8_t> panel;
  pstr(panel, "Gray (resizeable)");
  pstr(panel, "Default\\.\\Gray Panel.bmp");
  panel.push_back(n ? 3 : 2);
  panel.push_back(5);
  u32(panel, 0x404040);
  u32(panel, pw);
  u32(panel, ph);
  u32(panel, 23);
  pstr(panel, "Fruity fonts");
  u32(panel, 7);
  u32(panel, 0xFFFFFF);
  panel.insert(panel.end(), 8, 0);
  u32(panel, -1);
  panel.insert(panel.end(), 64, 0);
  u32(panel, static_cast<int64_t>(n));
  panel.insert(panel.end(), recs.begin(), recs.end());
  u32(o, 4);
  u32(o, static_cast<int64_t>(panel.size()));
  o.insert(o.end(), panel.begin(), panel.end());
  u32(o, 0);
  return o;
}

void registerDecoders4(std::map<std::string, PluginDecoder>& d) {
  auto add = [&](const char* name, PluginDecoder f) { d.emplace(name, std::move(f)); };  // never overrides
  add("Fruity Big Clock", decodeBigClock);
  add("VFX Color Mapper", decodeColorMapper);
  add("VFX Level Scaler", decodeLevelScaler);
  add("VFX Sequencer", decodeSequencer);
  add("Dashboard", decodeDashboard);
  add("Soundfont Player", decodeSoundfont);
  add("FL Studio Mobile Rack FX", decodeMobileRackFx);
  add("Fruity Mute 2", defaults("gl.mute", "Fruity Mute 2"));
  add("Fruity Phase Inverter", decodePhaseInverter);
  add("Wasp", defaults("gl.analog", "Wasp"));
  add("Wasp XT", defaults("gl.analog", "Wasp XT"));
  add("FL Slayer", defaults("gl.string", "FL Slayer"));
  add("Fruity LSD", defaults("gl.midiout", "Fruity LSD"));
}

}  // namespace gl::flp
