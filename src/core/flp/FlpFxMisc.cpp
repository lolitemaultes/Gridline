// Gridline — FLP import: decoders for FL envelope / time effects, vocoder,
// analyzers, notes and control panels.
//
// State layouts were established by comparing saved instances and factory
// presets (notes: analysis/demos/notes_gamma.md); the code is original.
//
// Several of these plugins save envelopes with one shared record layout
// ("envelope record" below):
//   u32 kind, u32 format (3 = 64-bit points), u32 n,
//   n × { f64 dx (beats since the previous point), f64 y (0..1), f32 tension, u32 flags },
//   u32 ?, i32 × 4 (loop start / loop end point indices, -1 = none, …)
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/spectral/spectral.h"
#include "../../packs/utility/utility.h"
#include "../PluginParams.h"
#include "FlpFx.h"

namespace gl::flp::fx {

namespace {

// ---------------------------------------------------------------- inflate
// Minimal RFC 1950/1951 decoder (zlib wrapper, stored / fixed / dynamic
// Huffman blocks). Returns false on malformed input.
class Inflater {
 public:
  Inflater(const uint8_t* in, size_t n) : in_(in), n_(n) {}
  bool run(std::vector<uint8_t>& out, size_t limit) {
    out_ = &out;
    limit_ = limit;
    int last = 0;
    do {
      if (!need(3)) return false;
      last = bits(1);
      int type = bits(2);
      bool ok = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : false;
      if (!ok) return false;
    } while (!last);
    return true;
  }

 private:
  struct Huff {
    uint16_t count[16] = {};
    uint16_t sym[320] = {};
  };
  const uint8_t* in_;
  size_t n_, pos_ = 0, limit_ = 0;
  uint32_t buf_ = 0;
  int cnt_ = 0;
  std::vector<uint8_t>* out_ = nullptr;

