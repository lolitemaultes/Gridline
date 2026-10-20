// Gridline — FLP export: FL's plugin host ("Fruity Wrapper") for hosted
// third-party plugins (VST2 / VST3 / AU), and for FL's own plugins that old
// projects load through the wrapper. The reverse of FlpWrapper.cpp (hosted
// plugins) and FlpFx2Mod.cpp decodeWrapper (FL plugins in the wrapper); see
// FlpEncode.h for the model.
//
// The state (event 213) is a u32 version and records {u32 id, u64 size,
// bytes} (FlpWrapper.h): 50 plugin kind, 51 VST2 id, 52 VST3 class id / AU
// component, 53 the plugin's own state, 54 name, 55 path, 56 vendor, plus
// wrapper settings (1, 2, 30..32) that are kept from the origin state or
// written with FL's defaults. A hosted plugin is written with its identity
// (from its registered class when installed, else from the type and the
// origin state) and the saved state Gridline keeps in PluginState::data, so
// FL loads the same installed plugin with the same settings. CLAP plugins
// have no FL wrapper form: they are not encoded here.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include "FlpEncode.h"
#include "../../host/HostedPlugins.h"

namespace gl::flp::enc {

namespace {

constexpr uint32_t kWrapperVersion = 8;

struct Rec {
  uint32_t id = 0;
  Bytes data;
};
std::vector<Rec> records(const Bytes& b) {
  std::vector<Rec> out;
  size_t p = 4;
  while (p + 12 <= b.size()) {
    const uint32_t id = getU32(b, p);
    const uint64_t n = static_cast<uint64_t>(getU32(b, p + 4)) | (static_cast<uint64_t>(getU32(b, p + 8)) << 32);
    if (n > b.size() - p - 12) break;
    out.push_back({id, Bytes(b.begin() + static_cast<std::ptrdiff_t>(p + 12), b.begin() + static_cast<std::ptrdiff_t>(p + 12 + n))});
    p += 12 + static_cast<size_t>(n);
  }
  return out;
}
const Rec* findRec(const std::vector<Rec>& rs, uint32_t id) {
  for (const Rec& r : rs)
    if (r.id == id) return &r;
  return nullptr;
}
Bytes build(uint32_t version, const std::vector<Rec>& rs) {
  ByteWriter w;
  w.u32(version);
  for (const Rec& r : rs) {
    w.u32(r.id);
    w.u64(r.data.size());
    w.bytes(r.data);
  }
  return w.b;
}
Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }
std::string str(const Bytes& b) { return std::string(b.begin(), b.end()); }

// Wrapper settings records with FL's defaults (used when there is no origin).
std::vector<Rec> defaultSettings() {
  std::vector<Rec> rs;
  {
    ByteWriter w;  // 1: wrapper options
    w.i32(-1), w.i32(-1);  // no remembered program / bank
    w.u32(12);             // option flags
    w.zeros(8);
    rs.push_back({1, w.b});
  }
  {
    ByteWriter w;  // 2: editor window
    w.u8(0);
    w.u32(561), w.u32(389);  // editor size
    w.u32(293280);           // window position
    w.zeros(12);
    rs.push_back({2, w.b});
  }
  {
    ByteWriter w;  // 30: processing (enabled, smart disable off)
    w.u32(1), w.u32(1);
    w.zeros(8);
    rs.push_back({30, w.b});
  }
  for (uint32_t id : {31u, 32u}) {  // 31 / 32: input / output mapping (default)
    ByteWriter w;
    w.u32(0), w.u32(1), w.u32(0);
    rs.push_back({id, w.b});
  }
  return rs;
}
std::vector<Rec> settingsOf(const std::vector<Rec>& tpl) {
  std::vector<Rec> rs;
  for (const Rec& r : tpl)
    if (r.id == 1 || r.id == 2 || (r.id >= 30 && r.id <= 32)) rs.push_back(r);
  return rs.empty() ? defaultSettings() : rs;
}
Rec kindRec(uint32_t kind) {
  ByteWriter w;
  w.u32(kind);
  w.zeros(12);
  return {50, w.b};
}

// ------------------------------------------------------------ hosted plugins

enum class Format { None, Vst2, Vst3, Au };
Format formatOf(const std::string& type) {
  if (type.rfind("vst2:", 0) == 0) return Format::Vst2;
  if (type.rfind("vst3:", 0) == 0) return Format::Vst3;
  if (type.rfind("au:", 0) == 0) return Format::Au;
  return Format::None;
}
bool hexValue(const std::string& s, uint32_t& v) {
  if (s.empty() || s.size() > 8) return false;
  v = 0;
  for (char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    v = (v << 4) | static_cast<uint32_t>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::tolower(static_cast<unsigned char>(c)) - 'a' + 10));
  }
  return true;
}
// A four-character code as host::fourCharCode prints it ("aufx" / "0x1234ABCD").
bool fourCC(const std::string& s, uint32_t& v) {
  if (s.size() == 10 && s.rfind("0x", 0) == 0) return hexValue(s.substr(2), v);
  if (s.size() != 4) return false;
  v = 0;
  for (char c : s) v = (v << 8) | static_cast<uint8_t>(c);
  return true;
}
// VST3 class id: 32 hex digits in SDK order → 16 bytes in COM GUID order.
bool vst3Guid(const std::string& hex, Bytes& g) {
  static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  if (hex.size() != 32) return false;
  g.assign(16, 0);
  for (int i = 0; i < 16; ++i) {
    uint32_t v = 0;
    if (!hexValue(hex.substr(static_cast<size_t>(2 * i), 2), v)) return false;
    g[static_cast<size_t>(order[i])] = static_cast<uint8_t>(v);
  }
  return true;
}
uint32_t kindOf(Format f, bool instrument) {
  switch (f) {
    case Format::Vst2: return instrument ? 4 : 0;
    case Format::Vst3: return instrument ? 8 : 7;
    case Format::Au: return instrument ? 10 : 9;
    default: return 0;
  }
}
bool kindIsInstrument(uint32_t k) { return k == 4 || k == 8 || k == 10; }

