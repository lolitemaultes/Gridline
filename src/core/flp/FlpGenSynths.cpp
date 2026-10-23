// Gridline — FLP import: decoder registry for FL's synthesizers.
//   Harmless → Additive ("gl.additive", FlpGenHarmless.cpp)
//   GMS      → Groove Synth ("gl.gms", FlpGenGms.cpp)
//   Sytrus   → Matrix FM (FlpGenSytrus.cpp); fallback: FM Synth ("gl.fm") or Analog
//   Harmor   → Resynth ("gl.harmor", FlpGenHarmor.cpp)
// The Gridline engines differ from FL's, so every mapping is approximate
// (exact = false); the decoded settings and what was approximated go to the
// import report.
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include "../PluginBrand.h"
#include "../PluginParams.h"
#include "../../packs/synths/synths.h"
#include "FlpGen.h"

namespace gl::flp::gen {

namespace {

namespace sy = gl::packs::synths;

double clampd(double v, double lo, double hi) { return std::clamp(v, lo, hi); }

// A Gridline factory preset of `type` by name (defaults when absent).
PluginState presetOf2(const std::string& type, const std::string& preset) {
  if (const PluginInfo* info = findPlugin(type))
    for (size_t i = 0; i < info->presets.size(); ++i)
      if (info->presets[i].name == preset) return presetState(*info, i);
  return base(type);
}

// -------------------------------------------------------------------- Sytrus
// State: dword header (word 3 + word version, or a plain version dword), then a
// zlib stream. Decompressed: dword per main-page control first (0 master
// level 0..160, 2 master pitch in semitones, 3..10 volume / filter envelope
// offsets), then FX settings, filter settings, patch notes (RTF) and nine
// units (6 operators + 3 filters) each holding its parameters and eight
// articulators (envelope + LFO point lists). The unit layout varies with the
// version and is not decoded; the patch is approximated from the main page.
bool decodeSytrus(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  if (!b.has(0, 8)) return false;
  std::vector<uint8_t> raw;
  if (!inflateZlib(b.sub(4, b.n - 4), raw, 4u << 20) || raw.size() < 64) return false;
  Bytes r(raw.data(), raw.size());
  const int level = r.i32(0), pitch = std::clamp(r.i32(8), -48, 48);
  // Channel name hints the role (bass / noise / pad / lead …).
  std::string name;
  for (const auto& e : events)
    if (e.id == 203) name = e.text();
  std::string ln;
  for (char ch : name) ln += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  auto has = [&](const char* w) { return ln.find(w) != std::string::npos; };
  PluginState s;
  std::string what;
  const double vol = clampd(level / 128.0 * 0.7, 0, 1);
  if (has("noise") || has("foley") || has("fx") || has("wind")) {
    s = base("gl.analog");
    setp(s, sy::anOsc(0, sy::OscOn), 0), setp(s, sy::anOsc(1, sy::OscOn), 0), setp(s, sy::anOsc(2, sy::OscOn), 0);
    setp(s, sy::AnNoise, 1.0);
    setp(s, sy::AnFltMode, sy::FltBp12), setp(s, sy::AnCutoff, 2500), setp(s, sy::AnReso, 0.2), setp(s, sy::AnFltEnv, 0);
    setp(s, sy::AnAmpA, 0.05), setp(s, sy::AnAmpS, 1.0), setp(s, sy::AnAmpR, 0.6);
    setp(s, sy::AnVolume, vol);
    what = pluginDisplayName("gl.analog") + " (filtered noise)";
  } else if (has("pad") || has("string")) {
    s = presetOf2("gl.analog", "Velvet Pad");
    setp(s, sy::AnTranspose, clampd(pitch, -24, 24));
    what = pluginDisplayName("gl.analog") + " (pad)";
  } else {
    // FM: bass patches use a short modulator decay; others an electric-piano-like stack.
    const bool bass = has("bass") || pitch <= -12;
    s = presetOf2("gl.fm", bass ? "FM bass" : "E. piano");
    setp(s, gl::pp::FmOctave, std::clamp(std::round(pitch / 12.0), -3.0, 3.0));
    setp(s, gl::pp::FmVolume, vol);
    what = pluginDisplayName("gl.fm") + (bass ? " (FM bass)" : "");
  }
  m.glType = s.type;
  m.state = s;
  m.exact = false;
  note(c, "Sytrus", "\"" + name + "\": master level / pitch decoded (" + std::to_string(pitch) +
                        " st); operators, matrix and articulators not decoded → " + what);
  // Modulation X / Y (tags 18 / 19, 0..512, centre 256 = the patch as saved):
  // Sytrus' performance macros. They drive the timbre of the approximation
  // around its saved value: modulator level / feedback (FM), cutoff / resonance (Analog).
  auto at = [&s](int i, double d) { return i >= 0 && static_cast<size_t>(i) < s.params.size() ? static_cast<double>(s.params[static_cast<size_t>(i)]) : d; };
  ParamTable t;
  if (s.type == "gl.fm") {
    const double lvl = at(gl::pp::fmParam(1, gl::pp::FmLevel), 0.55), fb = at(gl::pp::FmFeedback, 0.1);
    t.fn(18, gl::pp::fmParam(1, gl::pp::FmLevel), [lvl](double n) { return clampd(lvl * 2.0 * n, 0, 1); });
    t.fn(19, gl::pp::FmFeedback, [fb](double n) { return clampd(fb * 2.0 * n, 0, 1); });
  } else {
    const double cut = at(sy::AnCutoff, 2500), res = at(sy::AnReso, 0.2);
    t.fn(18, sy::AnCutoff, [cut](double n) { return clampd(cut * std::pow(2.0, (n - 0.5) * 8.0), 20, 20000); });
    t.fn(19, sy::AnReso, [res](double n) { return clampd(res + (n - 0.5), 0, 1); });
  }
  t.range(18, 0, 512), t.range(19, 0, 512);
  t.install(m);
  return true;
}

}  // namespace

void registerSynthDecoders(std::map<std::string, PluginDecoder>& d) {
  d["GMS"] = decodeGmsFull;
  d["Harmless"] = decodeHarmlessFull;
  d["Harmor"] = decodeHarmorFull;
  // Full decode to Matrix FM; the older approximation covers states that do not walk.
  d["Sytrus"] = [](Context& c, const std::vector<Event>& ev, PluginMapping& m) { return decodeSytrusMatrix(c, ev, m) || decodeSytrus(c, ev, m); };
}

}  // namespace gl::flp::gen
