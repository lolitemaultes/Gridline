// Gridline — FLP export: plugin state encoders for FlpFxMisc.cpp (Control
// Surface, Effector, NoteBook 2, Vocodex, Wave Candy). The reverse of the
// decoders in that file; see FlpEncode.h for the model.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "FlpEncode.h"
#include "../PluginParams.h"
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/spectral/spectral.h"
#include "../../packs/utility/utility.h"

namespace gl::flp::enc {

namespace {

namespace ut = gl::packs::utility;
namespace sp = gl::packs::spectral;
namespace dy = gl::packs::dynamics;
namespace md = gl::packs::modulation;

bool decoded(const std::string& flName, const Bytes& b, const EncodeContext& c, PluginState& out) {
  PluginMapping m;
  if (!decodeState(flName, b, m, c)) return false;
  out = m.state;
  return true;
}
float param(const PluginState& s, int k) { return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : 0.f; }
uint32_t versionOf(const Bytes& b) {
  uint32_t v = b.size() >= 4 ? getU32(b, 0) : 0;
  return v >= 0x10000 ? v >> 16 : v;
}
Bytes slice(const Bytes& b, size_t from, size_t to) {
  from = std::min(from, b.size()), to = std::min(std::max(from, to), b.size());
  return Bytes(b.begin() + static_cast<std::ptrdiff_t>(from), b.begin() + static_cast<std::ptrdiff_t>(to));
}
void append(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }

// UTF-8 → code points.
std::vector<uint32_t> codePoints(const std::string& s) {
  std::vector<uint32_t> v;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c, len = 1;
    if (c >= 0xF0) cp = c & 0x07, len = 4;
    else if (c >= 0xE0) cp = c & 0x0F, len = 3;
    else if (c >= 0xC0) cp = c & 0x1F, len = 2;
    for (uint32_t k = 1; k < len && i + k < s.size(); ++k) cp = cp << 6 | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += len;
    v.push_back(cp);
  }
  return v;
}
// UTF-8 → UTF-16 code units (surrogate pairs above the BMP).
std::vector<uint16_t> utf16Units(const std::string& s) {
  std::vector<uint16_t> u;
  for (uint32_t cp : codePoints(s)) {
    if (cp >= 0x10000 && cp < 0x110000) {
      cp -= 0x10000;
      u.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
      u.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      u.push_back(static_cast<uint16_t>(cp < 0x10000 ? cp : '?'));
    }
  }
  return u;
}

// ============================================================ Fruity NoteBook 2
// u32 version 0, u32 current page (1-based), pages { u32 page, character
// count (u8, or 0xFF + u32), UTF-16LE text (CR line ends) }, u32 -1, u8 1.
void putNotePages(ByteWriter& w, const std::vector<std::string>& pages) {
  for (size_t p = 0; p < pages.size(); ++p) {
    if (pages[p].empty()) continue;
    std::string text;
    for (char ch : pages[p]) text += ch == '\n' ? '\r' : ch;
    const std::vector<uint16_t> u = utf16Units(text);
    w.u32(static_cast<uint32_t>(p + 1));
    if (u.size() < 0xFF) {
      w.u8(static_cast<uint32_t>(u.size()));
    } else {
      w.u8(0xFF);
      w.u32(static_cast<uint32_t>(u.size()));
    }
    for (uint16_t x : u) w.u16(x);
  }
}
// Offset just after the page list's -1 terminator (0 when malformed).
size_t notePagesEnd(const Bytes& b) {
  size_t o = 8;
  while (o + 4 <= b.size()) {
    const int32_t page = static_cast<int32_t>(getU32(b, o));
    o += 4;
    if (page == -1) return o;
    if (o >= b.size()) return 0;
    size_t len = b[o++];
    if (len == 0xFF) {
      if (o + 4 > b.size()) return 0;
      len = getU32(b, o);
      o += 4;
    }
    if (o + 2 * len > b.size()) return 0;
    o += 2 * len;
  }
  return 0;
}

Encoder notebook2() {
  Encoder e;
  e.flName = "Fruity NoteBook 2";
  e.glType = "gl.notes";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(0);   // version
    w.u32(1);   // current page
    w.i32(-1);  // end of pages (all empty)
    w.u8(1);
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (!decoded("Fruity NoteBook 2", b, c, cur) || cur.data == g.data) return;
    const size_t end = notePagesEnd(b);
    if (!end) return;
    ByteWriter w;
    w.bytes(slice(b, 0, 8));
    putNotePages(w, ut::decodeNotes(g.data));
    w.i32(-1);
    append(w.b, slice(b, end, b.size()));
    b = std::move(w.b);
  };
  e.fields = [](const Bytes&) { return std::vector<Field>{field(ut::NtPage, 4, Type::U32, 1, ut::kNotePages)}; };
  return e;
}