// Hosted type the records describe ("" when none), as the decoder reads it.
std::string typeOfRecords(const std::vector<Rec>& rs) {
  const Rec* k = findRec(rs, 50);
  if (!k || k->data.size() < 4) return {};
  const uint32_t kind = getU32(k->data, 0);
  if (kind == 0 || kind == 4) {
    const Rec* id = findRec(rs, 51);
    return id && id->data.size() >= 4 ? host::vst2Type(getU32(id->data, 0)) : std::string();
  }
  const Rec* g = findRec(rs, 52);
  if (!g) return {};
  if ((kind == 7 || kind == 8) && g->data.size() >= 16) {
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    std::string s = "vst3:";
    char b[3];
    for (int i : order) {
      std::snprintf(b, sizeof b, "%02X", g->data[static_cast<size_t>(i)]);
      s += b;
    }
    return s;
  }
  if ((kind == 9 || kind == 10) && g->data.size() >= 12)
    return "au:" + host::auKey(getU32(g->data, 4), getU32(g->data, 8), getU32(g->data, 0));
  return {};
}

// Record 53 for the Gridline state data ("" → no record: FL loads the plugin's defaults).
bool stateRecord(Format f, const std::string& data, const host::HostedClass* cls, Bytes& out) {
  out.clear();
  if (data.empty()) return true;
  std::string bytes;
  if (f == Format::Vst2 && (data.rfind("vst2c1:", 0) == 0 || data.rfind("vst2p1:", 0) == 0)) {
    if (!host::base64Decode(data.substr(7), bytes)) return false;
    const bool chunk = data[4] == 'c';  // "vst2c1:" / "vst2p1:"
    ByteWriter w;
    w.i32(-9);  // VST state marker
    w.u8(5);    // state version
    w.i32(-2);  // 8 reserved bytes follow the size
    if (chunk) {
      w.u32(static_cast<uint32_t>(bytes.size()));
      w.zeros(8);
      w.bytes(bytes.data(), bytes.size());
    } else {
      // Parameter list: Gridline keeps the program first (FL's list has no
      // program: the plugin keeps its current one), then the values.
      if (bytes.size() < 4) return false;
      const size_t n = (bytes.size() - 4) / 4;
      w.u32(0);
      w.zeros(8);
      w.u32(static_cast<uint32_t>(n));
      w.bytes(bytes.data() + 4, n * 4);
      w.u32(1);  // trailer as FL writes it after a parameter list
      w.zeros(25);
    }
    out = w.b;
    return true;
  }
  if (f == Format::Vst3 && data.rfind("vst3s1:", 0) == 0) {
    std::string comp, ctrl;
    if (!host::decodeState(data, comp, ctrl)) return false;
    ByteWriter w;
    w.u32(1);  // VST3 state version
    auto rec = [&w](uint32_t id, const void* p, size_t n) {
      w.u32(id);
      w.u64(n);
      w.bytes(p, n);
    };
    const Bytes header(64, 0);  // header record (reserved)
    rec(1, header.data(), header.size());
    rec(3, comp.data(), comp.size());
    rec(2, ctrl.data(), ctrl.size());
    ByteWriter t;  // FL parameter index → ParamID (the class's exposed parameters)
    const size_t n = cls ? cls->params.size() : 0;
    t.u32(static_cast<uint32_t>(n));
    for (size_t k = 0; k < n; ++k) t.u32(cls->params[k].id);
    rec(4, t.b.data(), t.b.size());
    out = w.b;
    return true;
  }
  if (f == Format::Au && data.rfind("au1:", 0) == 0) {
    if (!host::base64Decode(data.substr(4), bytes)) return false;
    ByteWriter w;
    w.u32(0);
    w.u32(static_cast<uint32_t>(bytes.size()));
    w.u32(0);
    w.bytes(bytes.data(), bytes.size());
    out = w.b;
    return true;
  }
  return false;  // a state of another format (e.g. CLAP) cannot be carried
}