  bool need(int k) {
    while (cnt_ < k) {
      if (pos_ >= n_) return false;
      buf_ |= static_cast<uint32_t>(in_[pos_++]) << cnt_;
      cnt_ += 8;
    }
    return true;
  }
  int bits(int k) {
    if (!need(k)) return -1;
    int v = static_cast<int>(buf_ & ((1u << k) - 1));
    buf_ >>= k;
    cnt_ -= k;
    return v;
  }
  bool stored() {
    buf_ = 0, cnt_ = 0;
    if (pos_ + 4 > n_) return false;
    size_t len = in_[pos_] | (in_[pos_ + 1] << 8);
    pos_ += 4;
    if (pos_ + len > n_ || out_->size() + len > limit_) return false;
    out_->insert(out_->end(), in_ + pos_, in_ + pos_ + len);
    pos_ += len;
    return true;
  }
  static bool build(Huff& h, const uint8_t* lengths, int n) {
    std::fill(std::begin(h.count), std::end(h.count), 0);
    for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
    h.count[0] = 0;
    uint16_t offs[16] = {};
    for (int l = 1; l < 15; ++l) offs[l + 1] = static_cast<uint16_t>(offs[l] + h.count[l]);
    for (int i = 0; i < n; ++i)
      if (lengths[i]) h.sym[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    return true;
  }
  int decode(const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      int b = bits(1);
      if (b < 0) return -1;
      code |= b;
      int count = h.count[len];
      if (code - count < first) return h.sym[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }
  bool codes(const Huff& lit, const Huff& dist) {
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int s = decode(lit);
      if (s < 0) return false;
      if (s < 256) {
        if (out_->size() >= limit_) return false;
        out_->push_back(static_cast<uint8_t>(s));
      } else if (s == 256) {
        return true;
      } else {
        s -= 257;
        if (s >= 29) return false;
        int e = bits(lext[s]);
        if (e < 0) return false;
        size_t len = lbase[s] + static_cast<size_t>(e);
        int d = decode(dist);
        if (d < 0 || d >= 30) return false;
        int de = bits(dext[d]);
        if (de < 0) return false;
        size_t dd = dbase[d] + static_cast<size_t>(de);
        if (dd > out_->size() || out_->size() + len > limit_) return false;
        size_t from = out_->size() - dd;
        for (size_t k = 0; k < len; ++k) out_->push_back((*out_)[from + k]);
      }
    }
  }
  bool fixed() {
    static Huff lit, dist;
    static bool init = false;
    if (!init) {
      uint8_t l[288];
      int i = 0;
      for (; i < 144; ++i) l[i] = 8;
      for (; i < 256; ++i) l[i] = 9;
      for (; i < 280; ++i) l[i] = 7;
      for (; i < 288; ++i) l[i] = 8;
      build(lit, l, 288);
      uint8_t d[30];
      std::fill(d, d + 30, 5);
      build(dist, d, 30);
      init = true;
    }
    return codes(lit, dist);
  }
  bool dynamic() {
    int nlen = bits(5), ndist = bits(5), ncode = bits(4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return false;
    nlen += 257, ndist += 1, ncode += 4;
    if (nlen > 286 || ndist > 30) return false;
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lengths[320] = {};
    for (int i = 0; i < ncode; ++i) {
      int b = bits(3);
      if (b < 0) return false;
      lengths[order[i]] = static_cast<uint8_t>(b);
    }
    Huff lc;
    build(lc, lengths, 19);
    int idx = 0;
    std::fill(lengths, lengths + 320, 0);
    while (idx < nlen + ndist) {
      int sym = decode(lc);
      if (sym < 0) return false;
      if (sym < 16) {
        lengths[idx++] = static_cast<uint8_t>(sym);
      } else {
        int len = 0, rep;
        if (sym == 16) {
          if (idx == 0) return false;
          len = lengths[idx - 1];
          rep = 3 + bits(2);
        } else if (sym == 17) {
          rep = 3 + bits(3);
        } else {
          rep = 11 + bits(7);
        }
        if (rep < 3 || idx + rep > nlen + ndist) return false;
        while (rep--) lengths[idx++] = static_cast<uint8_t>(len);
      }
    }
    Huff lit, dist;
    build(lit, lengths, nlen);
    build(dist, lengths + nlen, ndist);
    return codes(lit, dist);
  }
};

// zlib stream (2-byte header) → bytes. Empty on failure.
std::vector<uint8_t> unzlib(const uint8_t* p, size_t n, size_t limit = 8u << 20) {
  std::vector<uint8_t> out;
  if (n < 2 || (p[0] & 0x0F) != 8 || ((p[0] << 8) | p[1]) % 31 != 0) return out;
  Inflater inf(p + 2, n - 2);
  if (!inf.run(out, limit)) out.clear();
  return out;
}

// ---------------------------------------------------------------- envelope records

uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
  return o + 4 <= b.size() ? static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
                                 (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24)
                           : 0;
}
double rdf64(const std::vector<uint8_t>& b, size_t o) {
  uint64_t u = static_cast<uint64_t>(rd32(b, o)) | (static_cast<uint64_t>(rd32(b, o + 4)) << 32);
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}
float rdf32(const std::vector<uint8_t>& b, size_t o) {
  uint32_t u = rd32(b, o);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}



// UTF-16 text → UTF-8. Surrogate pairs combine into one code point (lone
// halves become U+FFFD), and text stored big-endian (seen in some shipped
// notebooks) is detected from its byte pattern or a byte-order mark.
std::string utf16(const uint8_t* p, size_t bytes) {
  size_t i = 0;
  bool be = false;
  if (bytes >= 2 && p[0] == 0xFE && p[1] == 0xFF) be = true, i = 2;
  else if (bytes >= 2 && p[0] == 0xFF && p[1] == 0xFE) i = 2;
  else {
    size_t le = 0, beHits = 0;
    for (size_t k = 0; k + 1 < bytes; k += 2) {
      if (p[k + 1] == 0 && p[k] >= 0x20 && p[k] < 0x7F) ++le;
      if (p[k] == 0 && p[k + 1] >= 0x20 && p[k + 1] < 0x7F) ++beHits;
    }
    be = beHits > le * 2;
  }
  auto unit = [&](size_t k) { return be ? static_cast<uint32_t>((p[k] << 8) | p[k + 1]) : static_cast<uint32_t>(p[k] | (p[k + 1] << 8)); };
  std::string s;
  auto put = [&](uint32_t cp) {
    if (cp < 0x80) {
      s += static_cast<char>(cp);
    } else if (cp < 0x800) {
      s += static_cast<char>(0xC0 | (cp >> 6));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      s += static_cast<char>(0xE0 | (cp >> 12));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      s += static_cast<char>(0xF0 | (cp >> 18));
      s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    }
  };
  for (; i + 1 < bytes; i += 2) {
    uint32_t cu = unit(i);
    if (cu == 0) break;
    if (cu >= 0xD800 && cu < 0xDC00) {
      uint32_t lo = i + 3 < bytes ? unit(i + 2) : 0;
      if (lo >= 0xDC00 && lo < 0xE000) {
        put(0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00));
        i += 2;
      } else {
        put(0xFFFD);
      }
    } else if (cu >= 0xDC00 && cu < 0xE000) {
      put(0xFFFD);
    } else {
      put(cu);
    }
  }
  return s;
}

std::vector<uint8_t> stateBytes(const std::vector<Event>& evs) {
  const Event* e = pluginState(evs);
  return e ? std::vector<uint8_t>(e->data, e->data + e->size) : std::vector<uint8_t>{};
}

double gainDb(double g) { return g > 1e-6 ? 20.0 * std::log10(g) : -120.0; }

// ============================================================ Fruity NoteBook 2
// u32 version (0), u32 current page (1-based), then pages
// { u32 page (1-based), LEB128 character count, UTF-16LE text } until page = -1, u8 ?.
bool decodeNotebook(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  (void)c;
  std::vector<uint8_t> b = stateBytes(evs);
  begin(m, "gl.notes");
  if (b.size() < 12) return true;
  int current = static_cast<int>(rd32(b, 4));
  std::vector<std::string> pages(packs::utility::kNotePages);
  size_t o = 8;
  bool ok = true;
  while (o + 4 <= b.size()) {
    int32_t page = static_cast<int32_t>(rd32(b, o));
    o += 4;
    if (page == -1) break;
    size_t len = 0;
    int shift = 0;
    while (o < b.size()) {
      uint8_t v = b[o++];
      len |= static_cast<size_t>(v & 0x7F) << shift;
      shift += 7;
      if (!(v & 0x80) || shift > 28) break;
    }
    if (o + len * 2 > b.size() || page < 1) {
      ok = false;
      break;
    }
    std::string text = utf16(b.data() + o, len * 2);
    o += len * 2;
    std::string clean;  // FL uses CR line ends
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\r') {
        clean += '\n';
        if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
      } else {
        clean += text[i];
      }
    }
    if (page <= packs::utility::kNotePages) pages[static_cast<size_t>(page - 1)] = clean;
  }
  m.state.data = packs::utility::encodeNotes(pages);
  set(m, packs::utility::NtPage, std::clamp(current, 1, packs::utility::kNotePages));
  m.exact = ok;
  return true;
}

