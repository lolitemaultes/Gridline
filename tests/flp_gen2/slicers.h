// Gridline — second-wave instrument decoder tests: slicers (included by test_flp_gen2.cpp).
// Slicex, Fruity Slicer, Fruity Granulizer and Wave Traveller states built
// from scratch here; samples are small synthetic WAV files in the build folder.
#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "../../src/core/PluginParams.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Sample.h"
#include "../../src/packs/models/metal.h"
#include "../../src/packs/samplers/samplers.h"
#include "Gen2Test.h"
#include "core/Platform.h"

namespace gen2test {

namespace slicers {

// 16-bit mono 44.1 kHz WAV (a decaying 220 Hz tone) with optional cue points.
inline std::vector<uint8_t> wavBytes(uint32_t frames, const std::vector<uint32_t>& cues) {
  Bytes data;
  for (uint32_t i = 0; i < frames; ++i) {
    double v = 0.6 * std::sin(2 * 3.14159265358979 * 220.0 * i / 44100.0) * std::exp(-3.0 * (i % 4410) / 4410.0);
    data.u16(static_cast<uint16_t>(static_cast<int16_t>(v * 32767)));
  }
  Bytes body;
  body.str("WAVE", false);
  body.str("fmt ", false).u32(16).u16(1).u16(1).u32(44100).u32(88200).u16(2).u16(16);
  body.str("data", false).u32(static_cast<uint32_t>(data.size())).raw(data.b);
  if (!cues.empty()) {
    body.str("cue ", false).u32(static_cast<uint32_t>(4 + 24 * cues.size())).u32(static_cast<uint32_t>(cues.size()));
    for (size_t k = 0; k < cues.size(); ++k) body.u32(static_cast<uint32_t>(k + 1)).u32(cues[k]).str("data", false).u32(0).u32(0).u32(cues[k]);
  }
  Bytes riff;
  riff.str("RIFF", false).u32(static_cast<uint32_t>(body.size())).raw(body.b);
  return riff.b;
}

inline std::string writeFile(const std::string& name, const std::vector<uint8_t>& bytes) {
  std::filesystem::create_directories("gen2-slicers");
  std::string p = std::filesystem::absolute("gen2-slicers/" + name).string();
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return p;
}

// Slicex state: header (level, pitch), path, optional embedded RIFF, settings block with the region count.
inline std::vector<uint8_t> slicexState(bool external, const std::string& path, const std::vector<uint8_t>& riff, uint32_t regions,
                                        int32_t level = 12800, int32_t cents = 0) {
  Bytes s;
  s.u16(3).u16(6).u32(0).u32(0).i32(level).u32(256).u32(128).i32(cents).u32(256).u32(256);
  s.pad(0x5F);
  s.u8(external ? 1 : 0);
  s.u8(static_cast<uint8_t>(path.size())).str(path, false);
  if (!external) s.u32(static_cast<uint32_t>(riff.size())).raw(riff);
  size_t settings = s.size();
  s.pad(settings + 0x18C);
  s.u32(regions);
  for (uint32_t k = 0; k < regions; ++k) s.u32(0).u32(0).u32(0).u32(8000).u32(0).u32(12800).u32(128).u32(128).u32(0).u32(12800).u32(0).u32(0).u32(12800).u32(0).u32(0).u32(0).u32(12800);
  s.zeros(64);
  return s.b;
}

// Plays one note on a decoded instrument and returns the RMS of the first 0.25 s.
inline double noteRms(const gl::PluginState& st, double pitch) {
  gl::ChannelAux aux;
  gl::prepareChannelAux(st, aux);
  auto gen = gl::createGenerator(st.type);
  if (!gen) return -1;
  gl::ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 120;
  std::vector<float> L(11025), R(11025);
  gen->noteOn(1, pitch, 1.0f, 0);
  for (size_t p = 0; p < L.size(); p += 245) gen->process(st, aux, ctx, L.data() + p, R.data() + p, 245);
  double s = 0;
  for (size_t i = 0; i < L.size(); ++i) s += L[i] * L[i] + R[i] * R[i];
  return std::sqrt(s / (2.0 * static_cast<double>(L.size())));
}

}  // namespace slicers

inline void testSlicers() {
  using namespace gl::pp;
  using namespace slicers;
  gl::platform::setEnv("GRIDLINE_CACHE_DIR", std::filesystem::absolute("gen2-slicers/cache").string().c_str());
  for (const char* n : {"Slicex", "Fruity Slicer", "Fruity slicer", "Fruity Granulizer", "Fruity granulizer", "Wave Traveller"})
    expect(registered(n), (std::string("decoder registered: ") + n).c_str());

  // ---- Slicex with the sample embedded in the state (cue points = regions).
  {
    Run r;
    decode(r, "Slicex", slicexState(false, "%FLStudioFactoryData%\\Data\\gone.wav", wavBytes(1000, {0, 250, 500}), 3, 12800, 300));
    expect(r.ok && r.m.glType == "gl.slicer", "Slicex → gl.slicer");
    expect(near(param(r, SlMode), SlModeManual), "Slicex: manual markers");
    expect(near(param(r, slMarker(0)), 0) && near(param(r, slMarker(1)), 0.25) && near(param(r, slMarker(2)), 0.5) && param(r, slMarker(3)) < 0,
           "Slicex: cue points → markers 0 / 0.25 / 0.5");
    expect(near(param(r, SlPitch), 3) && near(param(r, SlVolume), 1) && near(param(r, SlBaseNote), 60), "Slicex: master pitch, level, base note");
    gl::SamplePtr smp = gl::sampleCache().get(r.m.state.data);
    expect(smp && smp->frames() == 1000, "Slicex: embedded sample extracted to the cache and loadable");
    expect(!r.m.exact, "Slicex marked approximate");
    expect(r.m.param && r.m.param(5) == SlPitch && r.m.param(99) == -1 && near(r.m.value(5, 0.75), 6), "Slicex: pitch automation link");
    expect(noteRms(r.m.state, 61) > 0.01, "Slicex: second slice is audible");
    Run again;
    decode(again, "Slicex", slicexState(false, "%FLStudioFactoryData%\\Data\\gone.wav", wavBytes(1000, {0, 250, 500}), 3));
    expect(again.m.state.data == r.m.state.data, "Slicex: same embedded sample reuses its cache file");
  }
  // ---- Slicex referencing a file (by %FLStudioData%, i.e. the factory Data folder): cues come from the file.
  {
    std::filesystem::create_directories("gen2-slicers/fl/Data/Loops");
    std::string file = writeFile("fl/Data/Loops/loop.wav", wavBytes(2000, {0, 500, 1000, 1500}));
    Run r;
    decode(r, "Slicex", slicexState(true, "%FLStudioData%\\Loops\\loop.wav", {}, 4, 6400, -1200),
           {}, std::filesystem::absolute("gen2-slicers/fl").string());
    expect(r.ok && std::filesystem::equivalent(r.m.state.data, file), "Slicex: %FLStudioData% path resolved");
    expect(near(param(r, slMarker(3)), 0.75) && param(r, slMarker(4)) < 0, "Slicex: four markers from the file's cue chunk");
    // J-33: master level law (11^(v/12800) − 1)/10 → raw 6400 = 0.2317.
    expect(near(param(r, SlVolume), (std::sqrt(11.0) - 1.0) / 10.0) && near(param(r, SlPitch), -12), "Slicex: level 6400 (law), pitch −1 octave");
  }
  // ---- Fruity Slicer: slice starts in frames, base note in the trailer.
  {
    std::string file = writeFile("beat.wav", wavBytes(4000, {}));
    Bytes s;
    s.u32(16).f32(8.f).f32(174.f).i32(0).i32(83).i32(4).i32(0).i32(4);
    s.u8(static_cast<uint8_t>(file.size())).str(file, false);
    s.i32(3);
    for (int32_t st : {0, 1000, 3000}) s.u8(1).str("K", false).i32(st).i32(-1).f32(-1.f).u8(0);
    s.u8(1).u8(62).i32(0).i32(44100).u8(1).u8(1).u8(1).zeros(8);
    for (const char* name : {"Fruity Slicer", "Fruity slicer"}) {
      Run r;
      decode(r, name, s.b);
      expect(r.ok && r.m.glType == "gl.slicer" && r.m.state.data == file, "Fruity Slicer → gl.slicer with its sample");
      expect(near(param(r, slMarker(1)), 0.25) && near(param(r, slMarker(2)), 0.75) && param(r, slMarker(3)) < 0, "Fruity Slicer: slice starts → markers");
      expect(near(param(r, SlBaseNote), 62), "Fruity Slicer: base note");
      expect(noteRms(r.m.state, 63) > 0.01 && noteRms(r.m.state, 70) < 1e-6, "Fruity Slicer: slice 2 sounds, keys past the slices are silent");
    }
  }
  // ---- Granulizer: knobs by tag after the version; sample = the channel's own sample.
  {
    std::string file = writeFile("grain.wav", wavBytes(8000, {}));
    Bytes s;
    s.u32(5);
    for (int32_t v : {200, 50, 413, 611, 0, 180, 128, 32, 1, 0, 2048, 300, 1, 0}) s.i32(v);
    Run r;
    Bytes path;
    path.utf16(file);
    r.add(196, path.b);
    decode(r, "Fruity Granulizer", s.b);
    // J-22: Granulizer → Grain Scanner (gl.grainscan), parameter index = control tag.
    using namespace gl::packs::models::metal;
    expect(r.ok && r.m.glType == "gl.grainscan" && r.m.state.data == file, "Granulizer → gl.grainscan with the channel sample");
    expect(near(param(r, GsGrainSpace), 413) && near(param(r, GsWaveSpace), 611), "Granulizer: grain / wave spacing");
    expect(near(param(r, GsStart), 2048) && near(param(r, GsStereo), 32) && near(param(r, GsLoop), 1) && near(param(r, GsTransSource), 1),
           "Granulizer: start, stereo, loop, transient source");
    expect(near(param(r, GsHoldOn), 0) && near(param(r, GsTransHold), 300), "Granulizer: moving playhead, transient hold");
    expect(noteRms(r.m.state, 60) > 1e-3, "Granulizer: audible");
    Run lower;
    decode(lower, "Fruity granulizer", s.b);
    expect(lower.ok, "Granulizer: lower-case internal name");
  }
  // ---- Wave Traveller: per-key position splines and mute toggles → scratch moves.
  {
    std::string file = writeFile("scr.wav", wavBytes(44100, {}));
    auto point = [](Bytes& b, float x, float y, int32_t type) {
      b.u32(0).f32(0).f32(x).f32(y).u32(0).f32(-0.042f).u32(0).u32(0).f32(0.042f).u32(0).f32(0.5f).u32(0).i32(type).u32(0).u32(0);
    };
    Bytes s;
    s.i32(1000).i32(12 + static_cast<int32_t>(file.size())).i32(1000005).i32(60).i32(static_cast<int32_t>(file.size())).str(file, false);
    // key 60: 3 points, muted between 0.5 and 0.75, start/end 0.2..0.6
    s.i32(2000).i32(104).i32(60).f32(2.f).f32(1.f).f32(1.f).f32(1.f).f32(0.2f).f32(0.6f).i32(0).i32(0).i32(3);
    s.i32(1).f32(0.f).i32(0).f32(0.5f).i32(1).f32(0.75f);
    s.i32(3), point(s, 0, 0, 1), point(s, 0.4f, 1, 2), point(s, 1, 0.5f, 1);
    s.i32(2), point(s, 0, 1, 1), point(s, 1, 1, 1);
    // key 61: default straight line
    s.i32(2000).i32(88).i32(61).f32(4.f).f32(1.f).f32(1.f).f32(1.f).f32(0.f).f32(1.f).i32(0).i32(0).i32(1).i32(1).f32(0.f);
    s.i32(2), point(s, 0, 0, 1), point(s, 1, 1, 1);
    s.i32(2), point(s, 0, 1, 1), point(s, 1, 1, 1);
    s.i32(3000).i32(0);
    Run r;
    decode(r, "Wave Traveller", s.b);
    // J-22: Wave Traveller → Spline Scratcher (gl.splinescratch): every key patch kept.
    expect(r.ok && r.m.glType == "gl.splinescratch", "Wave Traveller → gl.splinescratch");
    using namespace gl::packs::models::metal;
    SsData d;
    expect(parseScratchSpl(r.m.state.data, d) && d.path == file, "Wave Traveller: sample path");
    const SsPatch& p0 = d.keys[60];
    expect(p0.used && p0.pos.size() == 3 && near(p0.pos[1].y, 1) && near(p0.start, 0.2) && near(p0.end, 0.6) && near(p0.beats, 2),
           "Wave Traveller: key C5 patch (spline, window, length)");
    expect(!ssSoundOn(p0, 0.6f) && ssSoundOn(p0, 0.8f), "Wave Traveller: mute toggles kept");
    expect(!d.keys[61].used, "Wave Traveller: untouched patch stays default");
    expect(noteRms(r.m.state, 61) > 1e-3, "Wave Traveller: audible");
  }
  // ---- Truncated / garbage states never crash.
  {
    std::vector<uint8_t> junk(300, 0xFF);
    for (const char* n : {"Slicex", "Fruity Slicer", "Fruity Granulizer", "Wave Traveller"}) {
      for (size_t len : {size_t(0), size_t(3), size_t(40), size_t(120), size_t(300)}) {
        Run r;
        decode(r, n, std::vector<uint8_t>(junk.begin(), junk.begin() + static_cast<long>(len)));
        (void)r;
      }
    }
    Run cut;
    auto full = slicexState(false, "a.wav", wavBytes(500, {0, 100}), 2);
    full.resize(0x80);
    decode(cut, "Slicex", full);
    expect(true, "slicer decoders survive truncated and garbage states");
  }
}

}  // namespace gen2test