// Wrapper state for a hosted plugin. The template's records are kept where
// they describe the same plugin (name / path / vendor of a plugin that is
// not installed) and for the wrapper's own settings.
bool writeHosted(const PluginState& g, Bytes& b) {
  const Format f = formatOf(g.type);
  if (f == Format::None) return false;
  const std::vector<Rec> tpl = records(b);
  const bool sameAsTemplate = typeOfRecords(tpl) == g.type;
  std::shared_ptr<const host::HostedClass> cls = host::hostedClass(g.type);
  bool instrument = false;
  if (cls) instrument = cls->instrument;
  else if (const Rec* k = findRec(tpl, 50); sameAsTemplate && k && k->data.size() >= 4) instrument = kindIsInstrument(getU32(k->data, 0));
  else if (const PluginInfo* info = findPlugin(g.type)) instrument = info->kind == PluginKind::Generator;
  std::vector<Rec> rs = settingsOf(tpl);
  rs.push_back(kindRec(kindOf(f, instrument)));
  const std::string id = g.type.substr(g.type.find(':') + 1);
  if (f == Format::Vst2) {
    uint32_t v = 0;
    if (!hexValue(id, v)) return false;
    ByteWriter w;
    w.u32(v);
    rs.push_back({51, w.b});
  } else if (f == Format::Vst3) {
    Bytes guid;
    if (!vst3Guid(id, guid)) return false;
    rs.push_back({52, guid});
  } else {
    // "type/subtype/manufacturer" → u32 manufacturer, type, subtype, 0.
    const size_t s1 = id.find('/'), s2 = s1 == std::string::npos ? s1 : id.find('/', s1 + 1);
    uint32_t type = 0, sub = 0, manu = 0;
    if (s2 == std::string::npos || !fourCC(id.substr(0, s1), type) || !fourCC(id.substr(s1 + 1, s2 - s1 - 1), sub) ||
        !fourCC(id.substr(s2 + 1), manu))
      return false;
    ByteWriter w;
    w.u32(manu), w.u32(type), w.u32(sub), w.u32(0);
    rs.push_back({52, w.b});
  }
  // Identity text: the installed class, else the origin's records.
  auto keep = [&](uint32_t rid, const std::string& value) {
    if (!value.empty()) rs.push_back({rid, text(value)});
    else if (const Rec* r = findRec(tpl, rid); sameAsTemplate && r) rs.push_back(*r);
  };
  keep(54, cls ? cls->name : std::string());
  keep(55, cls && cls->format != "au" ? cls->bundle : std::string());
  keep(56, cls ? cls->vendor : std::string());
  Bytes st;
  if (!stateRecord(f, g.data, cls.get(), st)) return false;
  if (!g.data.empty()) rs.push_back({53, st});
  b = build(b.size() >= 4 ? std::max<uint32_t>(getU32(b, 0), 1) : kWrapperVersion, rs);
  return true;
}

