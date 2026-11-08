// Gridline - tracker modules (K-76): XM / MOD reading. See TrackerModule.h.
#include "TrackerModule.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace gl::tracker {

namespace {

// Bounds-checked little / big endian reads over a byte range.
struct Reader {
  const uint8_t* p = nullptr;
  size_t size = 0, pos = 0;
  bool ok = true;
  bool has(size_t n) const { return pos <= size && size - pos >= n; }
  uint8_t u8() {
    if (!has(1)) return ok = false, 0;
    return p[pos++];
  }
  uint16_t u16() {
    if (!has(2)) return ok = false, pos = size, 0;
    const uint16_t v = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8));
    pos += 2;
    return v;
  }
  uint16_t u16be() {
    if (!has(2)) return ok = false, pos = size, 0;
    const uint16_t v = static_cast<uint16_t>((p[pos] << 8) | p[pos + 1]);
    pos += 2;
    return v;
  }
  uint32_t u32() {
    if (!has(4)) return ok = false, pos = size, 0;
    const uint32_t v = static_cast<uint32_t>(p[pos]) | (static_cast<uint32_t>(p[pos + 1]) << 8) | (static_cast<uint32_t>(p[pos + 2]) << 16) |
                       (static_cast<uint32_t>(p[pos + 3]) << 24);
    pos += 4;
    return v;
  }
  std::string text(size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
      const uint8_t c = has(1) ? p[pos] : 0;
      if (has(1)) ++pos;
      s += static_cast<char>(c);
    }
    return s;
  }
  void skip(size_t n) { pos = has(n) ? pos + n : size; }
  void seek(size_t to) { pos = std::min(to, size); }
};

// A name as shown: up to the first NUL, control characters as spaces, Latin-1 / CP437 high
// characters kept only when they are printable ASCII (names are display text only).
std::string cleanName(const std::string& raw) {
  std::string s;
  for (char ch : raw) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == 0) break;
    s += (c < 32 || c >= 127) ? ' ' : static_cast<char>(c);
  }
  while (!s.empty() && s.back() == ' ') s.pop_back();
  size_t a = 0;
  while (a < s.size() && s[a] == ' ') ++a;
  return s.substr(a);
}

void sanitize(Module& m) {
  m.channels = std::clamp(m.channels + (m.channels & 1), 2, kMaxChannels);
  m.speed = std::clamp(m.speed, 1, 31);
  m.bpm = std::clamp(m.bpm, 32, 255);
  if (m.orders.empty()) m.orders.push_back(0);
  if (m.restart < 0 || m.restart >= static_cast<int>(m.orders.size())) m.restart = 0;
  m.patterns.resize(kMaxPatterns);
  for (auto& p : m.patterns) {
    if (p.rows <= 0 || p.rows > kMaxRows) p.rows = std::clamp(p.rows <= 0 ? 64 : p.rows, 1, kMaxRows);
    bool any = false;
    for (auto& c : p.cells) {
      if (c.note > 97) c.note = 0;
      if (c.instr > 128) c.instr = 0;
      if (c.efx > 35) c.efx = 0, c.param = 0;
      any = any || !c.empty();
    }
    if (!any) {  // an empty pattern plays as 64 empty rows (as FT2 loads it)
      p.cells.clear();
      p.rows = 64;
    }
  }
  m.instruments.resize(kMaxInstruments + 1);
  for (auto& in : m.instruments) {
    InstrumentDef& d = in.def;
    if (d.vibDepth > 0x0F) d.vibDepth = 0x0F;
    if (d.vibRate > 0x3F) d.vibRate = 0x3F;
    if (d.vibType > 3) d.vibType = 0;
    for (auto& k : d.keymap)
      if (k >= kMaxSamples) k = kMaxSamples - 1;
    for (Envelope* e : {&d.vol, &d.pan}) {
      if (e->count > 12) e->count = 12;
      if (e->loopStart > 11) e->loopStart = 11;
      if (e->loopEnd > 11) e->loopEnd = 11;
      if (e->sustain > 11) e->sustain = 11;
      for (int i = 0; i < kEnvPoints; ++i) {
        if (static_cast<uint16_t>(e->x[static_cast<size_t>(i)]) > 32767) e->x[static_cast<size_t>(i)] = 32767;
        const int lim = e == &d.vol ? 64 : 63;
        if (static_cast<uint16_t>(e->y[static_cast<size_t>(i)]) > lim) e->y[static_cast<size_t>(i)] = static_cast<int16_t>(lim);
      }
    }
    for (int i = 0; i < kMaxSamples; ++i) {
      Sample& s = in.samples[static_cast<size_t>(i)];
      SampleDef& sd = s.def;
      if (sd.volume > 64) sd.volume = 64;
      sd.relNote = static_cast<int8_t>(std::clamp<int>(sd.relNote, -48, 71));
      sd.length = static_cast<int32_t>(s.data.size());
      if (sd.loopStart < 0 || sd.loopLength <= 0 || static_cast<int64_t>(sd.loopStart) + sd.loopLength > sd.length) {
        sd.loopStart = sd.loopLength = 0;
        sd.loop = kLoopOff;
      }
      if (sd.loop == 3) sd.loop = kLoopPingPong;
      if (sd.loop == kLoopOff) sd.loopStart = sd.loopLength = 0;
      d.samples[static_cast<size_t>(i)] = sd;
    }
  }
}

