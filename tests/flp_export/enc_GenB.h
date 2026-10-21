// Gridline — FLP export tests: GenB plugin encoders (FlpEncGenB.cpp): Keyboard
// Controller, Voltage Controller, FPC, DrumSynth Live, Fruity Slicer, Slicex,
// BeepMap, MiniSynth, Toxic Biohazard.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include "core/PluginParams.h"
#include "engine/Slicing.h"
#include "enc_common.h"
#include "packs/models/models.h"
#include "packs/modulation/modulation.h"
#include "packs/modulation/modulation_dsp.h"

namespace enctest {

inline bool genbDecode(const std::string& flName, const Bytes& b, const std::string& glType, gl::PluginState& out) {
  gl::flp::PluginMapping m;
  if (!enc::decodeState(flName, b, m, ctx()) || m.state.type != glType) return false;
  out = m.state;
  return true;
}

// The encoder's defaults with stored values changed, decoded.
inline bool genbEdited(const std::string& flName, const std::string& glType, const std::function<void(Bytes&)>& edit, gl::PluginState& out) {
  const enc::Encoder* e = enc::findEncoder(flName, glType);
  if (!e || !e->defaults) return false;
  Bytes b = e->defaults();
  edit(b);
  return genbDecode(flName, b, glType, out);
}

// Encodes `g` as `flName` on `origin` (the encoder's defaults when null);
// GENB_DEBUG=1 prints what does not decode back.
inline enc::Result genbEncode(const std::string& flName, const gl::PluginState& g, const Bytes* origin = nullptr) {
  Bytes t;
  if (!origin) {
    const enc::Encoder* e = enc::findEncoder(flName, g.type);
    if (e && e->defaults) t = e->defaults();
    origin = &t;
  }
  enc::Result r = enc::encode(g, flName, origin, ctx());
  if (std::getenv("GENB_DEBUG") && r.quality != enc::Result::Exact && r.quality != enc::Result::Verbatim) {
    gl::flp::PluginMapping m;
    enc::decodeState(flName, r.state, m, ctx());
    std::printf("# %s: %s\n", flName.c_str(), enc::qualityName(r.quality));
    for (int k : r.mismatched)
      if (k >= 0 && static_cast<size_t>(k) < g.params.size() && static_cast<size_t>(k) < m.state.params.size())
        std::printf("#   param %d want %g got %g\n", k, static_cast<double>(g.params[static_cast<size_t>(k)]),
                    static_cast<double>(m.state.params[static_cast<size_t>(k)]));
    if (r.dataMismatch) std::printf("#   data want:\n%s\n#   data got:\n%s\n", g.data.c_str(), m.state.data.c_str());
  }
  return r;
}
inline bool genbExact(const std::string& flName, const gl::PluginState& g) {
  const enc::Result r = genbEncode(flName, g);
  return r.quality == enc::Result::Exact || r.quality == enc::Result::Verbatim;
}

// `nativeShort`: shipped states that cannot encode exactly from the defaults.
inline void genbCorpus(const char* flName, const std::string& what, int nativeShort, size_t max = 400) {
  if (!flInstalled()) return;
  Stats s = corpusRoundTrip(flName, max);
  expect(s.decoded > 0 && s.nativeExact + nativeShort >= s.decoded, (what + ": shipped states encode natively").c_str());
  expect(s.patchExact == s.decoded, (what + ": unchanged states are written verbatim").c_str());
}

// A small 16-bit mono WAV (frames of silence) in the temp folder.
inline std::string genbWav(const std::string& name, uint32_t frames) {
  gl::flp::ByteWriter w;
  w.bytes("RIFF", 4);
  w.u32(36 + 2 * frames);
  w.bytes("WAVEfmt ", 8);
  w.u32(16);
  w.u16(1), w.u16(1), w.u32(44100), w.u32(88200), w.u16(2), w.u16(16);
  w.bytes("data", 4);
  w.u32(2 * frames);
  w.zeros(2 * frames);
  const std::string path = (std::filesystem::temp_directory_path() / name).generic_string();  // FL states keep / separators
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(w.b.data()), static_cast<std::streamsize>(w.b.size()));
  return path;
}

