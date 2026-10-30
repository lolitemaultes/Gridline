// Gridline — Slicex region engine (see SlicexEngine.h).
#include "TimeStretch.h"
#include "SlicexEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include "../core/PluginParams.h"
#include "Sample.h"

namespace gl::slicex {

namespace {

constexpr double kPi = 3.14159265358979323846;

int32_t rd32(const uint8_t* p) {
  return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                              (static_cast<uint32_t>(p[3]) << 24));
}
float rdf(const uint8_t* p) {
  const uint32_t u = static_cast<uint32_t>(rd32(p));
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void wr32(std::vector<uint8_t>& o, int32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
}
void wrf(std::vector<uint8_t>& o, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  wr32(o, static_cast<int32_t>(u));
}
std::string hex(const std::vector<uint8_t>& b) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(b.size() * 2);
  for (uint8_t c : b) s += d[c >> 4], s += d[c & 15];
  return s;
}
bool unhex(const std::string& s, std::vector<uint8_t>& out) {
  out.clear();
  if (s == "-") return true;
  if (s.size() % 2) return false;
  auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
  for (size_t i = 0; i < s.size(); i += 2) {
    const int a = v(s[i]), b = v(s[i + 1]);
    if (a < 0 || b < 0) return false;
    out.push_back(static_cast<uint8_t>(a * 16 + b));
  }
  return true;
}
std::string hexOrDash(const std::vector<uint8_t>& b) { return b.empty() ? "-" : hex(b); }

bool sameF(float a, float b) { return std::memcmp(&a, &b, 4) == 0; }

inline float prm(const PluginState& s, int k, float d) {
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : d;
}

}  // namespace

bool Articulator::operator==(const Articulator& o) const {
  return version == o.version && sameF(floats[0], o.floats[0]) && sameF(floats[1], o.floats[1]) && sameF(floats[2], o.floats[2]) &&
         env == o.env && lfo == o.lfo && maps == o.maps;
}
bool Group::operator==(const Group& o) const { return hdr == o.hdr && arts == o.arts; }
bool Deck::operator==(const Deck& o) const {
  return flag == o.flag && rawPath == o.rawPath && file == o.file && embedded == o.embedded && riff == o.riff && view == o.view &&
         envs == o.envs && regions == o.regions;
}
bool Model::operator==(const Model& o) const {
  return version == o.version && knobs == o.knobs && opts == o.opts && view == o.view && decks == o.decks && groups == o.groups;
}
int Model::layering() const { return opts.size() >= 4 ? rd32(opts.data()) : 0; }
int Model::crossfade() const { return opts.size() >= 8 ? rd32(opts.data() + 4) : 3; }

Articulator defaultArticulator(int target) {
  Articulator a;
  a.floats[0] = 0, a.floats[1] = 1, a.floats[2] = 1;
  a.env = flart::defaultEnvelope();
  a.env.enabled = 0;
  a.lfo = flart::defaultLfo();
  a.lfo.extra[0] = 40000;
  const double neutral = (target == TVol || target == TSpeed) ? 1.0 : 0.5;
  for (auto& m : a.maps) m = flart::defaultMap(neutral);
  return a;
}

Model defaultModel() {
  Model m;
  m.opts.assign(kOptBytes, 0);
  const int32_t ints[4] = {0, 3, 5, 3};
  for (int i = 0; i < 4; ++i)
    for (int b = 0; b < 4; ++b) m.opts[static_cast<size_t>(i * 4 + b)] = static_cast<uint8_t>(static_cast<uint32_t>(ints[i]) >> (8 * b));
  for (int rel : {0x22, 0x25, 0x27, 0x2B}) m.opts[static_cast<size_t>(rel)] = 1;
  for (auto& d : m.decks) {
    d.view.assign(kDeckView, 0);
    for (auto& e : d.envs) {
      e = flart::defaultMap(0.5);
      e.kind = flart::KindDeck;
    }
    d.regions.assign(1, Region{});
  }
  for (auto& g : m.groups)
    for (int t = 0; t < kTargets; ++t) g.arts[static_cast<size_t>(t)] = defaultArticulator(t);
  return m;
}

// ------------------------------------------------------------ binary state
bool parseState(const uint8_t* p, size_t n, Model& out, std::array<std::vector<uint8_t>, 2>& riffOut) {
  out = defaultModel();
  riffOut[0].clear(), riffOut[1].clear();
  size_t at = 0;
  auto has = [&](size_t k) { return at <= n && n - at >= k; };
  if (!has(4 + 32)) return false;
  out.version = rd32(p);
  const int32_t ver = out.version > 0xFFFF ? (out.version >> 16) : out.version;
  if (ver < 0 || ver > 6) return false;
  at = 4;
  for (auto& k : out.knobs) k = rd32(p + at), at += 4;
  const size_t ob = ver >= 2 ? 47 : 46;
  if (!has(ob + 12)) return false;
  out.opts.assign(p + at, p + at + ob);
  at += ob;
  for (auto& v : out.view) v = rd32(p + at), at += 4;
  for (int d = 0; d < 2; ++d) {
    Deck& dk = out.decks[static_cast<size_t>(d)];
    dk.flag = 0;
    if (ver >= 2) {
      if (!has(1)) return false;
      dk.flag = p[at++];
    }
    dk.rawPath.clear();
    if (ver > 2 || dk.flag) {
      if (!has(1)) return false;
      size_t len = p[at++];
      if (len == 0xFF) {  // J-176: long path (255+ bytes): 0xFF, then a u32 length
        if (!has(4)) return false;
        len = static_cast<uint32_t>(rd32(p + at));
        at += 4;
      }
      if (!has(len)) return false;
      dk.rawPath.assign(reinterpret_cast<const char*>(p + at), len);
      at += len;
    }
    dk.embedded = dk.flag == 0;
    if (dk.embedded) {
      if (!has(4)) return false;
      const uint32_t len = static_cast<uint32_t>(rd32(p + at));
      at += 4;
      if (!has(len)) return false;
      riffOut[static_cast<size_t>(d)].assign(p + at, p + at + len);
      at += len;
    }
    if (!has(kDeckView)) return false;
    dk.view.assign(p + at, p + at + kDeckView);
    at += kDeckView;
    for (auto& e : dk.envs)
      if (!flart::readRecord(p, n, at, flart::KindDeck, e)) return false;
    if (!has(4)) return false;
    const int32_t nr = rd32(p + at);
    at += 4;
    if (nr < 0 || nr > 100000 || !has(static_cast<size_t>(nr) * 68)) return false;
    dk.regions.assign(static_cast<size_t>(nr), Region{});
    for (auto& r : dk.regions)
      for (auto& v : r.rec) v = rd32(p + at), at += 4;
  }
  for (auto& g : out.groups) {
    if (!has(20)) return false;
    for (auto& h : g.hdr) h = rd32(p + at), at += 4;
    for (auto& a : g.arts) {
      if (!has(16)) return false;
      a.version = rd32(p + at);
      for (int i = 0; i < 3; ++i) a.floats[i] = rdf(p + at + 4 + 4 * static_cast<size_t>(i));
      at += 16;
      if (!flart::readRecord(p, n, at, flart::KindEnv, a.env) || !flart::readRecord(p, n, at, flart::KindLfo, a.lfo)) return false;
      for (auto& m : a.maps)
        if (!flart::readRecord(p, n, at, flart::KindMap, m)) return false;
    }
  }
  return at == n;
}

