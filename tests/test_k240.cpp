// K-240: every time control of every built-in plugin gets the time knob's magnets (core/TimeKnob.h):
// round ms / seconds on times, note values on lengths in beats, steps or ticks, and note values at
// the song's tempo on slow rates in Hz. Prints the full list ("# K240 <type> <index> <name> <unit>").
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "Check.h"
#include "core/ParamText.h"
#include "core/PluginInfo.h"
#include "core/TimeKnob.h"

using namespace gl;

namespace {
const char* unitName(timeknob::Unit u) {
  switch (u) {
    case timeknob::Unit::Ticks: return "ticks";
    case timeknob::Unit::Steps: return "steps";
    case timeknob::Unit::Ms: return "ms";
    case timeknob::Unit::Seconds: return "s";
    case timeknob::Unit::Beats: return "beats";
    case timeknob::Unit::Hz: return "Hz";
  }
  return "?";
}
}  // namespace

int main() {
  int total = 0, plugins = 0;
  bool inRange = true, exact = true, monotone = true;
  std::map<std::string, int> perUnit;
  for (const PluginInfo& p : pluginCatalog()) {
    if (p.hidden) continue;
    int here = 0;
    for (size_t k = 0; k < p.params.size(); ++k) {
      const auto s = timeknob::forParam(p.type, static_cast<int>(k));
      if (!s) continue;
      const ParamDesc& d = p.params[k];
      ++here, ++total, ++perUnit[unitName(s->unit)];
      std::printf("# K240 %s\t%zu\t%s\t%s\t%s\t%zu magnets\n", p.type.c_str(), k, p.name.c_str(), d.name.c_str(), unitName(s->unit), s->magnets.size());
      double prev = -1;
      for (double m : s->magnets) {
        if (m < std::min(d.min, d.max) - 1e-9 || m > std::max(d.min, d.max) + 1e-9) inRange = false;
        const double pos = timeknob::toPos(*s, d, m);
        if (pos <= prev) monotone = false;
        prev = pos;
        if (std::fabs(timeknob::toValue(*s, d, pos, true) - m) > 1e-9 * std::max(1.0, std::fabs(m))) exact = false;
      }
    }
    plugins += here > 0;
  }
  std::printf("# K240 total %d controls in %d plugins (ms %d, s %d, beats %d, steps %d, ticks %d, Hz %d)\n", total, plugins, perUnit["ms"], perUnit["s"],
              perUnit["beats"], perUnit["steps"], perUnit["ticks"], perUnit["Hz"]);
  expect(total > 100 && plugins > 30, "time controls across the plugins have the time knob");
  expect(inRange, "every magnet is inside its parameter's range");
  expect(monotone, "the magnets run in order along each knob");
  expect(exact, "a magnet's knob position gives back exactly the magnet");

  // Rates in Hz: note values at the song's tempo, named in the readout.
  {
    ParamDesc rate{"LFO rate", "Hz", 0.01, 20, 1, ParamKind::Continuous, ParamScale::Log, {}};
    const auto s = timeknob::classify(rate, 120);
    bool two = false;
    for (double m : s ? s->magnets : std::vector<double>{}) two = two || std::fabs(m - 2.0) < 1e-9;
    expect(s && s->unit == timeknob::Unit::Hz && two, "a rate in Hz magnets to the note values at the tempo (a 1/4 at 120 BPM is 2 Hz)");
    expect(s && timeknob::readout(*s, 2.0, 120, false) == "1/4 (2.00 Hz)" && timeknob::readout(*s, 2.37, 120, false) == "2.37 Hz", "the rate readout names the note");
    const auto fast = timeknob::classify(rate, 60);
    bool one = false;
    for (double m : fast ? fast->magnets : std::vector<double>{}) one = one || std::fabs(m - 1.0) < 1e-9;
    expect(one, "the magnets follow the tempo");
    double v = 0;
    expect(s && paramtext::parse(rate, "1/8", v, &*s, 120) && std::fabs(v - 4.0) < 1e-9, "typing 1/8 into a rate gives 4 Hz at 120 BPM");
  }
  // What stays as it was: audio frequencies, filter cutoffs, unitless amounts, choices.
  {
    expect(!timeknob::classify({"Cutoff", "Hz", 20, 20000, 1000, ParamKind::Continuous, ParamScale::Log, {}}), "an audio frequency is not a rate");
    expect(!timeknob::classify({"Low cut", "Hz", 5, 40, 20, ParamKind::Continuous, ParamScale::Log, {}}), "a low cut is not a rate");
    expect(!timeknob::classify({"Band 1 below 20 Hz", "Hz", 1, 20, 10, ParamKind::Continuous, ParamScale::Log, {}}), "an equaliser band's low frequency is not a rate");
    expect(!timeknob::classify({"Mix", "%", 0, 1, 1, ParamKind::Continuous, ParamScale::Linear, {}}), "an amount is not a time");
    expect(!timeknob::classify({"Rate", "", 0, 1, 0.5, ParamKind::Continuous, ParamScale::Linear, {}}), "a rate without a unit keeps its knob");
    expect(!timeknob::forParam("vst3:anything", 0), "installed plugins keep their own knobs");
    const auto pre = timeknob::classify({"Pre-delay", "ms", 0, 500, 20, ParamKind::Continuous, ParamScale::Linear, {}});
    expect(pre && pre->unit == timeknob::Unit::Ms && pre->offset > 0, "a wide linear pre-delay gets round ms and the even travel");
    const auto att = timeknob::classify({"Attack", "ms", 0.1, 100, 5, ParamKind::Continuous, ParamScale::Log, {}});
    expect(att && att->offset == 0 && att->quantum == 0, "a log time keeps its own travel and fine values");
  }
  return finish();
}