// ---- XM ----

void unpackPattern(Reader r, Pattern& p, int rows, int channels) {
  p.cells.assign(static_cast<size_t>(rows) * kMaxChannels, Cell{});
  for (int row = 0; row < rows && r.has(1); ++row)
    for (int ch = 0; ch < channels && r.has(1); ++ch) {
      Cell c;
      const uint8_t b = r.u8();
      if (b & 0x80) {
        if (b & 1) c.note = r.u8();
        if (b & 2) c.instr = r.u8();
        if (b & 4) c.vol = r.u8();
        if (b & 8) c.efx = r.u8();
        if (b & 16) c.param = r.u8();
      } else {
        c.note = b;
        c.instr = r.u8();
        c.vol = r.u8();
        c.efx = r.u8();
        c.param = r.u8();
      }
      if (ch < kMaxChannels) p.cells[static_cast<size_t>(row) * kMaxChannels + static_cast<size_t>(ch)] = c;
    }
}

bool readXmPatterns(Reader& r, Module& m, int count, uint16_t version, std::string* err) {
  for (int i = 0; i < count; ++i) {
    const size_t start = r.pos;
    const uint32_t hlen = r.u32();
    r.u8();  // packing type
    int rows;
    if (version == 0x0102) rows = r.u8() + 1;
    else rows = r.u16();
    const uint16_t packed = r.u16();
    if (!r.ok) {
      if (err) *err = "the file ends inside pattern " + std::to_string(i);
      return false;
    }
    r.seek(start + std::max<uint32_t>(hlen, version == 0x0102 ? 8u : 9u));
    Pattern p;
    p.rows = rows;
    if (rows > kMaxRows) {
      p.rows = kMaxRows;
      m.warnings.push_back("pattern " + std::to_string(i) + " was longer than 256 rows: cut");
    }
    if (packed > 0) {
      if (!r.has(packed)) {
        if (err) *err = "the file ends inside pattern " + std::to_string(i);
        return false;
      }
      Reader pr{r.p + r.pos, packed, 0, true};
      unpackPattern(pr, p, p.rows, std::min(m.channels, 64));
      r.skip(packed);
    }
    if (i < kMaxPatterns) m.patterns[static_cast<size_t>(i)] = std::move(p);
  }
  return true;
}

struct XmSampleHeader {
  uint32_t length = 0, loopStart = 0, loopLength = 0;
  uint8_t flags = 0, reserved = 0;
};