Encoder hosted() {
  Encoder e;
  e.flName = "Fruity Wrapper";
  e.glType = "*hosted";
  e.nativeRank = 0;
  // CLAP plugins have no FL wrapper form (left to the exporter's placeholder).
  e.accepts = [](const PluginState& s) { return formatOf(s.type) != Format::None; };
  e.defaults = [] {
    std::vector<Rec> rs = defaultSettings();
    rs.push_back(kindRec(0));
    return build(kWrapperVersion, rs);  // no identity yet: written by `write`
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext&) { writeHosted(g, b); };
  return e;
}

// ------------------------------------------------------------ FL plugins in the wrapper
// Old projects load some of FL's own effects as VST plugins through the
// wrapper; the decoder maps them through the FL plugin's decoder from the
// VST parameter list (record 53: 21-byte header, u32 count, f32 values).

// FL parameter → Gridline parameter map of an FL plugin (as the wrapper
// decoder builds it from an empty state). Empty when it has none.
struct ParamMap {
  std::string glType;
  std::vector<int> gl;  // per FL parameter index (-1 = not mapped)
};
const ParamMap& paramMap(const std::string& flName) {
  static std::mutex mu;
  static std::map<std::string, ParamMap> cache;
  std::lock_guard<std::mutex> lock(mu);
  auto it = cache.find(flName);
  if (it != cache.end()) return it->second;
  ParamMap pm;
  PluginMapping m;
  if (findDecoder(flName) && flName != "Fruity Wrapper" && decodeState(flName, Bytes(4096, 0), m, EncodeContext{}) && m.param && m.value) {
    pm.glType = m.glType;
    int last = -1;
    std::vector<int> v(512, -1);
    for (int k = 0; k < 512; ++k)
      if ((v[static_cast<size_t>(k)] = m.param(k)) >= 0) last = k;
    v.resize(static_cast<size_t>(last + 1));
    pm.gl = v;
  }
  return cache.emplace(flName, pm).first->second;
}

// FL plugin to wrap for a Gridline type: the template's, else the native
// encoder's FL plugin, else any decoder that maps parameters to that type.
// A candidate fits when it maps every parameter that is not at its default.
bool wrapFits(const PluginState& g, const std::string& n);

std::string wrappedName(const PluginState& g, const std::string& tplName, const std::string& wrapAs = {}) {
  if (!wrapAs.empty() && wrapFits(g, wrapAs)) return wrapAs;
  auto fits = [&](const std::string& n) { return wrapFits(g, n); };
  if (fits(tplName)) return tplName;
  if (const Encoder* e = nativeEncoder(g); e && fits(e->flName)) return e->flName;
  for (const auto& [name, dec] : decoders())
    if (name.rfind("Fruity", 0) == 0 && fits(name)) return name;
  return {};
}

bool wrapFits(const PluginState& g, const std::string& n) {
  const PluginState def = defaultPluginState(g.type);
  auto fits = [&](const std::string& n) {
    if (n.empty()) return false;
    const ParamMap& pm = paramMap(n);
    if (pm.gl.empty() || pm.glType != g.type) return false;
    for (size_t k = 0; k < g.params.size() && k < def.params.size(); ++k)
      if (g.params[k] != def.params[k] && std::find(pm.gl.begin(), pm.gl.end(), static_cast<int>(k)) == pm.gl.end()) return false;
    return true;
  };
  return fits(n);
}

struct ParamList {
  size_t at = 0;  // offset of the first value in the state
  uint32_t count = 0;
};
ParamList paramList(const Bytes& b) {
  ParamList pl;
  size_t p = 4;
  while (p + 12 <= b.size()) {
    const uint32_t id = getU32(b, p);
    const uint64_t n = static_cast<uint64_t>(getU32(b, p + 4)) | (static_cast<uint64_t>(getU32(b, p + 8)) << 32);
    if (n > b.size() - p - 12) break;
    if (id == 53 && n >= 17) {
      // i32 -9, u8 version, i32 flag, u32 chunk size, 8 reserved bytes when the flag is -2.
      const size_t list = p + 12 + (static_cast<int32_t>(getU32(b, p + 12 + 5)) == -2 ? 21 : 13);
      pl.count = getU32(b, list);
      pl.at = list + 4;
      if (pl.at + 4 * static_cast<size_t>(pl.count) > p + 12 + n) pl.count = static_cast<uint32_t>((p + 12 + n - pl.at) / 4);
    }
    p += 12 + static_cast<size_t>(n);
  }
  return pl;
}

