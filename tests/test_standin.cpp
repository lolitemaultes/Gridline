// Gridline tests: built-in stand-ins for missing Serum / Kickstart instances
// (core/PluginStandIn.h) and Plasma's single Open path for preset files.
// Every state below is synthetic, written by this test from chosen values.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Json.h"
#include "core/Params.h"
#include "core/PluginInfo.h"
#include "core/PluginStandIn.h"
#include "core/Presets.h"
#include "core/Project.h"
#include "core/Song.h"
#include "host/Backend.h"
#include "host/HostedPlugins.h"
#include "host/PluginScan.h"
#include "packs/plasma/plasma.h"
#include "packs/tideline/tideline.h"

using namespace gl;
namespace pl = gl::packs::plasma;
namespace tl = gl::packs::tideline;
namespace fs = std::filesystem;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

const char* const kSerum2 = "vst3:56534558667350736572756D20320000";
const char* const kSerum1 = "vst2:58667358";
const char* const kKick2 = "vst3:ABCDEF019182FAEB43614E52434E4B32";

// ---- containers -----------------------------------------------------------
struct Cbor {
  std::vector<uint8_t> b;
  void head(int mt, uint64_t v) {
    if (v < 24) b.push_back(static_cast<uint8_t>(mt << 5 | v));
    else if (v < 256) b.push_back(static_cast<uint8_t>(mt << 5 | 24)), b.push_back(static_cast<uint8_t>(v));
    else if (v < 65536) b.push_back(static_cast<uint8_t>(mt << 5 | 25)), b.push_back(static_cast<uint8_t>(v >> 8)), b.push_back(static_cast<uint8_t>(v));
    else {
      b.push_back(static_cast<uint8_t>(mt << 5 | 26));
      for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
    }
  }
  void value(const Json& j) {
    switch (j.type) {
      case Json::Type::Object:
        head(5, j.object.size());
        for (const auto& kv : j.object) {
          head(3, kv.first.size());
          b.insert(b.end(), kv.first.begin(), kv.first.end());
          value(kv.second);
        }
        break;
      case Json::Type::Array:
        head(4, j.array.size());
        for (const auto& v : j.array) value(v);
        break;
      case Json::Type::String:
        head(3, j.str.size());
        b.insert(b.end(), j.str.begin(), j.str.end());
        break;
      case Json::Type::Number: {
        const double d = j.number;
        if (d == std::floor(d) && std::fabs(d) < 1e9) {
          if (d >= 0) head(0, static_cast<uint64_t>(d));
          else head(1, static_cast<uint64_t>(-d - 1));
          break;
        }
        b.push_back(0xFB);
        uint64_t u;
        std::memcpy(&u, &d, 8);
        for (int s = 56; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(u >> s));
        break;
      }
      default: b.push_back(0xF6); break;
    }
  }
};

std::vector<uint8_t> zstdRaw(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> f = {0x28, 0xB5, 0x2F, 0xFD, 0xA0};
  const uint32_t n = static_cast<uint32_t>(data.size());
  for (int s = 0; s < 32; s += 8) f.push_back(static_cast<uint8_t>(n >> s));
  const uint32_t hdr = (n << 3) | 1u;  // one raw block, last
  f.push_back(static_cast<uint8_t>(hdr));
  f.push_back(static_cast<uint8_t>(hdr >> 8));
  f.push_back(static_cast<uint8_t>(hdr >> 16));
  f.insert(f.end(), data.begin(), data.end());
  return f;
}

// "XferJson" container (.SerumPreset files and Serum 2 plugin states).
std::vector<uint8_t> xferJson(const Json& body, const std::string& name) {
  Cbor c;
  c.value(body);
  const std::string meta = "{\"fileType\":\"SerumPreset\",\"presetName\":\"" + name + "\",\"presetAuthor\":\"Test\",\"product\":\"Serum2\",\"version\":1}";
  std::vector<uint8_t> out = {'X', 'f', 'e', 'r', 'J', 's', 'o', 'n', 0};
  const uint64_t ml = meta.size();
  for (int s = 0; s < 64; s += 8) out.push_back(static_cast<uint8_t>(ml >> s));
  out.insert(out.end(), meta.begin(), meta.end());
  const uint32_t un = static_cast<uint32_t>(c.b.size());
  for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(un >> s));
  for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(2u >> s));
  auto z = zstdRaw(c.b);
  out.insert(out.end(), z.begin(), z.end());
  return out;
}