bool readXmInstrument(Reader& r, Module& m, int index, std::vector<std::vector<XmSampleHeader>>& pending, std::string* err) {
  const size_t start = r.pos;
  if (!r.has(4)) {
    if (err) *err = "the file ends before instrument " + std::to_string(index + 1);
    return false;
  }
  uint32_t size = r.u32();
  const uint32_t declared = size;
  if (size == 0 || size > 263) size = 263;
  const size_t headerEnd = start + size;
  Instrument in;
  // The header is read on its own: fields a short header does not hold stay zero (as in FT2).
  Reader hr{r.p + start, std::min<size_t>(size, r.size - start), 4, true};
  in.name = cleanName(hr.text(22));
  hr.u8();  // type
  const int nsmp = hr.u16();
  if (nsmp > 32) {
    if (err) *err = "instrument " + std::to_string(index + 1) + " is corrupt";
    return false;
  }
  hr.u32();  // sample header size
  std::vector<XmSampleHeader> hdrs;
  if (nsmp > 0) {
    for (int i = 0; i < 96; ++i) in.def.keymap[static_cast<size_t>(i)] = hr.u8();
    for (int i = 0; i < 12; ++i) {
      in.def.vol.x[static_cast<size_t>(i)] = static_cast<int16_t>(hr.u16());
      in.def.vol.y[static_cast<size_t>(i)] = static_cast<int16_t>(hr.u16());
    }
    for (int i = 0; i < 12; ++i) {
      in.def.pan.x[static_cast<size_t>(i)] = static_cast<int16_t>(hr.u16());
      in.def.pan.y[static_cast<size_t>(i)] = static_cast<int16_t>(hr.u16());
    }
    in.def.vol.count = hr.u8();
    in.def.pan.count = hr.u8();
    in.def.vol.sustain = hr.u8();
    in.def.vol.loopStart = hr.u8();
    in.def.vol.loopEnd = hr.u8();
    in.def.pan.sustain = hr.u8();
    in.def.pan.loopStart = hr.u8();
    in.def.pan.loopEnd = hr.u8();
    in.def.vol.flags = hr.u8();
    in.def.pan.flags = hr.u8();
    in.def.vibType = hr.u8();
    in.def.vibSweep = hr.u8();
    in.def.vibDepth = hr.u8();
    in.def.vibRate = hr.u8();
    in.def.fadeout = hr.u16();
    hr.u8();  // MIDI on
    hr.u8();  // MIDI channel
    hr.u16();
    hr.u16();
    in.def.mute = hr.u8() == 1;
    r.seek(declared > 263 ? start + declared : headerEnd);
    for (int i = 0; i < nsmp; ++i) {
      XmSampleHeader h;
      h.length = r.u32();
      h.loopStart = r.u32();
      h.loopLength = r.u32();
      SampleDef d;
      d.volume = r.u8();
      d.finetune = static_cast<int8_t>(r.u8());
      h.flags = r.u8();
      d.pan = r.u8();
      d.relNote = static_cast<int8_t>(r.u8());
      h.reserved = r.u8();
      std::string name = cleanName(r.text(22));
      if (!r.ok) {
        if (err) *err = "the file ends inside instrument " + std::to_string(index + 1);
        return false;
      }
      d.loop = h.flags & 3;
      if (i < kMaxSamples) {
        in.samples[static_cast<size_t>(i)].def = d;
        in.samples[static_cast<size_t>(i)].name = std::move(name);
      }
      hdrs.push_back(h);
    }
    in.sampleCount = nsmp;
    if (nsmp > kMaxSamples) m.warnings.push_back("instrument " + std::to_string(index + 1) + " had more than 16 samples: the extra ones were left out");
  } else {
    r.seek(declared > 263 ? start + declared : headerEnd);
  }
  if (index < kMaxInstruments) m.instruments[static_cast<size_t>(index + 1)] = std::move(in);
  pending[static_cast<size_t>(index)] = std::move(hdrs);
  return true;
}

