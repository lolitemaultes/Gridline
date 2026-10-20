// Gridline — FLP export: plugin state encoders for FlpFx2Mod.cpp and
// FlpFx2Patcher.cpp (Spreader, X-Y / X-Y-Z / Formula Controller, Send, dB
// Meter, NoteBook, HTML NoteBook, Transporter, Edison, Patcher). Fruity
// PanOMatic is decoded by the first wave (never overridden here) and the
// Fruity Wrapper has its own family (FlpEncWrapper.cpp). The reverse of the
// decoders in those files; see FlpEncode.h for the model.
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include "FlpEncode.h"
#include "../../packs/edison/edison.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/modulation/patchv2.h"
#include "../../packs/modulation/modulation_dsp.h"
#include "../../packs/spectral/spectral.h"
#include "../../packs/utility/utility.h"
#include "../PluginParams.h"

namespace gl::flp::enc {

namespace {

namespace md = gl::packs::modulation;
namespace ut = gl::packs::utility;
namespace sp = gl::packs::spectral;
namespace ed = gl::packs::edison;

float param(const PluginState& s, int k) { return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : 0.f; }

// UTF-8 → single-byte text (code points above 255 become '?'), the inverse
// of the decoders' latin1().
std::string toLatin1(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0) cp = c & 0x07, n = 4;
    else if (c >= 0xE0) cp = c & 0x0F, n = 3;
    else if (c >= 0xC0) cp = c & 0x1F, n = 2;
    for (size_t k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += n;
    out += cp <= 0xFF ? static_cast<char>(cp) : '?';
  }
  return out;
}

// UTF-8 → UTF-16 code units.
std::u16string toUtf16(const std::string& s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0) cp = c & 0x07, n = 4;
    else if (c >= 0xE0) cp = c & 0x0F, n = 3;
    else if (c >= 0xC0) cp = c & 0x1F, n = 2;
    for (size_t k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += n;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out += static_cast<char16_t>(0xD800 + (cp >> 10));
      out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
    } else {
      out += static_cast<char16_t>(cp);
    }
  }
  return out;
}
Bytes utf16Bytes(const std::string& s) {  // no terminator
  Bytes b;
  for (char16_t u : toUtf16(s)) b.push_back(static_cast<uint8_t>(u & 0xFF)), b.push_back(static_cast<uint8_t>(u >> 8));
  return b;
}

// ---------------------------------------------------------------- Spreader
// v3: u32 0x10003, i32 spread, separation, low split, mono, enabled, u32
// theme, u8 pre-FX monitor, u8 post-FX monitor. v2: no theme / monitors; v1:
// spread, separation, low split only.
constexpr uint32_t kSpreaderV3 = 0x10003;
Encoder spreader() {
  Encoder e;
  e.flName = "Spreader";
  e.glType = "gl.spread";
  e.nativeRank = 0;  // the plugin Gridline's spreader is modelled on
  e.defaults = [] {
    ByteWriter w;
    w.u32(kSpreaderV3);
    w.i32(5000);  // spread (50 %)
    w.i32(0);     // separation (centre)
    w.i32(1000);  // low-frequency split at its minimum: 0 Hz, no low band kept
    w.i32(0);     // mono low band off
    w.i32(1);     // enabled
    w.u32(0);     // editor theme
    w.u8(1);      // pre-FX monitor on
    w.u8(1);      // post-FX monitor on
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    const int64_t version = static_cast<int64_t>(getU32(b, 0)) - 0x10000;
    std::vector<Field> v;
    const size_t base = version <= 0 ? 0 : 4;
    v.push_back(i32(sp::SrSpread, base, 0, 10000));
    v.push_back(i32(sp::SrSeparation, base + 4, -10000, 10000));
    v.push_back(i32(sp::SrLowFreq, base + 8, 1000, 1000000));
    v.push_back(i32(sp::SrLowBypass, base + 8, 1000, 1000000));
    v.push_back(i32(sp::SrBassFreq, base + 8, 1000, 1000000));
    if (version > 0) {
      v.push_back(i32(sp::SrBassMono, 16, 0, 1));
      v.push_back(i32(sp::SrEnabled, 20, 0, 1));
      if (version >= 3 && b.size() >= 30) {
        v.push_back(field(sp::SrPreMonitor, 28, Type::U8, 0, 1));
        v.push_back(field(sp::SrPostMonitor, 29, Type::U8, 0, 1));
      }
    }
    return v;
  };
  return e;
}