// zlib stream of stored deflate blocks.
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t len = std::min<size_t>(data.size() - pos, 65535);
    z.push_back(pos + len >= data.size() ? 1 : 0);
    z.push_back(static_cast<uint8_t>(len));
    z.push_back(static_cast<uint8_t>(len >> 8));
    z.push_back(static_cast<uint8_t>(~len));
    z.push_back(static_cast<uint8_t>(~len >> 8));
    z.insert(z.end(), data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + len));
    pos += len;
  } while (pos < data.size());
  uint32_t a = 1, b = 0;
  for (uint8_t v : data) a = (a + v) % 65521, b = (b + a) % 65521;
  const uint32_t ad = b << 16 | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back(static_cast<uint8_t>(ad >> s));
  return z;
}

Json obj(std::initializer_list<std::pair<const char*, Json>> kv) {
  Json j = Json::makeObject();
  for (const auto& p : kv) j.object[p.first] = p.second;
  return j;
}
Json num(double d) { return Json::makeNumber(d); }
Json str(const char* s) { return Json::makeString(s); }

// A Serum 2 document: osc A at level 0.36 (plain), filter 1 low-pass at 0.4.
Json serum2Body() {
  Json osc0 = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamVolume", num(0.36)}, {"kParamUnison", num(3)}})}});
  Json filt = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamType", str("L24")}, {"kParamFreq", num(0.4)}})}});
  return obj({{"Oscillator0", osc0}, {"VoiceFilter0", filt}});
}

// Serum 1 chunk: parameters at 0x3460 (0..227) and 0x4AE0 (228..298), name at 0x4972.
std::vector<uint8_t> serum1Fxp(const std::vector<std::pair<int, float>>& values, const std::string& name) {
  std::vector<uint8_t> st(20704, 0);
  auto put = [&](int i, float v) {
    const size_t at = i < 228 ? 0x3460 + 4 * static_cast<size_t>(i) : 0x4AE0 + 4 * static_cast<size_t>(i - 228);
    std::memcpy(st.data() + at, &v, 4);
  };
  for (int i = 0; i < 299; ++i) put(i, 0.0f);
  for (const auto& [i, v] : values) put(i, v);
  for (int i = 0; i < 10; ++i) {  // effect rack order: the default (Hyper first)
    const uint32_t p = i == 9 ? 0 : static_cast<uint32_t>(i + 1);
    std::memcpy(st.data() + 0x3BE0 + 4 * static_cast<size_t>(i), &p, 4);
  }
  std::memcpy(st.data() + 0x4972, name.data(), std::min<size_t>(name.size(), 31));
  const auto chunk = zlibStored(st);
  std::vector<uint8_t> f = {'C', 'c', 'n', 'K', 0, 0, 0, 0, 'F', 'P', 'C', 'h', 0, 0, 0, 1, 'X', 'f', 's', 'X', 0, 0, 0, 1, 0, 0, 0, 1};
  f.resize(0x38, 0);
  std::memcpy(f.data() + 0x1C, name.data(), std::min<size_t>(name.size(), 27));
  const uint32_t n = static_cast<uint32_t>(chunk.size());
  for (int s = 24; s >= 0; s -= 8) f.push_back(static_cast<uint8_t>(n >> s));
  f.insert(f.end(), chunk.begin(), chunk.end());
  const uint32_t total = static_cast<uint32_t>(f.size() - 8);
  for (int k = 0; k < 4; ++k) f[4 + static_cast<size_t>(k)] = static_cast<uint8_t>(total >> (24 - 8 * k));
  return f;
}