// ============================================================ Effector
// "SSLF" chunk: u32 size, "10XF", …, then 12 normalized floats (parameter
// order: kind, bypass, mix, X, Y, mod rate, tempo reduce, mod amount X/Y,
// mod shape, input, output) at the end of the chunk.
int effectorKind(int fl) {
  // FL: distortion, lo-fi, flanger, phaser, filter, delay, reverb, panner,
  // trance gate, stutter, vowel, ring mod → Gridline kinds.
  static const int map[12] = {0, 1, 2, 3, 4, 5, 6, 8, 11, 9, 4, 7};
  return map[std::clamp(fl, 0, 11)];
}
std::shared_ptr<const KnobTable> effectorTable() {
  namespace dy = packs::dynamics;
  static auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 1, dy::EfKind, [](double v) { return effectorKind(static_cast<int>(std::lround(v * 11))); }},
      {1, 0, 1, dy::EfBypass, [](double v) { return v >= 0.5 ? 1.0 : 0.0; }},
      {2, 0, 1, dy::EfMix, {}},
      {3, 0, 1, dy::EfX, {}},
      {4, 0, 1, dy::EfY, {}},
      {5, 0, 1, dy::EfModRate, [](double v) { return std::clamp(std::lround(v * 9.0), 0L, 7L); }},
      {7, 0, 1, dy::EfModX, [](double v) { return std::clamp(v * 2 - 1, -1.0, 1.0); }},
      {8, 0, 1, dy::EfModY, [](double v) { return std::clamp(v * 2 - 1, -1.0, 1.0); }},
      {9, 0, 1, dy::EfModShape, [](double v) { return std::clamp(std::lround(v * 5.0), 0L, 5L); }},
      {10, 0, 1, dy::EfInput, [](double v) { return std::clamp(gainDb(v * 2.0), -24.0, 12.0); }},
      {11, 0, 1, dy::EfOutput, [](double v) { return std::clamp(gainDb(v * 2.0), -24.0, 12.0); }},
  });
  return t;
}
bool decodeEffector(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  (void)c;
  std::vector<uint8_t> b = stateBytes(evs);
  begin(m, "gl.effector");
  auto table = effectorTable();
  bindTable(m, table);
  // The table works in the stored 0..1 floats; the controls (and recorded
  // pattern values) are integers: knobs 0..256, kind / rate / shape indices.
  setRawRanges(m, {{0, 0, 11}, {1, 0, 1}, {2, 0, 256}, {3, 0, 256}, {4, 0, 256}, {5, 0, 9},
                   {7, 0, 256}, {8, 0, 256}, {9, 0, 5}, {10, 0, 256}, {11, 0, 256}});
  if (b.size() < 12 + 48 || std::memcmp(b.data(), "SSLF", 4) != 0) return true;
  size_t end = std::min(b.size(), static_cast<size_t>(rd32(b, 4)) + 8);
  if (end < 48 + 12) return true;
  std::vector<double> fl(12);
  for (size_t k = 0; k < 12; ++k) fl[k] = std::clamp(static_cast<double>(rdf32(b, end - 48 + 4 * k)), 0.0, 1.0);
  applyTable(m, *table, fl);
  m.exact = false;  // module algorithms differ; parameters map by role
  return true;
}

