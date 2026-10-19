// Gridline — J-4 tests: Resynth (gl.harmor), its drawn data / images, and the
// decode of FL Studio's shipped two-part additive synth states.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/FlpGen.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/models/harmor.h"
#include "../Check.h"
#include "fl_corpus.h"

using namespace gl;
using namespace gl::packs::models::hm;

namespace {

struct Ev {
  int at;
  bool on;
  uint32_t id;
  double pitch;
  float vel;
};
struct Out {
  std::vector<float> L, R;
  bool finite = true;
  float peak = 0;
  bool activeAtEnd = false;
};
Out render(const PluginState& st, const std::vector<Ev>& evs, int total, int block, double sr = 44100.0) {
  Out o;
  o.L.assign(static_cast<size_t>(total), 0.f);
  o.R.assign(static_cast<size_t>(total), 0.f);
  auto g = packs::createGenerator(st.type);
  if (!g) {
    o.finite = false;
    return o;
  }
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.tempo = 120;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    for (const auto& e : evs)
      if (e.at >= pos && e.at < pos + n) {
        if (e.on) g->noteOn(e.id, e.pitch, e.vel, e.at - pos);
        else g->noteOff(e.id, e.at - pos);
      }
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
  }
  for (size_t i = 0; i < o.L.size(); ++i) {
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) o.finite = false;
    o.peak = std::max({o.peak, std::fabs(o.L[i]), std::fabs(o.R[i])});
  }
  o.activeAtEnd = g->active();
  return o;
}
double tone(const std::vector<float>& x, int a, int b, double f, double sr = 44100) {
  const double w = 2 * 3.14159265358979 * f / sr, c = 2 * std::cos(w);
  double s1 = 0, s2 = 0;
  for (int i = a; i < b; ++i) {
    const double win = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * (i - a) / (b - a));
    const double s0 = x[static_cast<size_t>(i)] * win + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (b - a);
}
double zcHz(const std::vector<float>& x, int a, int b, double sr = 44100) {
  int z = 0;
  for (int i = a + 1; i < b; ++i)
    if (x[static_cast<size_t>(i - 1)] <= 0 && x[static_cast<size_t>(i)] > 0) ++z;
  return z * sr / (b - a);
}
float peakIn(const Out& o, int a, int b) {
  float p = 0;
  for (int i = std::max(0, a); i < std::min(b, static_cast<int>(o.L.size())); ++i)
    p = std::max({p, std::fabs(o.L[static_cast<size_t>(i)]), std::fabs(o.R[static_cast<size_t>(i)])});
  return p;
}
PluginState base() {
  PluginState s = defaultPluginState("gl.harmor");
  s.params[static_cast<size_t>(flt(0, 0, PfF1Type))] = 17;  // filter off
  return s;
}
void set(PluginState& s, int i, float v) { s.params[static_cast<size_t>(i)] = v; }
const std::vector<Ev> kOne = {{0, true, 1, 45, 0.9f}, {22050, false, 1, 0, 0}};  // 110 Hz
Curve flat(float y) {
  Curve c;
  c.pts = {{0, y, 0, 0}, {1, y, 0, 0}};
  return c;
}

}  // namespace