std::vector<uint8_t> writeState(const Model& m, const std::array<std::vector<uint8_t>, 2>& riff) {
  std::vector<uint8_t> o;
  const int32_t ver = m.version > 0xFFFF ? (m.version >> 16) : m.version;
  wr32(o, ver >= 6 ? m.version : 0x00060003);
  for (int32_t k : m.knobs) wr32(o, k);
  std::vector<uint8_t> opts = m.opts;
  opts.resize(kOptBytes, 0);
  o.insert(o.end(), opts.begin(), opts.end());
  for (int32_t v : m.view) wr32(o, v);
  for (int d = 0; d < 2; ++d) {
    const Deck& dk = m.decks[static_cast<size_t>(d)];
    const bool embedded = dk.flag == 0;
    o.push_back(dk.flag);
    const size_t len = dk.rawPath.size();
    if (len < 0xFF) {
      o.push_back(static_cast<uint8_t>(len));
    } else {  // J-176: long path: 0xFF, then a u32 length
      o.push_back(0xFF);
      wr32(o, static_cast<int32_t>(len));
    }
    o.insert(o.end(), dk.rawPath.begin(), dk.rawPath.end());
    if (embedded) {
      const auto& r = riff[static_cast<size_t>(d)];
      wr32(o, static_cast<int32_t>(r.size()));
      o.insert(o.end(), r.begin(), r.end());
    }
    std::vector<uint8_t> view = dk.view;
    view.resize(kDeckView, 0);
    o.insert(o.end(), view.begin(), view.end());
    for (const auto& e : dk.envs) {
      flart::Record r = e;
      r.kind = flart::KindDeck;
      flart::writeRecord(o, r);
    }
    wr32(o, static_cast<int32_t>(dk.regions.size()));
    for (const auto& r : dk.regions)
      for (int32_t v : r.rec) wr32(o, v);
  }
  for (const auto& g : m.groups) {
    for (int32_t h : g.hdr) wr32(o, h);
    for (const auto& a : g.arts) {
      wr32(o, a.version);
      for (float f : a.floats) wrf(o, f);
      flart::Record e = a.env, l = a.lfo;
      e.kind = flart::KindEnv, l.kind = flart::KindLfo;
      flart::writeRecord(o, e);
      flart::writeRecord(o, l);
      for (const auto& mp : a.maps) {
        flart::Record r = mp;
        r.kind = flart::KindMap;
        flart::writeRecord(o, r);
      }
    }
  }
  return o;
}

// -------------------------------------------------------------------- text
std::string writeText(const Model& m) {
  std::string s;
  char buf[160];
  std::snprintf(buf, sizeof buf, "slicex 1 %d\n", m.version);
  s += buf;
  s += "knobs";
  for (int32_t k : m.knobs) s += " " + std::to_string(k);
  s += "\nopts " + hexOrDash(m.opts) + "\n";
  std::snprintf(buf, sizeof buf, "view %d %d %d\n", m.view[0], m.view[1], m.view[2]);
  s += buf;
  for (int d = 0; d < 2; ++d) {
    const Deck& dk = m.decks[static_cast<size_t>(d)];
    std::vector<uint8_t> raw(dk.rawPath.begin(), dk.rawPath.end());
    std::snprintf(buf, sizeof buf, "deck %d %d %d %zu\n", d, dk.flag, dk.embedded ? 1 : 0, dk.regions.size());
    s += buf;
    s += "path " + hexOrDash(raw) + "\n";
    std::vector<uint8_t> file(dk.file.begin(), dk.file.end());
    s += "file " + hexOrDash(file) + "\n";
    s += "riff " + hexOrDash(dk.riff) + "\n";
    s += "dview " + hexOrDash(dk.view) + "\n";
    for (const auto& e : dk.envs) s += flart::writeText(e) + "\n";
    for (const auto& r : dk.regions) {
      std::snprintf(buf, sizeof buf, "reg %lld %lld", static_cast<long long>(r.start), static_cast<long long>(r.length));
      s += buf;
      for (int32_t v : r.rec) s += " " + std::to_string(v);
      // J-166: key and name (older text ends after the record: automatic key, no name).
      s += " " + std::to_string(r.key) + " " + hexOrDash(std::vector<uint8_t>(r.name.begin(), r.name.end()));
      s += "\n";
    }
  }
  for (size_t g = 0; g < m.groups.size(); ++g) {
    const Group& gr = m.groups[g];
    std::snprintf(buf, sizeof buf, "group %zu %d %d %d %d %d\n", g, gr.hdr[0], gr.hdr[1], gr.hdr[2], gr.hdr[3], gr.hdr[4]);
    s += buf;
    for (const auto& a : gr.arts) {
      std::snprintf(buf, sizeof buf, "art %d %.9g %.9g %.9g\n", a.version, static_cast<double>(a.floats[0]), static_cast<double>(a.floats[1]),
                    static_cast<double>(a.floats[2]));
      s += buf;
      s += flart::writeText(a.env) + "\n";
      s += flart::writeText(a.lfo) + "\n";
      for (const auto& mp : a.maps) s += flart::writeText(mp) + "\n";
    }
  }
  return s;
}