// ============================================================ Effector
// "SSLF" chunk: u32 size, "10XF", reserved bytes, then 12 normalized floats
// (kind, bypass, mix, X, Y, mod rate, tempo reduce, mod X, mod Y, mod shape,
// input, output) at the end of the chunk.
size_t effectorEnd(const Bytes& b) { return b.size() >= 8 ? std::min(b.size(), static_cast<size_t>(getU32(b, 4)) + 8) : 0; }

Encoder effector() {
  Encoder e;
  e.flName = "Effector";
  e.glType = "gl.effector";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return param(s, dy::EfModel) >= 0.5f; };  // reference engine
  e.defaults = [] {
    ByteWriter w;
    w.bytes("SSLF", 4);
    w.u32(4 + 256 + 48);  // chunk size
    w.bytes("10XF", 4);
    w.zeros(256);          // reserved (zero in fresh states)
    w.f32(6.f / 11.f);     // kind: reverb
    w.f32(1.f);            // bypass switch
    w.f32(1.f);            // mix
    w.f32(230.f / 256.f);  // X
    w.f32(31.f / 81.f);    // Y
    w.f32(1.f / 9.f);      // mod rate
    w.f32(0.f);            // tempo reduce
    w.f32(0.5f);           // mod X (none)
    w.f32(0.5f);           // mod Y (none)
    w.f32(0.f);            // mod shape
    w.f32(1.f);            // input (+6 dB knob end: 0.5 = unity)
    w.f32(0.5f);           // output (unity)
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> v;
    const size_t end = effectorEnd(b);
    if (end < 48 + 12) return v;
    const size_t at = end - 48;
    // Reference engine: the twelve stored floats as they are (EfRawKind..).
    for (int k = 0; k < 12; ++k) v.push_back(Field{dy::EfRawKind + k, at + 4 * static_cast<size_t>(k), Type::F32, 0, 1, [](double g) { return g; }, {}});
    return v;
  };
  return e;
}

// ============================================================ Wave Candy
// u32 version 9, i32 hold, 28 colour records (6 × i32: opacity, brightness,
// hue, saturation, glow, ARGB colour), short strings title and font name, 8
// bytes font metrics, the 88-byte main block (see the decoder), then window
// flags and rectangles.
constexpr uint32_t kWcVersion = 9;
// Default colour records (element order of the plugin; the values its plugin database entry holds).
const int64_t kWcColours[28][6] = {
    {256, 100, 0, 0, 0, 0x63000000},   {256, 80, 0, 0, 0, 0x4F000000},    {256, 160, 0, 0, 0, 0x9F000000},   {64, 256, 0, 0, 0, 0xFF000000},
    {64, 256, 0, 256, 0, 0xFFFFFFFF},  {64, 256, 0, 256, 0, 0xFFFFFFFF},  {64, 256, 0, 0, 0, 0xFF000000},   {128, 256, 0, 256, 0, 0xFFFFFFFF},
    {128, 256, 0, 256, 0, 0xFFFFFFFF}, {192, 256, 0, 256, 0, 0xFFFFFFFF}, {96, 256, 0, 256, 0, 0xFFFFFFFF},  {64, 256, 0, 256, 0, 0xFFFFFFFF},
    {256, 256, 0, 256, 128, 0xFFFFFFFF}, {0, 256, 0, 0, 0, 0},           {120, 256, 0, 120, 128, 0x771C1C53}, {200, 256, 168, 120, 256, 0xC7BA0B00},
    {256, 256, 128, 160, 256, 0xFFFFFF40}, {256, 256, 0, 256, 0, 0xFFFFFFFF}, {90, 200, 0, 0, 0, 0xC7000000},  {256, 256, 60, 90, 200, 0xFF14A067},
    {256, 256, 80, 100, 220, 0xFF0EB923}, {256, 256, 80, 110, 256, 0xFF00DB1B}, {256, 256, 128, 120, 256, 0xFFEFEF00}, {256, 256, 160, 120, 256, 0xFFEF3C00},
    {128, 256, 0, 256, 0, 0xFFFFFFFF}, {0, 0, 0, 256, 0, 0},              {24, 256, 0, 256, 0, 0xFFFFFFFF},  {32, 256, 0, 256, 0, 0xFFFFFFFF}};