// JUCE value tree writer (Kickstart 2 states).
struct Vt {
  std::vector<uint8_t> b;
  void cint(int64_t v) {
    uint64_t u = static_cast<uint64_t>(v < 0 ? -v : v);
    uint8_t bytes[8];
    int n = 0;
    while (u) bytes[n++] = static_cast<uint8_t>(u), u >>= 8;
    b.push_back(static_cast<uint8_t>(n | (v < 0 ? 0x80 : 0)));
    b.insert(b.end(), bytes, bytes + n);
  }
  void string(const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
  }
  void varInt(int v) {
    cint(5);
    b.push_back(1);
    uint8_t x[4];
    std::memcpy(x, &v, 4);
    b.insert(b.end(), x, x + 4);
  }
  void varDouble(double v) {
    cint(9);
    b.push_back(4);
    uint8_t x[8];
    std::memcpy(x, &v, 8);
    b.insert(b.end(), x, x + 8);
  }
  void varBool(bool v) {
    cint(1);
    b.push_back(v ? 2 : 3);
  }
  void varString(const std::string& s) {
    cint(static_cast<int64_t>(s.size()) + 2);
    b.push_back(5);
    string(s);
  }
  // {type, props {id, value}}, no children
  void leafInt(const char* id, int v) {
    string("int"), cint(2), string("id"), varString(id), string("value"), varInt(v), cint(0);
  }
  void leafFloat(const char* id, double v) {
    string("float"), cint(2), string("id"), varString(id), string("value"), varDouble(v), cint(0);
  }
  void leafBool(const char* id, bool v) {
    string("bool"), cint(2), string("id"), varString(id), string("value"), varBool(v), cint(0);
  }
};

// Kickstart 2 state: loop 1/2, mix 0.6, band on, curve 0 a straight duck.
std::string kick2State() {
  Vt t;
  t.string("PluginState"), t.cint(1), t.string("version"), t.varString("4");
  t.cint(6);
  t.leafInt("loopLength", 2);
  t.leafInt("usedWave", 0);
  t.leafFloat("mix", 0.6);
  t.leafBool("bandOn", true);
  t.leafFloat("bandFrequency", 0.25);
  // waves: one curve, points (x, y)
  t.string("enumeration"), t.cint(1), t.string("id"), t.varString("waves"), t.cint(1);
  t.string("IndexedWaveStateModel"), t.cint(0), t.cint(1);
  t.string("enumeration"), t.cint(1), t.string("id"), t.varString("points");
  const int xs[3] = {0, 4096, 8191};
  const double ys[3] = {-1.0, 1.0, 1.0};
  t.cint(3);
  for (int i = 0; i < 3; ++i) {
    t.string("Point"), t.cint(0), t.cint(2);
    t.leafInt("x", xs[i]);
    t.leafFloat("y", ys[i]);
  }
  const auto z = zlibStored(t.b);
  std::string comp = std::string("#zip#", 5) + std::string(1, '\0');
  comp.append(reinterpret_cast<const char*>(z.data()), z.size());
  return host::encodeState(comp, "");
}

std::string serum2State() {
  const auto x = xferJson(serum2Body(), "Hosted Lead");
  return host::encodeState(std::string(x.begin(), x.end()), "");
}

int hostedIndex(const char* title) {
  for (int i = 0; i < pl::serumParamCount(kSerum2); ++i)
    if (pl::serumParamName(kSerum2, i) == title) return i;
  return -1;
}

// A song with one hosted channel, an automation clip and a controller link on its parameters.
Song hostedSong(const PluginState& plug, uint32_t autoIndex, uint32_t linkIndex) {
  Song s;
  ensureMixer(s);
  Channel c;
  c.id = s.nextId++;
  c.name = "Hosted";
  c.plugin = plug;
  s.channels.push_back(c);
  AutomationClip a;
  a.id = s.nextId++;
  a.name = "Cutoff";
  a.owner = c.id;
  a.index = kChannelPluginBase + autoIndex;
  AutoPoint p0, p1;
  p0.pos = 0, p0.value = 0.1f;
  p1.pos = 384, p1.value = 0.9f;
  a.points = {p0, p1};
  s.automations.push_back(a);
  ControllerLink l;
  l.cc = 74;
  l.owner = c.id;
  l.index = kChannelPluginBase + linkIndex;
  l.min = 0.2f, l.max = 0.8f;
  s.links.push_back(l);
  return s;
}

