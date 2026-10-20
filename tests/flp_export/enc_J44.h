// Gridline — FLP export tests: J-44 (the last encoder-mode gaps): plugins
// that play the channel sample, state-less FL plugins, Ogun / Autogun states
// older than version 10. Corpus checks read FL Studio's shipped content when
// it is installed (skipped otherwise).
#pragma once
#include <cstdint>
#include <cstring>
#include "core.h"
#include "enc_common.h"
#include "core/PluginParams.h"
#include "core/flp/FlpGen.h"
#include "packs/models/metal.h"

namespace enctest {

inline void testEncJ44() {
  namespace mt = gl::packs::models::metal;

  // ---- A plugin that plays the channel sample (event 196 beside the state):
  // the encoder decodes its state with the path the channel writer adds.
  {
    gl::PluginState g = gl::defaultPluginState("gl.grainscan");
    g.data = "/j44/samples/grain source.wav";
    enc::Result r = enc::encode(g, "", nullptr, ctx());
    expect(r.flName == "Fruity Granulizer" && r.quality == enc::Result::Exact, "J-44 Granulizer: a sample-playing state is written exactly");
    // (FL's fresh 3x Osc as Gridline reads it, osc 1 switched to the custom wave)
    gl::flp::PluginMapping fresh;
    expect(enc::decodeState("3x Osc", enc::findEncoder("3x Osc", "gl.synth")->defaults(), fresh, ctx()), "J-44 3x Osc: fresh state decodes");
    gl::PluginState s = fresh.state;
    s.data = "/j44/samples/one cycle.wav";
    s.params[gl::pp::SynOsc1Shape] = 7.0f;  // custom wave: plays the channel sample
    enc::Result t = enc::encode(s, "", nullptr, ctx());
    expect(t.flName == "3x Osc" && t.quality == enc::Result::Exact, "J-44 3x Osc: a custom-wave oscillator with its channel sample is exact");
    enc::EncodeContext c = ctx();
    c.channelSample = "/j44/samples/grain source.wav";
    gl::flp::PluginMapping m;
    expect(enc::decodeState("Fruity Granulizer", r.state, m, c) && m.state.data == g.data, "J-44: decodeState presents the channel sample (event 196)");
  }

  // ---- Fruity Chorus: values between the integer steps (FL's older float
  // layout) are written in that layout; values on the steps stay integers.
  if (flInstalled()) {
    const Stats st = corpusRoundTrip("Fruity Chorus", 400);
    expect(st.states > 0 && st.nativeExact == st.decoded && st.patchExact == st.decoded, "J-44 Chorus: every shipped state exact, float layout included");
  }
  {
    const enc::Encoder* e = enc::findEncoder("Fruity Chorus", "gl.chorus");
    gl::flp::PluginMapping m;
    expect(e && e->defaults && enc::decodeState("Fruity Chorus", e->defaults(), m, ctx()), "J-44 Chorus: fresh state decodes");
    gl::PluginState g = m.state;
    g.params[gl::pp::ChoRate] = 2.45f;  // on the integer grid (3500), not on the float one
    Bytes t = e->defaults();
    enc::Result r = enc::encode(g, "Fruity Chorus", &t, ctx());
    expect(r.quality == enc::Result::Exact && r.state.size() == 52, "J-44 Chorus: an on-grid rate keeps the integer layout");
  }

  // ---- SoundFont Player without the origin: FL's database preset with the
  // instance's SoundFont in it.
  if (flInstalled()) {
    gl::PluginState g = gl::defaultPluginState("gl.multisampler");
    g.data = "#gl-zones 1\nsf2 0 0 - | /j44/banks/strings.sf2\n";
    enc::Result r = enc::encode(g, "Soundfont Player", nullptr, ctx());
    gl::flp::PluginMapping m;
    const bool ok = r.hasState && enc::decodeState("Soundfont Player", r.state, m, ctx());
    if (!(ok && m.state.data == g.data)) std::printf("# sf2: ok %d q %s [%s] mism %zu\n", ok, enc::qualityName(r.quality), m.state.data.c_str(), r.mismatched.size());
    expect(ok && m.state.data == g.data, "J-44 SoundFont Player: path written without origin");
  }

  // ---- Ogun / Autogun: states before version 10 (articulators 11 / 12 and,
  // before version 4, 4 / 5 not stored) load those articulators fresh, and
  // every shipped state is written exactly from Gridline's values.
  if (flInstalled()) {
    int old = 0, oldFresh = 0;
    for (const Bytes& b : corpusStates("Ogun", 2000)) {
      if (b.size() < 4) continue;
      uint32_t raw;
      std::memcpy(&raw, b.data(), 4);
      const int ver = raw > 0xFFFF ? static_cast<int>(raw >> 16) : static_cast<int>(raw);
      if (ver >= 4) continue;
      ++old;
      gl::flp::PluginMapping m;
      // articulator 4's knobs are parameters: FL's fresh 128 128 0 128 / 40000
      if (enc::decodeState("Ogun", b, m, ctx()) && m.state.params[static_cast<size_t>(mt::alKnob(4, 0))] == 128.0f &&
          m.state.params[static_cast<size_t>(mt::alKnob(4, 4))] == 40000.0f)
        ++oldFresh;
    }
    expect(old > 0 && oldFresh == old, "J-44 Ogun: version < 4 states load the unstored articulators fresh");
    for (const char* n : {"Ogun", "Autogun"}) {
      const Stats st = corpusRoundTrip(n, 2000);
      std::printf("  J-44 %-8s %4d states, %4d decode, %4d exact without origin\n", n, st.states, st.decoded, st.nativeExact);
      expect(st.decoded == st.states && st.nativeExact == st.decoded, (std::string("J-44 ") + n + ": every shipped state exact without origin").c_str());
    }
  }
}

}  // namespace enctest