Encoder wrappedFl() {
  Encoder e;
  e.flName = "Fruity Wrapper";
  e.glType = "";  // any Gridline type an FL plugin in the wrapper decodes to
  e.nativeRank = -1;  // native instances are written as the FL plugin itself
  e.defaults = [] {
    std::vector<Rec> rs = defaultSettings();
    rs.push_back(kindRec(0));
    return build(kWrapperVersion, rs);
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& ec) {
    if (formatOf(g.type) != Format::None) return;
    const std::vector<Rec> tpl = records(b);
    const Rec* nameRec = findRec(tpl, 54);
    const std::string tplName = nameRec ? str(nameRec->data) : std::string();
    const std::string name = wrappedName(g, tplName, ec.wrapAs);
    if (name.empty()) return;
    if (name == tplName && findRec(tpl, 53)) return;  // the origin's layout: the values are solved in place
    const ParamMap& pm = paramMap(name);
    // The list stops after the last parameter that is not at its default:
    // FL parameters past the list keep the decoder's defaults.
    const PluginState def = defaultPluginState(g.type);
    size_t count = pm.gl.empty() ? 0 : 1;  // the decoder needs a non-empty list
    for (size_t k = 0; k < pm.gl.size(); ++k) {
      const int gp = pm.gl[k];
      if (gp >= 0 && static_cast<size_t>(gp) < g.params.size() && static_cast<size_t>(gp) < def.params.size() &&
          g.params[static_cast<size_t>(gp)] != def.params[static_cast<size_t>(gp)])
        count = k + 1;
    }
    std::vector<Rec> rs = settingsOf(tpl);
    const PluginInfo* info = findPlugin(g.type);
    rs.push_back(kindRec(info && info->kind == PluginKind::Generator ? 4 : 0));
    rs.push_back({54, text(name)});
    ByteWriter w;
    w.i32(-9);  // VST state marker
    w.u8(5);    // state version
    w.i32(-2);  // 8 reserved bytes follow the size
    w.u32(0);   // no chunk: a parameter list
    w.zeros(8);
    w.u32(static_cast<uint32_t>(count));
    for (size_t k = 0; k < count; ++k) w.f32(0.5f);  // solved afterwards
    w.u32(1);
    w.zeros(25);
    rs.push_back({53, w.b});
    b = build(kWrapperVersion, rs);
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> v;
    const std::vector<Rec> rs = records(b);
    const Rec* nameRec = findRec(rs, 54);
    if (!nameRec) return v;
    const ParamMap& pm = paramMap(str(nameRec->data));
    const ParamList pl = paramList(b);
    // Several FL parameters may set one Gridline parameter: the last one wins.
    std::map<int, size_t> last;
    for (size_t k = 0; k < pl.count && k < pm.gl.size(); ++k)
      if (pm.gl[k] >= 0) last[pm.gl[k]] = k;
    for (const auto& [gp, k] : last) v.push_back(Field{gp, pl.at + 4 * k, Type::F32, 0, 1, {}});
    return v;
  };
  return e;
}

}  // namespace

std::vector<std::string> wrapCandidates(const PluginState& g) {
  std::vector<std::string> v;
  auto add = [&](const std::string& n) {
    if (!n.empty() && std::find(v.begin(), v.end(), n) == v.end() && wrapFits(g, n)) v.push_back(n);
  };
  if (const Encoder* e = nativeEncoder(g)) add(e->flName);
  for (const auto& [name, dec] : decoders())
    if (name.rfind("Fruity", 0) == 0) add(name);
  return v;
}

void registerWrapper(std::vector<Encoder>& v) {
  // Order matters for findEncoder: the last loose match wins, so hosted
  // types reach the hosted encoder and every other type the FL-plugin one.
  v.push_back(wrappedFl());
  v.push_back(hosted());
}

}  // namespace gl::flp::enc
