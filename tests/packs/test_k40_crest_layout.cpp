// Gridline: K-40 Crest's editor layout as a pure function: no control overlaps another, knobs have
// one size and sit on shared rows and columns, captions share their row's line, at every form scale
// and in both engines. The restyle only touches drawing: the parameter list, its ranges, defaults
// and the factory presets keep their checksum from before it.
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/packs/dynamics/dynamics.h"
#include "../../src/packs/dynamics/masterfl_layout.h"
#include "../Check.h"

using namespace gl;
using namespace gl::packs::dynamics;

namespace {

void ex(bool ok, const std::string& name) { expect(ok, name.c_str()); }

uint64_t mixIn(uint64_t h, const void* data, size_t n) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}
uint64_t mixStr(uint64_t h, const std::string& s) { return mixIn(h, s.data(), s.size() + 1); }
uint64_t mixNum(uint64_t h, double v) {
  const long long q = std::llround(v * 1000.0);  // to a thousandth: the same on every platform's libm
  return mixIn(h, &q, sizeof q);
}

uint64_t paramsChecksum(const PluginInfo& info) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (const ParamDesc& d : info.params) {
    h = mixStr(h, d.name);
    h = mixStr(h, d.unit);
    h = mixNum(h, d.min);
    h = mixNum(h, d.max);
    h = mixNum(h, d.def);
    for (const std::string& c : d.choices) h = mixStr(h, c);
  }
  return h;
}
uint64_t presetsChecksum(const PluginInfo& info) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < info.presets.size(); ++i) {
    const PluginState st = presetState(info, i);
    h = mixStr(h, info.presets[i].name);
    for (float v : st.params) h = mixNum(h, v);
  }
  return h;
}

void noFindings(const crest::Layout& l, float scale, const char* what) {
  const std::vector<std::string> found = crest::checkLayout(l, scale);
  for (const std::string& s : found) std::printf("  %s at %.2f: %s\n", what, static_cast<double>(scale), s.c_str());
  ex(found.empty(), std::string(what) + ": clean at scale " + std::to_string(scale));
}

}  // namespace

int main(int argc, char** argv) {
  const bool print = argc > 1 && std::string(argv[1]) == "--print";
  // ---- the layout
  for (bool ref : {true, false}) {
    const crest::Layout l = crest::makeLayout(ref);
    const char* what = ref ? "current engine" : "older engine";
    ex(l.W == crest::kW && l.H > 300 && l.H < 600, "the form has a sensible size");
    // 1.0 and 2.0 are the interface scales; the form itself runs from half to twice its width.
    for (float s : {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f}) noFindings(l, s, what);
    int knobs = 0;
    for (const crest::Item& i : l.items) knobs += i.kind == crest::Kind::Knob;
    ex(knobs == 14, "all fourteen knobs are placed");
    // Every knob has its caption, centred under it, on the row's line.
    for (const crest::Item& i : l.items) {
      if (i.kind != crest::Kind::Knob) continue;
      const crest::Rect c = crest::itemRect(l, (i.id + ".label").c_str());
      ex(c.w > crest::kKnob, i.id + ": has a caption wider than the knob");
      ex(std::fabs(c.cx() - i.r.cx()) < 0.01f, i.id + ": the caption is centred on the knob");
      ex(c.y >= i.r.y1(), i.id + ": the caption is below the knob");
    }
    // Rows and columns: same centre line for every crossing.
    ex(std::fabs(crest::itemRect(l, "pre").cy() - crest::itemRect(l, "lowCut").cy()) < 0.01f, "first row shares one line across the deck");
    ex(std::fabs(crest::itemRect(l, "stereo").cy() - crest::itemRect(l, "mix").cy()) < 0.01f, "second row shares one line across the deck");
    ex(std::fabs(crest::itemRect(l, "pre").cx() - crest::itemRect(l, "satThres").cx()) < 0.01f, "gain and saturation share columns");
    ex(std::fabs(crest::itemRect(l, "delay").cx() - crest::itemRect(l, "mix").cx()) < 0.01f, "delay and mix share a column");
    ex(std::fabs(crest::itemRect(l, "attack").cx() - crest::itemRect(l, "atkCurve").cx()) < 0.01f, "attack and its curve share a column");
    ex(std::fabs(crest::itemRect(l, "release").cx() - crest::itemRect(l, "relCurve").cx()) < 0.01f, "release and its curve share a column");
    ex(std::fabs(crest::itemRect(l, "low").cx() - crest::itemRect(l, "lowSlope").cx()) < 0.01f, "low split and its slope share a column");
    ex(std::fabs(crest::itemRect(l, "high").cx() - crest::itemRect(l, "highSlope").cx()) < 0.01f, "high split and its slope share a column");
    // One button height across the strip, the modes and the second row.
    for (const crest::Item& i : l.items)
      if (i.kind == crest::Kind::Control && i.id != "curve" && i.id != "monitor" && i.id != "speed" && i.id.rfind("band", 0) != 0)
        ex(std::fabs(i.r.h - crest::kControlH) < 0.01f, i.id + ": the shared control height");
    // Equal gutters: the deck's cells follow each other at one gutter and the displays share a height.
    ex(std::fabs(l.gain.r.x - l.mode.r.x1() - crest::kGutter) < 0.01f, "one gutter between cells");
    ex(std::fabs(l.split.r.x - l.mix.r.x1() - crest::kGutter) < 0.01f, "one gutter between the last cells");
    ex(std::fabs(l.curve.h - l.monitor.h) < 0.01f, "curve and monitor have one height");
    ex(std::fabs(l.curve.w - l.curve.h) < 0.01f, "the curve is square");
    ex(std::fabs(l.monitor.x1() - (l.W - crest::kMargin)) < 0.01f, "the monitor reaches the margin");
    ex(l.tabs.r.y == l.mode.r.y && l.tabs.r.h == l.mode.r.h, "the tabs and the cells share top and height");
    // A deliberately bad layout is caught: the checks really check.
    crest::Layout bad = l;
    bad.items.push_back(crest::Item{"stray", crest::Kind::Control, crest::itemRect(l, "pre"), -1, -1});
    ex(!crest::checkLayout(bad, 1.0f).empty(), "an overlap is reported");
    crest::Layout off = l;
    for (crest::Item& i : off.items)
      if (i.id == "post") i.r.y += 3;
    ex(!crest::checkLayout(off, 1.0f).empty(), "a knob off its row is reported");
  }
  // ---- parameters, ranges, defaults and presets: untouched by the interface work
  const PluginInfo* info = findPlugin("gl.master");
  ex(info != nullptr, "Crest exists");
  if (info) {
    const uint64_t params = paramsChecksum(*info), presets = presetsChecksum(*info);
    if (print) std::printf("params %" PRIx64 " presets %" PRIx64 " count %zu presets %zu\n", params, presets, info->params.size(), info->presets.size());
    ex(info->params.size() == static_cast<size_t>(kMsTotal), "the parameter count");
    ex(static_cast<int>(info->params.size()) == 295, "295 parameters, as before the restyle");
    ex(info->presets.size() == 6, "six factory presets, as before the restyle");
    ex(params == 0x9eb6c5327223634dull, "parameter names, units, ranges, defaults and choices are unchanged");
    ex(presets == 0xc4297901907f9a5bull, "factory preset names and values are unchanged");
    ex(MsRef0 == 193 && MsRef0 + MrLin == 258 && MsRef0 + MrOversample == 293, "parameter indices are unchanged");
  }
  return finish();
}