// Sample data of one instrument (after its headers, or after all patterns in old versions).
void readXmSamples(Reader& r, Module& m, int index, const std::vector<XmSampleHeader>& hdrs) {
  for (size_t i = 0; i < hdrs.size(); ++i) {
    const XmSampleHeader& h = hdrs[i];
    if (h.length == 0) continue;
    const bool is16 = (h.flags & 16) != 0, stereo = (h.flags & 32) != 0;
    const bool adpcm = h.reserved == 0xAD && !(h.flags & (16 | 32));
    const size_t avail = r.size - std::min(r.pos, r.size);
    const size_t want = adpcm ? 16 + (static_cast<size_t>(h.length) + 1) / 2 : h.length;
    const size_t take = std::min(want, avail);
    Sample* dst = (index < kMaxInstruments && i < static_cast<size_t>(kMaxSamples)) ? &m.instruments[static_cast<size_t>(index + 1)].samples[i] : nullptr;
    if (dst) {
      const uint8_t* src = r.p + r.pos;
      std::vector<int16_t>& out = dst->data;
      dst->sixteenBit = is16;
      if (adpcm) {
        int8_t lut[16] = {};
        for (size_t k = 0; k < 16 && k < take; ++k) lut[k] = static_cast<int8_t>(src[k]);
        int8_t cur = 0;
        for (size_t k = 16; k < take; ++k) {
          cur = static_cast<int8_t>(cur + lut[src[k] & 0x0F]);
          out.push_back(static_cast<int16_t>(cur * 256));
          cur = static_cast<int8_t>(cur + lut[src[k] >> 4]);
          out.push_back(static_cast<int16_t>(cur * 256));
        }
        if (out.size() > h.length) out.resize(h.length);
      } else if (is16) {
        const size_t n = take / 2;
        out.resize(n);
        int16_t acc = 0;
        for (size_t k = 0; k < n; ++k) {
          acc = static_cast<int16_t>(acc + static_cast<int16_t>(src[2 * k] | (src[2 * k + 1] << 8)));
          out[k] = acc;
        }
      } else {
        out.resize(take);
        int8_t acc = 0;
        for (size_t k = 0; k < take; ++k) {
          acc = static_cast<int8_t>(acc + static_cast<int8_t>(src[k]));
          out[k] = static_cast<int16_t>(acc * 256);
        }
      }
      SampleDef& d = dst->def;
      int64_t ls = h.loopStart, ll = h.loopLength;
      if (is16) ls /= 2, ll /= 2;
      if (stereo && !adpcm) {  // left then right, each delta coded on its own: played as the average
        const size_t half = out.size() / 2;
        std::vector<int16_t> mono(half);
        for (size_t k = 0; k < half; ++k) mono[k] = static_cast<int16_t>((out[k] + out[half + k]) >> 1);
        out = std::move(mono);
        ls /= 2, ll /= 2;
      }
      d.loopStart = static_cast<int32_t>(std::min<int64_t>(ls, 0x3FFFFFFF));
      d.loopLength = static_cast<int32_t>(std::min<int64_t>(ll, 0x3FFFFFFF));
      if (take < want) m.warnings.push_back("a sample of instrument " + std::to_string(index + 1) + " is cut short: the file ends early");
    }
    r.skip(want);
  }
}