// ============================================================ Wave Candy
// Display-only (audio passes untouched) → Scope, reference display engine.
// State: u32 version (1..9), i32 hold (parameter 0); 28 (v ≥ 5) or 26 colour
// records of 24 bytes; short strings title and font name; 8 bytes font
// metrics; main block (88 bytes at v9, older versions end early — missing
// fields keep their defaults): i32 view, gain (±120 = ±12 dB), fade (ms, 5001
// = never), bevel, punches, scope update (1/100 ms), scope window (‰ of the
// update), spectrum update (1/100 ms), max-res index, frequency curve
// (0..256), dB range, meter scale (0..256), decay (ms), hold (ms, 5001 =
// infinite); u8 pinch, interpolate, visible, desktop, -, keep in front, meter
// mode (v ≥ 5), auto view (v ≥ 4); i32 corners; i32 stereo separation (-1
// overlaid), vector update, vector thickness (1/100 px), vector mode; u8
// vector add, natural weighting, glow, enhanced frequency; then window flags
// and rectangles. Appearance (colours, fonts, bevel, window) is not imported.
bool decodeWaveCandy(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  (void)c;
  namespace ut = packs::utility;
  std::vector<uint8_t> b = stateBytes(evs);
  begin(m, "gl.scope");
  set(m, ut::ScEngine, 1);
  static auto table = std::make_shared<const KnobTable>(KnobTable{{0, 0, 1, ut::ScFreeze, [](double v) { return v > 0 ? 1.0 : 0.0; }}});
  bindTable(m, table);
  Reader r(b.data(), b.size());
  const int ver = static_cast<int>(r.u32());
  if (b.size() < 8 || ver < 1 || ver > 64) {
    m.exact = false;
    return true;
  }
  const int hold = static_cast<int32_t>(r.u32());
  r.skip(static_cast<size_t>(ver >= 5 ? 28 : 26) * 24);
  std::string title;
  {
    size_t n = r.u8();
    for (size_t i = 0; i < n && r.has(1); ++i) {
      uint8_t ch = r.u8();
      if (ch < 0x80) title += static_cast<char>(ch);
      else title += static_cast<char>(0xC0 | (ch >> 6)), title += static_cast<char>(0x80 | (ch & 0x3F));
    }
    r.skip(r.u8());  // font name
    r.skip(8);       // font height, style, pitch
  }
  const size_t len = 88 - 4 * (ver < 2) - 4 * (ver < 3) - 17 * (ver < 6) - (ver < 7) - (ver < 8) - (ver < 9);
  if (!r.has(len)) {
    m.exact = false;
    return true;
  }
  const size_t end = r.pos + len;
  auto i32 = [&](int def) { return r.pos + 4 <= end ? static_cast<int>(static_cast<int32_t>(r.u32())) : def; };
  auto u8 = [&](int def) { return r.pos + 1 <= end ? static_cast<int>(r.u8()) : def; };
  const int view = i32(0), gain = i32(0), fade = i32(5001);
  (void)i32(64), (void)i32(0);  // bevel / reflection, punches
  const int oscUpdate = i32(1000), oscWindow = i32(2000), specUpdate = i32(1500), maxRes = i32(5), curve = i32(0), range = i32(96);
  const int meterScale = i32(128), decay = i32(750), holdMs = i32(2500);
  const int pinch = u8(1), interp = u8(1);
  (void)u8(1), (void)u8(0), (void)u8(0), (void)u8(1);  // visible, on desktop, -, keep in front
  int meterMode = u8(0), autoView = u8(0);
  if (ver < 5) meterMode = 0;
  if (ver < 4) autoView = 0;
  (void)i32(256);  // corners
  const int sep = i32(-1), vecUpdate = i32(1000), vecThick = i32(350), vecMode = i32(2);
  const int vecAdd = u8(1), natural = u8(1);
  (void)u8(0);  // glow
  const int enhanced = u8(1);
  r.pos = end;

  static const int views[4] = {ut::SvOsc, ut::SvSpectrum, ut::SvMeters, ut::SvVector};
  set(m, ut::ScMode, views[std::clamp(view, 0, 3)]);
  set(m, ut::ScGain, std::clamp(gain, -120, 120) / 10.0);
  set(m, ut::ScWindow, std::clamp(oscUpdate / 100.0 * oscWindow / 1000.0 / 1000.0, 0.005, 0.5));
  set(m, ut::ScRange, std::clamp(range, 48, 144));
  set(m, ut::ScTilt, natural ? 3 : 0);
  {
    int fftSize = ut::scopeFftSize(maxRes), best = 0;
    for (int k = 1; k < 4; ++k)
      if (std::abs(std::log2(static_cast<double>(1024 << k) / fftSize)) < std::abs(std::log2(static_cast<double>(1024 << best) / fftSize))) best = k;
    set(m, ut::ScRes, best);
  }
  static const int legacyMeter[6] = {0, 0, 1, 2, 3, 3};
  set(m, ut::ScMeterMode, legacyMeter[std::clamp(meterMode, 0, 5)]);
  set(m, ut::ScHold, std::clamp(holdMs, 0, 5000) / 1000.0);
  set(m, ut::ScDecay, std::clamp(60.0 / (std::max(1, decay) / 1000.0), 3.0, 60.0));
  set(m, ut::ScVecMode, 0);
  set(m, ut::ScFreeze, hold > 0 ? 1 : 0);
  set(m, ut::ScAutoView, autoView ? 1 : 0);
  set(m, ut::ScWaveRefresh, std::clamp(oscUpdate / 100.0, 5.0, 50.0));
  set(m, ut::ScSplit, sep >= 0 ? 1 : 0);
  set(m, ut::ScSplitGap, sep >= 0 ? std::clamp(sep / 10.0, 0.0, 100.0) : 0.0);
  set(m, ut::ScTaper, pinch ? 1 : 0);
  set(m, ut::ScSmoothZoom, interp ? 1 : 0);
  set(m, ut::ScSpecRefresh, std::clamp(specUpdate / 100.0, 5.0, 100.0));
  set(m, ut::ScSpecBands, std::clamp(maxRes, 0, 8));
  set(m, ut::ScFreqCurve, std::clamp(curve, 0, 256) / 256.0);
  set(m, ut::ScPink, natural ? 1 : 0);
  set(m, ut::ScSharpen, enhanced ? 1 : 0);
  set(m, ut::ScMeterType, std::clamp(meterMode, 0, 5));
  set(m, ut::ScMeterCurve, std::clamp(meterScale, 0, 256) / 256.0);
  set(m, ut::ScHoldInf, holdMs >= 5001 ? 1 : 0);
  set(m, ut::ScFallTime, std::clamp(decay, 1, 5000));
  set(m, ut::ScVecStyle, std::clamp(vecMode, 0, 3));
  set(m, ut::ScVecWidth, std::clamp(vecThick / 100.0, 1.0, 6.0));
  set(m, ut::ScVecAdd, vecAdd ? 1 : 0);
  set(m, ut::ScVecRefresh, std::clamp(vecUpdate / 100.0, 5.0, 50.0));
  set(m, ut::ScFade, fade < 5001 ? 1 : 0);
  set(m, ut::ScFadeDelay, std::clamp(fade, 0, 5000));
  // The title becomes the display caption (the plugin's default title is not kept).
  if (!title.empty() && title != "Wave Candy") m.state.data = "GLSCOPE1\ntitle=" + title + "\n";
  m.exact = true;  // display-only: every functional setting maps exactly (appearance is Gridline's own)
  return true;
}