size_t wcTitleAt(const Bytes& b) { return 8 + 24 * static_cast<size_t>(versionOf(b) >= 5 ? 28 : 26); }
// Main block offset (0 when malformed).
size_t wcMainAt(const Bytes& b) {
  size_t o = wcTitleAt(b);
  if (o >= b.size()) return 0;
  o += 1 + b[o];
  if (o >= b.size()) return 0;
  o += 1 + b[o] + 8;
  return o + 88 <= b.size() ? o : 0;
}
std::string wcTitle(const std::string& data) {
  const std::string key = "\ntitle=";
  if (data.rfind("GLSCOPE1", 0) != 0) return {};
  const size_t p = data.find(key);
  if (p == std::string::npos) return {};
  const size_t e = data.find('\n', p + key.size());
  return data.substr(p + key.size(), e == std::string::npos ? std::string::npos : e - p - key.size());
}
void putShortString(ByteWriter& w, const std::string& utf8) {  // u8 length + Latin-1
  std::string s;
  for (uint32_t cp : codePoints(utf8))
    if (s.size() < 255) s += static_cast<char>(cp < 256 ? cp : '?');
  w.u8(static_cast<uint32_t>(s.size()));
  w.bytes(s.data(), s.size());
}

Encoder waveCandy() {
  Encoder e;
  e.flName = "Wave Candy";
  e.glType = "gl.scope";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return param(s, ut::ScEngine) >= 0.5f; };
  e.defaults = [] {
    ByteWriter w;
    w.u32(kWcVersion);
    w.i32(0);  // hold
    for (const auto& c : kWcColours)
      for (int64_t x : c) w.u32(static_cast<uint32_t>(x));
    putShortString(w, "Wave Candy");    // title
    putShortString(w, "Trebuchet MS");  // font
    w.i32(-15);                         // font height
    w.u8(1), w.u8(2), w.u8(15), w.u8(1);  // font style, pitch, quality, charset
    // Main block.
    w.i32(0);     // view: oscilloscope
    w.i32(0);     // gain (1/10 dB)
    w.i32(5001);  // fade (ms, 5001 = never)
    w.i32(64);    // bevel / reflection
    w.i32(0);     // punches
    w.i32(1000);  // scope update (1/100 ms)
    w.i32(2000);  // scope window (‰ of the update)
    w.i32(1500);  // spectrum update (1/100 ms)
    w.i32(5);     // max resolution index
    w.i32(0);     // frequency curve
    w.i32(96);    // dB range
    w.i32(128);   // meter scale
    w.i32(750);   // decay (ms)
    w.i32(2500);  // peak hold (ms, 5001 = infinite)
    w.u8(1), w.u8(1), w.u8(1), w.u8(0), w.u8(0), w.u8(0), w.u8(0), w.u8(0);  // pinch, interpolate, visible, desktop, -, in front, meter mode, auto view
    w.i32(256);   // corners
    w.i32(-1);    // stereo separation (overlaid)
    w.i32(1000);  // vector update
    w.i32(350);   // vector thickness (1/100 px)
    w.i32(2);     // vector mode
    w.u8(1), w.u8(1), w.u8(0), w.u8(1);  // vector add, natural weighting, glow, enhanced frequency
    // Window flags and rectangles (editor / detached).
    w.u8(0), w.u8(1), w.u8(1), w.u8(0);
    for (int32_t x : {600, 200, 901, 325, 1189, 933, 1260, 1004}) w.i32(x);
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (!decoded("Wave Candy", b, c, cur) || cur.data == g.data) return;
    const size_t at = wcTitleAt(b);
    if (at >= b.size() || at + 1 + b[at] > b.size()) return;
    const std::string title = wcTitle(g.data);
    ByteWriter w;
    w.bytes(slice(b, 0, at));
    putShortString(w, title.empty() ? "Wave Candy" : title);
    append(w.b, slice(b, at + 1 + b[at], b.size()));
    b = std::move(w.b);
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> v{i32(ut::ScFreeze, 4, 0, 1)};
    const size_t m = wcMainAt(b);
    if (versionOf(b) < 9 || !m) return v;
    auto I = [&](int gl, size_t off, double lo, double hi) { v.push_back(i32(gl, m + off, lo, hi)); };
    auto B = [&](int gl, size_t off) { v.push_back(field(gl, m + off, Type::U8, 0, 1)); };
    I(ut::ScMode, 0, 0, 3);
    I(ut::ScGain, 4, -120, 120);
    I(ut::ScFadeDelay, 8, 0, 5001);
    I(ut::ScFade, 8, 0, 5001);
    I(ut::ScWaveRefresh, 20, 100, 10000);
    I(ut::ScWindow, 24, 0, 10000);
    I(ut::ScSpecRefresh, 28, 100, 20000);
    I(ut::ScSpecBands, 32, 0, 8);
    I(ut::ScRes, 32, 0, 8);
    I(ut::ScFreqCurve, 36, 0, 256);
    I(ut::ScRange, 40, 48, 144);
    I(ut::ScMeterCurve, 44, 0, 256);
    I(ut::ScFallTime, 48, 1, 5000);
    I(ut::ScDecay, 48, 1, 20000);
    I(ut::ScHold, 52, 0, 5001);
    I(ut::ScHoldInf, 52, 0, 5001);
    B(ut::ScTaper, 56);
    B(ut::ScSmoothZoom, 57);
    v.push_back(field(ut::ScMeterType, m + 62, Type::U8, 0, 5));
    v.push_back(field(ut::ScMeterMode, m + 62, Type::U8, 0, 5));
    B(ut::ScAutoView, 63);
    I(ut::ScSplitGap, 68, -1, 1000);
    I(ut::ScSplit, 68, -1, 1000);
    I(ut::ScVecRefresh, 72, 100, 10000);
    I(ut::ScVecWidth, 76, 100, 600);
    I(ut::ScVecStyle, 80, 0, 3);
    B(ut::ScVecAdd, 84);
    B(ut::ScPink, 85);
    B(ut::ScTilt, 85);
    B(ut::ScSharpen, 87);
    return v;
  };
  return e;
}

