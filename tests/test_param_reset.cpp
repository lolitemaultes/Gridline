// Gridline — J-136 user request: "when i right click a knob i can choose reset
// or double click to reset, and if value is default already when i double click
// the text thing pops up i can choose the value by typing it in".
//
// Every parameter knob and slider decides its double-click through one rule
// (paramDoubleClick, used by the shared helpers in Widgets.cpp): back to the
// default when the value differs from it, the type-in when it already is the
// default (within a small step of the normalized travel). Tested here over every
// parameter of every built-in plugin, plus the edge cases of the rule.
#include <cmath>
#include <cstdio>
#include <string>
#include "../src/core/PluginInfo.h"
#include "Check.h"

using namespace gl;

namespace {
ParamDesc lin(double lo, double hi, double def) {
  ParamDesc d;
  d.name = "x", d.min = lo, d.max = hi, d.def = def;
  return d;
}
}  // namespace

int main() {
  // ---- the rule on hand-made descriptors
  {
    const ParamDesc d = lin(0, 1, 0.5);
    expect(paramDoubleClick(d, 0.5) == ParamDoubleClick::Type, "at the default: type");
    expect(paramDoubleClick(d, 0.5 + 0.5e-4) == ParamDoubleClick::Type, "within the epsilon: type");
    expect(paramDoubleClick(d, 0.5 + 1e-3) == ParamDoubleClick::Reset, "a small step off: reset");
    expect(paramDoubleClick(d, 0.0) == ParamDoubleClick::Reset, "far off: reset");
    expect(paramDoubleClick(d, NAN) == ParamDoubleClick::Reset, "NaN resets");
  }
  {
    ParamDesc d = lin(20, 20000, 1000);
    d.scale = ParamScale::Log, d.unit = "Hz";
    expect(paramDoubleClick(d, 1000) == ParamDoubleClick::Type, "log: at the default types");
    expect(paramDoubleClick(d, 1000.2) == ParamDoubleClick::Type, "log: within a normalized epsilon types");
    expect(paramDoubleClick(d, 1010) == ParamDoubleClick::Reset, "log: 1 % off resets");
  }
  {
    ParamDesc d = lin(0, 3, 1);
    d.kind = ParamKind::Choice, d.choices = {"A", "B", "C", "D"};
    expect(paramDoubleClick(d, 1) == ParamDoubleClick::Type, "choice: the default types");
    expect(paramDoubleClick(d, 2) == ParamDoubleClick::Reset, "choice: another entry resets");
    ParamDesc t = lin(0, 1, 1);
    t.kind = ParamKind::Toggle;
    expect(paramDoubleClick(t, 1) == ParamDoubleClick::Type && paramDoubleClick(t, 0) == ParamDoubleClick::Reset, "toggle");
  }
  {
    const ParamDesc d = lin(-24, 24, 0);  // bipolar, default in the middle
    expect(d.isDefault(0) && !d.isDefault(0.1), "bipolar default at 0");
    expect(lin(5, 5, 5).isDefault(5), "an empty range is always at its default");
  }

  // ---- every parameter of every built-in plugin: its default types, a value
  // off the default resets (the default itself is inside the range).
  int params = 0, bad = 0;
  for (const PluginInfo& p : pluginCatalog()) {
    for (const ParamDesc& d : p.params) {
      ++params;
      const bool typeAtDef = paramDoubleClick(d, d.def) == ParamDoubleClick::Type;
      const double nDef = d.toNormalized(d.def);
      const double off = d.fromNormalized(nDef < 0.5 ? 1.0 : 0.0);
      const bool resetOff = d.max <= d.min || d.isDefault(off) || paramDoubleClick(d, off) == ParamDoubleClick::Reset;
      if (!typeAtDef || !resetOff) {
        if (++bad <= 10) std::printf("# %s / %s: default %g, off %g\n", p.type.c_str(), d.name.c_str(), d.def, off);
      }
    }
  }
  std::printf("# %d parameters of %zu plugins\n", params, pluginCatalog().size());
  expect(params > 1000 && bad == 0, "every built-in parameter: the default types, a value off it resets");
  return finish();
}