// ---------------------------------------------------------------- X-Y Controller
// version 2, X, X min, X max, X tension, Y, Y min, Y max, Y tension, speed,
// acceleration, then the grid name (u8 length + text) and the input settings.
Encoder xyController() {
  Encoder e;
  e.flName = "Fruity X-Y Controller";
  e.glType = "gl.xyctl";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(2);      // version
    w.i32(32768);  // X (centre)
    w.i32(0);      // X min
    w.i32(65536);  // X max
    w.i32(0);      // X tension
    w.i32(32768);  // Y (centre)
    w.i32(0);      // Y min
    w.i32(65536);  // Y max
    w.i32(0);      // Y tension
    w.i32(1536);   // speed
    w.i32(1024);   // acceleration
    w.u8(0);       // invert Y off
    const std::string grid = "Default";  // grid preset name
    w.u8(static_cast<uint32_t>(grid.size()));
    w.bytes(grid.data(), grid.size());
    w.i32(-1);     // input device: none
    w.zeros(8);    // input options (reserved, zero)
    w.u32(1);      // input option flag
    w.zeros(4);    // reserved
    w.u32(70);     // input option value
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(md::XyX, 4, 0, 65536),          i32(md::XyXMin, 8, 0, 65536),  i32(md::XyXMax, 12, 0, 65536),
                              i32(md::XyXTension, 16, -128, 128), i32(md::XyY, 20, 0, 65536),    i32(md::XyYMin, 24, 0, 65536),
                              i32(md::XyYMax, 28, 0, 65536),      i32(md::XyYTension, 32, -128, 128), i32(md::XySmooth, 36, 1024, 2048)};
  };
  return e;
}

// ---------------------------------------------------------------- X-Y-Z Controller
// version 3, X, Y, Z, per-axis speed ×3, per-axis acceleration ×3, global
// speed, external-controller speed, u32 output count, u32 record stride,
// 8 reserved bytes, then one 80-byte mapping record per output, 16 bytes of
// output links (-1 = none).
constexpr int kXyzOutputs = 6;
Encoder xyzController() {
  Encoder e;
  e.flName = "Fruity X-Y-Z Controller";
  e.glType = "gl.xyzctl";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(3);  // version
    for (int k = 0; k < 3; ++k) w.i32(32768);  // X, Y, Z (centre)
    for (int k = 0; k < 3; ++k) w.i32(2048);   // per-axis speed
    for (int k = 0; k < 3; ++k) w.i32(3072);   // per-axis acceleration
    w.i32(2048);  // global speed
    w.i32(1024);  // external-controller speed
    w.u32(kXyzOutputs);
    w.u32(144);   // mapping block stride
    w.zeros(8);   // reserved
    for (int o = 0; o < kXyzOutputs; ++o) {
      // Output mapping: source axis mode, curve, polarity, 24 reserved bytes,
      // f64 range scale ×2 (1.0), 7 reserved bytes, u8 2, 4 reserved bytes,
      // 16 bytes of link targets (-1 = none).
      w.u32(3);
      w.u32(3);
      w.u32(2);
      w.zeros(24);
      w.f64(1.0);
      w.f64(1.0);
      w.zeros(7);
      w.u8(2);
      w.zeros(4);
      for (int k = 0; k < 4; ++k) w.i32(-1);
    }
    while (w.size() < 560) w.u8(0);
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(md::XzX, 4, 0, 65536), i32(md::XzY, 8, 0, 65536), i32(md::XzZ, 12, 0, 65536), i32(md::XzSmooth, 40, 0, 4096)};
  };
  return e;
}

// ---------------------------------------------------------------- Formula Controller
// v4: version, a, b, c (0..65536), formula (u8 length + ANSI), description
// (u8 length + UTF-16). v2: formula (u8 length + ANSI), description (u32 + RTF).
constexpr uint32_t kFormulaV4 = 4;
Encoder formulaController() {
  Encoder e;
  e.flName = "Fruity Formula Controller";
  e.glType = "gl.formulactl";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(kFormulaV4);
    w.i32(0);  // a
    w.i32(0);  // b
    w.i32(0);  // c
    const std::string f = md::kDefaultFormula;
    w.u8(static_cast<uint32_t>(f.size()));
    w.bytes(f.data(), f.size());
    w.u8(0);  // no description
    return w.b;
  };
  e.fields = [](const Bytes&) { return std::vector<Field>{i32(md::FmA, 4, 0, 65536), i32(md::FmB, 8, 0, 65536), i32(md::FmC, 12, 0, 65536)}; };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext&) {
    if (b.size() < 17) return;
    const uint32_t ver = getU32(b, 0);
    const size_t len = b[16];
    const bool wide = ver == 3;
    const size_t end = std::min(b.size(), 17 + len * (wide ? 2 : 1));
    Bytes text;
    size_t n = 0;
    if (wide) {
      std::u16string u = toUtf16(g.data);
      if (u.size() > 255) u.resize(255);
      for (char16_t c : u) text.push_back(static_cast<uint8_t>(c & 0xFF)), text.push_back(static_cast<uint8_t>(c >> 8));
      n = u.size();
    } else {
      std::string a = toLatin1(g.data);
      if (a.size() > 255) a.resize(255);
      text.assign(a.begin(), a.end());
      n = a.size();
    }
    Bytes out(b.begin(), b.begin() + 16);
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), text.begin(), text.end());
    out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(end), b.end());
    b = std::move(out);
  };
  return e;
}