bool readText(const std::string& text, Model& out) {
  out = defaultModel();
  std::istringstream in(text);
  std::string line, word;
  auto next = [&]() { return static_cast<bool>(std::getline(in, line)); };
  if (!next()) return false;
  {
    int fmt = 0;
    if (std::sscanf(line.c_str(), "slicex %d %d", &fmt, &out.version) != 2 || fmt != 1) return false;
  }
  if (!next()) return false;
  {
    std::istringstream l(line);
    l >> word;
    if (word != "knobs") return false;
    for (auto& k : out.knobs)
      if (!(l >> k)) return false;
  }
  auto field = [&](const char* key, std::vector<uint8_t>& v) {
    if (!next()) return false;
    std::istringstream l(line);
    std::string k, h;
    l >> k >> h;
    return k == key && unhex(h, v);
  };
  if (!field("opts", out.opts)) return false;
  if (!next() || std::sscanf(line.c_str(), "view %d %d %d", &out.view[0], &out.view[1], &out.view[2]) != 3) return false;
  for (int d = 0; d < 2; ++d) {
    Deck& dk = out.decks[static_cast<size_t>(d)];
    int dd = 0, flag = 0, emb = 0;
    size_t nr = 0;
    if (!next() || std::sscanf(line.c_str(), "deck %d %d %d %zu", &dd, &flag, &emb, &nr) != 4 || dd != d || nr > 100000) return false;
    dk.flag = static_cast<uint8_t>(flag);
    dk.embedded = emb != 0;
    std::vector<uint8_t> raw, file;
    if (!field("path", raw) || !field("file", file) || !field("riff", dk.riff) || !field("dview", dk.view)) return false;
    dk.rawPath.assign(raw.begin(), raw.end());
    dk.file.assign(file.begin(), file.end());
    for (auto& e : dk.envs)
      if (!next() || !flart::readText(line, e)) return false;
    dk.regions.assign(nr, Region{});
    for (auto& r : dk.regions) {
      if (!next()) return false;
      std::istringstream l(line);
      long long st = 0, len = 0;
      l >> word >> st >> len;
      if (word != "reg") return false;
      r.start = st, r.length = len;
      for (auto& v : r.rec)
        if (!(l >> v)) return false;
      int key = -1;
      std::string nm;
      if (l >> key) {
        r.key = key >= 0 && key <= 127 ? key : -1;
        std::vector<uint8_t> raw;
        if (l >> nm && unhex(nm, raw)) r.name.assign(raw.begin(), raw.end());
      }
    }
  }
  for (size_t g = 0; g < out.groups.size(); ++g) {
    Group& gr = out.groups[g];
    size_t gi = 0;
    if (!next() || std::sscanf(line.c_str(), "group %zu %d %d %d %d %d", &gi, &gr.hdr[0], &gr.hdr[1], &gr.hdr[2], &gr.hdr[3], &gr.hdr[4]) != 6 ||
        gi != g)
      return false;
    for (auto& a : gr.arts) {
      double f[3];
      if (!next() || std::sscanf(line.c_str(), "art %d %lf %lf %lf", &a.version, &f[0], &f[1], &f[2]) != 4) return false;
      for (int i = 0; i < 3; ++i) a.floats[i] = static_cast<float>(f[i]);
      if (!next() || !flart::readText(line, a.env) || !next() || !flart::readText(line, a.lfo)) return false;
      a.env.kind = flart::KindEnv, a.lfo.kind = flart::KindLfo;
      for (auto& mp : a.maps) {
        if (!next() || !flart::readText(line, mp)) return false;
        mp.kind = flart::KindMap;
      }
    }
  }
  return true;
}

std::string sampleOf(const std::string& data) {
  const size_t nl = data.find('\n');
  return nl == std::string::npos ? data : data.substr(0, nl);
}

bool modelOf(const std::string& data, Model& out) {
  const size_t nl = data.find('\n');
  if (nl == std::string::npos) return false;
  return readText(data.substr(nl + 1), out);
}

std::string makeData(const Model& m) { return m.decks[0].file + "\n" + writeText(m); }

// -------------------------------------------------------------------- laws
double masterGain(int32_t v) { return (std::pow(11.0, v / 12800.0) - 1.0) / 10.0; }
double regionGain(int32_t v) { return (std::pow(3.0, v / 8000.0) - 1.0) / 2.0; }

void panLaw(double pan, double vol, double& l, double& r) {
  const double a = (1.0 + std::clamp(pan, -1.0, 1.0)) * (kPi / 4.0);
  l = std::cos(a) * vol;
  r = std::sin(a) * vol;
}

namespace {

double flLevel(int32_t v, double full) {  // sign·(11^(|v|/full) − 1)/10
  const double x = std::fabs(static_cast<double>(v)) / full;
  return (v < 0 ? -1.0 : 1.0) * (std::pow(11.0, x) - 1.0) / 10.0;
}

}  // namespace

// ------------------------------------------------------------------ prepare
std::shared_ptr<const Aux> prepare(const PluginState& s) {
  auto a = std::make_shared<Aux>();
  if (!modelOf(s.data, a->model)) return nullptr;
  for (size_t g = 0; g < kGroups; ++g)
    for (size_t t = 0; t < kTargets; ++t) {
      const Articulator& ar = a->model.groups[g].arts[t];
      Aux::PArt& p = a->groups[g][t];
      const double neutral = (t == TVol || t == TSpeed) ? 1.0 : 0.5;
      p.env = flart::prepare(ar.env, neutral);
      p.lfo = flart::prepare(ar.lfo, 0.5);
      for (size_t k = 0; k < 4; ++k) p.maps[k] = flart::prepare(ar.maps[k], neutral);
    }
  a->keyMap = buildKeyMap(a->model, &s);
  return a;
}

// ------------------------------------------------------------------ key map
int explicitKey(const Model& m, const PluginState* s, int deck, int region) {
  if (deck < 0 || deck > 1 || region < 0 || static_cast<size_t>(region) >= m.decks[static_cast<size_t>(deck)].regions.size()) return -1;
  int k = m.decks[static_cast<size_t>(deck)].regions[static_cast<size_t>(region)].key;
  if (s && deck == 0 && region < pp::kMaxSlices) k = static_cast<int>(std::lround(prm(*s, pp::slParam2(region, pp::SliceKey), -1.0f)));
  return k >= 0 && k <= 127 ? k : -1;
}

KeyMap buildKeyMap(const Model& m, const PluginState* s) {
  KeyMap km;
  const bool sameDeck = m.opt(OSameDeck);
  // Explicit keys (both decks' regions in each map unless "same deck only").
  for (int d = 0; d < 2; ++d)
    for (int src = 0; src < 2; ++src) {
      if (sameDeck && src != d) continue;
      const int n = static_cast<int>(m.decks[static_cast<size_t>(src)].regions.size());
      for (int r = 0; r < n; ++r) {
        const int k = explicitKey(m, s, src, r);
        if (k >= 0) km[static_cast<size_t>(d)][static_cast<size_t>(k)].push_back(src * kDeckStride + r);
      }
    }
  // Automatic regions: base + index while that key is still free.
  for (int d = 0; d < 2; ++d) {
    const int count = static_cast<int>(m.decks[static_cast<size_t>(d)].regions.size());
    const int base = count > 65 ? std::max(0, 60 - 12 * ((count - 65 + 11) / 12)) : 60;
    for (int r = 0; r < count; ++r) {
      if (explicitKey(m, s, d, r) >= 0) continue;
      const int key = base + r;
      if (key < 0 || key > 127) continue;
      auto& slot = km[static_cast<size_t>(d)][static_cast<size_t>(key)];
      if (slot.empty()) slot.push_back(d * kDeckStride + r);
    }
  }
  return km;
}

