// Gridline tests: Gridline Suite Mini as the stand-in for a missing Kilohearts
// effect (packs/mini/mini_khs.h, core/PluginStandIn.h). Every project and state
// below is synthetic, written by this test from chosen values: no plugin state,
// preset or project of anyone else.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include "../../src/core/Json.h"
#include "../../src/core/Params.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginStandIn.h"
#include "../../src/core/Project.h"
#include "../../src/core/Song.h"
#include "../../src/host/HostedPlugins.h"
#include "../../src/host/PluginScan.h"
#include "../../src/packs/mini/mini.h"
#include "../../src/packs/mini/mini_khs.h"
#include "../Check.h"
#include "../Hermetic.h"

using namespace gl;
namespace mini = gl::packs::mini;
namespace khs = gl::packs::mini::khs;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

const khs::Product* product(const char* name) {
  for (const auto& p : khs::products())
    if (std::string(p.name) == name) return &p;
  return nullptr;
}

// The hosted parameter list of a product in a format, as a scanner would report it.
void registerFake(const khs::Product& p, const std::string& type) {
  host::ScanResult r;
  r.format = type.substr(0, type.find(':'));
  r.bundle = std::string("/fake/") + p.name;
  r.ok = true;
  host::ScannedClass c;
  c.uid = type.substr(type.find(':') + 1);
  c.name = p.name;
  c.vendor = "Kilohearts";
  for (int i : khs::hostedList(p, type, 0)) {
    host::ScannedParam q;
    q.id = static_cast<uint32_t>(r.format == "vst3" ? p.params[static_cast<size_t>(i)].vst3Id : i);
    q.title = p.params[static_cast<size_t>(i)].title;
    c.params.push_back(q);
  }
  r.classes.push_back(c);
  host::registerScanResult(r);
}

// Saved parameters of a product in a format: defaults 0.5, then the given titles.
std::vector<float> savedParams(const khs::Product& p, const std::string& type, std::vector<std::pair<const char*, float>> values) {
  const std::vector<int> list = khs::hostedList(p, type, 0);
  std::vector<float> out(list.size(), 0.5f);
  for (size_t k = 0; k < list.size(); ++k) {
    const khs::Param& q = p.params[static_cast<size_t>(list[k])];
    if (std::string(q.title) == "Enabled") out[k] = 1.0f;
    for (const auto& [t, v] : values)
      if (std::string(q.title) == t) out[k] = v;
  }
  return out;
}

int hostedIndex(const khs::Product& p, const std::string& type, const char* title) {
  const std::vector<int> list = khs::hostedList(p, type, 0);
  for (size_t k = 0; k < list.size(); ++k)
    if (std::string(p.params[static_cast<size_t>(list[k])].title) == title) return static_cast<int>(k);
  return -1;
}

// A song whose master track holds `plug` in slot 0, with an automation clip on hosted parameter `autoIndex`.
Song slotSong(const PluginState& plug, int autoIndex) {
  Song s;
  ensureMixer(s);
  MixerTrack& m = s.mixer[1];
  m.name = "Bus";
  m.slots[0].plugin = plug;
  AutomationClip a;
  a.id = s.nextId++;
  a.name = "Clip";
  a.owner = m.id;
  a.index = kSlotBase + static_cast<uint32_t>(autoIndex);
  AutoPoint p0, p1;
  p0.pos = 0, p0.value = 0.2f;
  p1.pos = 384, p1.value = 0.8f;
  a.points = {p0, p1};
  s.automations.push_back(a);
  return s;
}

// A synthetic opaque state: every byte value, so any re-encoding would show.
std::string opaqueState() {
  std::string s = "khs-test-state:";
  for (int i = 0; i < 256; ++i) s += static_cast<char>(i);
  return host::base64Encode(s);
}

float miniParam(const PluginState& s, int k) { return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : -999.f; }

}  // namespace