// ---------------------------------------------------------------- Fruity Send
// i32 pan (±128), volume (0..320), dry (0..256), target send (-1 = none).
Encoder send() {
  Encoder e;
  e.flName = "Fruity Send";
  e.glType = "gl.send";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.i32(0);    // pan (centre)
    w.i32(256);  // send volume (unity)
    w.i32(256);  // dry (full)
    w.i32(-1);   // no target
    return w.b;
  };
  // Without a target the decoder silences the send: a send that is heard
  // gets its target first (Gridline counts targets from 1, FL from 0).
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext&) {
    if (param(g, md::SnLevel) != 0 && param(g, md::SnTarget) >= 1) putI32(b, 12, static_cast<int32_t>(std::lround(param(g, md::SnTarget))) - 1);
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(md::SnTarget, 12, -1, 299), i32(md::SnPan, 0, -128, 128), i32(md::SnLevel, 4, 0, 320), i32(md::SnDry, 8, 0, 256)};
  };
  return e;
}

// ---------------------------------------------------------------- dB Meter (no state)
Encoder dbMeter() {
  Encoder e;
  e.flName = "Fruity dB Meter";
  e.glType = "gl.meter";
  e.nativeRank = 0;  // no settings: FL loads it as is (peak meter)
  e.stateless = true;  // FL writes no state for it (J-44: exact, not "defaults")
  return e;
}

// ---------------------------------------------------------------- NoteBook
// version 1000, page count, current page (1-based), per page u32 length + RTF.
constexpr uint32_t kNoteBookVersion = 1000;
std::string textToRtf(const std::string& text) {
  std::string r = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Arial;}}\\uc1\\pard\\f0\\fs18 ";
  const std::u16string u = toUtf16(text);
  for (char16_t c : u) {
    if (c == '\n') r += "\\par ";
    else if (c == '\t') r += "\\tab ";
    else if (c == '\\' || c == '{' || c == '}') r += '\\', r += static_cast<char>(c);
    else if (c >= 0x20 && c < 0x80) r += static_cast<char>(c);
    else if (c >= 0x80) r += "\\u" + std::to_string(c >= 0x8000 ? static_cast<int>(c) - 65536 : static_cast<int>(c)) + "?";
  }
  r += "\\par\r\n}";
  return r;
}
Bytes noteBookState(const std::vector<std::string>& pages, int current) {
  int count = 1;
  for (size_t i = 0; i < pages.size(); ++i)
    if (!pages[i].empty()) count = static_cast<int>(i) + 1;
  ByteWriter w;
  w.u32(kNoteBookVersion);
  w.u32(static_cast<uint32_t>(count));
  w.i32(current);
  for (int p = 0; p < count; ++p) {
    const std::string rtf = textToRtf(static_cast<size_t>(p) < pages.size() ? pages[static_cast<size_t>(p)] : std::string());
    w.u32(static_cast<uint32_t>(rtf.size() + 1));
    w.bytes(rtf.data(), rtf.size());
    w.u8(0);  // NUL-terminated
  }
  return w.b;
}
Encoder noteBook() {
  Encoder e;
  e.flName = "Fruity NoteBook";
  e.glType = "gl.notes";
  e.nativeRank = 0;
  e.defaults = [] { return noteBookState(std::vector<std::string>(1), 1); };
  e.fields = [](const Bytes&) { return std::vector<Field>{i32(ut::NtPage, 8, 1, ut::kNotePages)}; };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext&) {
    b = noteBookState(ut::decodeNotes(g.data), static_cast<int>(std::lround(param(g, ut::NtPage))));
  };
  return e;
}

// HTML NoteBook: version 1, u8 length + address. Only the first page
// (the address) is stored.
Encoder htmlNoteBook() {
  Encoder e;
  e.flName = "Fruity HTML NoteBook";
  e.glType = "gl.notes";
  e.nativeRank = -1;  // native notes go to the NoteBook
  e.defaults = [] {
    ByteWriter w;
    w.u32(1);  // version
    w.u8(0);   // no address
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext&) {
    std::string url = toLatin1(ut::decodeNotes(g.data)[0]);
    if (url.size() > 255) url.resize(255);
    ByteWriter w;
    w.u32(b.size() >= 4 ? getU32(b, 0) : 1);
    w.u8(static_cast<uint32_t>(url.size()));
    w.bytes(url.data(), url.size());
    b = w.b;
  };
  return e;
}