void registerFake(const char* type, const char* name, const char* vendor, bool instrument) {
  host::ScanResult r;
  const std::string t = type;
  r.format = t.substr(0, t.find(':'));
  r.bundle = std::string("/fake/") + name;
  r.ok = true;
  host::ScannedClass c;
  c.uid = t.substr(t.find(':') + 1);
  c.name = name;
  c.vendor = vendor;
  c.instrument = instrument;
  for (int i = 0; i < pl::serumParamCount(type); ++i) {  // the plugin's parameters (automation targets)
    host::ScannedParam p;
    p.id = static_cast<uint32_t>(i);
    p.title = pl::serumParamName(type, i);
    c.params.push_back(p);
  }
  r.classes.push_back(c);
  host::registerScanResult(r);
}

std::string writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream f(fs::u8path(path), std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return path;
}

}  // namespace

int main() {
  const std::string tmp = gl::test::hermeticTestEnv("standin");
  standin::setHiddenPlugins({"serum", "kickstart"});  // whatever this machine has installed
  pl::setExtraSerumFolders({});

  // ---- families and the hide hook
  expect(standin::familyOf(kSerum2) == standin::Family::Serum2 && standin::familyOf(kSerum1) == standin::Family::Serum1 &&
             standin::familyOf(kKick2) == standin::Family::Kickstart2,
         "families by plugin id");
  expect(standin::familyOf("vst3:0000", "Serum", "Xfer Records") == standin::Family::Serum1 &&
             standin::familyOf("vst3:0000", "Serum", "Someone Else") == standin::Family::None &&
             standin::familyOf("vst3:0000", "Serum 2 FX", "Xfer Records") == standin::Family::None,
         "families by name need the right vendor; the effect is not a synth");
  expect(standin::hidden(kSerum2) && standin::hidden(kKick2) && !standin::hidden("vst3:1234"), "hidden plugins (test hook)");

  // ---- Serum 2 project opens as Plasma
  const int iFreq = hostedIndex("Filter 1 Freq"), iLevel = hostedIndex("A Level");
  expect(iFreq >= 0 && iLevel >= 0, "Serum 2 parameter list");
  PluginState s2;
  s2.type = kSerum2;
  s2.data = serum2State();
  const Song orig = hostedSong(s2, static_cast<uint32_t>(iFreq), static_cast<uint32_t>(iLevel));
  const Json origJson = songToJson(orig);
  Song opened;
  expect(songFromJson(origJson, opened), "project with Serum 2 opens");
  const PluginState& st = opened.channels[0].plugin;
  expect(st.type == pl::kType && standin::isStandIn(st), "Serum 2 (not installed) opens as Plasma");
  {
    const float lvl = st.params[static_cast<size_t>(pl::oscP(0, pl::O_Level))];
    const float fon = st.params[static_cast<size_t>(pl::filtP(0, pl::F_Enable))];
    expect(std::fabs(lvl - 0.36f) < 0.01f && fon > 0.5f, "Serum 2 state mapped onto Plasma's parameters");
    const PluginState o = standin::originalState(st);
    expect(o.type == kSerum2 && o.data == s2.data, "the original plugin and its state are kept, untouched");
    expect(standin::describe(st).find("Serum 2 is not installed") != std::string::npos, "stand-in description");
  }
  {
    const auto& a = opened.automations[0];
    expect(a.index == kChannelPluginBase + static_cast<uint32_t>(pl::filtP(0, pl::F_Freq)), "automation follows to Plasma's cutoff");
    expect(a.points.size() == 2 && a.points[0].value < a.points[1].value, "automation values mapped (rising stays rising)");
    expect(!opened.links.empty() && opened.links[0].index == kChannelPluginBase + static_cast<uint32_t>(pl::oscP(0, pl::O_Level)),
           "controller link follows to Plasma's level");
  }
  // saved and reopened while still missing: unchanged
  const Json savedJson = songToJson(opened);
  Song again;
  expect(songFromJson(savedJson, again) && again.channels[0].plugin == opened.channels[0].plugin, "stand-in project saves and reopens");
  // ---- installed again: the original comes back, byte for byte
  registerFake(kSerum2, "Serum 2", "Xfer Records", true);
  standin::setHiddenPlugins({"kickstart"});
  Song restored;
  expect(songFromJson(savedJson, restored), "project reopens with the plugin installed");
  {
    const PluginState& p = restored.channels[0].plugin;
    expect(p.type == kSerum2 && p.data == s2.data && p.original.empty(), "installed: the original plugin is hosted with its untouched state");
    const Json a = songToJson(restored), b = origJson;
    expect(jsonCompact(*a.find("automations")) == jsonCompact(*b.find("automations")) && jsonCompact(*a.find("links")) == jsonCompact(*b.find("links")),
           "installed: automation and links are exactly as before");
  }
  Song direct;
  expect(songFromJson(origJson, direct) && direct.channels[0].plugin.type == kSerum2, "installed: no substitution");
  // hidden again (the hook): the stand-in returns
  standin::setHiddenPlugins({"serum", "kickstart"});
  Song hid;
  expect(songFromJson(origJson, hid) && hid.channels[0].plugin.type == pl::kType, "hidden installed plugin: stand-in");
  // FL export: every stand-in back to its plugin
  {
    Song work = hid;
    standin::restoreAll(work);
    expect(work.channels[0].plugin.type == kSerum2 && work.channels[0].plugin.data == s2.data &&
               work.automations[0].index == kChannelPluginBase + static_cast<uint32_t>(iFreq),
           "restoreAll (FL export): the original plugin and targets");
  }
  // "Replace with Plasma": the stand-in stays, the original is dropped
  {
    Song kept = hid;
    kept.channels[0].plugin.original.clear();
    standin::setHiddenPlugins({"kickstart"});
    Song re;
    expect(songFromJson(songToJson(kept), re) && re.channels[0].plugin.type == pl::kType && !standin::isStandIn(re.channels[0].plugin),
           "kept stand-in stays Plasma once the plugin is installed");
    standin::setHiddenPlugins({"serum", "kickstart"});
  }

  // ---- Serum 1 into Plasma
  {
    const auto fxp = serum1Fxp({{1, 0.6f}, {45, 0.3f}, {216, 1.0f}, {212, 1.0f}}, "Synthetic S1");
    PluginState s1;
    s1.type = kSerum1;
    s1.data = "vst2c1:" + host::base64Encode(std::string(fxp.begin() + 0x3C, fxp.end()));
    PluginState out;
    std::vector<std::string> notes;
    expect(standin::makeStandIn(s1, "", "", out, &notes) && out.type == pl::kType, "Serum 1 state opens as Plasma");
    const float lvl = out.params[static_cast<size_t>(pl::oscP(0, pl::O_Level))];
    expect(std::fabs(lvl - 0.36f) < 0.01f, "Serum 1 osc A level mapped (0.6 normalized)");
    expect(out.params[static_cast<size_t>(pl::filtP(0, pl::F_Enable))] > 0.5f, "Serum 1 filter switch mapped");
    standin::ParamLink l;
    expect(standin::paramLink(standin::Family::Serum1, s1, 45, l) && l.standin == pl::filtP(0, pl::F_Freq), "Serum 1 cutoff links to Plasma's cutoff");
  }

  // ---- Kickstart 2 into Tideline
  {
    PluginState k2;
    k2.type = kKick2;
    k2.data = kick2State();
    PluginState out;
    expect(standin::makeStandIn(k2, "", "", out) && out.type == "gl.tideline", "Kickstart 2 opens as Tideline");
    auto P = [&](int k) { return out.params[static_cast<size_t>(k)]; };
    expect(std::lround(P(tl::BwRate)) == 3 && std::fabs(P(tl::BwAmount) - 0.6f) < 1e-4f, "loop length and mix mapped");
    expect(std::lround(P(tl::BwShape)) == tl::kShapeCustom && std::lround(P(tl::BwBand)) == tl::BandLows, "curve and band mapped");
    expect(P(tl::BwNodeY0) < 0.05f && P(tl::BwNodeY0 + tl::kNodes - 1) > 0.95f, "curve points redrawn as Tideline's nodes");
    standin::ParamLink l;
    expect(standin::paramLink(standin::Family::Kickstart2, k2, 4, l) && l.standin == tl::BwAmount && std::fabs(l.map(0.5f) - 0.5f) < 1e-4f,
           "Kickstart 2 mix automation links to Amount");
    Song ks = hostedSong(k2, 4, 0);
    const Json kj = songToJson(ks);
    Song ko;
    expect(songFromJson(kj, ko) && ko.channels[0].plugin.type == "gl.tideline" && ko.automations[0].index == kChannelPluginBase + tl::BwAmount,
           "Kickstart 2 project: Tideline with its automation");
    expect(standin::originalState(ko.channels[0].plugin).data == k2.data, "Kickstart 2 state kept untouched");
  }

  // ---- one Open path: Plasma presets, .SerumPreset, Serum 1 .fxp
  {
    const std::string dir = (fs::u8path(tmp) / "presets").u8string();
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    PluginState plasma = defaultPluginState(pl::kType);
    plasma.params[static_cast<size_t>(pl::oscP(0, pl::O_Level))] = 0.42f;
    savePreset(dir, plasma, "Mine", "Plasma");
    const std::string mine = (fs::u8path(dir) / pl::kType / "Mine.json").u8string();
    const std::string lib = (fs::u8path(tmp) / "Serum Library").u8string();
    fs::create_directories(fs::u8path(lib) / "Bank A", ec);
    const auto s2b = xferJson(serum2Body(), "File Lead");
    const std::string sp = writeFile((fs::u8path(lib) / "Bank A" / "Lead.SerumPreset").u8string(), s2b);
    const std::string fx = writeFile((fs::u8path(lib) / "Old.fxp").u8string(), serum1Fxp({{1, 0.6f}}, "Old"));
    for (const std::string& f : {mine, sp, fx}) {
      PluginState out;
      pl::ImportReport rep;
      const bool ok = pl::openPresetFile(f, out, rep);
      expect(ok && out.type == pl::kType && pl::isOpenableName(f), "Open path loads " + fs::u8path(f).filename().u8string() + " (" + rep.format + ")");
      PluginState viaLibrary;
      expect(loadPreset(f, pl::kType, viaLibrary) && viaLibrary.params == out.params, "host preset load opens " + fs::u8path(f).filename().u8string());
    }
    PluginState o1;
    pl::ImportReport r1;
    pl::openPresetFile(mine, o1, r1);
    expect(std::fabs(o1.params[static_cast<size_t>(pl::oscP(0, pl::O_Level))] - 0.42f) < 1e-6f, "Plasma preset opens exactly");
    expect(foreignPresetType(sp) == pl::kType && foreignPresetType(fx) == pl::kType && foreignPresetType(mine).empty(), "Serum files belong to Plasma");
    PluginState bad;
    pl::ImportReport rb;
    const std::string junk = writeFile((fs::u8path(tmp) / "junk.fxp").u8string(), {'C', 'c', 'n', 'K', 1, 2});
    expect(!pl::openPresetFile(junk, bad, rb) && !rb.error.empty(), "damaged file: a message, no crash");
    // the library index finds both files below a root
    const auto index = pl::scanLibrary({lib});
    int serum = 0;
    for (const auto& e : index) serum += (e.file == sp && e.bank == "Bank A" && e.serum2) || (e.file == fx && e.bank.empty() && !e.serum2);
    expect(serum == 2, "library index lists .SerumPreset and .fxp files by bank");
  }
  return finish();
}
