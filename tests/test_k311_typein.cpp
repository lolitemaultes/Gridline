// Gridline tests - K-311: typed values on every plugin control (core/ParamText.h).
//  * numbers with or without their unit: "2.5k", "2.5 kHz", "-6", "-6 dB", "250 ms", "0.5 s";
//  * percentages, of the value for percent parameters and of the travel for the others;
//  * note values on delay time knobs ("1/4", "1/8 dotted", "2 bars"), plain fractions elsewhere;
//  * choices, toggles, notes, pan, ratios; values clamp to the range; nonsense is refused;
//  * every readout of every parameter of every built-in plugin reads back as its own value.
#include <cmath>
#include <cstdio>
#include <string>
#include "Check.h"
#include "core/ParamText.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/TimeKnob.h"
#include "core/TypeInPick.h"

using namespace gl;

namespace {
ParamDesc lin(const char* u, double mn, double mx, double d = 0) { return ParamDesc{"p", u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}}; }
bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }
bool reads(const ParamDesc& d, const char* text, double want, const timeknob::Spec* t = nullptr, double tempo = 120) {
  double v = -12345;
  const bool ok = paramtext::parse(d, text, v, t, tempo);
  if (!ok || !near(v, want)) std::printf("  \"%s\" (unit %s) -> %s %g, want %g\n", text, d.unit.c_str(), ok ? "ok" : "refused", v, want);
  return ok && near(v, want);
}
bool refuses(const ParamDesc& d, const char* text) {
  double v = 0;
  return !paramtext::parse(d, text, v);
}
}  // namespace