// ============================================================ Control Surface
// u32 version, then chunks { u32 id, u64 size, payload }. 2003 = control
// count; per control: 2100 (u32 type, …), 2103 name (UTF-16), 2104 rect
// (4 × f32), 2102 value(s), 2105.. property text ("control TQuickBtn …").
bool decodeSurface(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  begin(m, "gl.surface");
  namespace md = packs::modulation;
  struct Ctl {
    int type = 0;
    std::string name, props;
    double value = 0;
  };
  std::vector<Ctl> ctls;
  size_t o = 4;
  while (o + 12 <= b.size()) {
    uint32_t id = rd32(b, o);
    uint64_t size = static_cast<uint64_t>(rd32(b, o + 4)) | (static_cast<uint64_t>(rd32(b, o + 8)) << 32);
    o += 12;
    if (size > b.size() - o) break;
    const uint8_t* p = b.data() + o;
    if (id == 2100) {
      ctls.push_back({});
      ctls.back().type = static_cast<int>(rd32(b, o));
    } else if (!ctls.empty() && id == 2103) {
      ctls.back().name = utf16(p, static_cast<size_t>(size));
    } else if (!ctls.empty() && id == 2102 && size >= 4) {
      ctls.back().value = std::clamp(static_cast<double>(rdf32(b, o)), 0.0, 1.0);
    } else if (!ctls.empty() && id >= 2105 && id <= 2110) {
      ctls.back().props += utf16(p, static_cast<size_t>(size));
    }
    o += static_cast<size_t>(size);
  }
  std::string data;
  int k = 0;
  bool exact = true;
  for (const Ctl& ct : ctls) {
    if (k >= md::kSurfaceControls) {
      exact = false;
      break;
    }
    int kind = md::SkKnob;
    if (ct.props.find("TQuickBtn") != std::string::npos || ct.type == 3)
      kind = ct.props.find("StayDown = True") != std::string::npos ? md::SkToggle : md::SkButton;
    else if (ct.props.find("Slider") != std::string::npos)
      kind = md::SkSlider;
    else if (ct.props.find("XY") != std::string::npos)
      kind = md::SkPadX;
    data += std::to_string(kind) + " " + (ct.name.empty() ? "Control " + std::to_string(k + 1) : ct.name) + "\n";
    set(m, k, ct.value);
    ++k;
  }
  if (!data.empty()) m.state.data = data;
  // FL parameter k = control k.
  static auto table = [] {
    KnobTable t;
    for (int i = 0; i < md::kSurfaceControls; ++i) t.push_back({i, 0, 1, i, {}});
    return std::make_shared<const KnobTable>(t);
  }();
  bindTable(m, table);
  m.rawNorm = nullptr;  // user-defined controls: the recorded integer range is not known
  if (ctls.size() > static_cast<size_t>(md::kSurfaceControls)) c.report.warn("Control Surface: more than 16 controls, extra ones dropped");
  m.exact = false;  // layout, colours and skins are not reproduced
  (void)exact;
  return true;
}