bool parseXm(Reader& r, Module& m, std::string* err) {
  m.format = Format::Xm;
  r.seek(17);
  m.title = cleanName(r.text(20));
  r.u8();
  m.tracker = cleanName(r.text(20));
  const uint16_t version = r.u16();
  const uint32_t hsize = r.u32();
  const int orders = r.u16();
  m.restart = r.u16();
  const int chans = r.u16();
  const int pats = r.u16();
  const int insts = r.u16();
  const uint16_t flags = r.u16();
  m.speed = r.u16();
  m.bpm = r.u16();
  std::vector<uint8_t> ord(256);
  for (auto& o : ord) o = r.u8();
  if (!r.ok) {
    if (err) *err = "the file is too short to be a module";
    return false;
  }
  if (version < 0x0102 || version > 0x0104) {
    if (err) *err = "unsupported XM version";
    return false;
  }
  if (orders > kMaxOrders || pats > kMaxPatterns || chans == 0 || insts > 256) {
    if (err) *err = "the file is corrupt";
    return false;
  }
  m.channels = chans;
  if (chans > kMaxChannels) m.warnings.push_back("the module has more than 32 channels: the extra ones were left out");
  m.linear = (flags & 1) != 0;
  int len = std::min(orders, 255);
  while (len > 0 && ord[static_cast<size_t>(len - 1)] == 0xFF) --len;  // padding some files use
  m.orders.assign(ord.begin(), ord.begin() + std::max(len, 0));
  if (m.orders.empty()) m.orders.push_back(0);
  r.seek(60 + static_cast<size_t>(hsize));
  m.patterns.assign(kMaxPatterns, Pattern{});
  m.instruments.assign(kMaxInstruments + 1, Instrument{});
  std::vector<std::vector<XmSampleHeader>> pending(static_cast<size_t>(insts));
  if (version < 0x0104) {
    for (int i = 0; i < insts; ++i)
      if (!readXmInstrument(r, m, i, pending, err)) return false;
    if (!readXmPatterns(r, m, pats, version, err)) return false;
    for (int i = 0; i < insts; ++i) readXmSamples(r, m, i, pending[static_cast<size_t>(i)]);
  } else {
    if (!readXmPatterns(r, m, pats, version, err)) return false;
    for (int i = 0; i < insts; ++i) {
      if (!readXmInstrument(r, m, i, pending, err)) {
        // A module cut off in its instruments still plays what was read before.
        if (i == 0) return false;
        m.warnings.push_back(*err);
        break;
      }
      readXmSamples(r, m, i, pending[static_cast<size_t>(i)]);
    }
  }
  if (insts > kMaxInstruments) m.warnings.push_back("the module has more than 128 instruments: the extra ones were left out");
  return true;
}

// ---- MOD ----

constexpr uint16_t kPtPeriods[36] = {856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, 428, 404, 381, 360, 339, 320,
                                     302, 285, 269, 254, 240, 226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113};
// The 8-octave period table MOD notes are read against: ProTracker's three octaves, three more
// octaves below (x8, x4, x2) and two above (halved).
uint16_t modPeriod(int note) {
  const int oct = note / 12, k = note % 12;
  switch (oct) {
    case 0: return static_cast<uint16_t>(kPtPeriods[k] * 8);
    case 1: return static_cast<uint16_t>(kPtPeriods[k] * 4);
    case 2: return static_cast<uint16_t>(kPtPeriods[k] * 2);
    case 3: case 4: case 5: return kPtPeriods[(oct - 3) * 12 + k];
    case 6: return static_cast<uint16_t>(kPtPeriods[24 + k] / 2);
    default: return static_cast<uint16_t>(kPtPeriods[24 + k] / 4);
  }
}

enum ModKind { kMk, kFlt4, kFlt8, kMulti, kNt, kHmnt, kUnknown };

ModKind modKind(const char* id, int& chans) {
  auto is = [&](const char* t) { return std::memcmp(id, t, 4) == 0; };
  auto digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  chans = 4;
  if (is("M.K.") || is("M!K!") || is("NSMS") || is("LARD") || is("PATT")) return kMk;
  if (digit(id[0]) && id[1] == 'C' && id[2] == 'H' && id[3] == 'N') return chans = id[0] - '0', kMulti;
  if (digit(id[0]) && digit(id[1]) && id[2] == 'C' && (id[3] == 'H' || id[3] == 'N')) return chans = (id[0] - '0') * 10 + (id[1] - '0'), kMulti;
  if (is("CD61") || is("CD81")) return chans = id[2] - '0', kMulti;
  if (id[0] == 'F' && id[1] == 'A' && id[2] == '0' && id[3] >= '4' && id[3] <= '8') return chans = id[3] - '0', kMulti;
  if (is("OKTA") || is("OCTA")) return chans = 8, kMulti;
  if (is("FLT4") || is("EXO4")) return kFlt4;
  if (is("FLT8") || is("EXO8")) return chans = 8, kFlt8;
  if (is("N.T.")) return kNt;
  if (is("M&K!") || is("FEST")) return kHmnt;
  return kUnknown;
}