int main() {
  const ParamDesc hz{"Freq", "Hz", 20, 20000, 1000, ParamKind::Continuous, ParamScale::Log, {}};
  expect(reads(hz, "2.5k", 2500) && reads(hz, "2.5 kHz", 2500) && reads(hz, "2,5 khz", 2500) && reads(hz, "440", 440) && reads(hz, "440 Hz", 440),
         "frequencies: 2.5k, 2.5 kHz, 440, 440 Hz");
  expect(reads(hz, "50000", 20000) && reads(hz, "1", 20), "frequencies clamp to the range");
  expect(reads(hz, "50%", hz.fromNormalized(0.5)), "a percentage of a frequency is that much of the travel");
  const ParamDesc db = lin("dB", -24, 24);
  expect(reads(db, "-6", -6) && reads(db, "-6 dB", -6) && reads(db, "+3.5dB", 3.5) && reads(db, "\xE2\x88\x92" "6 dB", -6) && reads(db, "-60", -24),
         "gains: -6, -6 dB, +3.5dB, a typed minus sign, clamped");
  const ParamDesc pc = lin("%", 0, 1, 0.5);
  expect(reads(pc, "50%", 0.5) && reads(pc, "50", 0.5) && reads(pc, "12.5 %", 0.125) && reads(pc, "0.3x", 0.3) && reads(pc, "150%", 1),
         "percent parameters: 50%, 50, 12.5 %, 0.3x raw, clamped");
  const ParamDesc pct = lin("pct", 0, 100, 50);
  expect(reads(pct, "25%", 25) && reads(pct, "25", 25), "parameters already in percent");
  const ParamDesc amount = lin("", 0, 1, 0);
  expect(reads(amount, "1/4", 0.25) && reads(amount, "25%", 0.25) && reads(amount, "0.25", 0.25), "a bare amount: 1/4, 25%, 0.25");
  const ParamDesc sec = lin("s", 0, 10, 1);
  expect(reads(sec, "250 ms", 0.25) && reads(sec, "0.5 s", 0.5) && reads(sec, "2", 2) && reads(sec, "250", 0.25), "seconds: 250 ms, 0.5 s, 2, bare 250 as ms");
  const ParamDesc ms = lin("ms", 0, 2000, 100);
  expect(reads(ms, "250", 250) && reads(ms, "0.5s", 500) && reads(ms, "12.5 ms", 12.5), "milliseconds");
  const ParamDesc st = lin("st", -24, 24);
  expect(reads(st, "-12", -12) && reads(st, "+7 st", 7), "semitones");
  const ParamDesc pan = lin("pan", -1, 1);
  expect(reads(pan, "50% left", -0.5) && reads(pan, "30 R", 0.3) && reads(pan, "L100", -1) && reads(pan, "centre", 0) && reads(pan, "Centred", 0) &&
             reads(pan, "-0.25", -0.25) && reads(pan, "-50", -0.5),
         "pan: 50% left, 30 R, L100, centre, -0.25, -50");
  const ParamDesc ratio = lin("x", 1, 20, 4);
  expect(reads(ratio, "4:1", 4) && reads(ratio, "8", 8), "ratios: 4:1, 8");
  const ParamDesc times = lin("times", 0.5, 2, 1);
  expect(reads(times, "x1.5", 1.5) && reads(times, "1.5x", 1.5) && reads(times, "1.25", 1.25), "multipliers: x1.5, 1.5x");
  const ParamDesc note = lin("note", 0, 127, 60);
  expect(reads(note, "C4", 60) && reads(note, "a#3", 58) && reads(note, "Bb3", 58) && reads(note, "64", 64), "notes: C4, a#3, Bb3, 64");
  const ParamDesc choice{"Shape", "", 0, 2, 0, ParamKind::Choice, ParamScale::Linear, {"Sine", "Rounded saw", "Square"}};
  expect(reads(choice, "rounded saw", 1) && reads(choice, "Square", 2) && reads(choice, "1.4", 1), "choices by name or number");
  const ParamDesc toggle{"On", "", 0, 1, 0, ParamKind::Toggle, ParamScale::Linear, {}};
  expect(reads(toggle, "on", 1) && reads(toggle, "Off", 0) && reads(toggle, "1", 1), "toggles");
  expect(refuses(db, "loud") && refuses(db, "") && refuses(hz, "3 dB") && refuses(db, "6 Hz") && refuses(st, "abc"), "nonsense and wrong units are refused");

  // Delay time knobs take note values at the song tempo.
  const timeknob::Spec ticks{timeknob::Unit::Ticks, {}, 0, 0};
  const ParamDesc tk = lin("", 1, 192 * 16, 192);
  expect(reads(tk, "1/4", 192, &ticks) && reads(tk, "1/8 dotted", 144, &ticks) && reads(tk, "1/8d", 144, &ticks) && reads(tk, "1/16 triplet", 32, &ticks) &&
             reads(tk, "2 bars", 192 * 8, &ticks) && reads(tk, "1.5 beats", 288, &ticks),
         "synced delay times: 1/4, 1/8 dotted, 1/16 triplet, 2 bars, 1.5 beats");
  const timeknob::Spec msSpec{timeknob::Unit::Ms, {}, 0, 0};
  expect(reads(ms, "1/4", 500, &msSpec, 120) && reads(ms, "1/8", 250, &msSpec, 120) && reads(ms, "300 ms", 300, &msSpec, 120),
         "free delay times: a note value at the tempo, or milliseconds");
  const timeknob::Spec steps{timeknob::Unit::Steps, {}, 0, 0};
  const ParamDesc sk = lin("", 1, 64, 4);
  expect(reads(sk, "1/4", 4, &steps) && reads(sk, "1/16", 1, &steps), "step delay times");

  // The start text shows the value with its unit; every readout of every built-in parameter reads back.
  expect(paramtext::initial(hz, 2500) == "2.50 kHz" && paramtext::initial(db, -6) == "-6.0 dB", "the box starts with the value and its unit");
  int tried = 0, failed = 0;
  for (const PluginInfo& p : pluginCatalog()) {
    for (size_t k = 0; k < p.params.size(); ++k) {
      const ParamDesc& d = p.params[k];
      for (double n : {0.0, 0.37, 1.0}) {
        const double v = d.kind == ParamKind::Continuous ? d.fromNormalized(n) : std::round(d.fromNormalized(n));
        const std::string text = d.format(v);
        double back = 0;
        ++tried;
        // The readout rounds: back must land within the readout's own precision (its value shown again).
        if (!paramtext::parse(d, text, back) || d.format(back) != text) {
          if (++failed <= 20) std::printf("  %s %s (%s): \"%s\" -> %s\n", p.type.c_str(), d.name.c_str(), d.unit.c_str(), text.c_str(), d.format(back).c_str());
        }
      }
    }
  }
  std::printf("  readouts read back: %d of %d\n", tried - failed, tried);
  expect(tried > 1000 && failed == 0, "every readout of every built-in parameter reads back as itself");

  // Which parameter a right-click on an editor control means (core/TypeInPick.h).
  using typein::Event;
  auto touch = [](uint32_t k) {
    Event e;
    e.pid = ParamId{7, k};
    return e;
  };
  auto item = [](unsigned id, float x0, float y0, float x1, float y1) {
    Event e;
    e.item = true, e.id = id, e.x0 = x0, e.y0 = y0, e.x1 = x1, e.y1 = y1;
    return e;
  };
  // A knob asks for its parameter, then adds its item; an XY pad asks for two.
  std::vector<Event> ev = {touch(1), item(10, 0, 0, 40, 40), touch(2), touch(3), item(11, 50, 0, 150, 100), item(12, 160, 0, 200, 20), touch(4)};
  auto at = [&](float x, float y, unsigned hov = 0) { return typein::paramsAt(ev, x, y, x, y, hov); };
  expect(at(20, 20).size() == 1 && at(20, 20)[0].index == 1, "a knob: its own parameter");
  expect(at(100, 50).size() == 2 && at(100, 50)[0].index == 2 && at(100, 50)[1].index == 3, "an XY pad: both of its parameters");
  expect(at(170, 10).size() == 1 && at(170, 10)[0].index == 4, "a control that asks after its item");
  expect(at(300, 300).empty() && typein::paramsAt(ev, 20, 20, 100, 50).empty(), "nothing under the pointer, or pressed elsewhere: no menu");
  // A row of cells for one parameter (a shape selector asks once, then adds a cell per shape).
  std::vector<Event> row = {touch(5), item(20, 0, 0, 30, 30), item(21, 30, 0, 60, 30), item(22, 60, 0, 90, 30)};
  expect(typein::paramsAt(row, 75, 15, 75, 15).size() == 1 && typein::paramsAt(row, 75, 15, 75, 15)[0].index == 5, "a cell of a selector row: the row's parameter");
  // A lone big button after a small knob is not that knob.
  std::vector<Event> lone = {touch(6), item(30, 0, 0, 20, 20), item(31, 30, 0, 200, 80)};
  expect(typein::paramsAt(lone, 100, 40, 100, 40).empty(), "a button of another size after a knob: no guess");
  // A display that reads every band is no single control.
  std::vector<Event> many;
  for (uint32_t k = 0; k < 20; ++k) many.push_back(touch(k));
  many.push_back(item(40, 0, 0, 400, 200));
  expect(typein::paramsAt(many, 100, 100, 100, 100).empty(), "a display that reads many parameters: no menu");
  // ImGui's hovered item wins when items overlap.
  std::vector<Event> overlap = {touch(8), item(50, 0, 0, 100, 100), touch(9), item(51, 0, 0, 100, 100)};
  expect(typein::paramsAt(overlap, 50, 50, 50, 50, 50)[0].index == 8 && typein::paramsAt(overlap, 50, 50, 50, 50)[0].index == 9,
         "the hovered item, else the topmost");
  return finish();
}
