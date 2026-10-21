// Gridline — FLP export tests (J-81): the plugin host record (event 212) of a
// mixer effect names the FL insert and slot FL loads it into; Parametric EQ 2
// states written from scratch have FL's own layout; every encoder's
// from-scratch output stays inside its stored ranges.
#pragma once
#include <cmath>
#include "core.h"
#include "enc_common.h"
#include "core/PluginInfo.h"
#include "packs/edison/edison.h"

namespace j81 {

using gl::flp::Bytes;
namespace enc = gl::flp::enc;

// Every mixer effect of an exported file: (insert, slot) where it sits in the
// event stream, and the first two words of its 212 record.
struct Placed {
  int insert, slot;
  uint32_t w0, w1;
  std::string name;
};
inline std::vector<Placed> placed(const Bytes& flp) {
  std::vector<Placed> v;
  gl::flp::File f;
  std::string err;
  if (!gl::flp::readBytes(flp, f, &err)) return v;
  gl::flp::Sections sec = gl::flp::split(f);
  for (size_t i = 0; i < sec.inserts.size(); ++i)
    for (size_t k = 0; k < sec.inserts[i].slots.size(); ++k) {
      const auto& evs = sec.inserts[i].slots[k].events;
      const gl::flp::Event* n = gl::flp::find(evs, 201);
      const gl::flp::Event* w = gl::flp::find(evs, 212);
      if (!n) continue;
      v.push_back({static_cast<int>(i), static_cast<int>(k), w ? w->u32(0) : 0xFFFFFFFFu, w ? w->u32(4) : 0xFFFFFFFFu, n->text()});
    }
  return v;
}
inline bool allPlacedRight(const std::vector<Placed>& v, size_t expectCount) {
  if (v.size() != expectCount) {
    std::printf("  %zu effects found, %zu expected\n", v.size(), expectCount);
    return false;
  }
  for (const auto& p : v)
    if (static_cast<int>(p.w0) != p.insert || static_cast<int>(p.w1) != p.slot) {
      std::printf("  misplaced: %s at insert %d slot %d says %u/%u\n", p.name.c_str(), p.insert, p.slot, p.w0, p.w1);
      return false;
    }
  return true;
}

inline Bytes exportBytes(const gl::Song& s, bool useOrigin) {
  gl::flp::ExportOptions o;
  o.useOrigin = useOrigin;
  gl::flp::ExportReport rep;
  Bytes out;
  if (!gl::flp::exportFlp(s, out, rep, o)) return {};
  return out;
}

inline void testSlots() {
  using namespace gl;
  // Effects on the master and on many inserts, in varying slots.
  Song s = makeEmptySong();
  while (s.mixer.size() < 41) {
    MixerTrack t;
    t.id = s.allocateId();
    t.sends.push_back({s.mixer[0].id, 1.0f, false});
    s.mixer.push_back(t);
  }
  static const char* kTypes[] = {"gl.eq", "gl.comp", "gl.reverb", "gl.limiter"};
  size_t count = 0;
  s.mixer[0].slots[0].plugin = defaultPluginState("gl.comp");
  s.mixer[0].slots[3].plugin = defaultPluginState("gl.limiter");
  count += 2;
  for (size_t i = 1; i < s.mixer.size(); ++i) {
    s.mixer[i].slots[i % 10].plugin = defaultPluginState(kTypes[i % 4]);
    ++count;
    if (i % 3 == 0) {
      s.mixer[i].slots[(i + 4) % 10].plugin = defaultPluginState("gl.eq");
      ++count;
    }
  }
  s.mixer[6].slots[0].plugin = defaultPluginState("gl.eq");  // the demo's case: EQ 2 on insert 6 slot 1
  s.mixer[6].slots[0].plugin.params[pp::eqParam(0, pp::EqGain)] = 3;
  count = 0;
  for (const auto& t : s.mixer)
    for (const auto& sl : t.slots) count += sl.plugin.empty() ? 0 : 1;
  const Bytes a = exportBytes(s, false);
  expect(!a.empty(), "J-81: many-insert song exports");
  expect(allPlacedRight(placed(a), count), "J-81: every mixer effect's 212 record names its own insert and slot (master included)");

  // Imported (FL origin kept), then two inserts swapped: the FL-written records follow the move.
  Song back;
  gl::flp::Report r;
  std::string err;
  expect(gl::importFlpBytes(a, back, r, &err), "J-81: exported file imports");
  const Bytes b = exportBytes(back, true);
  expect(allPlacedRight(placed(b), count), "J-81: re-export from origin keeps every effect in place");
  std::swap(back.mixer[5], back.mixer[9]);
  std::swap(back.mixer[2], back.mixer[30]);
  const Bytes c = exportBytes(back, true);
  expect(allPlacedRight(placed(c), count), "J-81: effects of moved inserts are written for their new insert");
}

// Parametric EQ 2 written from scratch: FL 25's layout (version 8, 36 knob
// words, the A/B spare 36, 62 bytes of editor options = 354 bytes), every knob
// inside FL's ranges, for default / extreme Gridline settings.
inline void testEq2Layout() {
  using namespace gl;
  const PluginInfo* info = findPlugin("gl.eq");
  expect(info != nullptr, "J-81: gl.eq registered");
  if (!info) return;
  auto check = [&](const PluginState& p, const char* what) {
    // Through the EQ 2 encoder (a default instance may export another way when that is exact).
    const enc::Encoder* e2 = enc::findEncoder("Fruity Parametric EQ 2", "gl.eq");
    const Bytes fresh = e2 && e2->defaults ? e2->defaults() : Bytes{};
    const enc::Result r = enc::encode(p, "Fruity Parametric EQ 2", &fresh, enctest::ctx());
    bool ok = r.flName == "Fruity Parametric EQ 2" && r.hasState && r.state.size() == 354 && gl::flp::getU32(r.state, 0) == 8;
    // Ranges: levels ±1800, frequencies / widths 0..65536, types 0..7, orders -3..3, main ±1800 (both A and B).
    static const int lo[5] = {-1800, 0, 0, 0, -3}, hi[5] = {1800, 65536, 65536, 7, 3};
    for (int ab = 0; ok && ab < 2; ++ab)
      for (int k = 0; k < 36; ++k) {
        const int32_t v = static_cast<int32_t>(gl::flp::getU32(r.state, 4 + 4 * static_cast<size_t>(36 * ab + k)));
        const int g = k < 35 ? k / 7 : 0;
        if (v < lo[g] || v > hi[g]) ok = false;
      }
    if (!ok) std::printf("  EQ 2 %s: %s, %zu bytes, version %u\n", what, r.flName.c_str(), r.state.size(), r.state.size() >= 4 ? gl::flp::getU32(r.state, 0) : 0u);
    expect(ok, (std::string("J-81: EQ 2 from scratch has FL's layout and ranges: ") + what).c_str());
  };
  {
    PluginState p = defaultPluginState("gl.eq");
    p.params[pp::EqModel] = 1;  // FL's band model (the one written as Parametric EQ 2)
    check(p, "defaults");
  }
  for (int ext = 0; ext < 2; ++ext) {
    PluginState p = defaultPluginState("gl.eq");
    for (size_t i = 0; i < info->params.size() && i < p.params.size(); ++i) p.params[i] = ext ? info->params[i].max : info->params[i].min;
    check(p, ext ? "all parameters at maximum" : "all parameters at minimum");
  }
}

// Every encoder that writes Gridline-native instances: its output for the
// type's default / minimum / maximum settings is finite and inside each stored
// field's range (the ranges FL's own states use).
inline void testEncoderRanges() {
  using namespace gl;
  int checked = 0, bad = 0;
  for (const auto& e : enc::encoders()) {
    if (e.nativeRank < 0 || e.glType.empty() || e.glType[0] == '*') continue;
    const PluginInfo* info = findPlugin(e.glType);
    if (!info) continue;
    for (int how = 0; how < 3; ++how) {
      PluginState p = defaultPluginState(e.glType);
      if (how)
        for (size_t i = 0; i < info->params.size() && i < p.params.size(); ++i) p.params[i] = how == 1 ? info->params[i].min : info->params[i].max;
      const enc::Result r = enc::encode(p, "", nullptr, enctest::ctx());
      if (!r.hasState || r.flName.empty()) continue;
      const enc::Encoder* used = enc::findEncoder(r.flName, e.glType);
      if (!used || !used->fields || used->finish) continue;  // finish: fields address the pre-finish (stored) form
      ++checked;
      // A value the solver did not touch keeps the plugin's own fresh-state value (FL's), which may lie
      // outside the range searched.
      Bytes fresh = used->defaults ? used->defaults() : Bytes{};
      if (fresh.empty() && used->defaultsIn) fresh = used->defaultsIn(enctest::ctx());
      for (const enc::Field& f : used->fields(r.state)) {
        if (f.hi < f.lo) continue;
        const double v = enc::readField(r.state, f.offset, f.type);
        const double d = fresh.size() == r.state.size() ? enc::readField(fresh, f.offset, f.type) : NAN;
        if (!std::isfinite(v) || ((v < f.lo - 1e-6 || v > f.hi + 1e-6) && v != d)) {
          if (++bad < 10) std::printf("  %s (%s) setting %d: field at %zu = %g outside %g..%g\n", r.flName.c_str(), e.glType.c_str(), how, f.offset, v, f.lo, f.hi);
        }
      }
    }
  }
  expect(checked > 50, "J-81: encoder range sweep covers the native encoders");
  expect(bad == 0, "J-81: every encoder's from-scratch fields are inside their stored ranges");
}

// A song with every Gridline effect / note processor type on its own insert:
// every mixer-slot plugin FL gets is a native FL effect (never FL's plugin
// wrapper, an FL instrument such as Dashboard, or a VFX note plugin, which FL
// runs only inside Patcher), each in its own insert / slot.
inline void testSlotPlugins() {
  using namespace gl;
  Song s = makeEmptySong();
  size_t n = 0;
  for (const PluginInfo& pi : pluginCatalog()) {
    if (pi.kind == PluginKind::Generator) continue;
    if (s.mixer.size() >= 126) break;
    MixerTrack t;
    t.id = s.allocateId();
    t.sends.push_back({s.mixer[0].id, 1.0f, false});
    t.slots[0].plugin = defaultPluginState(pi.type);
    s.mixer.push_back(t);
    ++n;
  }
  const Bytes a = exportBytes(s, false);
  const std::vector<Placed> v = placed(a);
  expect(allPlacedRight(v, n), "J-81: every effect type exports into its own insert and slot");
  bool ok = true;
  for (const Placed& p : v) {
    bool generator = false, effect = false;  // Patcher is both
    for (const auto& x : enc::encoders())
      if (x.flName == p.name) (x.generator ? generator : effect) = true;
    generator = generator && !effect;
    if (p.name == "Fruity Wrapper" || p.name.rfind("VFX ", 0) == 0 || generator) {
      std::printf("  mixer slot holds %s\n", p.name.c_str());
      ok = false;
    }
  }
  expect(ok, "J-81: no FL plugin wrapper, FL instrument or VFX note plugin in a mixer slot");
}

// Native instances are never written as an FL plugin inside FL's plugin
// wrapper (that is the plugin's old VST build; FL 2026 reports it missing).
inline void testNoWrappedFl() {
  using namespace gl;
  bool ok = true;
  for (const PluginInfo& pi : pluginCatalog()) {
    if (isHostedType(pi.type)) continue;
    for (int model = 0; model < 3; ++model) {
      PluginState p = defaultPluginState(pi.type);
      if (pi.type == "gl.eq") p.params[pp::EqModel] = static_cast<float>(model);
      else if (model) break;
      const enc::Result r = enc::encode(p, "", nullptr, enctest::ctx());
      if (r.flName == "Fruity Wrapper") {
        std::printf("  %s (model %d) written through Fruity Wrapper\n", pi.type.c_str(), model);
        ok = false;
      }
    }
  }
  expect(ok, "J-81: no built-in type is written as an FL plugin inside Fruity Wrapper");
}

// Formula Controller from scratch: FL's v4 layout to the last byte (version,
// a, b, c, u8 formula length + text, u8 description length + UTF-16, the
// closing byte), with text in both strings.
inline void testFormulaLayout() {
  using namespace gl;
  for (const char* formula : {"", "a*b+c"}) {
    PluginState p = defaultPluginState("gl.formulactl");
    p.data = formula;
    const enc::Result r = enc::encode(p, "", nullptr, enctest::ctx());
    const Bytes& b = r.state;
    bool ok = r.flName == "Fruity Formula Controller" && b.size() > 18 && gl::flp::getU32(b, 0) == 4;
    size_t at = 16;
    if (ok) {
      const size_t nf = b[at];
      ok = nf > 0;
      at += 1 + nf;
    }
    if (ok && at < b.size()) {
      const size_t nd = b[at];
      ok = nd > 0;
      at += 1 + 2 * nd;
    } else {
      ok = false;
    }
    ok = ok && at + 1 == b.size() && b[at] == 0;
    expect(ok, (std::string("J-81: Formula Controller state has FL's full v4 layout, formula '") + formula + "'").c_str());
  }
}

// MIDI Out with controllers: the records only in the v4 layout (a v6/v7 state
// with records hangs FL's MIDI Out); the instance decodes back.
inline void testMidiOutRecords() {
  using namespace gl;
  PluginState p = defaultPluginState("gl.midiout");
  p.params[static_cast<size_t>(pp::moKnob(0, pp::MoKnobNumber))] = 74;
  p.params[static_cast<size_t>(pp::moKnob(1, pp::MoKnobNumber))] = 71;
  const enc::Result r = enc::encode(p, "", nullptr, enctest::ctx());
  const bool records = r.state.size() > 0x142 && r.state[0x142] != 0xFF;
  expect(r.flName == "MIDI Out" && records && gl::flp::getU32(r.state, 0) == 4, "J-81: MIDI Out with controllers is written as v4");
  gl::flp::PluginMapping m;
  const size_t k = static_cast<size_t>(pp::moKnob(1, pp::MoKnobNumber));
  expect(enc::decodeState("MIDI Out", r.state, m, enctest::ctx()) && m.state.params.size() > k && std::lround(m.state.params[k]) == 71,
         "J-81: the v4 MIDI Out state decodes back");
}

// Edison from scratch (FL installed): the editor records FL reads between the
// audio and the settings are present.
inline void testEdisonFull() {
  using namespace gl;
  if (!enctest::flInstalled()) return;
  const enc::Result r = enc::encode(defaultPluginState("gl.edison"), "", nullptr, enctest::ctx());
  gl::packs::edison::FlEdison fl;
  expect(r.flName == "Edison" && gl::packs::edison::decodeFlState(r.state.data(), r.state.size(), fl) && fl.version >= 6 && !fl.extra.empty(),
         "J-81: Edison from scratch carries its editor records");
}

inline void testJ81() {
  testSlots();
  testEq2Layout();
  testEncoderRanges();
  testSlotPlugins();
  testNoWrappedFl();
  testFormulaLayout();
  testMidiOutRecords();
  testEdisonFull();
}

}  // namespace j81