void modEffect(Cell& c, ModKind k) {
  switch (c.efx) {
    case 0xC: if (c.param > 64) c.param = 64; break;
    case 0x1: case 0x2: case 0xA: if (c.param == 0) c.efx = 0; break;
    case 0x5: if (c.param == 0) c.efx = 0x3; break;
    case 0x6: if (c.param == 0) c.efx = 0x4; break;
    case 0xE:
      if (c.param == 0x10 || c.param == 0x20 || c.param == 0xA0 || c.param == 0xB0) c.efx = 0, c.param = 0;
      break;
    default: break;
  }
  if (k == kNt || k == kHmnt) {
    if (c.efx == 0xD) c.param = 0;
    if (c.efx == 0xF && c.param == 0) c.efx = 0;
  } else if (k == kFlt4 || k == kFlt8) {
    if (c.efx == 0xE) c.efx = 0, c.param = 0;
    if (c.efx == 0xF && c.param > 0x1F) c.param = 0x1F;
  }
}

Cell modCell(const uint8_t* b, ModKind k) {
  Cell c;
  const uint16_t period = static_cast<uint16_t>(((b[0] & 0x0F) << 8) | b[1]);
  for (int n = 0; n < 8 * 12; ++n)
    if (period >= modPeriod(n)) {
      c.note = static_cast<uint8_t>(n + 1);
      break;
    }
  if (period == 0) c.note = 0;
  c.instr = static_cast<uint8_t>((b[0] & 0xF0) | (b[2] >> 4));
  c.efx = b[2] & 0x0F;
  c.param = b[3];
  modEffect(c, k);
  return c;
}