int main() {
  const PluginInfo* info = findPlugin("gl.harmor");
  expect(info != nullptr, "catalogue entry");
  if (!info) return finish();
  expect(static_cast<int>(info->params.size()) == HmParamCount, "parameter count");
  expect(info->presets.size() >= 2 && info->presets.size() <= 6, "2-6 factory presets");
  bool sane = true;
  for (const auto& p : info->params) sane = sane && !p.name.empty() && p.def >= p.min && p.def <= p.max;
  expect(sane, "every parameter named, defaults in range");
  {
    PluginState s;
    s.type = "gl.harmor";
    s.params = {1};
    normalizePluginState(s);
    expect(static_cast<int>(s.params.size()) == HmParamCount, "normalizePluginState pads");
  }
  // Default: a saw at the note pitch, released to silence.
  {
    Out o = render(base(), kOne, 44100 * 2, 256);
    expect(o.finite && o.peak > 0.02f, "default patch sounds");
    expect(std::fabs(zcHz(o.L, 4000, 20000) - 110) < 3, "plays the note pitch");
    expect(tone(o.L, 4000, 20000, 220) > 0.3 * tone(o.L, 4000, 20000, 110), "saw spectrum");
    expect(peakIn(o, 50000, 88200) < 1e-4f && !o.activeAtEnd, "release ends in silence");
  }
  // Timbre curve with a "wave" segment: odd partials only.
  {
    PluginState s = base();
    PatchData d;
    d.part[0].shapes[ShTimbre1].pts = {{0, 0.25f, 0, 0}, {1, 0.25f, 0, 6}};
    s.data = serializePatch(d);
    Out o = render(s, kOne, 20000, 256);
    expect(tone(o.L, 4000, 16000, 220) < 0.05 * tone(o.L, 4000, 16000, 330), "timbre wave segment drops the even partials");
  }
  // Part B one octave up (frequency multiplier), mixed in.
  {
    PluginState s = base();
    set(s, pf(1, PfEnabled), 1), set(s, pf(1, PfFreqMul), 2), set(s, flt(1, 0, PfF1Type), 17);
    PatchData d;
    for (int k = 0; k < 2; ++k) d.part[static_cast<size_t>(k)].shapes[ShTimbre1].pts = {{0, 0.25f, 0, 0}, {0.004f, 0, 0, 0}, {0.996f, 0, 0, 0}};
    s.data = serializePatch(d);
    Out o = render(s, kOne, 20000, 256);
    expect(tone(o.L, 4000, 16000, 220) > 0.3 * tone(o.L, 4000, 16000, 110), "part B (x2) adds the octave");
  }
  // Filter 1 low pass.
  {
    PluginState s = base();
    set(s, flt(0, 0, PfF1Type), 0), set(s, flt(0, 0, PfF1Cut), 0.45f);
    Out o = render(s, kOne, 20000, 256), r = render(base(), kOne, 20000, 256);
    expect(tone(o.L, 4000, 16000, 110 * 20) < 0.1 * tone(r.L, 4000, 16000, 110 * 20), "filter 1 low pass");
    bool ok = true;
    for (int f = 0; f < kFltShapes; ++f)
      for (int rt = 0; rt < kResShapes; rt += 3) {
        PluginState q = base();
        set(q, flt(0, 0, PfF1Type), static_cast<float>(f)), set(q, flt(0, 0, PfF1ResType), static_cast<float>(rt));
        set(q, flt(0, 0, PfF1Res), 1), set(q, flt(0, 0, PfF1Osc), 0.5f), set(q, flt(0, 1, PfF1Type), static_cast<float>(19 - f));
        Out x = render(q, kOne, 6000, 256);
        ok = ok && x.finite && x.peak < 10;
      }
    expect(ok, "every filter shape x resonance shape finite");
  }
  // Volume articulator: drawn envelope with sustain, then release.
  {
    PluginState s = base();
    PatchData d;
    Curve& e = d.part[0].arts[ArVol].env;
    e.pts = {{0, 0, 0, 0}, {0.01f, 1, 0, 0}, {0.1f, 0.5f, 0, 0}, {0.05f, 0, 0, 0}};
    e.sustain = 2;
    e.enabled = true;
    s.data = serializePatch(d);
    Out o = render(s, kOne, 44100, 256);
    expect(peakIn(o, 15000, 21000) > 0.01f && peakIn(o, 26000, 44100) < 1e-4f, "volume envelope: sustain then release");
  }
  // Pitch articulator LFO (square) swings the pitch.
  {
    PluginState s = base();
    set(s, pf(0, PfPitchArt), 1200);
    PatchData d;
    ArtData& a = d.part[0].arts[ArPitch];
    a.lfo.pts = {{0, 0.75f, 0, 2}, {0.5f, 0.25f, 0, 2}, {0.5f, 0.25f, 0, 0}};
    a.lfo.enabled = true;
    a.lfoSpeed = 3;
    s.data = serializePatch(d);
    Out o = render(s, kOne, 30000, 256);
    const double x = zcHz(o.L, 1000, 6000), y = zcHz(o.L, 8500, 13500);
    expect(std::fabs(x - y) > 40, "pitch LFO articulator");
  }
  // Velocity map on the volume articulator.
  {
    PluginState s = base();
    set(s, GmVelVol, 0);
    PatchData d;
    d.part[0].arts[ArVol].maps[MkVel].pts = {{0, 0, 0, 0}, {1, 1, 0, 0}};
    s.data = serializePatch(d);
    Out lo = render(s, {{0, true, 1, 45, 0.2f}}, 12000, 256), hi = render(s, {{0, true, 1, 45, 1.0f}}, 12000, 256);
    expect(hi.peak > 2.5f * lo.peak, "velocity map scales the volume");
  }
  // Image layer: a picture with one bright row = one partial.
  {
    PluginState s = base();
    PatchData d;
    ImageData& im = d.part[0].image;
    im.cols = 4, im.rows = 8, im.gain.assign(32, 0);
    for (int c = 0; c < 4; ++c) im.gain[static_cast<size_t>(c * 8 + 2)] = 255;  // partial 3
    s.data = serializePatch(d);
    Out o = render(s, kOne, 20000, 256);
    expect(tone(o.L, 4000, 16000, 330) > 10 * tone(o.L, 4000, 16000, 110), "image layer plays its partials");
    PatchData e;
    expect(parsePatch(s.data, e) && e.part[0].image.gain == im.gain && e.part[0].image.rows == 8, "image data round trip");
  }
  // Audio resynthesis: a 220 Hz tone with a strong 3rd harmonic.
  {
    std::vector<float> x(44100);
    for (size_t i = 0; i < x.size(); ++i)
      x[i] = static_cast<float>(0.3 * std::sin(2 * 3.14159265358979 * 220 * i / 44100.0) + 0.6 * std::sin(2 * 3.14159265358979 * 660 * i / 44100.0));
    ImageData im;
    double f0 = 0;
    expect(analyzeAudio(x, 44100, 16, 0.01, im, &f0) && std::fabs(f0 - 220) < 3, "resynthesis finds the pitch");
    const int c = im.cols / 2;
    expect(im.gain[static_cast<size_t>(c * im.rows + 2)] > 200 && im.gain[static_cast<size_t>(c * im.rows + 0)] > 60 &&
               im.gain[static_cast<size_t>(c * im.rows + 1)] < 30,
           "resynthesis tracks the harmonics");
  }
  // Picture import (PPM): bright bottom row → fundamental.
  {
    const std::string path = "/tmp/gl_harmor_test.ppm";
    {
      std::ofstream f(path, std::ios::binary);
      f << "P6\n4 8\n255\n";
      for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 4; ++x) {
          const unsigned char v = y == 7 ? 255 : 0;
          f.put(static_cast<char>(v)), f.put(static_cast<char>(v)), f.put(static_cast<char>(v));
        }
    }
    ImageData im;
    expect(imageFromPicture(path, 8, im) && im.cols == 4 && im.gain[0] == 255 && im.gain[1] == 0, "picture becomes an image layer");
  }
  // Unison, phaser, harmonizer, blur, prism, pluck: finite and audible.
  {
    PluginState s = base();
    set(s, pf(0, PfUniOrder), 7), set(s, pf(0, PfUniPitch), 30), set(s, pf(0, PfPhMix), 1), set(s, pf(0, PfHzMix), 0.8f),
        set(s, pf(0, PfHBlur), 8), set(s, pf(0, PfPrism), 0.6f), set(s, pf(0, PfPluck), 0.3f), set(s, pf(0, PfTremolo), 0.5f),
        set(s, pf(0, PfTBlurAtt), 0.2f), set(s, pf(0, PfVibrato), 50);
    Out o = render(s, kOne, 30000, 256);
    double diff = 0;
    for (size_t i = 4000; i < 16000; ++i) diff += std::fabs(o.L[i] - o.R[i]);
    expect(o.finite && o.peak > 0.01f && o.peak < 4 && diff > 0.5, "unison / phaser / harmonizer / blur / prism / pluck");
  }
  // Effects.
  {
    bool ok = true;
    for (int d = 0; d < 12; ++d) {
      PluginState s = base();
      set(s, FxDistType, static_cast<float>(d)), set(s, FxDistMix, 1), set(s, FxChOrder, static_cast<float>(d % 4));
      set(s, FxDlOn, 1), set(s, FxRvOn, 1), set(s, FxCompType, static_cast<float>(d % 5));
      Out o = render(s, {{0, true, 1, 57, 1}, {6000, false, 1, 0, 0}}, 40000, 256);
      ok = ok && o.finite && o.peak < 6 && peakIn(o, 30000, 40000) > 1e-5f;
    }
    expect(ok, "distortion types, chorus, delay, reverb, compressor");
  }
  // Block-size invariance.
  {
    PluginState s = presetState(*info, 1);
    Out a = render(s, kOne, 30000, 1), b = render(s, kOne, 30000, 64), c = render(s, kOne, 30000, 999);
    float d = 0;
    for (size_t i = 0; i < a.L.size(); ++i) d = std::max({d, std::fabs(a.L[i] - b.L[i]), std::fabs(a.L[i] - c.L[i])});
    expect(d < 1e-4f, "block-size invariant");
  }
  for (size_t i = 0; i < info->presets.size(); ++i) {
    Out o = render(presetState(*info, i), {{0, true, 1, 60, 0.8f}, {20000, false, 1, 0, 0}}, 40000, 512);
    char name[96];
    std::snprintf(name, sizeof name, "preset '%s' sounds (peak %.2f)", info->presets[i].name.c_str(), o.peak);
    expect(o.finite && o.peak > 0.02f && o.peak < 4.f, name);
  }
  {
    PluginState mn = base(), mx = base();
    for (size_t i = 0; i < info->params.size(); ++i) {
      mn.params[i] = static_cast<float>(info->params[i].min);
      mx.params[i] = static_cast<float>(info->params[i].max);
    }
    Out a = render(mn, kOne, 20000, 256), b = render(mx, kOne, 20000, 256);
    expect(a.finite && b.finite && b.peak < 1e3f, "parameter extremes finite");
  }
  // Data text.
  {
    PatchData d;
    d.part[1].arts[ArF1Freq].env.pts = {{0, 0.2f, 0, 0}, {0.5f, 0.9f, 0.3f, 1}};
    d.part[1].arts[ArF1Freq].env.enabled = true;
    d.part[1].arts[ArF1Freq].env.sustain = 1;
    d.part[0].arts[ArPan].maps[MkKey] = flat(0.7f);
    d.part[0].shapes[ShPrism] = flat(0.6f);
    d.part[0].image.source = "/tmp/some file.wav";
    PatchData e;
    expect(parsePatch(serializePatch(d), e) && e.part[1].arts[ArF1Freq].env.pts.size() == 2 && e.part[1].arts[ArF1Freq].env.sustain == 1 &&
               e.part[1].arts[ArF1Freq].env.pts[1].kind == 1 && e.part[0].arts[ArPan].maps[MkKey].pts.size() == 2 &&
               e.part[0].shapes[ShPrism].pts[0].y == 0.6f && e.part[0].image.source == "/tmp/some file.wav",
           "patch data round trip");
    expect(serializePatch(PatchData()).empty(), "default data serializes empty");
    PatchData junk;
    expect(!parsePatch("nope", junk) && parsePatch("hmr 1\ne 0 99 1 0 0 0 0 x\ni 0 5 5 1 0.1 AAAA -\ns 1 2 z\n", junk), "malformed data ignored");
  }
  // Malformed states are refused.
  {
    flp::File f;
    Song song;
    flp::Report rep;
    flp::Context c(f, song, rep);
    std::vector<uint8_t> junk = {0x13, 0, 3, 0, 7, 0, 0, 0, 0x10, 0, 0, 0, 1, 2, 3};
    flp::Event e;
    e.id = 213;
    e.data = junk.data();
    e.size = static_cast<uint32_t>(junk.size());
    flp::PluginMapping m;
    expect(!flp::gen::decodeHarmorFull(c, {e}, m), "truncated state refused");
  }
  if (testfl::flInstalled()) {
    for (int pass = 0; pass < 2; ++pass) {
      auto all = pass == 0 ? testfl::decodePresets("Harmor") : testfl::decodeProjects("Harmor");
      int total = 0, decoded = 0, finite = 0, sounding = 0, images = 0;
      for (const auto& d : all) {
        ++total;
        if (!d.ok || d.state.type != "gl.harmor") {
          std::printf("# not decoded: %s\n", d.where.c_str());
          continue;
        }
        ++decoded;
        testfl::maybeDump(d);
        images += d.state.data.find("\ni ") != std::string::npos || d.state.data.find("\nx ") != std::string::npos;
        Out o = render(d.state, {{0, true, 1, 60, 0.8f}, {8000, false, 1, 0, 0}}, 16000, 512);
        finite += o.finite && o.peak < 20;
        sounding += o.peak > 1e-3f;
        if (!o.finite || o.peak >= 20) std::printf("# unstable: %s (peak %g)\n", d.where.c_str(), o.peak);
        else if (o.peak <= 1e-3f) std::printf("# silent: %s\n", d.where.c_str());
      }
      std::printf("# Harmor %s: %d states, %d decoded, %d finite, %d sounding, %d with images\n", pass ? "projects" : "presets", total, decoded,
                  finite, sounding, images);
      expect(total > 0 && decoded == total, pass ? "every project state decodes" : "every shipped preset decodes");
      expect(finite == decoded, "every decoded state renders finite");
      expect(sounding >= decoded * 9 / 10, "decoded states sound");
    }
  } else {
    std::printf("# FL Studio not installed: corpus checks skipped\n");
  }
  return finish();
}