int regionKey(const KeyMap& km, int deck, int region) {
  if (deck < 0 || deck > 1) return -1;
  const int e = deck * kDeckStride + region;
  for (int k = 0; k < 128; ++k)
    for (int v : km[static_cast<size_t>(deck)][static_cast<size_t>(k)])
      if (v == e) return k;
  return -1;
}

// ------------------------------------------------------------- deck audio
namespace {

uint32_t le32(const uint8_t* p) { return static_cast<uint32_t>(rd32(p)); }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
void put32(std::vector<uint8_t>& o, uint32_t v) { wr32(o, static_cast<int32_t>(v)); }
void putId(std::vector<uint8_t>& o, const char* id) { o.insert(o.end(), id, id + 4); }

}  // namespace

WavRegions readWavRegions(const uint8_t* p, size_t n) {
  WavRegions w;
  if (!p || n < 12 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) return w;
  w.riff = true;
  const size_t end = std::min<size_t>(n, static_cast<size_t>(le32(p + 4)) + 8);
  struct Cue {
    uint32_t id, pos;
  };
  std::vector<Cue> cues;
  std::vector<std::pair<uint32_t, uint32_t>> lengths;    // cue id → length
  std::vector<std::pair<uint32_t, std::string>> labels;  // cue id → name
  std::vector<std::pair<uint32_t, int>> keys;            // cue id → key
  uint32_t blockAlign = 0, dataSize = 0, factFrames = 0, tag = 0;
  bool haveData = false;
  for (size_t q = 12; q + 8 <= end;) {
    const uint8_t* id = p + q;
    const uint32_t len = le32(p + q + 4);
    const size_t body = q + 8, bl = std::min<size_t>(len, end - body);
    if (std::memcmp(id, "fmt ", 4) == 0 && bl >= 14) {
      tag = le16(p + body);
      blockAlign = le16(p + body + 12);
    } else if (std::memcmp(id, "fact", 4) == 0 && bl >= 4) {
      factFrames = le32(p + body);
    } else if (std::memcmp(id, "data", 4) == 0) {
      dataSize = len;
      haveData = true;
    } else if (std::memcmp(id, "cue ", 4) == 0 && bl >= 4) {
      const uint32_t c = le32(p + body);
      for (uint32_t k = 0; k < c && k < 100000 && 4 + 24 * static_cast<size_t>(k) + 24 <= bl; ++k) {
        const uint8_t* e = p + body + 4 + 24 * static_cast<size_t>(k);
        cues.push_back({le32(e), le32(e + 4)});  // id, position (the field the plugin reads)
      }
    } else if (std::memcmp(id, "LIST", 4) == 0 && bl >= 4 && std::memcmp(p + body, "adtl", 4) == 0) {
      for (size_t r = 4; r + 8 <= bl;) {
        const uint8_t* sid = p + body + r;
        const uint32_t sl = le32(sid + 4);
        const size_t sb = std::min<size_t>(sl, bl - r - 8);
        if (std::memcmp(sid, "ltxt", 4) == 0 && sb >= 8) lengths.emplace_back(le32(sid + 8), le32(sid + 12));
        if (std::memcmp(sid, "labl", 4) == 0 && sb >= 4) {
          std::string nm(reinterpret_cast<const char*>(sid + 12), sb - 4);
          nm = nm.substr(0, nm.find('\0'));
          labels.emplace_back(le32(sid + 8), nm);
        }
        if (sl > bl - r - 8) break;
        r += 8 + sl + (sl & 1);
      }
    } else if (std::memcmp(id, "tlst", 4) == 0 && bl >= 4) {
      const uint32_t c = le32(p + body);
      size_t r = 4;
      for (uint32_t k = 0; k < c && r + 24 <= bl; ++k) {
        const uint8_t* e = p + body + r;
        if (std::memcmp(e, "cue ", 4) == 0 && le32(e + 8) == 1 && e[14] == 0x90 && le32(e + 16) == 0)
          keys.emplace_back(le32(e + 4) + 1, std::min<int>(e[13], 127));
        const uint32_t extra = le32(e + 20);
        if (extra > bl - r - 24) break;
        r += 24 + extra;
      }
    }
    if (len > end - q - 8) break;
    q += 8 + len + (len & 1);
  }
  const bool linear = tag == 1 || tag == 3 || tag == 0xFFFE;
  if (haveData && linear && blockAlign > 0) w.frames = dataSize / blockAlign;
  else if (!linear && factFrames > 0) w.frames = factFrames;
  for (const Cue& c : cues) {
    Region r;
    r.start = c.pos;
    for (const auto& [cid, l] : lengths)
      if (cid == c.id && l > 0) r.length = l;
    for (const auto& [cid, nm] : labels)
      if (cid == c.id) r.name = nm;
    for (const auto& [cid, k] : keys)
      if (cid == c.id) r.key = k;
    w.regions.push_back(r);
  }
  return w;
}

