// Gridline — FLP import, second wave: panning / stereo / controller / routing
// / notes / host plugins.
//
// State layouts (little-endian; "ints" = int32 in parameter-tag order):
//   Fruity PanOMatic      pan (0..256), volume (0..320, 256 = unity), LFO shape
//                         (0..2), LFO target (bit 0 pan, bit 1 volume), LFO
//                         amount (0..128), LFO speed (0..55000). No version.
//   Spreader              version (0x10003), spread (0..10000), separation
//                         (±10000), low-frequency bypass (1000..1000000: 0..500 Hz
//                         over the first half, 500 Hz..10 kHz over the second),
//                         mono (low band), enabled, theme, pre / post monitors.
//   Fruity X-Y Controller version, X level, X min, X max, X tension (±128),
//                         Y level, Y min, Y max, Y tension (levels 0..65536),
//                         speed (1024..2048, 2048 = immediate), acceleration
//                         (256..1024), then skin / input device strings.
//   Fruity Formula Ctrl   v2: version, a, b, c (0..65536), formula (u8 length,
//                         ANSI), description (u32 length, RTF). v4: version,
//                         a, b, c, formula (u8 length, UTF-16), description
//                         (u8 length, UTF-16); v4 formulas are ANSI in every
//                         shipped state.
//   Fruity Send           pan (±128), volume (0..320, 256 = unity), dry
//                         (0..256), target send (-1 = none).
//   Fruity dB Meter       no state.
//   Fruity NoteBook       version 1000, page count, current page, then per page
//                         u32 length + RTF text.
//   Fruity HTML NoteBook  version, u8 length + URL / file name.
//   Fruity Wrapper        chunks (u32 id, u64 size, payload): 54 = plugin
//                         name, 55 = plugin path, … (third-party plugin, not
//                         hosted: reported by name).
//   Edison                i32 version, header ints, u32 size + RIFF/WAVE of
//                         the loaded audio, editor records, settings ints
//                         (decoded by packs/edison: decodeFlState).
//   Transporter           version, dry level, loop level A, loop level B
//                         (0..12800, 12800 = unity), biases, lengths, …
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include "../../packs/edison/edison.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/spectral/spectral.h"
#include "../../packs/utility/utility.h"
#include "../PluginParams.h"
#include "FlpFx2.h"
#include "FlpWrapper.h"