int main() {
  gl::test::hermeticTestEnv("mini_khs");
  standin::setHiddenPlugins({});

  // ---- 1. the table: every entry resolves to a real Mini plugin
  {
    std::set<std::string> ids, minis;
    bool resolves = true, maps = true, family = true, unique = true;
    for (const auto& p : khs::products()) {
      const PluginInfo* info = findPlugin(p.mini);
      resolves = resolves && info && mini::findEntry(p.mini) && info->category == mini::kCategory;
      if (!info) {
        std::printf("# %s: no Mini plugin %s\n", p.name, p.mini);
        continue;
      }
      for (const auto& m : p.maps) {
        const bool titled = std::any_of(p.params.begin(), p.params.end(), [&](const khs::Param& q) { return std::string(q.title) == m.title; });
        bool ok = titled && m.mini >= 0 && static_cast<size_t>(m.mini) < info->params.size();
        if (ok && m.read == khs::Read::Index) {
          for (const auto& q : p.params)
            if (std::string(q.title) == m.title && q.vst3Steps > 1) ok = ok && m.v.size() == static_cast<size_t>(q.vst3Steps) + 1;
        }
        if (ok && m.read == khs::Read::Beats) ok = m.beats && m.beatCount == static_cast<int>(info->params[static_cast<size_t>(m.mini)].max) + 1;
        if (!ok) std::printf("# %s: map %s does not resolve\n", p.name, m.title);
        maps = maps && ok;
      }
      for (const std::string& t : {khs::auType(p), khs::vst3Type(p), khs::vst2Type(p)}) {
        unique = unique && ids.insert(t).second;
        family = family && standin::familyOf(t) == standin::Family::Kilohearts && standin::standInTypeOf(t) == p.mini &&
                 standin::pluginNameOf(t) == p.name;
      }
      family = family && standin::familyOf("clap:org.example.other", p.name, "Kilohearts") == standin::Family::Kilohearts &&
               standin::familyOf("clap:org.example.other", p.name, "Someone Else") == standin::Family::None;
      minis.insert(p.mini);
    }
    std::printf("# %zu Kilohearts plugins -> %zu Mini plugins\n", khs::products().size(), minis.size());
    expect(khs::products().size() == 35 && minis.size() == 35, "35 Kilohearts effects, each with its own Mini counterpart");
    expect(resolves, "every table entry names a real Gridline Suite Mini plugin");
    expect(maps, "every parameter mapping names a parameter of both plugins (choice lists complete)");
    expect(unique, "AU, VST3 and VST2 identifiers are unique");
    expect(family, "identifiers and names (vendor Kilohearts) resolve to the Kilohearts family and the right Mini type");
    expect(standin::familyOf("vst3:00000000000000000000000000000000") == standin::Family::None, "other plugins are no Kilohearts plugin");
  }

  const khs::Product* comp = product("kHs Compressor");
  const khs::Product* filt = product("kHs Filter");
  const khs::Product* dly = product("kHs Delay");
  const khs::Product* gate = product("kHs Gate");
  expect(comp && filt && dly && gate, "table rows");
  if (!comp || !filt || !dly || !gate) return finish();

  // ---- 2. parameter mapping
  const std::string compVst3 = khs::vst3Type(*comp);
  PluginState c3;
  c3.type = compVst3;
  // VST3: Mode is a choice saved as its index (0 = RMS); Threshold 0.5 = -17 dB; Attack 0.5 = 22.4 ms; Ratio 0.2 = 2:1.
  c3.params = savedParams(*comp, compVst3, {{"Mode", 0.f}, {"Threshold", 0.5f}, {"Attack", 0.5f}, {"Ratio", 0.2f}, {"Release", 0.f}});
  c3.data = opaqueState();
  {
    PluginState st;
    std::vector<std::string> notes;
    expect(standin::makeStandIn(c3, "", "", st, &notes) && st.type == "gl.minipress", "Compressor (VST3) -> Press");
    expect(std::fabs(miniParam(st, mini::press::Threshold) + 17.f) < 0.01f, "Threshold carried over in dB");
    expect(std::fabs(miniParam(st, mini::press::Attack) - 22.36f) < 0.05f && std::fabs(miniParam(st, mini::press::Release) - 5.f) < 1e-3f,
           "Attack and Release carried over in ms (clamped to the Mini range)");
    expect(std::fabs(miniParam(st, mini::press::Ratio) - 2.f) < 1e-3f, "Ratio carried over");
    expect(miniParam(st, mini::press::Detect) == 1.f, "Mode RMS (a VST3 choice index) -> Detect RMS");
    const PluginState def = defaultPluginState("gl.minipress");
    expect(miniParam(st, mini::press::Makeup) == miniParam(def, mini::press::Makeup) && miniParam(st, mini::press::Knee) == miniParam(def, mini::press::Knee),
           "settings without a counterpart stay at the Mini defaults");
    expect(!notes.empty(), "the stand-in lists what it only approximates");
    expect(standin::describe(st) == "Not installed: kHs Compressor, using Gridline Suite Mini Press", "notice text");
  }
  {
    // AU / VST2 lists keep "Enabled"; choices are normalized there (Type 0.3 = High pass).
    const std::string au = khs::auType(*filt);
    PluginState f;
    f.type = au;
    f.params = savedParams(*filt, au, {{"Type", 0.3f}, {"Cutoff", 0.5f}, {"Gain", 1.f}, {"Slope", 0.2f}});
    PluginState st;
    expect(standin::makeStandIn(f, "", "", st) && st.type == "gl.minisieve", "Filter (AU) -> Sieve");
    expect(miniParam(st, mini::sieve::Type) == 1.f && std::fabs(miniParam(st, mini::sieve::Cutoff) - 640.f) < 1.f &&
               std::fabs(miniParam(st, mini::sieve::Gain) - 12.f) < 1e-3f && miniParam(st, mini::sieve::Slope) == 1.f,
           "Filter type, cutoff, gain and slope carried over (AU order, normalized choices)");
    const std::string v2 = khs::vst2Type(*gate);
    PluginState g;
    g.type = v2;
    g.params = savedParams(*gate, v2, {{"Range", 1.f}, {"Threshold", 0.f}, {"Flip", 1.f}});
    expect(standin::makeStandIn(g, "", "", st) && st.type == "gl.minilatch" && miniParam(st, mini::latch::Floor) == -80.f &&
               std::fabs(miniParam(st, mini::latch::Threshold) + 60.f) < 1e-3f && miniParam(st, mini::latch::Flip) == 1.f,
           "Gate (VST2): range, threshold and flip carried over");
    // Synced delay: 3 steps of a sixteenth = a dotted eighth.
    const std::string dv3 = khs::vst3Type(*dly);
    PluginState d;
    d.type = dv3;
    d.params = savedParams(*dly, dv3, {{"Delay Time (steps)", 2.f / 63.f}, {"Delay Denominator", 4.f}, {"Sync Mode", 1.f}, {"Bounce", 1.f}});
    expect(standin::makeStandIn(d, "", "", st) && st.type == "gl.miniecho" && miniParam(st, mini::echo::Sync) == 1.f &&
               mini::kNoteBeats[static_cast<int>(miniParam(st, mini::echo::Steps))] == 0.75 && miniParam(st, mini::echo::PingPong) == 1.f,
           "Delay: steps x denominator -> the nearest synced note value, bounce -> ping pong");
    // Name only (an FL project names the plugin): defaults, no failure.
    PluginState n;
    n.type = "vst3:0123456789ABCDEF0123456789ABCDEF";
    expect(standin::makeStandIn(n, "kHs Compressor", "Kilohearts", st) && st.type == "gl.minipress", "by name: the Mini counterpart with its defaults");
  }

  // ---- 3. substitution only when missing
  const int iThr = hostedIndex(*comp, compVst3, "Threshold");
  const Song orig = slotSong(c3, iThr);
  const Json origJson = songToJson(orig);
  Song opened;
  expect(songFromJson(origJson, opened), "project with a missing Compressor opens");
  const PluginState& st = opened.mixer[1].slots[0].plugin;
  expect(st.type == "gl.minipress" && standin::isStandIn(st), "missing: Press stands in");
  expect(opened.automations[0].index == kSlotBase + static_cast<uint32_t>(mini::press::Threshold) && opened.automations[0].points.size() == 2 &&
             opened.automations[0].points[0].value < opened.automations[0].points[1].value,
         "automation follows to Press's threshold (rising stays rising)");
  {
    const auto lines = standin::summary(opened);
    expect(lines.size() == 1 && lines[0] == "Not installed: kHs Compressor (Bus), using Gridline Suite Mini Press", "load notice names the plugin, the track and the stand-in");
  }
  // ---- 4. the original state round-trips byte for byte through save / load with the stand-in in use
  {
    const std::string text = jsonStringify(songToJson(opened), 1);
    Json j;
    Song again;
    expect(jsonParse(text, j) && songFromJson(j, again) && again.mixer[1].slots[0].plugin == st, "saved with the stand-in and reopened: unchanged");
    const PluginState o = standin::originalState(again.mixer[1].slots[0].plugin);
    expect(o.type == c3.type && o.data == c3.data && o.params == c3.params, "the original plugin, its parameters and its state are kept byte for byte");
    // a second save of the reopened project
    Json j2;
    Song third;
    expect(jsonParse(jsonStringify(songToJson(again), 1), j2) && songFromJson(j2, third) && standin::originalState(third.mixer[1].slots[0].plugin).data == c3.data,
           "saved twice: the original state is still identical");
    // installed now: the original comes back exactly
    registerFake(*comp, compVst3);
    Song restored;
    expect(songFromJson(j2, restored), "project reopens with the plugin installed");
    const PluginState& r = restored.mixer[1].slots[0].plugin;
    expect(r.type == c3.type && r.data == c3.data && r.params == c3.params && r.original.empty(), "installed: the original plugin is hosted with its untouched state");
    expect(jsonCompact(*songToJson(restored).find("automations")) == jsonCompact(*origJson.find("automations")), "installed: automation exactly as before");
  }
  {
    Song direct;
    expect(songFromJson(origJson, direct) && direct.mixer[1].slots[0].plugin.type == compVst3 && !standin::isStandIn(direct.mixer[1].slots[0].plugin),
           "installed: hosted as itself, no substitution");
    standin::setHiddenPlugins({"kilohearts"});
    Song hid;
    expect(songFromJson(origJson, hid) && hid.mixer[1].slots[0].plugin.type == "gl.minipress", "hidden (test hook, word \"kilohearts\"): stand-in");
    standin::setHiddenPlugins({"serum", "kickstart"});
    Song other;
    expect(songFromJson(origJson, other) && other.mixer[1].slots[0].plugin.type == compVst3, "other family words leave Kilohearts plugins alone");
    standin::setHiddenPlugins({});
  }
  return finish();
}