inline void testEncGenB() {
  using gl::flp::putF32;
  using gl::flp::putI32;
  namespace pp = gl::pp;
  namespace mod = gl::packs::modulation;
  namespace md = gl::packs::models;

  // ---- Keyboard Controller: smoothing words + key values (data text).
  {
    expect(defaultsDecode("Fruity Keyboard Controller", "gl.keyctl"), "Keyboard Controller: defaults decode");
    gl::PluginState g;
    bool ok = genbEdited("Fruity Keyboard Controller", "gl.keyctl", [](Bytes& b) { putI32(b, 4, 128), putI32(b, 8, 40); }, g);
    std::vector<float> keys = mod::parseKeyValues(g.data);
    for (size_t k = 0; k < keys.size(); ++k) keys[k] = static_cast<float>((k * 7) % 128) / 127.0f;
    g.data = mod::formatKeyValues(keys);
    expect(ok && nativeRoundTrip("Fruity Keyboard Controller", g), "Keyboard Controller: smoothing and a key map encode back exactly");
    const enc::Encoder* e = enc::nativeEncoder(g);
    expect(e && e->flName == "Fruity Keyboard Controller", "Keyboard Controller: native home of gl.keyctl");
    genbCorpus("Fruity Keyboard Controller", "Keyboard Controller", 0);
  }

  // ---- Voltage Controller: offsets, scales, sources, clock, pulse, pitch scaling.
  {
    expect(defaultsDecode("Fruity Voltage Controller", "gl.cvctl"), "Voltage Controller: defaults decode");
    gl::PluginState g;
    bool ok = genbEdited("Fruity Voltage Controller", "gl.cvctl", [](Bytes& b) {
      putI32(b, 4, 40000), putI32(b, 8, 1000);   // offsets
      putI32(b, 0x24, 30000);                    // left scale
      b[0x44] = 8, b[0x45] = 2, b[0x4C] = 1;     // clock / trigger, enabled
      putI32(b, 0x63, 96), putI32(b, 0x67, 441);  // 96 PPQ, 10 ms pulse
      gl::flp::putF64(b, 0x6B, 1.2);             // 1.2 V/oct
    }, g);
    expect(ok && nativeRoundTrip("Fruity Voltage Controller", g), "Voltage Controller: an edited output encodes back exactly");
    genbCorpus("Fruity Voltage Controller", "Voltage Controller", 0);
  }

  // ---- MiniSynth: 43 floats after "0rev".
  {
    expect(defaultsDecode("MiniSynth", "gl.pocketsynth"), "MiniSynth: defaults decode");
    gl::PluginState g;
    bool ok = genbEdited("MiniSynth", "gl.pocketsynth", [](Bytes& b) {
      const size_t at = 4 + std::string(b.begin(), b.end()).find("0rev");
      putF32(b, at + 4 * 5, 7.0f / 9.0f);  // spread saw (fifth)
      putF32(b, at + 4 * 6, 0.3f);         // osc mod
      putF32(b, at + 4 * 8, 0.6f);         // cutoff
      putF32(b, at + 4 * 20, 0.2f);        // LFO amount (negative side)
      putF32(b, at + 4 * 18, 0.5f);        // LFO → cutoff
      putF32(b, at + 4 * 23, 0.7f);        // sustain
    }, g);
    expect(ok && nativeRoundTrip("MiniSynth", g), "MiniSynth: an edited patch encodes back exactly");
    // Patch path: moving the oscillator shape changes both oscillators (one
    // stored model), so only the native check is complete.
    genbCorpus("MiniSynth", "MiniSynth", 0);
  }

  // ---- BeepMap: tag words + the canvas as a bitmap.
  {
    // J-33: BeepMap → Pixel Tone (raw controls, the picture as RGB text).
    expect(defaultsDecode("BeepMap", "gl.pixeltone"), "BeepMap: defaults decode");
    gl::PluginState g;
    bool ok = genbEdited("BeepMap", "gl.pixeltone", [](Bytes& b) { putI32(b, 4, 4800), putI32(b, 12, 2), putI32(b, 20, 1); }, g);
    std::string pic = "P 8 4\n";
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 8; ++x) pic += (x / 2 == y) ? "FF8000" : "000000";
      pic += "\n";
    }
    g.data = pic;
    expect(ok && nativeRoundTrip("BeepMap", g), "BeepMap: a picture and edited controls encode back exactly");
    genbCorpus("BeepMap", "BeepMap", 0);
  }

  // ---- Toxic Biohazard → Hex Synth (J-33): imported states round trip verbatim.
  genbCorpus("Toxic Biohazard", "Toxic Biohazard", 1000);

  // ---- DrumSynth Live: selected key + 16 patches (joint search of coupled values).
  {
    expect(defaultsDecode("Fruity DrumSynth Live", "gl.drumkit"), "DrumSynth Live: defaults decode");
    gl::PluginState g;
    bool ok = genbEdited("Fruity DrumSynth Live", "gl.drumkit", [](Bytes& b) {
      putI32(b, 4, 36);  // selected key: pads from C3
      const size_t rec = 8 + 120;  // unnamed patches
      auto v = [&](int key, int i, int32_t x) { putI32(b, rec + 152 * static_cast<size_t>(key) + 4 * static_cast<size_t>(i), x); };
      v(36, 0, 110), v(36, 8, 100), v(36, 14, 30), v(36, 4, 200);  // pad 1: membrane
      v(37, 32, 1), v(37, 14, 90), v(37, 0, 60);                    // pad 2: noise band (plate)
      v(38, 10, 700), v(38, 2, -20), v(38, 24, 3);                  // pad 3: noise tone, choke
      v(39, 8, 10), v(39, 14, 20);                                  // pad 4: quiet both (plate)
    }, g);
    expect(ok && nativeRoundTrip("Fruity DrumSynth Live", g), "DrumSynth Live: coupled level / noise / tone values encode back exactly");
    genbCorpus("Fruity DrumSynth Live", "DrumSynth Live", 0);
  }

  // ---- Fruity Slicer: path, slice starts, base note, pitch.
  {
    expect(defaultsDecode("Fruity Slicer", "gl.slicer"), "Fruity Slicer: defaults decode");
    gl::PluginState g;
    bool ok = genbDecode("Fruity Slicer", enc::findEncoder("Fruity Slicer", "gl.slicer")->defaults(), "gl.slicer", g);
    g.data = genbWav("gridline_genb_slicer.wav", 44100);
    const float marks[] = {0.0f, 0.25f, 0.3f, 0.71f};  // whole frames of 44100
    for (int k = 0; k < 4; ++k) g.params[pp::slMarker(k)] = marks[k];
    g.params[pp::SlBaseNote] = 48;
    // J-41: the processing settings (engine 2): method, fades, declick, a reversed slice.
    g.params[pp::SlProcMethod] = 2, g.params[pp::SlProcFadeIn] = 12, g.params[pp::SlProcFadeOut] = 80, g.params[pp::SlProcDeclick] = 0;
    g.params[pp::slParam(2, pp::SliceReverse)] = 1;
    expect(ok && g.params[pp::SlEngine] == pp::SlEngineProcessed && genbExact("Fruity Slicer", g),
           "Fruity Slicer: sample, four slices, base note, method, fades, declick and a reversed slice encode back exactly");
    {  // stretch amount and pitch (cents) come back as written (the stretch itself is approximate)
      gl::PluginState h = g;
      h.params[pp::SlProcPitch] = -300, h.params[pp::SlProcFit] = 167;
      enc::Result r = enc::encode(h, "Fruity Slicer", nullptr, ctx());
      gl::flp::PluginMapping back;
      expect(r.hasState && enc::decodeState("Fruity Slicer", r.state, back, ctx()) && back.state.params.size() > pp::SlProcFit && back.state.params[pp::SlProcPitch] == -300.0f &&
                 back.state.params[pp::SlProcFit] == 167.0f && !back.exact,
             "Fruity Slicer: stretch amount and pitch encode back (approximate: Gridline's own stretcher)");
    }
    const enc::Encoder* e = enc::nativeEncoder(g);
    expect(e && e->flName == "Fruity Slicer", "Fruity Slicer: native home of gl.slicer");
    genbCorpus("Fruity Slicer", "Fruity Slicer", 0);
    genbCorpus("Fruity slicer", "Fruity slicer (old name)", 0);
  }

  // ---- Slicex: no default state; slices rewritten into an embedded copy.
  {
    const enc::Encoder* e = enc::findEncoder("Slicex", "gl.slicer");
    expect(e && e->nativeRank == 0 && e->accepts && !e->accepts(gl::defaultPluginState("gl.slicer")),
           "Slicex: native home of region-model slicers only (J-41)");
    // A minimal state: header, embedded 1000-frame WAV, settings with one region.
    const std::string wavPath = genbWav("gridline_genb_slicex.wav", 1000);
    std::ifstream f(wavPath, std::ios::binary);
    Bytes riff((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    gl::flp::ByteWriter w;
    w.zeros(0x60);
    w.u8(0);  // no path
    w.u32(static_cast<uint32_t>(riff.size()));
    w.bytes(riff);
    w.zeros(0x18C);
    w.u32(1);
    for (int32_t v : {0, 0, 0, 8000, 0, 12800, 128, 128, 0, 12800, 0, 0, 12800, 0, 0, 0, 12800}) w.i32(v);
    putI32(w.b, 0x0C, 12800);
    gl::PluginState g;
    bool ok = genbDecode("Slicex", w.b, "gl.slicer", g);
    g.params[pp::slMarker(1)] = 0.25f, g.params[pp::slMarker(2)] = 0.6f;
    g.params[pp::slParam2(1, pp::SliceEnd)] = 0.5f;
    // J-33 laws: region (3^(v/8000) − 1)/2 → volume 4000; master (11^(v/12800) − 1)/10 → 6400.
    g.params[pp::slParam(2, pp::SliceGain)] = static_cast<float>(20.0 * std::log10((std::pow(3.0, 4000.0 / 8000.0) - 1.0) / 2.0));
    g.params[pp::SlVolume] = static_cast<float>(std::clamp((std::pow(11.0, 6400.0 / 12800.0) - 1.0) / 10.0, 0.0, 1.0));
    enc::Result r = genbEncode("Slicex", g, &w.b);
    // The embedded copy gets a new cache file name, so only the parameters are compared.
    expect(ok && r.hasState && r.mismatched.empty(), "Slicex: slices, a region end and region settings encode back exactly");
    genbCorpus("Slicex", "Slicex", 1000);
  }

  // ---- FPC: pads, layers, sample paths, names, colours, cut groups, envelopes.
  {
    expect(defaultsDecode("FPC", "gl.pads"), "FPC: defaults decode");
    gl::PluginState g;
    bool ok = genbDecode("FPC", enc::findEncoder("FPC", "gl.pads")->defaults(), "gl.pads", g);
    // Data text in the decoder's order: bank A layer-1 lines, then per pad
    // its name, colour, other layers and envelopes, bank B pads in turn.
    std::string d;
    for (int k = 0; k < pp::kPads; ++k) d = gl::setPadSamplePath(d, k, 0, k == 0 ? "/tmp/genb kick.wav" : "none");
    d = gl::padDataSet(d, "N1", "Kick");
    d = gl::padDataSet(d, "C1", "FF8000");
    d = gl::setPadSamplePath(d, 0, 1, "/tmp/genb kick 2.wav");
    d = gl::padDataSet(d, "E1.v", "1 1 -1 -1 0,0,0 0.1,1,0.2 0.5,0,0");
    for (int k = pp::kPads; k < pp::kPadsAll; ++k) d = gl::setPadSamplePath(d, k, 0, k == 20 ? "/tmp/genb clap.wav" : "none");
    g.data = d;
    g.params[pp::padL(0, 0, pp::LayEnabled)] = 1, g.params[pp::padL(0, 1, pp::LayEnabled)] = 1;
    g.params[pp::padL(20, 0, pp::LayEnabled)] = 1;
    g.params[pp::padL(0, 0, pp::LayVelHigh)] = 63, g.params[pp::padL(0, 1, pp::LayVelLow)] = 64;
    g.params[pp::padL(0, 1, pp::LayTune)] = 1.5f;
    g.params[pp::padP(0, pp::PadLevel)] = 1.0f, g.params[pp::padP(2, pp::PadPan)] = -0.5f;
    g.params[pp::padP(2, pp::PadCut)] = 1, g.params[pp::padE(3, pp::PadCutBy)] = 1;
    g.params[pp::padE(0, pp::PadLayerMode)] = pp::LayerRandom;
    g.params[pp::padE(5, pp::PadNote)] = 70;
    gl::flp::PluginMapping m;
    expect(ok && genbExact("FPC", g), "FPC: an edited kit encodes back exactly");
    const enc::Encoder* e = enc::nativeEncoder(g);
    expect(e && e->flName == "FPC", "FPC: native home of gl.pads");
    genbCorpus("FPC", "FPC", 0);
  }
}

}  // namespace enctest
