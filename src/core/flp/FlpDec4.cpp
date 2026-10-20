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
  m.exact = false;  // panel artwork, control kinds and MIDI output are not reproduced
  if (b.size() < 32) return true;
  const uint32_t n = b.u32(28);
  if (n > 4096 || !b.has(32, 4 * static_cast<size_t>(n))) return true;
  // Values are 0..65536 (centre 32768), or MIDI-style 0..127 in panels whose
  // controls all use that range.
  uint32_t top = 0;
  for (uint32_t k = 1; k < n; ++k) top = std::max(top, b.u32(32 + 4 * static_cast<size_t>(k)));
  const double scale = top <= 127 ? 127.0 : 65536.0;
  for (uint32_t k = 1; k < n && static_cast<int>(k - 1) < md::kSurfaceControls; ++k)
    set(m, static_cast<int>(k - 1), std::clamp(b.u32(32 + 4 * static_cast<size_t>(k)) / scale, 0.0, 1.0));
  // Control names: a 32-byte Pascal name field with the caption 48 bytes later.
  auto pascal = [&](size_t at) -> std::string {
    const uint8_t len = b.u8(at);
    if (len == 0 || len > 31 || !b.has(at + 1, len)) return "";
    std::string s;
    for (size_t i = 0; i < len; ++i) {
      const char ch = static_cast<char>(b.u8(at + 1 + i));
      if (!std::isprint(static_cast<unsigned char>(ch))) return "";
      s += ch;
    }
    return s;
  };
  std::string data;
  int k = 0;
  for (size_t at = 32 + 4 * static_cast<size_t>(n) + 4; at + 80 <= b.size() && k < md::kSurfaceControls;) {
    std::string name = pascal(at);
    if (name.empty() || pascal(at + 48).empty()) {
      ++at;
      continue;
    }
    const bool slider = name.find("Slider") != std::string::npos || name.find("slider") != std::string::npos;
    data += std::to_string(slider ? md::SkSlider : md::SkKnob) + " " + name + "\n";
    ++k;
    at += 80;
  }
  if (!data.empty()) m.state.data = data;
  if (n > static_cast<uint32_t>(md::kSurfaceControls) + 1) c.report.warn("Dashboard: more than 16 controls, extra ones dropped");
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

void registerDecoders4(std::map<std::string, PluginDecoder>& d) {
  auto add = [&](const char* name, PluginDecoder f) { d.emplace(name, std::move(f)); };  // never overrides
  add("Fruity Big Clock", decodeBigClock);
  add("VFX Color Mapper", decodeColorMapper);
  add("VFX Level Scaler", defaults("gl.velscale", "VFX Level Scaler"));
  add("VFX Sequencer", defaults("gl.notesq", "VFX Sequencer"));
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