namespace gl::flp::fx2 {

namespace {

namespace pp = gl::pp;
namespace md = gl::packs::modulation;
namespace ut = gl::packs::utility;
namespace sp = gl::packs::spectral;

// ------------------------------------------------------------------ PanOMatic → Auto Pan
// LFO speed 0..55000 → Hz (see effects_wave2.md).
double panSpeedHz(double v) { return expMap(v / 55000.0, 0.05, 20.0); }

bool decodePanOMatic(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  std::vector<double> t = ints(r, 6);
  if (t.size() < 2) return false;
  t.resize(6, 0);
  fx::begin(m, "gl.autopan");
  const int target = static_cast<int>(t[3]);
  const double amount = clampd(t[4] / 128.0, 0, 1);
  fx::set(m, pp::ApStaticPan, clampd((t[0] - 128.0) / 128.0, -1, 1));
  fx::set(m, pp::ApStaticVol, clampd(t[1] / 256.0, 0, 1.25));
  fx::set(m, pp::ApShape, clampd(t[2], 0, 2));
  fx::set(m, pp::ApSync, 0);
  fx::set(m, pp::ApRateHz, panSpeedHz(t[5]));
  fx::set(m, pp::ApPan, (target & 1) ? amount : 0.0);
  fx::set(m, pp::ApVolume, (target & 2) ? amount : 0.0);
  m.exact = target == 0;  // static pan / volume map 1:1; the LFO rate curve is approximate
  const int amountParam = (target & 2) && !(target & 1) ? pp::ApVolume : pp::ApPan;
  bindKnobs(m, {{0, 0, 256, pp::ApStaticPan, [](double v) { return clampd((v - 128.0) / 128.0, -1, 1); }},
           {1, 0, 320, pp::ApStaticVol, [](double v) { return clampd(v / 256.0, 0, 1.25); }},
           {2, 0, 2, pp::ApShape, nullptr},
           {4, 0, 128, amountParam, [](double v) { return clampd(v / 128.0, 0, 1); }},
           {5, 0, 55000, pp::ApRateHz, panSpeedHz}});
  return true;
}

// ------------------------------------------------------------------ Spreader → Spreader
// v3: 0x10003, 5 ints (spread, separation, low bypass, mono, enabled), u32
// theme, u8 pre-FX monitor, u8 post-FX monitor. v2: 0x10002 + 5 ints. v1
// (12 bytes): the first three ints only; the plugin zero-fills mono and
// enabled, so such a state loads disabled.
bool decodeSpreader(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 12) return false;
  const uint32_t ver = r.u32();
  std::vector<double> t;
  int64_t version = static_cast<int64_t>(ver) - 0x10000;
  if (version <= 0) {  // v1: the knobs start at the first word
    version = 1;
    t.push_back(static_cast<int32_t>(ver));
    for (double v : ints(r, 2)) t.push_back(v);
  } else {
    t = ints(r, 5);
  }
  if (t.size() < 3) return false;
  t.resize(5, 0.0);
  int pre = 1, post = 1;
  if (version >= 3 && r.n - r.pos >= 6) {
    r.u32();  // editor theme
    pre = r.u8() != 0;
    post = r.u8() != 0;
  }
  fx::begin(m, "gl.spread");
  const double f = sp::spreadLowHz(t[2]);
  fx::set(m, sp::SrModel, 1);
  fx::set(m, sp::SrMode, 0);
  fx::set(m, sp::SrSpread, clampd(t[0] / 10000.0, 0, 1));
  fx::set(m, sp::SrSeparation, clampd(t[1] / 10000.0, -1, 1));
  fx::set(m, sp::SrLowBypass, f > 0 ? 1 : 0);
  fx::set(m, sp::SrLowFreq, clampd(f, 10, 10000));
  fx::set(m, sp::SrBassMono, t[3] != 0 ? 1 : 0);
  fx::set(m, sp::SrBassFreq, clampd(f, 20, 500));  // legacy engine equivalent
  fx::set(m, sp::SrMonoCheck, 0);
  fx::set(m, sp::SrEnabled, t[4] != 0 ? 1 : 0);
  fx::set(m, sp::SrPreMonitor, pre);
  fx::set(m, sp::SrPostMonitor, post);
  m.exact = true;  // every control maps 1:1 (the engine clamps the split to >= 10 Hz as well)
  bindKnobs(m, {{0, 0, 10000, sp::SrSpread, [](double v) { return clampd(v / 10000.0, 0, 1); }},
           {1, -10000, 10000, sp::SrSeparation, [](double v) { return clampd(v / 10000.0, -1, 1); }},
           {2, 1000, 1000000, sp::SrLowFreq, [](double v) { return clampd(sp::spreadLowHz(v), 10, 10000); }},
           {3, 0, 1, sp::SrBassMono, nullptr},
           {4, 0, 1, sp::SrEnabled, nullptr}});
  return true;
}

// ------------------------------------------------------------------ X-Y Controller → X-Y Pad
// Speed 1024..2048 (2048 = follow immediately) → smoothing time (s).
double xySmooth(double speed) { return speed >= 2048 ? 0.001 : expMap((2048.0 - speed) / 1024.0, 0.001, 2.0); }

bool decodeXY(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 44) return false;
  r.i32();  // version
  std::vector<double> t = ints(r, 10);
  if (t.size() < 10) return false;
  fx::begin(m, "gl.xyctl");
  auto lvl = [](double v) { return clampd(v / 65536.0, 0, 1); };
  auto ten = [](double v) { return clampd(v / 128.0, -1, 1); };
  KnobTable tab = {{0, 0, 65536, md::XyX, lvl},     {1, 0, 65536, md::XyXMin, lvl}, {2, 0, 65536, md::XyXMax, lvl},
                   {3, -128, 128, md::XyXTension, ten}, {4, 0, 65536, md::XyY, lvl},    {5, 0, 65536, md::XyYMin, lvl},
                   {6, 0, 65536, md::XyYMax, lvl},   {7, -128, 128, md::XyYTension, ten}, {8, 1024, 2048, md::XySmooth, xySmooth}};
  fx::applyTable(m, tab, t);
  m.exact = t[8] >= 2048 && t[9] >= 1024;  // speed / acceleration follow-up curves are approximate
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ X-Y-Z Controller → X-Y-Z Pad
// version 3, X, Y, Z (0..65536), per-axis speed (0..4096) and acceleration
// (0..3072), global speed, external-controller speed, then per-output mapping
// records (range doubles, curve). Only the positions and the global
// smoothing are carried over.
bool decodeXYZ(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 11 * 4) return false;
  r.i32();  // version
  std::vector<double> t = ints(r, 11);
  if (t.size() < 11) return false;
  fx::begin(m, "gl.xyzctl");
  auto lvl = [](double v) { return clampd(v / 65536.0, 0, 1); };
  KnobTable tab = {{0, 0, 65536, md::XzX, lvl},
                   {1, 0, 65536, md::XzY, lvl},
                   {2, 0, 65536, md::XzZ, lvl},
                   {9, 0, 4096, md::XzSmooth, [](double v) { return expMap(clampd((4096.0 - v) / 4096.0, 0, 1), 0.001, 2.0); }}};
  fx::applyTable(m, tab, t);
  m.exact = false;  // per-output ranges and smoothing curves approximate
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Formula Controller
bool decodeFormula(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  if (b.size() < 17) return false;
  Reader r(b.data(), b.size());
  const uint32_t ver = r.u32();
  std::vector<double> abc = ints(r, 3);
  if (abc.size() < 3) return false;
  fx::begin(m, "gl.formulactl");
  auto lvl = [](double v) { return clampd(v / 65536.0, 0, 1); };
  KnobTable tab = {{0, 0, 65536, md::FmA, lvl}, {1, 0, 65536, md::FmB, lvl}, {2, 0, 65536, md::FmC, lvl}};
  fx::applyTable(m, tab, abc);
  std::string formula;
  size_t len = r.u8();
  // J-34: every shipped v4 state stores the formula as ANSI bytes (only the
  // description that follows is UTF-16), so only v3 is read as UTF-16.
  if (ver == 3) {
    if (r.has(len * 2)) formula = utf16(b.data() + r.pos, len), r.skip(len * 2);
  } else if (r.has(len)) {
    formula = latin1(b.data() + r.pos, len), r.skip(len);
  }
  formula = normalizeLines(formula);
  m.exact = !formula.empty();
  if (formula.empty()) {
    formula = md::kDefaultFormula;
    c.report.warn("Formula Controller: no formula text in the state; default formula used");
  }
  m.state.data = formula;
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Fruity Send → Mixer Send
bool decodeSend(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  std::vector<double> t = ints(r, 4);
  if (t.size() < 3) return false;
  t.resize(4, -1);
  fx::begin(m, "gl.send");
  KnobTable tab = {{0, -128, 128, md::SnPan, [](double v) { return clampd(v / 128.0, -1, 1); }},
                   {1, 0, 320, md::SnLevel, [](double v) { return clampd(v / 256.0, 0, 1.25); }},
                   // J-42: dry level on FL's volume curve (1 exactly at 256).
                   {2, 0, 256, md::SnDry, [](double v) { const double g = clampd(v / 256.0, 0, 1); return g == 1 ? 1.0 : 0.1 * (std::pow(11.0, g) - 1.0); }}};
  fx::applyTable(m, tab, t);
  const int target = static_cast<int>(t[3]);
  // J-42: FL sends only for a target index > 0 (0 and −1 = none); a target is an index
  // into the hosting insert's route list, not an insert number (not resolved here).
  m.exact = target <= 0;
  if (target > 0 && target < 4096) {  // corrupt targets: treated as none
    // FL's send targets are counted from the first insert after the master.
    fx::set(m, md::SnTarget, target + 1);
  } else {
    fx::set(m, md::SnLevel, 0);
    c.report.warn("Fruity Send: no send target selected; send level set to 0");
  }
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ dB Meter → Level Meter
bool decodeDbMeter(Context&, const std::vector<Event>&, PluginMapping& m) {
  fx::begin(m, "gl.meter");
  fx::set(m, ut::LmMode, 0);  // peak
  m.exact = true;            // no settings
  return true;
}

// ------------------------------------------------------------------ NoteBook / HTML NoteBook → Notes
bool decodeNoteBook(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  fx::begin(m, "gl.notes");
  std::vector<std::string> pages(ut::kNotePages);
  Reader r(b.data(), b.size());
  r.u32();  // version
  const int count = static_cast<int>(r.u32());
  const int current = static_cast<int>(r.u32());
  bool ok = count >= 0 && count <= 1000;
  for (int p = 0; ok && p < count && r.has(4); ++p) {
    size_t len = r.u32();
    if (!r.has(len)) {
      ok = false;
      break;
    }
    std::string raw(reinterpret_cast<const char*>(b.data() + r.pos), len);
    r.skip(len);
    while (!raw.empty() && raw.back() == '\0') raw.pop_back();
    if (p < ut::kNotePages) pages[static_cast<size_t>(p)] = rtfToText(raw);
  }
  m.state.data = ut::encodeNotes(pages);
  fx::set(m, ut::NtPage, std::clamp(current, 1, ut::kNotePages));
  m.exact = ok;  // text kept; RTF formatting dropped
  return true;
}

bool decodeHtmlNoteBook(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  fx::begin(m, "gl.notes");
  std::vector<std::string> pages(ut::kNotePages);
  if (b.size() >= 5) {
    size_t len = b[4];
    if (5 + len <= b.size()) pages[0] = latin1(b.data() + 5, len);
  }
  m.state.data = ut::encodeNotes(pages);
  m.exact = false;  // the page shows the address instead of rendering it
  return true;
}

// ------------------------------------------------------------------ Fruity Wrapper (third-party plugins)
bool decodeWrapper(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  std::string name, vendor, path;
  std::vector<float> params;  // chunk 53: the hosted plugin's normalized parameters
  Reader r(b.data(), b.size());
  r.u32();  // version
  while (r.left() >= 12) {
    uint32_t id = r.u32();
    uint64_t lo = r.u32(), hi = r.u32();
    uint64_t size = lo | (hi << 32);
    if (size > r.left()) break;
    if (id == 54) name = latin1(b.data() + r.pos, static_cast<size_t>(size));
    if (id == 55) path = latin1(b.data() + r.pos, static_cast<size_t>(size));
    if (id == 56) vendor = latin1(b.data() + r.pos, static_cast<size_t>(size));
    if (id == 53 && size >= 17) {
      // i32 -9, u8 version, i32 flag, u32 chunk size (0: parameter list), 8
      // reserved bytes when the flag is -2 (FL 11 states have flag 0 and
      // none; J-34 fix: the list used to be read at a fixed offset).
      Reader pr(b.data() + r.pos, static_cast<size_t>(size));
      pr.skip(5);
      const int32_t flag = pr.i32();
      pr.skip(flag == -2 ? 12 : 4);
      uint32_t count = pr.u32();
      for (uint32_t k = 0; k < count && k < 4096 && pr.has(4); ++k) params.push_back(pr.f32());
    }
    r.skip(static_cast<size_t>(size));
  }
  // The channel / slot display name (203) only labels the report: it is the
  // user's name, never the plugin's identity.
  std::string label = name;
  if (const Event* e = find(evs, 203); e && label.empty()) label = e->text();
  if (label.empty()) label = "?";
  // FL's own plugins loaded as plugins-in-a-wrapper: map through their
  // decoder (defaults from an empty state, then the saved parameters).
  // Third-party plugins are never replaced by a built-in, whatever their name.
  if (!params.empty() && !name.empty() && name != "Fruity Wrapper" && flp::findDecoder(name) && flp::isImageLinePlugin(vendor, name, path)) {
    std::vector<uint8_t> zero(4096, 0);
    PluginMapping inner;
    if (decodeEmbedded(c, name, zero.data(), zero.size(), inner) && inner.param && inner.value) {
      inner.state = defaultPluginState(inner.glType);
      for (size_t k = 0; k < params.size(); ++k) {
        int gp = inner.param(static_cast<int>(k));
        if (gp >= 0 && std::isfinite(params[k])) fx::set(inner, gp, inner.value(static_cast<int>(k), clampd(params[k], 0, 1)));
      }
      std::string flName = m.flName;
      m = inner;
      m.flName = flName;
      m.exact = false;
      c.report.warn("\"" + name + "\" loaded through the plugin wrapper: mapped from its parameter values");
      return true;
    }
  }
  m.flName = "Fruity Wrapper (" + label + ")";  // reported as unmapped under the hosted plugin's name
  c.report.warn("Third-party plugin \"" + label + "\" could not be identified; its slot is left empty");
  return false;
}

// ------------------------------------------------------------------ Edison → Audio Editor (gl.edison)
// Recording settings and the embedded audio (with its markers, regions and
// loop points) map onto the audio editor; the audio is written to the import
// cache, named by content hash (see packs/edison).
std::string edisonAudioDir() {
  if (const char* e = std::getenv("GRIDLINE_CACHE_DIR"); e && *e) return std::string(e) + "/flp-edison";
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/gridline/flp-edison";
  if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.cache/gridline/flp-edison";
  return (std::filesystem::temp_directory_path() / "gridline-flp-edison").string();
}

bool decodeEdison(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  fx::begin(m, gl::packs::edison::kType);
  PluginState st;
  std::string note;
  const bool ok = gl::packs::edison::mapFlState(b.data(), b.size(), edisonAudioDir(), st, &note);
  m.state = st;
  m.exact = false;  // editor panels (envelope / LFO records) are kept, not interpreted
  if (!note.empty() && (!ok || note.find("imported into") != std::string::npos)) c.report.warn(note);
  return true;
}

// ------------------------------------------------------------------ Transporter → pass-through (dry level)
bool decodeTransporter(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  r.i32();  // version
  std::vector<double> t = ints(r, 1);
  if (t.empty()) return false;
  fx::begin(m, "gl.utility");
  fx::set(m, pp::UtlGain, clampd(dbOf(t[0] / 12800.0, -36), -36, 24));
  m.exact = false;
  c.report.warn("Transporter: loop capture / playback not reproduced; dry signal kept");
  bindKnobs(m, {{0, 0, 12800, pp::UtlGain, [](double v) { return clampd(dbOf(v / 12800.0, -36), -36, 24); }}});
  return true;
}

}  // namespace

void registerFx2Mod(std::map<std::string, PluginDecoder>& d) {
  d["Fruity PanOMatic"] = decodePanOMatic;
  d["Spreader"] = decodeSpreader;
  d["Fruity X-Y Controller"] = decodeXY;
  d["Fruity X-Y-Z Controller"] = decodeXYZ;
  d["Fruity Formula Controller"] = decodeFormula;
  d["Fruity Send"] = decodeSend;
  d["Fruity dB Meter"] = decodeDbMeter;
  d["Fruity NoteBook"] = decodeNoteBook;
  d["Fruity HTML NoteBook"] = decodeHtmlNoteBook;
  d["Fruity Wrapper"] = decodeWrapper;
  d["Edison"] = decodeEdison;
  d["Transporter"] = decodeTransporter;
}

}  // namespace gl::flp::fx2