// ============================================================ Control Surface
// u32 version 1, chunks { u32 id, u64 size, payload }: 2000 panel settings,
// then per control 2100 (type, …), 2103 name, 2104 rectangle, 2102 value(s),
// 2105.. property text. Only the control values are written: controls (their
// kinds, names and layout) cannot be created from Gridline's state.
Encoder controlSurface() {
  Encoder e;
  e.flName = "Control Surface";
  e.glType = "gl.surface";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(1);  // version
    w.u32(2000);
    w.u64(64);  // panel settings
    w.u32(6);   // (first panel word of a fresh surface)
    w.zeros(60);
    return w.b;
  };
  // Values of the value controls in order (a pad has two; bevels and labels none, J-41).
  e.fields = [](const Bytes& b) {
    std::vector<Field> v;
    size_t o = 4;
    int k = 0, type = -1;
    while (o + 12 <= b.size()) {
      const uint32_t id = getU32(b, o);
      const uint64_t size = static_cast<uint64_t>(getU32(b, o + 4)) | static_cast<uint64_t>(getU32(b, o + 8)) << 32;
      o += 12;
      if (size > b.size() - o) break;
      if (id == 2100) type = size >= 4 ? static_cast<int>(getU32(b, o)) : 0;
      if (id == 2102 && type >= 0 && type != 5 && type != 6) {
        const int n = type == 1 ? 2 : 1;
        for (int j = 0; j < n && 4 * static_cast<uint64_t>(j) + 4 <= size; ++j, ++k)
          if (k < md::kSurfaceMax) v.push_back(field(md::surfaceParam(k), o + 4 * static_cast<size_t>(j), Type::F32, 0, 1));
        type = -1;
      }
      o += static_cast<size_t>(size);
    }
    return v;
  };
  // J-39: the controls themselves (Gridline's kind + name per control): a
  // control the template already holds with the same kind and name keeps its
  // chunks (FL's layout, colours, captions); the others are written as FL's
  // control kinds with their class only (wheel, slider, button; toggles
  // "StayDown"), on a grid. Values are solved by the fields afterwards.
  // J-41: an imported surface carries its FL chunks ("#fl <hex>" line): it is
  // written back from them and the values are solved into its 2102 records.
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    if (const size_t at = g.data.find("#fl "); at != std::string::npos && (at == 0 || g.data[at - 1] == '\n')) {
      const size_t endl = g.data.find('\n', at);
      const std::string hx = g.data.substr(at + 4, endl == std::string::npos ? std::string::npos : endl - at - 4);
      Bytes raw;
      auto v = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : 0; };
      for (size_t i = 0; i + 1 < hx.size(); i += 2) raw.push_back(static_cast<uint8_t>(v(hx[i]) * 16 + v(hx[i + 1])));
      if (!raw.empty()) {
        b = raw;
        return;
      }
    }
    PluginState cur;
    if (decoded("Control Surface", b, c, cur) && cur.data == g.data) return;
    struct Want {
      int kind;
      std::string name;
    };
    std::vector<Want> want;
    for (size_t pos = 0; pos < g.data.size();) {
      size_t nl = g.data.find('\n', pos);
      if (nl == std::string::npos) nl = g.data.size();
      const std::string line = g.data.substr(pos, nl - pos);
      pos = nl + 1;
      const size_t sp = line.find(' ');
      if (sp == std::string::npos || line[0] == '#') continue;
      want.push_back({std::atoi(line.c_str()), line.substr(sp + 1)});
    }
    // Template chunks: panel chunks, and each control's chunk run.
    struct Chunk {
      uint32_t id;
      Bytes data;
    };
    std::vector<Chunk> panel;
    std::vector<std::vector<Chunk>> ctls;
    for (size_t o = 4; o + 12 <= b.size();) {
      const uint32_t id = getU32(b, o);
      const uint64_t size = static_cast<uint64_t>(getU32(b, o + 4)) | static_cast<uint64_t>(getU32(b, o + 8)) << 32;
      if (size > b.size() - o - 12) break;
      Chunk ch{id, slice(b, o + 12, o + 12 + static_cast<size_t>(size))};
      o += 12 + static_cast<size_t>(size);
      if (id == 2100) ctls.emplace_back();
      if (id >= 2100 && !ctls.empty()) ctls.back().push_back(std::move(ch));
      else if (id < 2100) panel.push_back(std::move(ch));
    }
    auto utf16Bytes = [](const std::string& s) {
      Bytes out;
      for (uint16_t u : utf16Units(s)) out.push_back(static_cast<uint8_t>(u)), out.push_back(static_cast<uint8_t>(u >> 8));
      return out;
    };
    // What the decoder makes of one template control (kind, name).
    auto readCtl = [&](const std::vector<Chunk>& cs, int k) {
      Want w{md::SkKnob, ""};
      int type = 0;
      std::string props;
      for (const Chunk& ch : cs) {
        std::string t;
        for (size_t i = 0; i + 1 < ch.data.size(); i += 2) t += static_cast<char>(ch.data[i]);  // ASCII view suffices for the class tests
        if (ch.id == 2100 && ch.data.size() >= 4) type = static_cast<int>(getU32(ch.data, 0));
        if (ch.id >= 2105 && ch.id <= 2110) props += t;
      }
      if (props.find("TQuickBtn") != std::string::npos || type == 3) w.kind = props.find("StayDown = True") != std::string::npos ? md::SkToggle : md::SkButton;
      else if (props.find("Slider") != std::string::npos) w.kind = md::SkSlider;
      else if (props.find("XY") != std::string::npos) w.kind = md::SkPadX;
      (void)k;
      return w;
    };
    auto prop = [&](uint32_t id, const std::string& text) { return Chunk{id, utf16Bytes(text)}; };
    std::vector<std::vector<Chunk>> out;
    for (size_t k = 0; k < want.size() && k < static_cast<size_t>(md::kSurfaceControls); ++k) {
      const Want& w = want[k];
      if (k < ctls.size()) {  // same kind: keep FL's chunks, with the name
        if (readCtl(ctls[k], static_cast<int>(k)).kind == w.kind) {
          std::vector<Chunk> cs = ctls[k];
          bool named = false;
          for (Chunk& ch : cs)
            if (ch.id == 2103) ch.data = utf16Bytes(w.name), named = true;
          if (!named) cs.insert(cs.begin() + 1, Chunk{2103, utf16Bytes(w.name)});
          out.push_back(cs);
          continue;
        }
      }
      const bool btn = w.kind == md::SkButton || w.kind == md::SkToggle;
      const int type = btn ? 3 : w.kind == md::SkSlider ? 2 : 0;
      const char* cls = btn ? "TQuickBtn" : w.kind == md::SkSlider ? "TVectorSlider" : w.kind == md::SkPadX || w.kind == md::SkPadY ? "TVectorXY" : "TVectorWheel";
      std::vector<Chunk> cs;
      ByteWriter t;
      t.u32(static_cast<uint32_t>(type));
      t.zeros(28);
      cs.push_back({2100, t.b});
      cs.push_back({2103, utf16Bytes(w.name)});
      ByteWriter r;  // rectangle: a 4-column grid of 64 × 64 cells
      const float x = static_cast<float>(8 + 72 * (k % 4)), y = static_cast<float>(8 + 72 * (k / 4));
      r.f32(x), r.f32(y), r.f32(x + 64), r.f32(y + 64);
      cs.push_back({2104, r.b});
      ByteWriter v;
      v.f32(0.5f), v.u32(0), v.u32(0);
      cs.push_back({2102, v.b});
      cs.push_back(prop(2105, std::string("control ") + cls + "\r\nend\r\n"));
      if (w.kind == md::SkToggle) cs.push_back(prop(2107, "control TQuickBtn\r\n  StayDown = True\r\nend\r\n"));
      cs.push_back({2101, {}});
      out.push_back(cs);
    }
    ByteWriter w;
    w.u32(b.size() >= 4 ? getU32(b, 0) : 1);
    for (const Chunk& ch : panel) w.u32(ch.id), w.u64(ch.data.size()), w.bytes(ch.data);
    for (const auto& cs : out)
      for (const Chunk& ch : cs) w.u32(ch.id), w.u64(ch.data.size()), w.bytes(ch.data);
    b = w.b;
  };
  return e;
}