std::vector<uint8_t> writeWavRegions(const std::vector<uint8_t>& riff, const std::vector<Region>& regions) {
  if (riff.size() < 12 || std::memcmp(riff.data(), "RIFF", 4) != 0 || std::memcmp(riff.data() + 8, "WAVE", 4) != 0) return riff;
  std::vector<uint8_t> o(riff.begin(), riff.begin() + 12);
  const size_t end = std::min<size_t>(riff.size(), static_cast<size_t>(le32(riff.data() + 4)) + 8);
  // Every chunk but the region chunks, in place.
  for (size_t q = 12; q + 8 <= end;) {
    const uint8_t* id = riff.data() + q;
    const uint32_t len = le32(id + 4);
    const size_t total = std::min<size_t>(8 + static_cast<size_t>(len) + (len & 1), end - q);
    const bool region = std::memcmp(id, "cue ", 4) == 0 || std::memcmp(id, "tlst", 4) == 0 ||
                        (std::memcmp(id, "LIST", 4) == 0 && len >= 4 && q + 12 <= end && std::memcmp(id + 8, "adtl", 4) == 0);
    if (!region) o.insert(o.end(), id, id + total);
    if (len > end - q - 8) break;
    q += 8 + len + (len & 1);
  }
  if (!regions.empty()) {
    // "cue ": id k + 1, position = sample offset = start, chunk "data".
    putId(o, "cue ");
    put32(o, static_cast<uint32_t>(4 + 24 * regions.size()));
    put32(o, static_cast<uint32_t>(regions.size()));
    for (size_t k = 0; k < regions.size(); ++k) {
      const uint32_t pos = static_cast<uint32_t>(std::max<int64_t>(0, regions[k].start));
      put32(o, static_cast<uint32_t>(k + 1)), put32(o, pos), putId(o, "data"), put32(o, 0), put32(o, 0), put32(o, pos);
    }
    // "LIST adtl": names and lengths.
    std::vector<uint8_t> l;
    putId(l, "adtl");
    for (size_t k = 0; k < regions.size(); ++k) {
      const Region& r = regions[k];
      if (!r.name.empty()) {
        const uint32_t sl = static_cast<uint32_t>(4 + r.name.size() + 1);
        putId(l, "labl"), put32(l, sl), put32(l, static_cast<uint32_t>(k + 1));
        l.insert(l.end(), r.name.begin(), r.name.end());
        l.push_back(0);
        if (sl & 1) l.push_back(0);
      }
      if (r.length > 0) {
        putId(l, "ltxt"), put32(l, 20), put32(l, static_cast<uint32_t>(k + 1)), put32(l, static_cast<uint32_t>(r.length));
        putId(l, "rgn "), put32(l, 0), put32(l, 0);
      }
    }
    putId(o, "LIST");
    put32(o, static_cast<uint32_t>(l.size()));
    o.insert(o.end(), l.begin(), l.end());
    // "tlst": explicit keys.
    size_t keyed = 0;
    for (const Region& r : regions) keyed += r.key >= 0 ? 1 : 0;
    if (keyed > 0) {
      putId(o, "tlst");
      put32(o, static_cast<uint32_t>(4 + 24 * keyed));
      put32(o, static_cast<uint32_t>(keyed));
      for (size_t k = 0; k < regions.size(); ++k) {
        if (regions[k].key < 0) continue;
        putId(o, "cue "), put32(o, static_cast<uint32_t>(k)), put32(o, 1);
        o.push_back(0xFF), o.push_back(static_cast<uint8_t>(std::min(regions[k].key, 127))), o.push_back(0x90), o.push_back(0);
        put32(o, 0), put32(o, 0);
      }
    }
  }
  const uint32_t size = static_cast<uint32_t>(o.size() - 8);
  for (int i = 0; i < 4; ++i) o[4 + static_cast<size_t>(i)] = static_cast<uint8_t>(size >> (8 * i));
  return o;
}

// ------------------------------------------------------------------- engine
void Engine::configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) {
  st_ = &s;
  aux_ = dynamic_cast<const Aux*>(aux.custom.get());
  ctx_ = ctx;
  deck_[0] = aux.sample(0);
  deck_[1] = aux.sample(1);
  const bool hq = aux_ && aux_->model.opt(OHqEnv);
  const double nBlk = hq ? 4.0 : 48.0;
  block_ = std::max(4, static_cast<int>(std::lround(ctx.sampleRate * nBlk / 44100.0 / 4.0)) * 4);
  modX_ = prm(s, pp::SlModX, 0.5f), modY_ = prm(s, pp::SlModY, 0.5f);
}

bool Engine::active() const {
  for (const auto& v : voices_)
    if (v.on) return true;
  return false;
}

void Engine::noteOn(uint32_t id, double pitch, float velocity, const NoteProps& props) {
  if (!aux_ || !st_) return;
  const int k = noteKey(pitch, props);  // J-180: the written key; the note's fine pitch tunes the region
  if (k < 0 || k > 127) return;
  const int deck = props.color & 1;
  const auto& layers = aux_->keyMap[static_cast<size_t>(deck)][static_cast<size_t>(k)];
  const int n = static_cast<int>(layers.size());
  if (n == 0) return;
  std::array<double, 16> gain{};
  const int m = std::min(n, 16);
  const int mode = aux_->model.layering(), xf = aux_->model.crossfade();
  if (n == 1 || mode <= 1 || mode >= 7) {
    for (int i = 0; i < m; ++i) gain[static_cast<size_t>(i)] = (mode == 1 && n > 1) ? 1.0 / n : 1.0;
  } else {
    double x = 0;
    if (mode == 2) x = velocity;
    else if (mode == 3) x = modX_;
    else if (mode == 4) x = modY_;
    else if (mode == 5) {
      rng_ ^= rng_ << 13, rng_ ^= rng_ >> 7, rng_ ^= rng_ << 17;
      x = static_cast<double>(rng_ >> 11) / 9007199254740992.0;
    } else {
      int& c = cycle_[static_cast<size_t>(k)];
      x = static_cast<double>(c % n) / std::max(1, n - 1);
      ++c;
    }
    const int sel = std::clamp(static_cast<int>(std::nearbyint(n * x - 0.5)), 0, n - 1);
    for (int i = 0; i < m; ++i) {
      if (xf == 0) {
        gain[static_cast<size_t>(i)] = i == sel ? 1.0 : 0.0;
        continue;
      }
      const double f = 1.0 - std::fabs((n - 1) * x - i);
      double g = 0;
      if (f > 0) {
        if (xf == 1) g = f;
        else if (xf == 2) g = (std::cos(kPi * (1.0 - f)) + 1.0) * 0.5;
        else if (xf == 3) g = 1.0 - (std::exp(std::log(9.0) * (1.0 - f)) - 1.0) / 8.0;
        else g = f >= 0.5 ? 1.0 : (std::cos(kPi * (1.0 - 2.0 * f)) + 1.0) * 0.5;
      }
      gain[static_cast<size_t>(i)] = g;
    }
  }
  bool first = true;
  for (int i = 0; i < m; ++i) {
    if (gain[static_cast<size_t>(i)] <= 1e-6) continue;
    const int e = layers[static_cast<size_t>(i)];  // deck * kDeckStride + region (explicit keys can layer the other deck)
    startVoice(e / kDeckStride, e % kDeckStride, gain[static_cast<size_t>(i)], pitch - k, velocity, props, id, first);
    first = false;
  }
}

