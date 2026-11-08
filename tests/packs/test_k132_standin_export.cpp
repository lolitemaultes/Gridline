// Gridline: K-132, a missing tape stop effect opened as its Gridline Suite Mini stand-in keeps its
// automation through the FL export: written, read back, written again and read back again, the clip
// still drives the same setting. Every project and state here is synthetic.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/Json.h"
#include "../../src/core/Params.h"
#include "../../src/core/PluginStandIn.h"
#include "../../src/core/Project.h"
#include "../../src/core/Song.h"
#include "../../src/core/flp/Flp.h"
#include "../../src/core/flp/FlpExport.h"
#include "../../src/host/HostedPlugins.h"
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

int hostedIndex(const khs::Product& p, const std::string& type, const char* title) {
  const std::vector<int> list = khs::hostedList(p, type, 0);
  for (size_t k = 0; k < list.size(); ++k)
    if (std::string(p.params[static_cast<size_t>(list[k])].title) == title) return static_cast<int>(k);
  return -1;
}

const AutomationClip* clipNamed(const Song& s, const char* name) {
  for (const auto& a : s.automations)
    if (a.name == name) return &a;
  return nullptr;
}

const MixerTrack* trackHolding(const Song& s, const std::string& type, int& slot) {
  for (const auto& t : s.mixer)
    for (size_t k = 0; k < t.slots.size(); ++k)
      if (t.slots[k].plugin.type == type) return slot = static_cast<int>(k), &t;
  return nullptr;
}

// FL export then FL import.
bool throughFl(const Song& in, Song& out, std::string& why) {
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions opt;
  if (!flp::exportFlp(in, bytes, rep, opt)) return why = "export failed", false;
  flp::Report r;
  std::string err;
  if (!importFlpBytes(std::vector<uint8_t>(bytes.begin(), bytes.end()), out, r, &err, "", "")) return why = "import failed: " + err, false;
  return true;
}

// The automation clip `name` drives slot `slot` setting `index` of the track holding the stand-in.
bool drives(const Song& s, const char* name, uint32_t index, std::string& why) {
  int slot = -1;
  const MixerTrack* t = trackHolding(s, "gl.minihalt", slot);
  const AutomationClip* a = clipNamed(s, name);
  if (!t) return why = "no stand-in on the mixer", false;
  if (!a) return why = "no automation clip", false;
  const uint32_t want = kSlotBase + static_cast<uint32_t>(slot) * kSlotStride + index;
  char b[160];
  std::snprintf(b, sizeof b, "owner %llu (track %llu), index %u (want %u)", static_cast<unsigned long long>(a->owner), static_cast<unsigned long long>(t->id), a->index, want);
  why = b;
  return a->owner == t->id && a->index == want && a->points.size() >= 2 && a->points.front().value < a->points.back().value;
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k132_standin_export");
  standin::setHiddenPlugins({"kilohearts"});  // whatever this machine has installed

  const khs::Product* stop = product("kHs Tape Stop");
  expect(stop != nullptr, "the tape stop row");
  if (!stop) return finish();
  for (const std::string& type : {khs::vst3Type(*stop), khs::auType(*stop), khs::vst2Type(*stop)}) {
    const std::string fmt = type.substr(0, type.find(':'));
    for (const char* title : {"Stop Time", "Start Time", "Curve", "Play"}) {
      const int hosted = hostedIndex(*stop, type, title);
      if (hosted < 0) continue;
      PluginState plug;
      plug.type = type;
      plug.params = std::vector<float>(khs::hostedList(*stop, type, 0).size(), 0.5f);
      // VST3: a synthetic saved state (the wrapper then carries its parameter list).
      if (fmt == "vst3") plug.data = host::encodeState("k132-component-state", "k132-controller-state");
      Song s;
      ensureMixer(s);
      s.mixer[1].name = "Bus";
      s.mixer[1].slots[0].plugin = plug;
      AutomationClip a;
      a.id = s.nextId++;
      a.name = "Clip";
      a.owner = s.mixer[1].id;
      a.index = kSlotBase + static_cast<uint32_t>(hosted);
      AutoPoint p0, p1;
      p0.pos = 0, p0.value = 0.2f;
      p1.pos = 384, p1.value = 0.8f;
      a.points = {p0, p1};
      s.automations.push_back(a);
      Clip c;
      c.id = s.nextId++, c.kind = ClipKind::Automation, c.source = a.id, c.track = 0, c.start = 0, c.length = 384;
      s.playlist.push_back(c);
      Song opened;
      expect(songFromJson(songToJson(s), opened), fmt + " " + title + ": opens");
      int slot = -1;
      expect(trackHolding(opened, "gl.minihalt", slot) != nullptr, fmt + " " + title + ": missing, the stand-in opens");
      const AutomationClip* oa = clipNamed(opened, "Clip");
      const uint32_t index = oa ? oa->index - kSlotBase : 0;
      std::string why;
      Song once, twice;
      const bool first = throughFl(opened, once, why) && drives(once, "Clip", index, why);
      expect(first, fmt + " " + title + ": exported and read back, the clip drives the same setting (" + why + ")");
      const bool second = first && throughFl(once, twice, why) && drives(twice, "Clip", index, why);
      expect(second, fmt + " " + title + ": exported a second time and read back, still linked (" + why + ")");
    }
  }
  standin::setHiddenPlugins({});
  return finish();
}