// ============================================================ Vocodex
// u32 version (0x00080003: version 8), 13 curve records {i32 3, i32 kind, i32
// n, n × {f64 dx, f64 y, f32 tension, u32 flags}, u32, i32 × 4}, i32 × 20
// tags, i32 × 7 (hold mode, order, unison - 1, bands, native rate, modulator
// input, carrier input), 8 option bytes, 13 pitch-follow bytes, carrier tone
// name (u8 length + text), i32 held key count + keys.
constexpr uint32_t kVxVersion = 0x00080003;
void putVxRecord(ByteWriter& w, int32_t kind, const std::vector<sp::VxPoint>& pts) {
  w.i32(3);
  w.i32(kind);
  w.u32(static_cast<uint32_t>(pts.size()));
  for (size_t i = 0; i < pts.size(); ++i) {
    w.f64(pts[i].dx);
    w.f64(pts[i].y);
    w.f32(static_cast<float>(pts[i].t));
    w.u32(i == 0 ? 0u : (pts[i].t > 0 ? 1u : pts[i].t < 0 ? 0xFFu : 2u) << 24);  // tension direction (2 = straight)
  }
  w.u32(0);
  for (int k = 0; k < 4; ++k) w.i32(-1);
}
// Layout of a state: end of the curve records, of the fixed ints and flags.
struct VxLayout {
  bool ok = false;
  size_t mapsEnd = 0, tags = 0, ints = 0, flags = 0, pf = 0, tone = 0, keys = 0, end = 0;
  int ver = 0, nTags = 0, nFlags = 0;
};
VxLayout vxLayout(const Bytes& b) {
  VxLayout L;
  L.ver = static_cast<int>(versionOf(b));
  if (L.ver < 2 || L.ver > 8) return L;
  size_t o = 4;
  for (int k = 0; k < sp::kVxMaps; ++k) {
    if (o + 12 > b.size() || getU32(b, o) != 3) return L;
    const size_t n = getU32(b, o + 8);
    if (n < 1 || n > 4096 || o + 12 + n * 24 + 20 > b.size()) return L;
    o += 12 + n * 24 + 20;
  }
  L.mapsEnd = L.tags = o;
  L.nTags = L.ver < 3 ? 19 : 20;
  L.ints = L.tags + 4 * static_cast<size_t>(L.nTags);
  L.flags = L.ints + 28;
  L.nFlags = L.ver >= 5 ? 8 : (L.ver == 4 ? 5 : 4);
  L.pf = L.flags + static_cast<size_t>(L.nFlags);
  L.tone = L.pf + (L.ver >= 4 ? sp::kVxMaps : 0);
  if (L.tone > b.size()) return L;
  L.keys = L.tone < b.size() ? L.tone + 1 + b[L.tone] : L.tone;
  if (L.keys > b.size()) return L;
  L.end = L.keys;
  if (L.keys + 4 <= b.size()) L.end = std::min(b.size(), L.keys + 4 + 4 * static_cast<size_t>(std::clamp<int32_t>(static_cast<int32_t>(getU32(b, L.keys)), 0, 128)));
  L.ok = true;
  return L;
}
// Carrier tone name for a tone family (words the decoder recognises; 0 = none).
const char* vxToneName(int tone) {
  static const char* const names[9] = {"", "Square", "PWM", "Sine", "Super", "Strings", "Organ", "Buzz", "Noise"};
  return names[std::clamp(tone, 0, 8)];
}

