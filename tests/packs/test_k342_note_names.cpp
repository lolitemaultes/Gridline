// Gridline - K-342: root, key and note controls of the built-in plugins show note names (60 = C4,
// as everywhere in Gridline), never a MIDI number.
//   1. Grain Scanner's Root shows C4 at its default, as Granular's Base note does;
//   2. the controls whose -1 means something else show that word (Any, Auto, Off) and type back;
//   3. audit: every parameter of every built-in plugin named like a root, key or note control,
//      on a MIDI note range, has a note unit (the list of the ones that are not keys is below).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <tuple>
#include "../../src/core/ParamText.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/models/metal.h"
#include "../Check.h"

using namespace gl;

namespace {

void ex(bool ok, const std::string& name) { expect(ok, name.c_str()); }

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

const ParamDesc* param(const char* type, int index) {
  const PluginInfo* p = findPlugin(type);
  return p && index >= 0 && static_cast<size_t>(index) < p->params.size() ? &p->params[static_cast<size_t>(index)] : nullptr;
}

// Named like a root / key / note control, but not a key: amounts that follow the key, velocities,
// lengths, modes, toggles of note behaviour.
bool notAKey(const std::string& n) {
  static const char* words[] = {"track", "follow", "velocity", "length", "pan", "cutoff", "pass", "scaling", "mode", "per key", "hold", "fold",
                                "off level", "> offset", "one voice", "trigger", "held", "random", "chance", "glide", "count", "spread", "gate",
                                "law", "amount", "depth", "time", "rate", "range", "octave", "semitone", "detune", "fine", "transpose", "shift"};
  for (const char* w : words)
    if (has(n, w)) return true;
  return false;
}

}  // namespace

int main() {
  // 1.
  const ParamDesc* root = param("gl.grainscan", packs::models::metal::GsRoot);
  const ParamDesc* base = param("gl.granular", pp::GrBaseNote);
  ex(root && root->format(60) == "C4" && root->format(61) == "C#4" && root->format(48) == "C3", "Grain Scanner Root: C4 at 60, C#4, C3 (" + (root ? root->format(60) : "-") + ")");
  ex(base && root && base->format(60) == root->format(60), "Grain Scanner and Granular name the same key the same way");
  double v = 0;
  ex(root && paramtext::parse(*root, "a#3", v) && v == 58, "Grain Scanner Root: typed as a note name");
  // 2.
  for (const auto& [type, index, word] : {std::make_tuple("gl.pads", pp::padE(0, pp::PadNote), "Auto"), std::make_tuple("gl.slicer", pp::slParam2(0, pp::SliceKey), "Auto"),
                                          std::make_tuple("gl.slicer", static_cast<int>(pp::SlLayerKey), "Off")}) {
    const ParamDesc* d = param(type, index);
    ex(d && d->format(-1) == word && d->format(64) == "E4", std::string(type) + ": -1 reads " + word + ", 64 reads E4 (" + (d ? d->format(-1) + " / " + d->format(64) : "-") + ")");
    double w = 0;
    ex(d && paramtext::parse(*d, lower(word), w) && w == -1 && paramtext::parse(*d, "e4", w) && w == 64, std::string(type) + ": the word and a note type back");
  }
  // 3.
  int checked = 0;
  std::string bad;
  for (const PluginInfo& p : pluginCatalog()) {
    if (p.type.rfind("gl.", 0) != 0) continue;
    for (const ParamDesc& d : p.params) {
      const std::string n = lower(d.name);
      if (!(has(n, "root") || has(n, "key") || has(n, "note") || has(n, "tonic"))) continue;
      if (d.kind != ParamKind::Continuous || notAKey(n)) continue;
      if (d.min < -1 || d.max > 127 || d.max < 24) continue;  // not on a MIDI note range
      ++checked;
      if (d.unit != "note" && d.unit.rfind("note-", 0) != 0) bad += "  " + p.type + " \"" + d.name + "\" unit \"" + d.unit + "\"\n";
    }
  }
  std::printf("# %d root / key / note controls checked\n", checked);
  if (!bad.empty()) std::printf("# without note names:\n%s", bad.c_str());
  ex(checked > 20 && bad.empty(), "every root, key and note control of the built-in plugins shows note names");
  return finish();
}