// ============================================================ Vocodex
// u16 version, u16 ?, envelope records (13 in FL 25) (equalizer / articulation maps),
// then i32 parameters in tag order: wet, soundgoodizer, modulator level,
// modulator HP, carrier level, carrier LP, noise level, noise HP, hold,
// attack, attack curve, release, release curve, freeze, band width, flatness,
// modulator band width, modulator pitch, unison pitch, unison pan, …, band count.
// Vocodex: reference-law conversions (raw tag value → Gridline units).
double vxWetDb(double v) { return std::clamp(gainDb((std::pow(7.0, v / 1000.0) - 1.0) / 6.0), -60.0, 18.0); }
double vxPassDb(double v) { return std::clamp(gainDb((std::pow(11.0, v / 12800.0) - 1.0) / 10.0), -60.0, 6.0); }
double vxHpHz(double v) { return v <= 0 ? packs::spectral::kVxOffLow : std::clamp(packs::spectral::vxFilterHz(v), 40.0, 15000.0); }
double vxLpHz(double v) { return v >= 65536 ? packs::spectral::kVxOffHigh : std::clamp(packs::spectral::vxFilterHz(v), 40.0, 15000.0); }
std::shared_ptr<const KnobTable> vocodexTable() {
  namespace sp = packs::spectral;
  static auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 2000, sp::VcWetLevel, vxWetDb},
      {1, 0, 1000, sp::VxEnhance, [](double v) { return std::clamp(v / 1000.0, 0.0, 1.0); }},
      {2, 0, 16000, sp::VcModLevel, vxPassDb},
      {3, 0, 65536, sp::VxModHp, vxHpHz},
      {4, 0, 16000, sp::VcCarLevel, vxPassDb},
      {5, 0, 65536, sp::VxCarLp, vxLpHz},
      {6, 0, 12800, sp::VxNoise, [](double v) { return std::clamp(v / 12800.0, 0.0, 1.0); }},
      {7, 0, 65536, sp::VxNoiseHp, vxHpHz},
      {8, 0, 10000, sp::VxHoldMs, [](double v) { return sp::vxHoldMs(std::clamp(v, 0.0, 10000.0)); }},
      {9, 0, 10000, sp::VxAttackMs, [](double v) { return sp::vxAttackMs(std::clamp(v, 0.0, 10000.0)); }},
      {10, 0, 2000, sp::VxAttCurve, [](double v) { return std::pow(2.0, std::clamp(v, 0.0, 2000.0) / 1000.0); }},
      {11, 0, 10000, sp::VxReleaseMs, [](double v) { return sp::vxReleaseMs(std::clamp(v, 0.0, 10000.0)); }},
      {12, 0, 2000, sp::VxRelCurve, [](double v) { return std::pow(2.0, std::clamp(v, 0.0, 2000.0) / 1000.0); }},
      {13, 0, 1, sp::VcHold, [](double v) { return v >= 0.5 ? 1.0 : 0.0; }},
      {14, 5, 1000, sp::VxWidth, [](double v) { return std::clamp(std::pow(5.0, v / 1000.0) - 1.0, 0.008, 4.0); }},
      {15, 0, 100, sp::VxFlatness, [](double v) { return std::clamp(v / 100.0, 0.0, 1.0); }},
      {16, 0, 4000, sp::VxModWidth, [](double v) { return std::clamp(std::pow(2.0, v / 1000.0) / 4.0, 0.25, 4.0); }},
      {17, -1200, 1200, sp::VcFormant, [](double v) { return std::clamp(v / 100.0, -12.0, 12.0); }},
      {18, 0, 1000, sp::VxUnisonShift, [](double v) { return std::clamp(v / 1000.0, 0.0, 1.0); }},
      {19, -256, 256, sp::VxUnisonPan, [](double v) { return std::clamp(v / 256.0, -1.0, 1.0); }},
  });
  return t;
}
// Built-in carrier tone family for a saved carrier tone name (keyword match;
// the reference's tones are synthesizer presets, approximated).
int vocodexTone(std::string name) {
  for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  auto has = [&](const char* k) { return name.find(k) != std::string::npos; };
  if (has("noise") || has("breath") || has("whisper")) return 8;
  if (has("organ")) return 6;
  if (has("string") || has("ensemble") || has("pad") || has("choir") || has("phase")) return 5;
  if (has("super") || has("stack") || has("unison") || has("group")) return 4;
  if (has("pwm") || has("pulse")) return 2;
  if (has("square")) return 1;
  if (has("tri") || has("sine") || has("soft")) return 3;
  if (has("growl") || has("power") || has("buzz") || has("elderly") || has("robot")) return 7;
  return 0;
}
// State: u32 version (2, 3, 5 or 0x00080003 → 8), 13 curve records
// {i32 3, i32 kind, i32 n, n × {f64 dx, f64 y, f32 tension, u32 flags}, u32,
// 4 × i32}, 20 tag ints (19 before version 3), 7 ints (hold mode, order,
// unison − 1, band count, native rate, modulator input, carrier input), flag
// bytes, pitch-follow bytes (version ≥ 4), carrier tone name, held keys.
bool decodeVocodex(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  namespace sp = packs::spectral;
  std::vector<uint8_t> b = stateBytes(evs);
  begin(m, "gl.vocoder");
  auto table = vocodexTable();
  bindTable(m, table);
  set(m, sp::VxModel, 1);
  auto fail = [&]() {
    c.report.warn("Vocodex: state layout not recognised, defaults used");
    m.exact = false;
    return true;
  };
  if (b.size() < 4) return fail();
  const uint32_t raw = rd32(b, 0);
  const int ver = raw >= 0x10000 ? static_cast<int>(raw >> 16) : static_cast<int>(raw);
  if (ver < 2 || ver > 8) return fail();
  size_t o = 4;
  sp::VxData d = sp::vxDefaultData();
  for (int k = 0; k < sp::kVxMaps; ++k) {
    if (o + 12 > b.size() || rd32(b, o) != 3) return fail();
    uint32_t n = rd32(b, o + 8);
    if (n < 1 || n > 4096 || o + 12 + static_cast<size_t>(n) * 24 + 20 > b.size()) return fail();
    size_t p = o + 12;
    std::vector<sp::VxPoint> pts;
    for (uint32_t i = 0; i < n; ++i, p += 24) {
      double dx = rdf64(b, p), y = rdf64(b, p + 8);
      float tn = rdf32(b, p + 16);
      if (!std::isfinite(dx) || !std::isfinite(y) || !std::isfinite(tn)) return fail();
      pts.push_back({std::max(0.0, dx), std::clamp(y, 0.0, 1.0), std::clamp(static_cast<double>(tn), -1.0, 1.0)});
    }
    d.maps[k] = std::move(pts);
    o = p + 20;
  }
  const int nTags = ver < 3 ? 19 : 20;
  if (o + static_cast<size_t>(nTags + 7) * 4 > b.size()) return fail();
  std::vector<double> fl(20, 0.0);
  for (int k = 0, tag = 0; k < nTags; ++k, ++tag) {
    if (ver < 3 && tag == 7) fl[static_cast<size_t>(tag++)] = 58000;
    fl[static_cast<size_t>(tag)] = static_cast<int32_t>(rd32(b, o + 4 * static_cast<size_t>(k)));
  }
  if (ver < 3 && fl[3] == 62000) fl[3] = 58000;
  if (fl[7] > 0 && fl[7] < 3) fl[7] = 58000;
  o += static_cast<size_t>(nTags) * 4;
  int32_t iv[7];
  for (int k = 0; k < 7; ++k) iv[k] = static_cast<int32_t>(rd32(b, o + 4 * static_cast<size_t>(k)));
  o += 28;
  applyTable(m, *table, fl);
  set(m, sp::VxHoldMode, iv[0] ? 1 : 0);
  set(m, sp::VxOrder, std::clamp(iv[1], 1, 4) - 1);
  set(m, sp::VxUnison, std::clamp(iv[2], 0, sp::kVxMaxUnison - 1) + 1);
  set(m, sp::VxBands, std::clamp(iv[3], 5, sp::kVxMaxBands));
  set(m, sp::VcModSource, iv[5] >= 1 ? 1 : 0);
  set(m, sp::VxCarSource, iv[6] < 0 ? 0 : (iv[6] == 0 ? 1 : 2));
  // Flag bytes: L-R encoding, min times, contour mod, threaded, subtractive
  // denoising, contour noise, contour from carrier, contour from consonants.
  const int nFlags = ver >= 5 ? 8 : (ver == 4 ? 5 : 4);
  if (o + static_cast<size_t>(nFlags) > b.size()) return fail();
  uint8_t fv[8] = {0, 1, 1, 1, 1, 1, 0, 1};
  for (int k = 0; k < nFlags; ++k) fv[k] = b[o + static_cast<size_t>(k)];
  if (ver < 5) fv[4] = 1;
  o += static_cast<size_t>(nFlags);
  set(m, sp::VxLrEncoding, fv[0] ? 1 : 0);
  set(m, sp::VxMinTimes, fv[1] ? 1 : 0);
  set(m, sp::VxContourMod, fv[2] ? 1 : 0);
  set(m, sp::VxDenoise, fv[4] ? 1 : 0);
  set(m, sp::VxContourNoise, fv[5] ? 1 : 0);
  set(m, sp::VxContourCar, fv[6] ? 1 : 0);
  set(m, sp::VxContourCons, fv[7] ? 1 : 0);
  if (ver >= 4) {
    if (o + sp::kVxMaps > b.size()) return fail();
    for (int k = 0; k < sp::kVxMaps; ++k) d.pf[k] = b[o + static_cast<size_t>(k)] != 0;
    o += sp::kVxMaps;
  }
  std::string tone;
  if (o < b.size()) {
    size_t len = b[o++];
    if (o + len > b.size()) return fail();
    tone.assign(reinterpret_cast<const char*>(b.data() + o), len);
    o += len;
  }
  if (o + 4 <= b.size()) {
    int32_t nk = static_cast<int32_t>(rd32(b, o));
    o += 4;
    for (int32_t k = 0; k < nk && k < 128 && o + 4 <= b.size(); ++k, o += 4) {
      int32_t key = static_cast<int32_t>(rd32(b, o));
      if (key >= 0 && key < 128 && d.keys.size() < 16) d.keys.push_back(key);
    }
  }
  set(m, sp::VxTone, vocodexTone(tone));
  m.state.data = sp::vxSerialize(d);
  // The control laws are reproduced; the built-in carrier synth (its tones)
  // and the unison detune table are approximations.
  const bool synthCarrier = iv[6] < 0 && !fv[0];
  m.exact = !synthCarrier && iv[2] == 0;
  if (synthCarrier) c.report.warn("Vocodex: built-in carrier tone approximated");
  return true;
}

}  // namespace

std::vector<uint8_t> inflateZlib(const uint8_t* p, size_t n, size_t limit) { return unzlib(p, n, limit); }

void registerFxMisc(std::map<std::string, PluginDecoder>& d) {
  d["Fruity NoteBook 2"] = decodeNotebook;
  d["Effector"] = decodeEffector;
  d["Wave Candy"] = decodeWaveCandy;
  d["Control Surface"] = decodeSurface;
  d["Vocodex"] = decodeVocodex;
}

}  // namespace gl::flp::fx