Encoder vocodex() {
  Encoder e;
  e.flName = "Vocodex";
  e.glType = "gl.vocoder";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return param(s, sp::VxModel) >= 0.5f; };  // reference model
  e.defaults = [] {
    ByteWriter w;
    w.u32(kVxVersion);
    const sp::VxData d = sp::vxDefaultData();
    for (int k = 0; k < sp::kVxMaps; ++k) putVxRecord(w, 3, d.maps[k]);
    for (int32_t x : {1000,    // 0 wet
                      150,     // 1 soundgoodizer
                      12800,   // 2 modulator level
                      55000,   // 3 modulator high pass
                      0,       // 4 carrier level
                      28000,   // 5 carrier low pass
                      0,       // 6 noise level
                      55000,   // 7 noise high pass
                      1666,    // 8 hold
                      1115,    // 9 attack
                      0,       // 10 attack curve
                      3500,    // 11 release
                      0,       // 12 release curve
                      0,       // 13 freeze
                      349,     // 14 band width
                      100,     // 15 flatness
                      2000,    // 16 modulator band width
                      0,       // 17 modulator pitch
                      250,     // 18 unison pitch
                      128})    // 19 unison pan
      w.i32(x);
    for (int32_t x : {0,    // hold mode
                      2,    // order
                      0,    // unison - 1
                      47,   // bands
                      0,    // native rate
                      1,    // modulator input
                      0})   // carrier input
      w.i32(x);
    for (uint32_t f : {0u, 1u, 1u, 1u, 1u, 1u, 0u, 1u}) w.u8(f);  // L-R, min times, contour mod, threaded, denoise, contour noise / carrier / consonants
    for (int k = 0; k < sp::kVxMaps; ++k) w.u8(d.pf[k] ? 1 : 0);  // pitch follow per curve
    w.u8(0);    // carrier tone name (none)
    w.i32(1);   // held keys
    w.i32(60);  // middle C
    return w.b;
  };
  // Curves, pitch follow, held keys (data) and the carrier tone name.
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (!decoded("Vocodex", b, c, cur)) return;
    sp::VxData d, dc;
    sp::vxParse(g.data, d);
    sp::vxParse(cur.data, dc);
    // J-65: the imported tone name is kept in the data; otherwise the family's name.
    const bool toneOk = param(cur, sp::VxTone) == param(g, sp::VxTone) && (!d.hasTone || (dc.hasTone && dc.tone == d.tone));
    if (cur.data == g.data && toneOk) return;
    const VxLayout L = vxLayout(b);
    if (!L.ok) return;
    ByteWriter w;
    w.bytes(slice(b, 0, 4));
    size_t o = 4;
    for (int k = 0; k < sp::kVxMaps; ++k) {
      putVxRecord(w, static_cast<int32_t>(getU32(b, o + 4)), d.maps[k]);
      o += 12 + static_cast<size_t>(getU32(b, o + 8)) * 24 + 20;
    }
    append(w.b, slice(b, L.mapsEnd, L.pf));
    if (L.ver >= 4)
      for (int k = 0; k < sp::kVxMaps; ++k) w.u8(d.pf[k] ? 1 : 0);
    if (toneOk) {
      append(w.b, slice(b, L.tone, L.keys));
    } else {
      const int fam = static_cast<int>(std::lround(param(g, sp::VxTone)));
      const bool keep = d.hasTone && !d.tone.empty() && sp::vxToneFamily(d.tone) == fam;
      const std::string name = keep ? d.tone : vxToneName(fam);
      w.u8(static_cast<uint32_t>(name.size()));
      w.bytes(name.data(), name.size());
    }
    w.i32(static_cast<int32_t>(d.keys.size()));
    for (int k : d.keys) w.i32(k);
    append(w.b, slice(b, L.end, b.size()));
    b = std::move(w.b);
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> v;
    const VxLayout L = vxLayout(b);
    if (!L.ok || L.ver < 3) return v;
    const struct {
      int gl;
      double lo, hi;
    } tags[20] = {{sp::VcWetLevel, 0, 2000},  {sp::VxEnhance, 0, 1000},    {sp::VcModLevel, 0, 16000}, {sp::VxModHp, 0, 65536},
                  {sp::VcCarLevel, 0, 16000}, {sp::VxCarLp, 0, 65536},     {sp::VxNoise, 0, 12800},    {sp::VxNoiseHp, 0, 65536},
                  {sp::VxHoldMs, 0, 10000},   {sp::VxAttackMs, 0, 10000},  {sp::VxAttCurve, 0, 2000},  {sp::VxReleaseMs, 0, 10000},
                  {sp::VxRelCurve, 0, 2000},  {sp::VcHold, 0, 1},          {sp::VxWidth, 5, 1000},     {sp::VxFlatness, 0, 100},
                  {sp::VxModWidth, 0, 4000},  {sp::VcFormant, -1200, 1200}, {sp::VxUnisonShift, 0, 1000}, {sp::VxUnisonPan, -256, 256}};
    for (size_t k = 0; k < 20; ++k) v.push_back(i32(tags[k].gl, L.tags + 4 * k, tags[k].lo, tags[k].hi));
    v.push_back(i32(sp::VxHoldMode, L.ints + 0, 0, 1));
    v.push_back(i32(sp::VxOrder, L.ints + 4, 1, 4));
    v.push_back(i32(sp::VxUnison, L.ints + 8, 0, sp::kVxMaxUnison - 1));
    v.push_back(i32(sp::VxBands, L.ints + 12, 5, sp::kVxMaxBands));
    v.push_back(i32(sp::VcModSource, L.ints + 20, 0, 1));
    v.push_back(i32(sp::VxCarSource, L.ints + 24, -1, 1));
    const int flagParams[8] = {sp::VxLrEncoding, sp::VxMinTimes, sp::VxContourMod, -1, sp::VxDenoise, sp::VxContourNoise, sp::VxContourCar, sp::VxContourCons};
    for (int k = 0; k < L.nFlags; ++k)
      if (flagParams[k] >= 0 && !(k == 4 && L.ver < 5)) v.push_back(field(flagParams[k], L.flags + static_cast<size_t>(k), Type::U8, 0, 1));
    return v;
  };
  return e;
}

}  // namespace

void registerFxMisc(std::vector<Encoder>& v) {
  v.push_back(notebook2());
  v.push_back(effector());
  v.push_back(waveCandy());
  v.push_back(controlSurface());
  v.push_back(vocodex());
}

}  // namespace gl::flp::enc