bool parseMod(Reader& r, Module& m, std::string* err) {
  if (r.size < 1084) {
    if (err) *err = "the file is not a module";
    return false;
  }
  int chans = 4;
  const ModKind kind = modKind(reinterpret_cast<const char*>(r.p + 1080), chans);
  if (kind == kUnknown) {
    if (err) *err = "the file is not a supported module";
    return false;
  }
  m.format = Format::Mod;
  m.linear = false;
  m.speed = 6;
  m.bpm = 125;
  m.tracker = "ProTracker";
  r.seek(0);
  m.title = cleanName(r.text(20));
  struct Hdr {
    std::string name;
    uint16_t len, loopStart, loopLen;
    uint8_t fine, vol;
  } h[31];
  for (auto& s : h) {
    s.name = cleanName(r.text(22));
    s.len = r.u16be();
    s.fine = r.u8();
    s.vol = r.u8();
    s.loopStart = r.u16be();
    s.loopLen = r.u16be();
  }
  int numOrders = r.u8();
  m.restart = r.u8();
  std::vector<uint8_t> ord(128);
  for (auto& o : ord) o = r.u8();
  r.skip(4);
  if (kind == kMk && numOrders == 129) numOrders = 127;
  if (chans <= 0 || numOrders < 1 || numOrders > 128) {
    if (err) *err = "the file is not a supported module";
    return false;
  }
  m.channels = std::min(chans, kMaxChannels);
  int numPats = 0;
  for (auto& o : ord) {
    if (kind == kFlt8) o = static_cast<uint8_t>(o >> 1);
    numPats = std::max<int>(numPats, o);
  }
  ++numPats;
  m.orders.assign(ord.begin(), ord.begin() + numOrders);
  m.patterns.assign(kMaxPatterns, Pattern{});
  m.instruments.assign(kMaxInstruments + 1, Instrument{});
  for (int i = 0; i < numPats && i < kMaxPatterns; ++i) {
    m.patterns[static_cast<size_t>(i)].rows = 64;
    m.patterns[static_cast<size_t>(i)].cells.assign(64 * kMaxChannels, Cell{});
  }
  if (kind != kFlt8) {
    for (int i = 0; i < numPats; ++i)
      for (int row = 0; row < 64; ++row)
        for (int ch = 0; ch < chans; ++ch) {
          if (!r.has(4)) {
            r.pos = r.size;
            continue;
          }
          const Cell c = modCell(r.p + r.pos, kind);
          r.skip(4);
          if (ch < kMaxChannels && i < kMaxPatterns) m.patterns[static_cast<size_t>(i)].cells[static_cast<size_t>(row) * kMaxChannels + static_cast<size_t>(ch)] = c;
        }
  } else {
    for (int i = 0; i < numPats * 2; ++i)
      for (int row = 0; row < 64; ++row)
        for (int ch = 0; ch < 4; ++ch) {
          if (!r.has(4)) {
            r.pos = r.size;
            continue;
          }
          const Cell c = modCell(r.p + r.pos, kind);
          r.skip(4);
          const int pi = i >> 1;
          if (pi < kMaxPatterns) m.patterns[static_cast<size_t>(pi)].cells[static_cast<size_t>(row) * kMaxChannels + static_cast<size_t>(ch + (i & 1) * 4)] = c;
        }
  }
  for (int i = 0; i < 31; ++i) {
    Instrument& in = m.instruments[static_cast<size_t>(i + 1)];
    in.name = h[i].name;
    if (h[i].len == 0) continue;
    in.sampleCount = 1;
    Sample& s = in.samples[0];
    s.name = h[i].name;
    uint8_t fine = h[i].fine;
    if (kind == kHmnt) fine = static_cast<uint8_t>((-fine & 0x1F) >> 1);
    s.def.finetune = static_cast<int8_t>((fine & 0x0F) << 4);
    s.def.volume = h[i].vol;
    int32_t len = 2 * h[i].len, ls = 2 * h[i].loopStart, ll = 2 * h[i].loopLen;
    if (ll > 2 && ls + ll > len && (ls >> 1) + ll <= len) ls >>= 1;
    if (ls + ll > len) {
      if (ls >= len) ls = ll = 0;
      else ll = len - ls;
    }
    s.def.loop = ls + ll > 2 ? kLoopForward : kLoopOff;
    if (s.def.loop == kLoopOff) ls = ll = 0;
    s.def.loopStart = ls;
    s.def.loopLength = ll;
    s.data.assign(static_cast<size_t>(len), 0);
    const size_t take = std::min(static_cast<size_t>(len), r.size - std::min(r.pos, r.size));
    for (size_t k = 0; k < take; ++k) s.data[k] = static_cast<int16_t>(static_cast<int8_t>(r.p[r.pos + k]) * 256);
    if (take < static_cast<size_t>(len)) m.warnings.push_back("sample " + std::to_string(i + 1) + " is cut short: the file ends early");
    r.skip(static_cast<size_t>(len));
  }
  if (chans > kMaxChannels) m.warnings.push_back("the module has more than 32 channels: the extra ones were left out");
  return true;
}

std::string lowerExt(const std::string& path) {
  std::string e = std::filesystem::u8path(path).extension().u8string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

}  // namespace

bool isModulePath(const std::string& path) {
  const std::string e = lowerExt(path);
  return e == ".xm" || e == ".mod";
}

bool parseModule(const uint8_t* data, size_t size, Module& out, std::string* err) {
  out = Module{};
  if (!data || size < 60) {
    if (err) *err = "the file is too short to be a module";
    return false;
  }
  Reader r{data, size, 0, true};
  bool ok;
  if (std::memcmp(data, "Extended Module: ", 17) == 0) ok = parseXm(r, out, err);
  else ok = parseMod(r, out, err);
  if (!ok) return false;
  sanitize(out);
  return true;
}

bool parseModule(const std::vector<uint8_t>& bytes, Module& out, std::string* err) { return parseModule(bytes.data(), bytes.size(), out, err); }

}  // namespace gl::tracker