void Engine::startVoice(int deck, int region, double layerGain, double tune, float velocity, const NoteProps& props, uint32_t id, bool master) {
  const Model& md = aux_->model;
  const Deck& dk = md.decks[static_cast<size_t>(deck)];
  if (region < 0 || static_cast<size_t>(region) >= dk.regions.size()) return;
  const SamplePtr& smp = deck_[deck];
  if (!smp || smp->frames() < 2) return;
  const Region& rg = dk.regions[static_cast<size_t>(region)];
  std::array<int32_t, kRegionInts> rec = rg.rec;
  const PluginState& s = *st_;
  // Deck A regions: gain / pan / pitch / cut group / delay / end from the slicer's parameters.
  double gain = regionGain(rec[RVolume]), pitchC = rec[RPitch], delayBeats = rec[RDelay] / 12800.0;
  int cut = rec[RCut];
  int64_t endF = -1;
  if (deck == 0 && region < pp::kMaxSlices) {
    const double db = prm(s, pp::slParam(region, pp::SliceGain), 0);
    gain = db <= -24.0 + 1e-9 ? 0.0 : std::pow(10.0, db / 20.0);
    pitchC = prm(s, pp::slParam(region, pp::SlicePitch), 0) * 100.0;
    cut = static_cast<int>(std::lround(prm(s, pp::slParam2(region, pp::SliceCut), 0)));
    delayBeats = prm(s, pp::slParam2(region, pp::SliceDelay), 0) * 2.0;  // stored as seconds at 120 BPM
    const double e = prm(s, pp::slParam2(region, pp::SliceEnd), -1);
    if (e >= 0) endF = static_cast<int64_t>(std::llround(e * static_cast<double>(smp->frames())));
  }
  const int64_t frames = static_cast<int64_t>(smp->frames());
  int64_t s0 = std::clamp<int64_t>(rg.start, 0, frames - 1), e0;
  if (endF >= 0) e0 = endF;
  else if (rg.length >= 0) e0 = s0 + rg.length;
  else e0 = static_cast<size_t>(region + 1) < dk.regions.size() ? dk.regions[static_cast<size_t>(region + 1)].start : frames;
  e0 = std::clamp<int64_t>(e0, s0 + 1, frames);
  // Cut groups stop the voices of the group (short declick).
  if (cut > 0)
    for (auto& v : voices_)
      if (v.on && v.cut == cut && v.fade < 0) v.fade = block_;
  Voice* pick = nullptr;
  for (auto& v : voices_)
    if (!v.on) {
      pick = &v;
      break;
    }
  if (!pick) pick = &voices_[next_++ % voices_.size()];
  Voice& v = *pick;
  v = Voice{};
  v.on = true, v.held = true, v.master = master;
  v.id = id;
  v.deck = deck, v.region = region, v.cut = cut;
  v.reverse = props.color > 13;
  v.start = static_cast<double>(s0), v.end = static_cast<double>(e0);
  if (md.opt(OLoopHalf)) v.loopStart = v.start + (v.end - v.start) * 0.5;
  v.layerGain = layerGain, v.regionGain = gain, v.velocity = velocity, v.notePan = props.pan;
  v.ampAmt = rec[RAmpAmt] / 12800.0, v.filtAmt = rec[RFiltAmt] / 12800.0, v.speedAmt = 8.0 * rec[RSpeedAmt] / 12800.0;
  v.startAmt = 2.0 * rec[RStartAmt] / 12800.0;
  v.vx = props.modX - 0.5, v.vy = props.modY - 0.5;
  const double masterPitch = prm(s, pp::SlPitch, 0) * 100.0;
  v.fine = tune * 100.0;  // the note's tuning beyond its key (fine pitch, channel pitch), cents
  v.baseRate = smp->sampleRate / ctx_.sampleRate * std::pow(2.0, (pitchC + masterPitch + v.fine) / 1200.0);
  v.rate = v.baseRate;
  v.delay = static_cast<int64_t>(std::llround(ctx_.sampleRate * delayBeats * 60.0 / std::max(1.0, ctx_.tempo)));
  // Articulator groups per target (stored 0-based; −1 = none).
  const int groupOf[kTargets] = {rec[RAmpGroup], rec[RAmpGroup], rec[RFiltGroup], rec[RFiltGroup], rec[RSpeedGroup], rec[RStartGroup]};
  const double rnd = flLevel(md.knobs[3], 256.0);
  rng_ ^= rng_ << 13, rng_ ^= rng_ >> 7, rng_ ^= rng_ << 17;
  const double rr = static_cast<double>(rng_ >> 11) / 9007199254740992.0;
  for (int t = 0; t < kTargets; ++t) {
    ArtRun& ar = v.art[static_cast<size_t>(t)];
    const int g = groupOf[t];
    const bool mult = t == TVol || t == TSpeed;
    double init = t == TVol ? 1.0 : 0.0;
    if (g < 0 || g >= kGroups) {
      ar.group = -1;
      v.init[static_cast<size_t>(t)] = init;
      continue;
    }
    ar.group = g;
    const Aux::PArt& p = aux_->groups[static_cast<size_t>(g)][static_cast<size_t>(t)];
    const Articulator& a = md.groups[static_cast<size_t>(g)].arts[static_cast<size_t>(t)];
    const double centre = mult ? 0.0 : 0.5;
    flart::Scales sc;
    sc.attack = a.env.extra[0] / 128.0, sc.decay = a.env.extra[1] / 128.0, sc.sustain = a.env.extra[2] / 128.0, sc.release = a.env.extra[3] / 128.0;
    if (p.env.enabled) flart::envStart(ar.env, p.env, sc, centre);
    if (p.lfo.enabled) {
      flart::envStart(ar.amp, p.lfo, flart::Scales{}, 0.5);
      flart::LfoShape sh;
      sh.set(a.lfo.extra[1], a.lfo.extra[2], a.lfo.extra[3], (a.lfo.flags & flart::kBipolarTen) != 0);
      ar.phase = sh.startPhase(a.lfo.phase);
    }
    if (t == TSpeed && (p.env.enabled || p.lfo.enabled)) init = 1.0;
    if (!p.maps[2].flat) init = mult ? flart::mapValue(p.maps[2], velocity, 0.0) : flart::mapValue(p.maps[2], velocity, 0.5);
    if (!p.maps[3].flat) {
      const double r = flart::mapValue(p.maps[3], rr, 0.0);
      init = mult ? init * (1.0 + rnd * (r - 1.0)) : init + rnd * (r - 0.5);
    }
    v.init[static_cast<size_t>(t)] = init;
  }
  // Sample start (articulator value at note-on).
  {
    const double aStart = articulator(v, TStart, 0.0);
    const double ofs = rec[RStart] / 12800.0;
    const double sOfs = v.startAmt * aStart + ofs;
    if (sOfs > 0) {
      const double len = v.end - v.start;
      const double off = md.opt(OStartWhole) ? std::min(sOfs, 1.0) * len : sOfs * std::min(len, 0.1 * smp->sampleRate);
      v.pos = std::clamp(v.start + off, v.start, v.end - 1);
    } else {
      v.pos = v.start;
    }
    if (v.reverse) v.pos = v.end - 1 - (v.pos - v.start);
  }
  // Region filter (only with an active filter group).
  if (rec[RFiltGroup] >= 0 && rec[RFiltGroup] < kGroups) {
    const auto& hdr = md.groups[static_cast<size_t>(rec[RFiltGroup])].hdr;
    v.stages = std::clamp(hdr[4], 0, 3);
    v.ftype = hdr[3] & 0xFF;
  }
  v.gl = v.gr = 0;  // declick: ramps up over the first control block
  control(v);
  if (act_ && deck == 0) act_->started(static_cast<int>(pick - voices_.data()), region);  // J-186
}

