// Gridline — FLP export tests: Dec3 plugin encoders (FlpEncDec3.cpp).
#pragma once
#include <functional>
#include <string>
#include "enc_common.h"
#include "packs/modulation/modulation.h"
#include "packs/spacefx/spacefx.h"

namespace enctest {

// The encoder's defaults edited, decoded: a non-default Gridline state.
inline bool dec3Edited(const std::string& flName, const std::string& glType, const std::function<void(Bytes&)>& edit, gl::PluginState& out) {
  const enc::Encoder* e = enc::findEncoder(flName, glType);
  if (!e || !e->defaults) return false;
  Bytes b = e->defaults();
  edit(b);
  gl::flp::PluginMapping m;
  if (!enc::decodeState(flName, b, m, ctx()) || m.state.type != glType) return false;
  out = m.state;
  return true;
}

// Defaults decode; an edited state round-trips natively.
inline void dec3Synthetic(const char* flName, const char* glType, const std::string& what, const std::function<void(Bytes&)>& edit) {
  expect(defaultsDecode(flName, glType), (what + ": defaults decode").c_str());
  gl::PluginState g;
  bool ok = dec3Edited(flName, glType, edit, g);
  expect(ok && !(g == gl::defaultPluginState(glType)), (what + ": edited state decodes to a non-default state").c_str());
  expect(ok && nativeRoundTrip(flName, g), (what + ": edited state encodes back exactly").c_str());
}

// Shipped states: `exact` of them encode natively (-1: all), all written verbatim.
inline void dec3Corpus(const char* flName, const std::string& what, int exact = -1) {
  if (!flInstalled()) return;
  Stats s = corpusRoundTrip(flName);
  const int want = exact < 0 ? s.decoded : exact;
  expect(s.decoded > 0 && s.nativeExact >= want, (what + ": shipped states encode exactly (" + std::to_string(s.nativeExact) + "/" + std::to_string(s.decoded) + ")").c_str());
  expect(s.patchExact == s.decoded, (what + ": unchanged states are written verbatim").c_str());
}

inline void testEncDec3() {
  using gl::flp::putI32;
  using gl::flp::putU8;
  namespace mod = gl::packs::modulation;

  dec3Synthetic("Fruity Peak Controller", "gl.peakctl", "Peak Controller", [](Bytes& b) {
    putI32(b, 0, 30000), putI32(b, 4, -100), putI32(b, 12, 20), putI32(b, 28, 9000), putI32(b, 32, 2), putU8(b, 40, 1);
  });
  dec3Corpus("Fruity Peak Controller", "Peak Controller");

  // Center's default (removing DC) is the non-default Gridline utility.
  dec3Synthetic("Fruity Center", "gl.utility", "Center", [](Bytes& b) { putI32(b, 4, 1); });
  dec3Corpus("Fruity Center", "Center");
  {
    // A native utility that only removes DC is written as Center.
    gl::PluginState g = gl::defaultPluginState("gl.utility");
    g.params[gl::pp::UtlDcRemove] = 1;
    enc::Result r = enc::encode(g, "", nullptr, ctx());
    expect(r.flName == "Fruity Center" && r.quality == enc::Result::Exact, "Center: native DC removal exports as Center");
  }

  dec3Synthetic("Fruity Spectroman", "gl.spectro", "Spectroman", [](Bytes& b) { putI32(b, 8, 96), putI32(b, 12, 40), putU8(b, 16, 0); });
  dec3Corpus("Fruity Spectroman", "Spectroman");

  dec3Synthetic("Tuner", "gl.tuner", "Tuner", [](Bytes& b) { putI32(b, 4, 1200), putI32(b, 12, 1), putI32(b, 16, 1); });
  dec3Corpus("Tuner", "Tuner");

  dec3Synthetic("Fruity 7 Band EQ", "gl.geq", "7 Band EQ", [](Bytes& b) { putI32(b, 4, 675), putI32(b, 16, -1800), putI32(b, 28, 1250); });
  dec3Corpus("Fruity 7 Band EQ", "7 Band EQ");

  dec3Synthetic("Transmitter", "gl.transplit", "Transmitter", [](Bytes& b) {
    putI32(b, 4 + 4 * 0, 4000), putI32(b, 4 + 4 * 3, 11000), putI32(b, 4 + 4 * 6, 2500);
    putI32(b, 4 + 4 * 38, 4), putI32(b, 4 + 4 * 25, 7000), putI32(b, 4 + 4 * 28, 3000), putI32(b, 4 + 4 * 26, 6000);  // echo slot
    putI32(b, 4 + 4 * 39, 1), putI32(b, 4 + 4 * 22, 5000);                                                     // bounce slot
  });
  dec3Corpus("Transmitter", "Transmitter");

  dec3Synthetic("Multiband Delay", "gl.banddelay", "Multiband Delay", [](Bytes& b) {
    for (int k = 0; k < 16; ++k) putI32(b, 8 + 4 * (16 + k), static_cast<int32_t>((0.2 + 0.05 * k) * 1073741824.0));  // bank 0 volume
    for (int k = 0; k < 16; ++k) putI32(b, 8 + 4 * (48 + 32 + k), static_cast<int32_t>((k % 2 ? 0.3 : 0.7) * 1073741824.0));  // bank 1 pan
    putI32(b, 8 + 4 * 386, 2500), putI32(b, 8 + 4 * 390, 400), putI32(b, 8 + 4 * 391, 64);
  });
  {
    // J-42: band-delay model states carry the 384 band values verbatim; a full morph
    // and a linear delay curve in bank 8 keep every parameter. (Spectral-engine states
    // export through the reference's controls and stay approximate.)
    gl::PluginState g = gl::defaultPluginState("gl.banddelay");
    namespace sfx = gl::packs::spacefx;
    g.params[sfx::BdModel] = 1;
    std::string d = sfx::formatBandCurves(sfx::defaultBandCurves()) + "\nmbd8";
    for (int i = 0; i < 384; ++i) {
      const int t = (i / 16) % 3, k = i % 16;
      char buf[24];
      std::snprintf(buf, sizeof buf, " %.9g", t == 0 ? k / 15.0 : t == 1 ? 1.0 : 0.5);
      d += buf;
    }
    g.data = d;
    g.params[sfx::BdRMorph] = 1.0f;
    g.params[sfx::BdRScale] = 0.75f;
    const enc::Encoder* e = enc::findEncoder("Multiband Delay", "gl.banddelay");
    Bytes t = e ? e->defaults() : Bytes{};
    enc::Result r = enc::encode(g, "Multiband Delay", &t, ctx());
    bool refKept = true;
    for (int k = sfx::BdModel; k < sfx::BdTotal; ++k)
      for (int m : r.mismatched) refKept = refKept && m != k;
    expect(r.quality != enc::Result::Defaults && refKept, "Multiband Delay: a full morph and a linear delay curve keep every band-delay parameter");
  }
  dec3Corpus("Multiband Delay", "Multiband Delay");

  dec3Synthetic("VFX Key Mapper", "gl.keymap", "Key Mapper", [](Bytes& b) {
    putI32(b, 12, 5);                     // offset +5
    gl::flp::putU16(b, 24 + 6 * 4 + 4, 7);  // E → G (muting changes record sizes: see below)
  });
  {
    // Gridline maps in multi mode: keys play the mapped and the original note.
    gl::PluginState g = gl::defaultPluginState("gl.keymap");
    g.params[mod::KmMap + 0] = 4, g.params[mod::KmMap + 7] = -3, g.params[mod::KmMap + 11] = -13, g.params[mod::KmMulti] = 1;
    expect(nativeRoundTrip("VFX Key Mapper", g), "Key Mapper: multi-mode maps and a muted key encode back exactly");
  }
  dec3Corpus("VFX Key Mapper", "Key Mapper");

  {
    expect(defaultsDecode("VFX Keyboard Splitter", "gl.split"), "Keyboard Splitter: defaults decode");
    gl::PluginState g = gl::defaultPluginState("gl.split");
    auto sp = [&](int z, int f, float v) { g.params[static_cast<size_t>(mod::splitParam(z, f))] = v; };
    sp(0, mod::SpOn, 1), sp(0, mod::SpLow, 0), sp(0, mod::SpHigh, 59), sp(0, mod::SpTranspose, 12);
    sp(0, mod::SpVelLow, 0), sp(0, mod::SpVelHigh, 1);
    sp(1, mod::SpOn, 1), sp(1, mod::SpLow, 60), sp(1, mod::SpHigh, 127), sp(1, mod::SpTranspose, -7);
    sp(1, mod::SpVelLow, 32 / 127.0f), sp(1, mod::SpVelHigh, 100 / 127.0f);
    for (int z = 2; z < mod::kSplitZones; ++z) sp(z, mod::SpOn, 0);
    expect(nativeRoundTrip("VFX Keyboard Splitter", g), "Keyboard Splitter: two key / velocity zones encode back exactly");
  }
  dec3Corpus("VFX Keyboard Splitter", "Keyboard Splitter");

  {
    expect(defaultsDecode("VFX Script", "gl.notescript"), "VFX Script: defaults decode");
    gl::flp::PluginMapping m;
    const enc::Encoder* e = enc::findEncoder("VFX Script", "gl.notescript");
    bool pass = e && enc::decodeState("VFX Script", e->defaults(), m, ctx()) && m.exact;
    expect(pass, "VFX Script: the default script is an exact pass-through");
  }
  dec3Corpus("VFX Script", "VFX Script");

  // VFX Envelope: no default state; shipped states are written verbatim and
  // point edits are solved in place.
  if (flInstalled()) {
    std::vector<Bytes> v = corpusStates("VFX Envelope", 1);
    gl::flp::PluginMapping m;
    bool ok = !v.empty() && enc::decodeState("VFX Envelope", v[0], m, ctx());
    if (ok) {
      gl::PluginState g = m.state;
      g.params[static_cast<size_t>(mod::envPointLevel(0, 1))] = 0.25f;
      enc::Result r = enc::encode(g, "VFX Envelope", &v[0], ctx());
      expect(r.quality == enc::Result::Exact, "VFX Envelope: a moved point level encodes exactly");
    } else {
      expect(false, "VFX Envelope: shipped state decodes");
    }
    Stats s = corpusRoundTrip("VFX Envelope");
    expect(s.patchExact == s.decoded, "VFX Envelope: unchanged states are written verbatim");
  }

  // Big Clock: one colour scheme byte.
  expect(defaultsDecode("Fruity Big Clock", "gl.clock"), "Big Clock: defaults decode");
  dec3Corpus("Fruity Big Clock", "Big Clock");

  dec3Synthetic("VFX Color Mapper", "gl.colormap", "Color Mapper", [](Bytes& b) { putI32(b, 4, 3), putI32(b, 8, 0), putI32(b, 60, 5); });
  dec3Corpus("VFX Color Mapper", "Color Mapper", 0);  // FL's default maps colours 8..15 to ports 8..15 (Gridline has 8 targets)

  // Decoders that read no setting: unchanged instances keep FL's state.
  dec3Corpus("VFX Level Scaler", "Level Scaler", 0);
  dec3Corpus("VFX Sequencer", "VFX Sequencer", 0);
  dec3Corpus("Fruity Slicer 2", "Slicer 2", 0);
  dec3Corpus("Dashboard", "Dashboard", 0);
  dec3Corpus("FL Studio Mobile Rack FX", "Mobile Rack FX", 0);
  {
    expect(enc::findEncoder("Wasp", "gl.analog") && enc::findEncoder("Fruity LSD", "gl.midiout") && enc::findEncoder("FL Slayer", "gl.string") &&
               enc::findEncoder("Fruity Mute 2", "gl.mute") && enc::findEncoder("Soundfont Player", "gl.multisampler") &&
               enc::findEncoder("FL Studio Mobile Rack FX", "gl.pocketrack"),
           "Dec4: defaults-only plugins have encoders");
    gl::PluginState g = gl::defaultPluginState("gl.mute");
    enc::Result r = enc::encode(g, "", nullptr, ctx());
    expect(r.flName == "Fruity Mute 2" && r.quality == enc::Result::Defaults, "Mute 2: a native mute exports as Mute 2 with FL's defaults");
  }
}

}  // namespace enctest
