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
//   FL Studio Mobile Rack FX  → the Gridline effect of the rack's first module
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

// FL Studio Mobile rack modules (type id → Gridline effect), from the rack's
// shipped one-module presets.
const char* mobileModuleType(uint32_t t) {
  switch (t) {
    case 3: case 30: return "gl.eq";          // parametric / tuned EQ
    case 4: return "gl.effector";            // multi effect
    case 6: return "gl.dist";
    case 7: return "gl.geq";                 // graphic EQ
    case 8: case 20: return "gl.comp";       // compressor, leveler
    case 9: return "gl.filter";
    case 11: return "gl.chorus";
    case 13: case 40: return "gl.flanger";   // flanger, comb filter
    case 14: return "gl.phaser";
    case 15: case 34: return "gl.stereo";    // stereoizer, spacer
    case 16: case 18: return "gl.delay";     // trance / tape delay
    case 24: return "gl.tuner";
    case 26: return "gl.analyzer";
    case 27: return "gl.gate";
    case 28: return "gl.limiter";
    case 29: return "gl.scduck";             // auto duck
    case 35: return "gl.reverb";
    case 36: return "gl.multiband";
    case 41: return "gl.waveshaper";
    case 42: return "gl.timegate";           // tape stop
    case 43: return "gl.bbdchorus";          // wow & flutter
    case 44: return "gl.spread";
    case 45: return "gl.pitcher";
    case 46: return "gl.autopan";            // tremolo
    default: return nullptr;
  }
}

bool decodeMobileRackFx(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  std::vector<uint32_t> modules;
  for (size_t at = b.find("RMOd"); at != std::string::npos && b.has(at, 16); at = b.find("RMOd", at + 4)) modules.push_back(b.u32(at + 8));
  if (modules.empty()) {  // an empty rack passes audio through
    begin(m, "gl.utility");
    m.exact = true;
    return true;
  }
  const char* type = mobileModuleType(modules.front());
  if (!type) return false;
  begin(m, type);
  m.exact = false;  // module settings are not converted
  if (modules.size() > 1) c.report.warn("FL Studio Mobile rack: only the first of " + std::to_string(modules.size()) + " modules imported");
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