// Articulator value of target t, advanced by `dtBlocks` control blocks.
double Engine::articulator(Voice& v, int t, double dtBlocks) {
  const ArtRun& ar0 = v.art[static_cast<size_t>(t)];
  if (!ar0.on()) return t == TVol ? 1.0 : 0.0;
  ArtRun& ar = v.art[static_cast<size_t>(t)];
  const Model& md = aux_->model;
  const Group& gr = md.groups[static_cast<size_t>(ar.group)];
  const Articulator& a = gr.arts[static_cast<size_t>(t)];
  const Aux::PArt& p = aux_->groups[static_cast<size_t>(ar.group)][static_cast<size_t>(t)];
  const bool mult = t == TVol || t == TSpeed;
  const double centre = mult ? 0.0 : 0.5;
  const double spt = ctx_.sampleRate * 60.0 / (std::max(1.0, ctx_.tempo) * std::max(1, ctx_.ppq));
  const double ticks = dtBlocks * block_ / spt;
  const double masterLfo = flLevel(md.knobs[4], 128.0);
  double envAmt = a.floats[1], lfoAmt = a.floats[2] * masterLfo;
  if (t == TCut) envAmt = gr.hdr[0] / 128.0, lfoAmt = gr.hdr[0] / 128.0 * masterLfo;
  flart::Scales sc;
  sc.attack = a.env.extra[0] / 128.0, sc.decay = a.env.extra[1] / 128.0, sc.sustain = a.env.extra[2] / 128.0, sc.release = a.env.extra[3] / 128.0;
  double out = v.init[static_cast<size_t>(t)];
  // Mod X / Y maps (positions follow the note's and the global mod values).
  const double mx = std::clamp(v.vx + modX_, 0.0, 1.0), my = std::clamp(v.vy + modY_, 0.0, 1.0);
  if (!p.maps[0].flat) out = mult ? out * flart::mapValue(p.maps[0], mx, 0.0) : out + flart::mapValue(p.maps[0], mx, 0.5);
  if (!p.maps[1].flat) out = mult ? out * flart::mapValue(p.maps[1], my, 0.0) : out + flart::mapValue(p.maps[1], my, 0.5);
  double env = 0, lfo = 0;
  if (p.env.enabled) {
    if (ticks > 0) flart::envTick(ar.env, p.env, sc, centre, ticks * flart::unitsPerTick(p.env.tempo, ctx_.tempo, ctx_.ppq));
    env = ar.env.out;
  }
  if (p.lfo.enabled) {
    if (ticks > 0) {
      const bool delayed = ar.amp.predelay();
      flart::envTick(ar.amp, p.lfo, flart::Scales{}, 0.5, ticks * flart::unitsPerTick(p.lfo.tempo, ctx_.tempo, ctx_.ppq));
      if (!(a.lfo.flags & flart::kFrozen) && !delayed) {
        const double inc = flart::lfoRate(a.lfo.extra[0]) * 4294967296.0 * flart::unitsPerTick(p.lfo.tempo, ctx_.tempo, ctx_.ppq) * ticks;
        ar.phase += static_cast<uint32_t>(static_cast<uint64_t>(std::llround(inc)) & 0xFFFFFFFFull);
      }
    }
    flart::LfoShape sh;
    sh.set(a.lfo.extra[1], a.lfo.extra[2], a.lfo.extra[3], (a.lfo.flags & flart::kBipolarTen) != 0);
    lfo = sh.value(ar.phase) * ar.amp.out;
  }
  if (t == TSpeed) {
    double sp = 0;
    if (p.env.enabled) sp += env;
    if (p.lfo.enabled) sp += lfoAmt * lfo;
    return out * sp;
  }
  if (p.env.enabled) out = mult ? out * env : out + envAmt * env;
  if (p.lfo.enabled) out = mult ? out * (1.0 + 2.0 * lfoAmt * lfo) : out + lfoAmt * lfo;
  return out;
}

void Engine::control(Voice& v) {
  const Model& md = aux_->model;
  const double dt = v.started ? 1.0 : 0.0;
  v.started = true;
  const double aPan = articulator(v, TPan, dt), aVol = articulator(v, TVol, dt);
  const double aCut = articulator(v, TCut, dt), aRes = articulator(v, TRes, dt);
  const double aSpeed = articulator(v, TSpeed, dt);
  (void)articulator(v, TStart, dt);
  const Region& rg = md.decks[static_cast<size_t>(v.deck)].regions[static_cast<size_t>(v.region)];
  const double regionPan =
      v.deck == 0 && v.region < pp::kMaxSlices ? prm(*st_, pp::slParam(v.region, pp::SlicePan), 0) : rg.rec[RPan] / 6400.0;
  const double pan = std::clamp(regionPan + v.notePan + v.ampAmt * 2.0 * aPan, -1.0, 1.0);
  const double master = prm(*st_, pp::SlMaster, 1);
  const double vol = (v.ampAmt * (aVol - 1.0) + 1.0) * v.regionGain * v.layerGain * v.velocity * master;
  double l, r;
  panLaw(pan, vol, l, r);
  if (v.fade >= 0) l = r = 0;  // release declick / cut
  v.tl = l, v.tr = r;
  v.dl = (v.tl - v.gl) / block_, v.dr = (v.tr - v.gr) / block_;
  if (v.art[TSpeed].on()) v.rate = v.baseRate * (v.speedAmt * aSpeed + 1.0);
  // Filter coefficients (spec §A5).
  if (v.stages > 0) {
    const auto& hdr = md.groups[static_cast<size_t>(rg.rec[RFiltGroup])].hdr;
    const double gCut = (hdr[1] - 128) / 256.0, gRes = (hdr[2] - 128) / 256.0;
    const double c = rg.rec[RCutoff] / 256.0 + v.filtAmt * (gCut + aCut);
    const double q = rg.rec[RReso] / 256.0 + v.filtAmt * (gRes + aRes);
    const double ratio = 44100.0 / ctx_.sampleRate;
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    if (v.ftype == 0 || v.ftype == 3) {
      const bool lp = v.ftype == 0;
      double f = c <= 0 ? 0.0 : (std::exp(7.31389 * c) - 1.0) * (lp ? 0.000664667 : 0.00039993);
      if (!lp) f = std::min(f, 0.6001);
      const double w = std::clamp(2.0 * kPi * ratio * (f + (lp ? 0.003 : 0.0001)), 0.0, 2.849517);
      double Q = q <= 0 ? 0.1 : (q >= 1 ? 50.0 : (std::exp(7.60140 * q) - 1.0) * 0.02495 + 0.1);
      Q = std::pow(Q, 1.0 / v.stages);
      const double cw = std::cos(w), sw = std::sin(w), al = sw * 0.5 / Q;
      if (lp) b0 = (1 - cw) * 0.5, b1 = 1 - cw, b2 = b0;
      else b0 = (1 + cw) * 0.5, b1 = -(1 + cw), b2 = b0;
      a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
    } else {
      double f = c <= 0 ? 0.0 : (std::exp(7.31389 * c) - 1.0) * 0.00039993;
      f = std::min(f, 1.0001);
      const double w = std::clamp(2.0 * kPi * ratio * (f + 0.0001), 1e-6, 2.849517);
      double bw = q <= 0 ? 0.01 : (q >= 1 ? 12.0 : (std::exp(4.61512 * q) - 1.0) * 0.1199 + 0.01);
      bw *= (v.ftype == 1 ? v.stages : 1.0 / v.stages) * 0.346574;
      const double cw = std::cos(w), sw = std::sin(w), al = sw * std::sinh(bw * w / sw);
      if (v.ftype == 1) b0 = al, b1 = 0, b2 = -al;
      else b0 = 1, b1 = -2 * cw, b2 = 1;
      a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
    }
    for (int k = 0; k < v.stages; ++k) {
      Biquad& bq = v.bq[k];
      bq.b0 = b0 / a0, bq.b1 = b1 / a0, bq.b2 = b2 / a0, bq.a1 = a1 / a0, bq.a2 = a2 / a0;
    }
  }
  // After the note-off the voice ends once every articulator has finished
  // (disabled parts count as finished): gated playback.
  if (v.released && v.fade < 0) {
    bool done = true;
    for (int t = 0; t < kTargets; ++t) {
      const ArtRun& ar = v.art[static_cast<size_t>(t)];
      if (!ar.on()) continue;
      const Aux::PArt& p = aux_->groups[static_cast<size_t>(ar.group)][static_cast<size_t>(t)];
      if ((p.env.enabled && !ar.env.ended) || (p.lfo.enabled && !ar.amp.ended)) done = false;
    }
    if (done) {
      v.fade = block_;
      v.tl = v.tr = 0;
      v.dl = (v.tl - v.gl) / block_, v.dr = (v.tr - v.gr) / block_;
    }
  }
}