// ---------------------------------------------------------------- Transporter
// v4: version, 20 ints by tag (dry, loop A / B levels 0..12800, biases,
// spread, lengths, ratio, sensitivity, bouncing, buttons, modes), u8 flush on
// stop, u8 lookahead, i32 architecture, i32 trigger option.
Encoder transporter() {
  Encoder e;
  e.flName = "Transporter";
  e.glType = "gl.utility";
  e.nativeRank = -1;  // only the dry level is carried: never a native home for the utility
  e.defaults = [] {
    ByteWriter w;
    w.u32(4);      // version
    w.i32(12800);  // 0 dry level (unity)
    w.i32(12800);  // 1 loop level A
    w.i32(12800);  // 2 loop level B
    w.i32(0);      // 3 reverse bias
    w.i32(0);      // 4 octave shift bias
    w.i32(0);      // 5 octave down / up bias
    w.i32(512);    // 6 spread and auto panning
    w.i32(500);    // 7 minimum length A
    w.i32(8);      // 8 A/B ratio
    w.i32(512);    // 9 transient sensitivity
    w.i32(512);    // 10 bouncing rate
    for (int tag = 11; tag < 20; ++tag) w.i32(0);  // 11..19 buttons / modes off
    w.u8(1);       // flush on pause / stop
    w.u8(1);       // lookahead
    w.i32(2);      // architecture: stereo to stereo
    w.i32(1);      // trigger option
    return w.b;
  };
  e.fields = [](const Bytes&) { return std::vector<Field>{i32(pp::UtlGain, 4, 0, 12800)}; };
  return e;
}

