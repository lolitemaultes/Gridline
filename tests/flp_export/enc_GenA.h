// Gridline — FLP export tests: GenA plugin encoders (FlpEncGenA.cpp).
#pragma once
#include <functional>
#include "core/PluginParams.h"
#include "core/flp/FlpGen.h"
#include "enc_common.h"
#include "packs/models/harmor.h"
#include "packs/models/models.h"
#include "packs/modulation/modulation.h"
#include "packs/samplers/samplers.h"
#include "packs/synths/gms.h"
#include "packs/synths/matrixfm.h"

namespace enctest {

// Decodes `b` as `flName` (false when it does not decode to `glType`).
inline bool genaDecode(const std::string& flName, const Bytes& b, const std::string& glType, gl::PluginState& out) {
  gl::flp::PluginMapping m;
  if (!enc::decodeState(flName, b, m, ctx()) || m.state.type != glType) return false;
  out = m.state;
  return true;
}

// The encoder's defaults with stored values changed, decoded.
inline bool genaEdited(const std::string& flName, const std::string& glType, const std::function<void(Bytes&)>& edit, gl::PluginState& out) {
  const enc::Encoder* e = enc::findEncoder(flName, glType);
  if (!e || !e->defaults) return false;
  Bytes b = e->defaults();
  edit(b);
  return genaDecode(flName, b, glType, out);
}

// A shipped state with its decoded Gridline state edited, encoded on that
// state (patch path): true when it decodes back exactly.
inline bool genaPatch(const std::string& flName, size_t index, const std::function<void(gl::PluginState&)>& edit, Bytes* outState = nullptr) {
  std::vector<Bytes> states = corpusStates(flName, index + 1);
  if (states.size() <= index) return false;
  gl::PluginState g;
  gl::flp::PluginMapping m;
  if (!enc::decodeState(flName, states[index], m, ctx())) return false;
  g = m.state;
  edit(g);
  enc::Result r = enc::encode(g, flName, &states[index], ctx());
  if (outState) *outState = r.state;
  return r.quality == enc::Result::Exact;
}

inline void genaCorpus(const char* flName, const std::string& what, int nativeShort, size_t max = 400) {
  if (!flInstalled()) return;
  Stats s = corpusRoundTrip(flName, max);
  expect(s.decoded > 0 && s.nativeExact + nativeShort >= s.decoded, (what + ": shipped states encode natively").c_str());
  expect(s.patchExact == s.decoded, (what + ": unchanged states are written verbatim").c_str());
}

inline void testEncGenA() {
  using gl::flp::putI32;
  namespace pp = gl::pp;

  // ---- 3x Osc: oscillator tags, switches, the channel envelope / filter / LFO copies.
  {
    expect(defaultsDecode("3x Osc", "gl.synth"), "3x Osc: defaults decode");
    gl::PluginState g;
    bool ok = genaEdited("3x Osc", "gl.synth", [](Bytes& b) {
      putI32(b, 4 + 4 * 1, 3);          // osc 1 saw
      putI32(b, 4 + 4 * 9, 7);          // osc 2 coarse +7
      putI32(b, 4 + 4 * 13, 100);       // osc 3 mix
      putI32(b, 4 + 4 * 20, 32);        // phase randomness
      b[89] = 1, b[91] = 1, b[92] = 1;  // invert osc 2, osc 3 AM, HQ
      putI32(b, 96 + 68 + 4, -1);       // volume envelope on
      putI32(b, 96 + 68 + 12, 9000);    // attack
      putI32(b, 96 + 68 + 24, 90);      // sustain
      putI32(b, 444, 120), putI32(b, 448, 60), putI32(b, 456, 3);  // filter copy: cutoff, resonance, HP
      putI32(b, 96 + 136 + 44, 50);     // mod X LFO
    }, g);
    expect(ok, "3x Osc: synthetic state decodes");
    expect(ok && nativeRoundTrip("3x Osc", g), "3x Osc: an edited patch with envelope / filter / LFO encodes back exactly");
    gl::PluginState d;
    if (genaDecode("3x Osc", enc::findEncoder("3x Osc", "gl.synth")->defaults(), "gl.synth", d)) {
      d.params[pp::SynLfoTarget] = 0;
      expect(nativeRoundTrip("3x Osc", d), "3x Osc: defaults round trip");
    }
    const enc::Encoder* e = enc::nativeEncoder(gl::defaultPluginState("gl.synth"));
    expect(e && e->flName == "3x Osc", "3x Osc: native home of the Trio Synth");
    // 4 shipped states have a volume / filter decay from an uncommon hold + decay pair.
    genaCorpus("3x Osc", "3x Osc", 4);
  }

  // ---- FL Keys: sample set name + 15 tags.
  {
    expect(defaultsDecode("FL Keys", "gl.keys"), "FL Keys: defaults decode");
    gl::PluginState g;
    expect(genaDecode("FL Keys", enc::findEncoder("FL Keys", "gl.keys")->defaults(), "gl.keys", g), "FL Keys: defaults decode to Keys");
    g.params[pp::KyModel] = 5;  // the other piano set
    g.params[pp::KyModMode] = 1, g.params[pp::KyTremDepth] = 0.4f;
    expect(nativeRoundTrip("FL Keys", g), "FL Keys: set name, auto-pan depth encode back exactly");
    genaCorpus("FL Keys", "FL Keys", 0);
  }

  // ---- MIDI Out: header words, controller records (added when needed).
  {
    expect(defaultsDecode("MIDI Out", "gl.midiout"), "MIDI Out: defaults decode");
    gl::PluginState g;
    expect(genaDecode("MIDI Out", enc::findEncoder("MIDI Out", "gl.midiout")->defaults(), "gl.midiout", g), "MIDI Out: defaults decode");
    g.params[pp::MoChannel] = 5, g.params[pp::MoProgram] = 12;
    g.params[pp::moKnob(2, pp::MoKnobNumber)] = 74, g.params[pp::moKnob(2, pp::MoKnobValue)] = 100;
    g.params[pp::moKnob(2, pp::MoKnobChannel)] = 3, g.params[pp::moKnob(2, pp::MoKnobMax)] = 64;
    expect(nativeRoundTrip("MIDI Out", g), "MIDI Out: channel, program and a third controller encode back exactly");
    genaCorpus("MIDI Out", "MIDI Out", 0);
  }

  // ---- Envelope Controller: no default state; points and counts on a shipped state.
  if (flInstalled()) {
    namespace md = gl::packs::modulation;
    expect(genaPatch("Fruity Envelope Controller", 0, [](gl::PluginState& g) {
      g.params[md::envPointLevel(0, 1)] = 0.25f;
      g.params[md::envParam(0, md::EaNumPoints)] = g.params[md::envParam(0, md::EaNumPoints)] + 1;
    }), "Envelope Controller: a point level and a point count change encode exactly");
    genaCorpus("Fruity Envelope Controller", "Envelope Controller", 1000);  // no defaults: native writes none
  }

  // ---- GMS.
  {
    namespace gm = gl::packs::synths::gms;
    expect(defaultsDecode("GMS", "gl.gms"), "GMS: defaults decode");
    gl::PluginState g;
    bool ok = genaEdited("GMS", "gl.gms", [](Bytes& b) {
      const size_t f = 8 + 0xE10;
      gl::flp::putF32(b, f + 12 * 4, 0.4f);   // cutoff
      gl::flp::putF32(b, f + 16 * 4, 0.7f);   // envelope 1 amount
      gl::flp::putF32(b, f + 18 * 4, 1.0f);   // … inverted
      gl::flp::putF32(b, f + 150 * 4, 0.0f);  // an effect off
    }, g);
    expect(ok, "GMS: synthetic state decodes");
    expect(ok && nativeRoundTrip("GMS", g), "GMS: cutoff, inverted envelope, effect switch encode back exactly");
    if (ok) {
      g.params[gm::lfo(1, gm::LfAmt)] = -0.25f;  // sign through the invert switch
      expect(nativeRoundTrip("GMS", g), "GMS: a negative LFO amount encodes back exactly");
    }
    genaCorpus("GMS", "GMS", 0);
  }

  // ---- FLEX: the kept preset identity and controls are written back.
  if (flInstalled()) {
    // Two states: a solved float prints differently in the kept text / a cutoff from three controls.
    genaCorpus("FLEX", "FLEX", 2);
    std::vector<Bytes> st = corpusStates("FLEX", 1);
    gl::PluginState g;
    if (!st.empty() && genaDecode("FLEX", st[0], "gl.analog", g)) {
      expect(nativeRoundTrip("FLEX", g), "FLEX: preset identity and controls rebuilt from the Gridline state");
    }
  }

  // ---- DirectWave: zones patched on a shipped program.
  if (flInstalled()) {
    namespace smp = gl::packs::samplers;
    expect(genaPatch("DirectWave", 0, [](gl::PluginState& g) {
      smp::ZoneMap zm = smp::parseZoneMap(g.data);
      if (zm.zones.size() < 2) return;
      zm.zones[0].root = 48, zm.zones[0].gain = -6.0f, zm.zones[0].pan = 0.25f, zm.zones[0].loop = smp::LoopOff;
      zm.zones.pop_back();
      g.data = smp::serializeZoneMap(zm);
    }), "DirectWave: zone root / gain / pan / loop and zone count encode exactly");
    genaCorpus("DirectWave", "DirectWave", 1000);  // no defaults: native writes none
  }

  // ---- Harmless.
  {
    namespace ad = gl::packs::models;
    expect(defaultsDecode("Harmless", "gl.additive"), "Harmless: defaults decode");
    gl::PluginState g;
    bool ok = genaEdited("Harmless", "gl.additive", [](Bytes& b) {
      putI32(b, 4 + 54 * 4, 4000);   // cutoff
      putI32(b, 4 + 59 * 4, 300);    // resonance
      putI32(b, 4 + 65 * 4, 4);      // unison order
      putI32(b, 4 + 400 + 16, 3);    // timbre colour
    }, g);
    expect(ok, "Harmless: synthetic state decodes");
    if (ok) {
      g.params[ad::AdPlayMode] = ad::ApMono;
      g.params[ad::AdFx + ad::FxChorus] = 0.5f;
      ad::AdditiveData t;
      t.level = {1.0f, 0.5f, 0.25f}, t.phase = {0.0f, 0.1f, 0.2f};
      g.data = ad::serializeAdditiveData(t);
      expect(nativeRoundTrip("Harmless", g), "Harmless: filter, unison, colour, mono, chorus, custom table encode back exactly");
    }
    genaCorpus("Harmless", "Harmless", 0, 120);
  }

  // ---- Harmor: no default state; part / global / FX numbers on shipped states.
  if (flInstalled()) {
    namespace hm = gl::packs::models::hm;
    expect(genaPatch("Harmor", 0, [](gl::PluginState& g) {
      g.params[hm::flt(0, 0, hm::PfF1Cut)] = 0.5f;
      g.params[hm::pf(1, hm::PfUniOrder)] = 5;
      g.params[hm::GmPitch] = 700;
      g.params[hm::FxRvOn] = 1;
    }), "Harmor: filter cutoff, unison, pitch, reverb switch encode exactly");
    genaCorpus("Harmor", "Harmor", 1000, 60);
  }

  // ---- Sytrus: zlib patch re-packed, solved, compressed again.
  if (flInstalled()) {
    namespace mf = gl::packs::synths::mfm;
    Bytes out;
    expect(genaPatch("Sytrus", 0, [](gl::PluginState& g) {
      g.params[mf::fm(0, 1)] = 0.5f;
      g.params[mf::op(1, mf::OpRatio)] = 2.0f;
      g.params[mf::flt(0, mf::FlCut)] = 0.25f;
      g.params[mf::MModX] = 0.75f;
    }, &out), "Sytrus: matrix cell, ratio, cutoff, mod X encode exactly");
    // The stream is compressed and its adler-32 trailer matches the patch.
    std::vector<uint8_t> raw;
    const bool inflated = out.size() > 10 && gl::flp::gen::inflateZlib(gl::flp::gen::Bytes(out.data() + 4, out.size() - 4), raw);
    uint32_t a = 1, s = 0;
    for (uint8_t x : raw) a = (a + x) % 65521u, s = (s + a) % 65521u;
    const size_t n = out.size();
    const uint32_t trailer = n >= 4 ? (static_cast<uint32_t>(out[n - 4]) << 24 | static_cast<uint32_t>(out[n - 3]) << 16 |
                                       static_cast<uint32_t>(out[n - 2]) << 8 | out[n - 1])
                                    : 0;
    expect(inflated && trailer == ((s << 16) | a), "Sytrus: re-compressed stream with a valid adler-32");
    expect(inflated && out.size() < raw.size() / 2, "Sytrus: the written stream is compressed");
    genaCorpus("Sytrus", "Sytrus", 1000, 60);
  }
}

}  // namespace enctest