void Engine::noteOff(uint32_t id) {
  if (!aux_) return;
  for (auto& v : voices_) {
    if (!v.on || v.id != id || !v.held) continue;
    v.held = false;
    if (v.delay > 0) {  // still waiting on its delay: dropped
      v.on = false;
      continue;
    }
    v.released = true;
    for (int t = 0; t < kTargets; ++t) {
      ArtRun& ar = v.art[static_cast<size_t>(t)];
      if (!ar.on()) continue;
      const Aux::PArt& p = aux_->groups[static_cast<size_t>(ar.group)][static_cast<size_t>(t)];
      const Articulator& a = aux_->model.groups[static_cast<size_t>(ar.group)].arts[static_cast<size_t>(t)];
      flart::Scales sc;
      sc.attack = a.env.extra[0] / 128.0, sc.decay = a.env.extra[1] / 128.0, sc.sustain = a.env.extra[2] / 128.0, sc.release = a.env.extra[3] / 128.0;
      const bool mult = t == TVol || t == TSpeed;
      if (p.env.enabled) flart::envRelease(ar.env, p.env, sc, mult ? 0.0 : 0.5);
      if (p.lfo.enabled) flart::envRelease(ar.amp, p.lfo, flart::Scales{}, 0.5);
    }
  }
}

void Engine::allOff() {
  for (auto& v : voices_)
    if (v.on && v.held) noteOff(v.id);
}

void Engine::kill() {
  for (auto& v : voices_) v = Voice{};
}

// J-186: the deck A regions the voices play, for the editor (after each block).
void Engine::publish() {
  if (!act_) return;
  const int n = std::min(static_cast<int>(voices_.size()), RegionActivity::kSlots);
  for (int i = 0; i < n; ++i) {
    const Voice& v = voices_[static_cast<size_t>(i)];
    if (v.on && v.deck == 0)
      act_->set(i, v.region, v.delay > 0 ? -1 : static_cast<int64_t>(v.pos), static_cast<float>(std::max(std::fabs(v.gl), std::fabs(v.gr))),
                v.held && !v.released);
    else
      act_->off(i);
  }
  for (int i = n; i < RegionActivity::kSlots; ++i) act_->off(i);
}

void Engine::render(float* L, float* R, int n) {
  if (!aux_) {
    publish();
    return;
  }
  for (auto& v : voices_) {
    if (!v.on) continue;
    const SamplePtr& smp = deck_[v.deck];
    if (!smp) {
      v.on = false;
      continue;
    }
    const float* sl = smp->left.data();
    const float* sr = smp->right.data();
    const int64_t frames = static_cast<int64_t>(smp->frames());
    for (int i = 0; i < n && v.on; ++i) {
      if (v.delay > 0) {
        --v.delay;
        continue;
      }
      const double x = v.pos;
      int64_t i0 = static_cast<int64_t>(std::floor(x));
      const double fr = x - static_cast<double>(i0);
      i0 = std::clamp<int64_t>(i0, 0, frames - 1);
      double l, r;
      if (fr < 1e-9 && std::fabs(v.rate) <= 1.000001) {  // exact unless reading faster
        l = sl[i0], r = sr[i0];
      } else {  // J-87: band-limited (was linear): no aliasing when pitched up
        float a, b;
        stretch::readSinc2(sl, sr, frames, x, std::fabs(v.rate), *sinc_, a, b);
        l = a, r = b;
      }
      for (int k = 0; k < v.stages; ++k) l = v.bq[k].run(0, l), r = v.bq[k].run(1, r);
      v.gl += v.dl, v.gr += v.dr;
      L[i] += static_cast<float>(l * v.gl);
      R[i] += static_cast<float>(r * v.gr);
      v.pos += v.reverse ? -v.rate : v.rate;
      if (!v.reverse && v.pos >= v.end) {
        if (v.loopStart >= 0) v.pos = v.loopStart + std::fmod(v.pos - v.loopStart, std::max(1.0, v.end - v.loopStart));
        else v.on = false;
      } else if (v.reverse && v.pos < v.start) {
        v.on = false;
      }
      if (v.fade > 0 && --v.fade == 0) v.on = false;
      // Per-voice control blocks of block_ samples from the voice start.
      if (++v.blockPos >= block_) {
        v.blockPos = 0;
        v.gl = v.tl, v.gr = v.tr;
        control(v);
      }
    }
  }
  publish();
}

}  // namespace gl::slicex