// ---------------------------------------------------------------- Edison
// u32 version, head ints, u32 audio size + RIFF/WAVE image (with cue / label
// / sampler chunks: markers, regions, loop), editor records, settings ints
// (record mode, …, maximum time, …, follow host) — the layout of
// packs/edison decodeFlState. States the decoder does not recognise are kept
// as hex in the data (first 4 KB) and written back when complete.
constexpr int kEdisonVersion = 11;
Bytes fromHex(const std::string& h) {
  Bytes b;
  for (size_t i = 0; i + 1 < h.size(); i += 2) b.push_back(static_cast<uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
  return b;
}
bool isHex(const std::string& h) {
  return h.size() % 2 == 0 && std::all_of(h.begin(), h.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}
// RIFF/WAVE image of the content (32-bit float, as Edison keeps it).
Bytes wavImage(const ed::Content& c) {
  static std::atomic<int> serial{0};
  std::error_code ec;
  const auto path = std::filesystem::temp_directory_path(ec) /
                    ("gridline-flp-edison-" + std::to_string(reinterpret_cast<uintptr_t>(&serial) & 0xFFFF) + "-" + std::to_string(serial++) + ".wav");
  Bytes out;
  if (ed::writeWavFile(path.string(), c, 32, nullptr)) {
    std::ifstream f(path, std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  std::filesystem::remove(path, ec);
  return out;
}
Bytes edisonState(int version, const std::vector<int32_t>& head, const Bytes& wav, const Bytes& extra, const std::vector<int32_t>& settings) {
  ByteWriter w;
  w.u32(static_cast<uint32_t>(version));
  for (int32_t v : head) w.i32(v);
  w.u32(static_cast<uint32_t>(wav.size()));
  if (ed::flEdisonSize64(version)) w.u32(0);  // J-39: a u64 size from version 13
  w.bytes(wav);
  w.bytes(extra);
  for (int32_t v : settings) w.i32(v);
  return w.b;
}
std::vector<int32_t> ints(const Bytes& b) {
  std::vector<int32_t> v;
  for (size_t o = 0; o + 4 <= b.size(); o += 4) v.push_back(static_cast<int32_t>(getU32(b, o)));
  return v;
}
Encoder edison() {
  Encoder e;
  e.flName = "Edison";
  e.glType = ed::kType;
  e.nativeRank = 0;
  e.defaults = [] {
    std::vector<int32_t> settings(kEdisonVersion - 1, 0);
    settings[0] = ed::RecOnInput;  // record mode
    settings[2] = 0;               // maximum time: unlimited
    settings[4] = 0;               // follow host off
    return edisonState(kEdisonVersion, {0, 0, 0}, wavImage(ed::makeContent(ed::makeAudio(44100, 2, 0))), {}, settings);
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> v;
    ed::FlEdison fl;
    if (!ed::decodeFlState(b.data(), b.size(), fl)) return v;
    const size_t at = b.size() - fl.settings.size() * 4;
    if (fl.settings.size() > 0) v.push_back(i32(ed::EdRecMode, at, 0, 3));
    if (fl.settings.size() > 2) v.push_back(i32(ed::EdRecMax, at + 8, 0, 7));
    if (fl.settings.size() > 4) v.push_back(i32(ed::EdFollowHost, at + 16, 0, 1));
    return v;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext&) {
    ed::State s;
    ed::decodeState(g.data, s);
    // A state the decoder did not recognise: its bytes, when complete.
    if (s.flVersion == 0 && s.path.empty() && !s.flExtra.empty() && isHex(s.flExtra) && s.flExtra.size() < 2 * 4096) {
      b = fromHex(s.flExtra);
      return;
    }
    ed::FlEdison base;
    if (!ed::decodeFlState(b.data(), b.size(), base)) {
      base = ed::FlEdison{};
      base.version = kEdisonVersion;
      base.head = {0, 0, 0};
      base.settings.assign(kEdisonVersion - 1, 0);
    }
    int version = base.version;
    std::vector<int32_t> head = base.head, settings = base.settings;
    Bytes extra = base.extra;
    if (s.flVersion > 0) {  // the parts an FL import kept: "head:extra:settings" (hex)
      const size_t c1 = s.flExtra.find(':'), c2 = c1 == std::string::npos ? c1 : s.flExtra.find(':', c1 + 1);
      if (c2 != std::string::npos) {
        version = s.flVersion;
        head = ints(fromHex(s.flExtra.substr(0, c1)));
        extra = fromHex(s.flExtra.substr(c1 + 1, c2 - c1 - 1));
        settings = ints(fromHex(s.flExtra.substr(c2 + 1)));
      }
    }
    ed::Content c;
    if (s.path.empty() || !ed::loadContent(s.path, c, nullptr)) c = ed::makeContent(ed::makeAudio(44100, 2, 0));
    else {
      c.markers = s.markers;
      c.regions = s.regions;
      c.loopStart = s.loopStart;
      c.loopEnd = s.loopEnd;
    }
    b = edisonState(version, head, wavImage(c), extra, settings);
  };
  return e;
}

// ---------------------------------------------------------------- Patcher
// u32 version 4, then chunks {u32 id, u64 size, payload}; id 0 = container.
//   1000 header (u32 next free id, …), 1002 editor window, nodes:
//   { 5000 header (u32 kind 2 effect / 3 generator, u64 id, …), 5004 plugin,
//     5008 name, 5010 display name (UTF-16), 5005 the plugin's own state,
//     5007 options (f32 mix, …), 5009 ports (u32 group, u32 index, u64 id,
//     u32 kind: 3 input, 2 output, 0 extra), 5001 end },
//   I/O { 1400 (u64 port id, u32 group, u32 kind 7 input / 6 output, u32
//     channels, …), 1402 name, 1401 end }, links { 1300 (u64 destination,
//   u64 source, f32 gain, u32 2, u32 group, u32 0), 1301 end }, 1500 view,
//   layout { 1600 (u64 node id, f32 x, f32 y, …) }, 1001 end.
// The effect version (gl.patcherfx) is wired as Gridline runs it: the chain
// in order, parallel nodes fed by the same sources; the instrument version
// (gl.patcher) feeds the note input to the generators and runs the effects
// after them.
struct Chunk {
  uint32_t id = 0;
  Bytes data;                 // leaf payload
  std::vector<Chunk> kids;    // container (id 0) children
  bool container = false;
};
void putChunk(ByteWriter& w, uint32_t id, const Bytes& data) {
  w.u32(id);
  w.u64(data.size());
  w.bytes(data);
}
Bytes serialize(const std::vector<Chunk>& cs) {
  ByteWriter w;
  for (const Chunk& c : cs) putChunk(w, c.id, c.container ? serialize(c.kids) : c.data);
  return w.b;
}
bool parseChunks(const uint8_t* p, size_t n, std::vector<Chunk>& out, int depth) {
  size_t o = 0;
  while (o + 12 <= n) {
    Chunk c;
    c.id = static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) | (static_cast<uint32_t>(p[o + 2]) << 16) |
           (static_cast<uint32_t>(p[o + 3]) << 24);
    uint64_t size = 0;
    for (int i = 7; i >= 0; --i) size = (size << 8) | p[o + 4 + static_cast<size_t>(i)];
    if (size > n - o - 12) return false;
    const uint8_t* q = p + o + 12;
    std::vector<Chunk> kids;
    if (c.id == 0 && size >= 12 && depth < 4 && parseChunks(q, static_cast<size_t>(size), kids, depth + 1)) {
      c.container = true;
      c.kids = std::move(kids);
    } else {
      c.data.assign(q, q + size);
    }
    out.push_back(std::move(c));
    o += 12 + static_cast<size_t>(size);
  }
  return o == n;
}
Chunk leaf(uint32_t id, Bytes data) {
  Chunk c;
  c.id = id;
  c.data = std::move(data);
  return c;
}
Chunk box(std::vector<Chunk> kids) {
  Chunk c;
  c.container = true;
  c.kids = std::move(kids);
  return c;
}

// Gridline graph → FL plugin name + state per node (through the encoders).
struct EncodedNode {
  std::string flName;
  Bytes state;
  bool generator = false;
  float mix = 1.0f;
  bool parallel = false;
};
bool encodeNodes(const md::PatchGraph& g, const EncodeContext& c, std::vector<EncodedNode>& out) {
  out.clear();
  for (const md::PatchNode& n : g.nodes) {
    if (n.plugin.type == "gl.patcher" || n.plugin.type == "gl.patcherfx") continue;  // FL's patcher does not nest here
    const PluginInfo* info = findPlugin(n.plugin.type);
    Result r = encode(n.plugin, "", nullptr, c);
    if (r.flName.empty()) continue;  // no FL plugin for this node
    EncodedNode e;
    e.flName = r.flName;
    e.state = r.state;
    e.generator = info && info->kind == PluginKind::Generator;
    e.mix = n.mix;
    e.parallel = n.parallel;
    out.push_back(std::move(e));
  }
  return true;
}

// A short display name: the FL name without its "Fruity " prefix.
std::string shortName(const std::string& fl) { return fl.rfind("Fruity ", 0) == 0 ? fl.substr(7) : fl; }

Bytes buildPatcher(const std::vector<EncodedNode>& nodes, bool instrument) {
  uint64_t next = 1000;
  std::vector<Chunk> top;
  const uint64_t inPort = next++, outPort = next++;
  struct Ids {
    uint64_t node, audioIn, eventIn, audioOut;
  };
  std::vector<Ids> ids;
  for (const EncodedNode& n : nodes) {
    Ids i{};
    i.node = next++;
    i.audioIn = next++;
    if (n.generator) i.eventIn = next++;
    i.audioOut = next++;
    ids.push_back(i);
  }
  // Header: next free id, one patch page; editor window (rectangle in pixels).
  {
    ByteWriter w;
    w.u64(next + 64);
    w.u32(1);  // pages
    w.zeros(52);
    top.push_back(leaf(1000, w.b));
    ByteWriter v;
    v.zeros(17);
    v.u32(15);  // view flags
    v.f32(40), v.f32(185), v.f32(548), v.f32(185);  // window rectangle
    v.zeros(7);
    top.push_back(leaf(1002, v.b));
  }
  auto port = [](uint32_t group, uint32_t index, uint64_t id, uint32_t kind) {
    ByteWriter w;
    w.u32(group);
    w.u32(index);
    w.u64(id);
    w.u32(kind);
    w.zeros(8);
    return leaf(5009, w.b);
  };
  for (size_t k = 0; k < nodes.size(); ++k) {
    const EncodedNode& n = nodes[k];
    std::vector<Chunk> kids;
    ByteWriter h;
    h.u32(n.generator ? 3 : 2);  // node kind
    h.u64(ids[k].node);
    h.zeros(28);
    kids.push_back(leaf(5000, h.b));
    kids.push_back(leaf(5004, utf16Bytes(n.flName)));
    kids.push_back(leaf(5008, utf16Bytes(n.flName)));
    kids.push_back(leaf(5010, utf16Bytes(shortName(n.flName))));
    kids.push_back(leaf(5005, n.state));
    ByteWriter o;
    o.f32(n.mix);        // mix
    o.i32(-1);           // no mixer target
    o.zeros(8);
    o.i32(0x7FFFFFFF);   // voice limit: none
    o.u32(60);           // root note
    o.zeros(16);
    kids.push_back(leaf(5007, o.b));
    kids.push_back(port(0, 0, ids[k].audioIn, 3));
    if (n.generator) kids.push_back(port(1, 0, ids[k].eventIn, 3));
    kids.push_back(port(0, 0, ids[k].audioOut, 2));
    kids.push_back(leaf(5001, {}));
    top.push_back(box(std::move(kids)));
  }
  auto io = [](uint64_t id, uint32_t group, uint32_t kind, bool audio, const std::string& name) {
    ByteWriter w;
    w.u64(id);
    w.u32(group);
    w.u32(kind);
    if (audio) {
      w.u32(2);  // stereo
      w.zeros(8);
    } else {
      w.u8(0);
      w.i32(-1);  // any MIDI channel
      w.zeros(7);
    }
    return box({leaf(1400, w.b), leaf(1402, utf16Bytes(name)), leaf(1401, {})});
  };
  if (instrument) top.push_back(io(inPort, 1, 7, false, "Notes"));
  else top.push_back(io(inPort, 0, 7, true, "Audio input"));
  top.push_back(io(outPort, 0, 6, true, "Audio output"));
  auto link = [](uint64_t dst, uint64_t src, uint32_t group) {
    ByteWriter w;
    w.u64(dst);
    w.u64(src);
    w.f32(1.0f);  // gain
    w.u32(2);
    w.u32(group);
    w.u32(0);
    return box({leaf(1300, w.b), leaf(1301, {})});
  };
  // Wiring: generators take the notes and feed the effect chain; effects run
  // in order, a parallel node shares the sources of the node before it.
  std::vector<uint64_t> sources, groupOut, prevSources;
  if (!instrument) sources = {inPort};
  for (size_t k = 0; k < nodes.size(); ++k)
    if (nodes[k].generator) {
      top.push_back(link(ids[k].eventIn, inPort, 1));
      groupOut.push_back(ids[k].audioOut);
    }
  if (instrument) sources = groupOut, groupOut.clear();
  bool first = true;
  for (size_t k = 0; k < nodes.size(); ++k) {
    if (nodes[k].generator) continue;
    if (nodes[k].parallel && !first) {
      for (uint64_t s : prevSources) top.push_back(link(ids[k].audioIn, s, 0));
      groupOut.push_back(ids[k].audioOut);
      continue;
    }
    if (!first) sources = groupOut;
    prevSources = sources;
    for (uint64_t s : sources) top.push_back(link(ids[k].audioIn, s, 0));
    groupOut = {ids[k].audioOut};
    first = false;
  }
  if (first && !instrument) groupOut = sources;  // no effects: input straight to the output
  for (uint64_t s : groupOut) top.push_back(link(outPort, s, 0));
  {
    ByteWriter w;
    w.f32(1.0f);  // zoom
    w.zeros(8);
    w.u32(600), w.u32(470);  // editor size
    w.zeros(20);
    top.push_back(leaf(1500, w.b));
  }
  top.push_back(leaf(0, {}));
  top.push_back(leaf(0, {}));
  for (size_t k = 0; k < nodes.size(); ++k) {
    ByteWriter w;
    w.u64(ids[k].node);
    w.f32(60.0f + 140.0f * static_cast<float>(k));  // x
    w.f32(nodes[k].generator ? 40.0f : 120.0f);     // y
    w.zeros(8);
    top.push_back(box({leaf(1600, w.b)}));
  }
  top.push_back(leaf(1001, {}));
  ByteWriter out;
  out.u32(4);  // version
  out.bytes(serialize(top));
  return out.b;
}

// Origin states: when the graph keeps its shape (same plugins, flags and
// mixes), only the changed nodes' own states are re-encoded in place, so the
// rest of FL's patch (controllers, wiring, layout) is kept.
bool patchInPlace(const PluginState& g, Bytes& b, const EncodeContext& c) {
  PluginMapping cur;
  if (b.size() < 16 || !decodeState("Patcher", b, cur, c) || cur.state.type != g.type) return false;
  const md::PatchGraph from = md::parsePatchGraph(cur.state.data), to = md::parsePatchGraph(g.data);
  if (from.nodes.size() != to.nodes.size()) return false;
  for (size_t k = 0; k < from.nodes.size(); ++k)
    if (from.nodes[k].plugin.type != to.nodes[k].plugin.type || from.nodes[k].parallel != to.nodes[k].parallel ||
        from.nodes[k].mix != to.nodes[k].mix || from.nodes[k].bypass != to.nodes[k].bypass)
      return false;
  for (int m = 0; m < 8; ++m)
    if (from.macroNames[m] != to.macroNames[m] || from.macros[m].size() != to.macros[m].size()) return false;
  std::vector<Chunk> top;
  if (!parseChunks(b.data() + 4, b.size() - 4, top, 0)) return false;
  // FL nodes with a state, decoded one by one, matched to the graph's nodes.
  struct FlNode {
    Chunk* chunk;
    std::string name;
    PluginState decoded;
    bool used = false;
  };
  std::vector<FlNode> fl;
  for (Chunk& t : top) {
    if (!t.container || t.kids.empty() || t.kids[0].id != 5000) continue;
    std::string name;
    const Chunk* st = nullptr;
    for (const Chunk& k : t.kids) {
      if (k.id == 5008) {
        std::string s;
        for (size_t i = 0; i + 1 < k.data.size(); i += 2) s += static_cast<char>(k.data[i]);
        name = s;
      }
      if (k.id == 5005) st = &k;
    }
    PluginMapping pm;
    if (!st || name.empty() || !decodeState(name, st->data, pm, c)) continue;
    fl.push_back({&t, name, pm.state, false});
  }
  bool changed = false;
  for (size_t k = 0; k < from.nodes.size(); ++k) {
    const PluginState& was = from.nodes[k].plugin;
    const PluginState& want = to.nodes[k].plugin;
    FlNode* hit = nullptr;
    for (FlNode& f : fl)
      if (!f.used && f.decoded == was) {
        hit = &f;
        break;
      }
    if (!hit) return false;
    hit->used = true;
    if (was == want) continue;
    for (Chunk& k2 : hit->chunk->kids)
      if (k2.id == 5005) {
        Result r = encode(want, hit->name, &k2.data, c);
        if (!r.hasState) return false;
        k2.data = r.state;
        changed = true;
      }
  }
  if (!changed) return true;
  ByteWriter out;
  out.u32(getU32(b, 0));
  out.bytes(serialize(top));
  b = out.b;
  return true;
}

// J-41: version-2 graphs (FL patches as wired) are written back chunk for
// chunk: the kept FL items, each node's chunks around its plugin state, the
// state re-encoded from the node's Gridline state (or kept as is when
// Gridline has no equivalent).
bool writePatchV2(const PluginState& g, Bytes& b, const EncodeContext& c) {
  md::PatchV2 graph;
  if (!md::isPatchV2(g.data) || !md::parsePatchV2(g.data, graph)) return false;
  // The patch's parameters are its targets' values in FL: a host Param slot
  // moved away from its target's value is written into the target.
  const md::PatchV2Topo topo = md::patchV2Topology(graph);
  for (size_t k = 0; k < topo.slots.size() && k < static_cast<size_t>(md::kPatchMaps); ++k) {
    const auto& sl = topo.slots[k];
    if (sl.node < 0 || static_cast<size_t>(sl.node) >= graph.nodes.size()) continue;
    md::PatchV2Node& n = graph.nodes[static_cast<size_t>(sl.node)];
    const md::PatchV2Map* mp = n.map(sl.fl);
    const size_t pi = static_cast<size_t>(md::PhMap0) + k;
    const float v = pi < g.params.size() ? g.params[pi] : std::nanf("");
    if (!mp || mp->gl < 0 || static_cast<size_t>(mp->gl) >= n.plugin.params.size() || !std::isfinite(v)) continue;
    const float cur = n.plugin.params[static_cast<size_t>(mp->gl)];
    if (std::fabs(mp->invert(cur) - v) > 1e-6) n.plugin.params[static_cast<size_t>(mp->gl)] = static_cast<float>(mp->eval(v));
  }
  ByteWriter out;
  out.u32(graph.version);
  for (const std::string& it : graph.items) {
    if (it.empty() || it[0] != '#') {
      out.bytes(md::patchUnhex(it));
      continue;
    }
    const size_t k = static_cast<size_t>(std::strtoul(it.c_str() + 1, nullptr, 10));
    if (k >= graph.nodes.size()) continue;
    const md::PatchV2Node& n = graph.nodes[k];
    Bytes payload = md::patchUnhex(n.head);
    if (n.tail != "-") {
      Bytes state;
      if (!n.raw.empty()) {
        state = md::patchUnhex(n.raw);
      } else {
        const Bytes orig = md::patchUnhex(n.orig);
        Result r = encode(n.plugin, n.flName, orig.empty() ? nullptr : &orig, c);
        state = r.hasState ? r.state : orig;
      }
      const Bytes ch = md::patchChunkBytes(5005, state);
      payload.insert(payload.end(), ch.begin(), ch.end());
      const Bytes tail = md::patchUnhex(n.tail);
      payload.insert(payload.end(), tail.begin(), tail.end());
    }
    out.bytes(md::patchChunkBytes(0, payload));
  }
  b = out.b;
  return true;
}

Encoder patcher(bool instrument) {
  Encoder e;
  e.flName = "Patcher";
  e.glType = instrument ? "gl.patcher" : "gl.patcherfx";
  e.nativeRank = 0;
  e.generator = instrument;
  e.defaults = [instrument] { return buildPatcher({}, instrument); };
  e.write = [instrument](const PluginState& g, Bytes& b, const EncodeContext& c) {
    if (writePatchV2(g, b, c)) return;
    if (patchInPlace(g, b, c)) return;
    md::PatchGraph graph = md::parsePatchGraph(g.data);
    std::vector<EncodedNode> nodes;
    encodeNodes(graph, c, nodes);
    if (instrument)  // the decoder lists generators first, then the effects
      std::stable_partition(nodes.begin(), nodes.end(), [](const EncodedNode& n) { return n.generator; });
    b = buildPatcher(nodes, instrument);
  };
  return e;
}

}  // namespace

void registerFx2Mod(std::vector<Encoder>& v) {
  v.push_back(spreader());
  v.push_back(xyController());
  v.push_back(xyzController());
  v.push_back(formulaController());
  v.push_back(send());
  v.push_back(dbMeter());
  v.push_back(noteBook());
  v.push_back(htmlNoteBook());
  v.push_back(transporter());
  v.push_back(edison());
  v.push_back(patcher(false));
  v.push_back(patcher(true));
}

}  // namespace gl::flp::enc
