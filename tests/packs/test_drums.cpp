// Gridline — J-22 tests: Membrane Pad (gl.membranepad), Membrane Kit
// (gl.membranekit), Kick Sculptor (gl.kicksculpt), Sweep Kick (gl.sweepkick)
// and the decode of FL Studio's shipped drum-model / kick states.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/Flp.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/models/drums.h"
#include "../Check.h"
#include "fl_corpus.h"

using namespace gl;
using namespace gl::packs::models::dr;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

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
  double rms = 0;
};
Out render(const PluginState& st, const std::vector<Ev>& evs, int total, int block = 256, double sr = 44100.0, double tempo = 120) {
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
  ctx.tempo = tempo;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    for (const auto& e : evs)
      if (e.at >= pos && e.at < pos + n) {
        if (e.on) g->noteOn(e.id, e.pitch, e.vel, e.at - pos);
        else g->noteOff(e.id, e.at - pos);
      }
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
  }
  double acc = 0;
  for (size_t i = 0; i < o.L.size(); ++i) {
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) o.finite = false;
    o.peak = std::max({o.peak, std::fabs(o.L[i]), std::fabs(o.R[i])});
    acc += o.L[i] * o.L[i];
  }
  o.rms = std::sqrt(acc / std::max<size_t>(1, o.L.size()));
  return o;
}
Out hit(const PluginState& st, double pitch = 60, float vel = 0.9f, int len = 44100) {
  return render(st, {{0, true, 1, pitch, vel}, {20000, false, 1, pitch, 0}}, len);
}
PluginState fresh(const char* type) {
  PluginState s;
  s.type = type;
  normalizePluginState(s);
  return s;
}
// Energy above ~4 kHz (first difference) relative to the signal.
double brightness(const Out& o) {
  double d = 0, a = 0;
  for (size_t i = 1; i < o.L.size(); ++i) {
    d += (o.L[i] - o.L[i - 1]) * (o.L[i] - o.L[i - 1]);
    a += o.L[i] * o.L[i];
  }
  return a > 0 ? std::sqrt(d / a) : 0;
}
// Zero-crossing rate over the first `n` samples (pitch proxy).
double zcr(const Out& o, size_t from, size_t n) {
  int z = 0;
  for (size_t i = from + 1; i < std::min(o.L.size(), from + n); ++i)
    if ((o.L[i - 1] < 0) != (o.L[i] < 0)) ++z;
  return z;
}
// Time (samples) until the level stays below `frac` of the peak.
size_t ringLength(const Out& o, float frac = 0.01f) {
  size_t last = 0;
  for (size_t i = 0; i < o.L.size(); ++i)
    if (std::fabs(o.L[i]) > o.peak * frac) last = i;
  return last;
}

void corpus(const char* plugin, const char* folder, const char* type, int minPresets, int minProjects) {
  if (!testfl::flInstalled()) {
    std::printf("# FL Studio not installed: corpus test for %s skipped\n", plugin);
    return;
  }
  auto pre = testfl::decodePresets(folder);
  auto prj = testfl::decodeProjects(plugin);
  int ok = 0, exact = 0, finite = 0, audible = 0, n = 0;
  std::vector<float> peaks;
  for (auto* v : {&pre, &prj})
    for (const auto& d : *v) {
      ++n;
      testfl::maybeDump(d);
      if (!d.ok || d.state.type != type) {
        std::printf("#   not decoded: %s\n", d.where.c_str());
        continue;
      }
      ++ok;
      exact += d.exact ? 1 : 0;
      // The kit plays its first channel's key, the others C5.
      double key = 60;
      if (d.state.type == "gl.membranekit") {
        key = -1;
        for (int c = 0; c < kKitPads && key < 0; ++c)
          if (d.state.params[static_cast<size_t>(kitPad(c, KpMute))] < 0.5f && d.state.params[static_cast<size_t>(kitPad(c, KpKey))] > 0)
            key = d.state.params[static_cast<size_t>(kitPad(c, KpKey))];
        if (key < 0) key = 36;
      }
      Out o = render(d.state, {{0, true, 1, key, 0.9f}, {11025, false, 1, key, 0}}, 22050);
      finite += o.finite ? 1 : 0;
      peaks.push_back(o.peak);
      if (o.peak > 1e-3f) audible += 1;
      else std::printf("#   silent: %s\n", d.where.c_str());
    }
  std::printf("# %s: %zu presets + %zu project states; decoded %d / %d (walked to the end), exact %d, finite %d, audible %d\n", plugin,
              pre.size(), prj.size(), ok, n, exact, finite, audible);
  if (!peaks.empty()) {
    std::sort(peaks.begin(), peaks.end());
    std::printf("# %s: output peak min %.3g / median %.3g / 95%% %.3g / max %.3g\n", plugin, static_cast<double>(peaks.front()),
                static_cast<double>(peaks[peaks.size() / 2]), static_cast<double>(peaks[peaks.size() * 95 / 100]), static_cast<double>(peaks.back()));
  }
  expect(static_cast<int>(pre.size()) >= minPresets, std::string(plugin) + ": shipped presets found");
  expect(static_cast<int>(prj.size()) >= minProjects, std::string(plugin) + ": project states found");
  expect(ok == n, std::string(plugin) + ": every shipped state decodes (exact length walk)");
  expect(finite == ok, std::string(plugin) + ": every decoded state renders finite");
  expect(audible >= ok * 95 / 100, std::string(plugin) + ": decoded states sound (>= 95%)");
}

PluginState stateFromBytes(const char* plugin, const std::vector<uint8_t>& bytes, bool& ok) {
  flp::File file;
  Song song;
  flp::Report rep;
  flp::Context c(file, song, rep);
  std::vector<flp::Event> evs(2);
  evs[0].id = 201;
  std::string name = plugin;
  std::vector<uint8_t> nm(name.begin(), name.end());
  nm.push_back(0);
  evs[0].data = nm.data();
  evs[0].size = static_cast<uint32_t>(nm.size());
  evs[1].id = 213;
  evs[1].data = bytes.data();
  evs[1].size = static_cast<uint32_t>(bytes.size());
  flp::PluginMapping m;
  const flp::PluginDecoder* dec = flp::findDecoder(plugin);
  ok = dec && (*dec)(c, evs, m);
  PluginState s = m.state;
  if (s.type.empty()) s.type = m.glType;
  return s;
}

}  // namespace

int main() {
  // ---------------------------------------------------------- catalogue
  for (const char* type : {"gl.membranepad", "gl.membranekit", "gl.kicksculpt", "gl.sweepkick"}) {
    const PluginInfo* p = findPlugin(type);
    expect(p != nullptr, std::string(type) + " registered");
    if (!p) continue;
    expect(p->presets.size() >= 2 && p->presets.size() <= 6, std::string(type) + " has 2-6 factory presets");
    for (const auto& pr : p->presets) {
      PluginState s = fresh(type);
      for (const auto& [k, v] : pr.values) s.params[static_cast<size_t>(k)] = v;
      normalizePluginState(s);
      Out o = hit(s, 60);
      if (!(o.finite && o.peak > 1e-3f && o.peak < 4.0f)) std::printf("#   preset peak %g finite %d\n", static_cast<double>(o.peak), o.finite ? 1 : 0);
      expect(o.finite && o.peak > 1e-3f && o.peak < 4.0f, std::string(type) + " preset '" + pr.name + "' sounds");
    }
  }
  expect(findPlugin("gl.membranepad")->params.size() == PdCount, "pad: 40 params (FL tags 0..37 + output + engine)");
  expect(findPlugin("gl.membranekit")->params.size() == KmCount, "kit: 720 params (FL tags 0..719)");
  expect(findPlugin("gl.kicksculpt")->params.size() == KsCount, "kick sculptor: 23 FL tags + noise sample");
  expect(findPlugin("gl.sweepkick")->params.size() == SkCount, "sweep kick: 6 FL tags + seed");

  // ---------------------------------------------------------- Membrane Pad behaviours
  {
    PluginState s = fresh("gl.membranepad");
    Out base = hit(s);
    expect(base.finite && base.peak > 0.01f, "pad: note on sounds");
    // Silence without notes.
    Out quiet = render(s, {}, 4096);
    expect(quiet.peak == 0.0f, "pad: silent without notes");
    // Size: bigger = lower.
    PluginState big = s, small = s;
    big.params[MdSize] = 0.9f;
    small.params[MdSize] = 0.1f;
    expect(zcr(hit(big), 2000, 8000) < zcr(hit(small), 2000, 8000), "pad: larger membrane is lower");
    // Keyboard pitch.
    // (J-22 modal engine; the reference engine (J-33, test_j33) does not transpose by key.)
    PluginState modal = s;
    modal.params[PdEngine] = 0;
    expect(zcr(hit(modal, 72), 2000, 8000) > zcr(hit(modal, 48), 2000, 8000), "pad: keyboard raises the pitch (modal engine)");
    // Pitch control.
    PluginState up = s, dn = s;
    up.params[MdPitch] = 1.0f;
    dn.params[MdPitch] = 0.0f;
    expect(zcr(hit(up), 2000, 8000) > zcr(hit(dn), 2000, 8000), "pad: pitch control");
    // Membrane decay: longer ring.
    PluginState lo = s, hi = s;
    lo.params[MdMemDecay] = 0.3f;
    hi.params[MdMemDecay] = 0.95f;
    expect(ringLength(hit(hi)) > ringLength(hit(lo)), "pad: membrane decay lengthens the ring");
    // Hold gates the ring.
    PluginState held = s;
    held.params[MdHold] = 0.25f;
    held.params[MdMemDecay] = 0.95f;
    expect(ringLength(hit(held)) < ringLength(hit(hi)), "pad: short hold cuts the ring");
    // Cutoff brightens.
    PluginState dull = s, bright = s;
    dull.params[MdMemCutoff] = 0.05f;
    bright.params[MdMemCutoff] = 1.0f;
    dull.params[MdMalletNoise] = bright.params[MdMalletNoise] = 0.6f;
    expect(brightness(hit(bright)) > brightness(hit(dull)), "pad: membrane cutoff brightens");
    // LOW band: a high-pass (low cut) removes the body.
    PluginState lc = s;
    lc.params[MdLowFreq] = 0.8f;
    expect(brightness(hit(lc)) > brightness(base), "pad: low cut thins the drum");
    // HIGH shelf: cut darkens.
    PluginState hs = s;
    hs.params[MdHighLevel] = 0.0f;
    hs.params[MdMalletNoise] = 0.8f;
    PluginState hs2 = hs;
    hs2.params[MdHighLevel] = 1.0f;
    expect(brightness(hit(hs)) < brightness(hit(hs2)), "pad: high shelf gain");
    // Velocity route to mallet level: soft notes quieter.
    PluginState vr = s;
    vr.params[MdVelSrc1] = 1;  // mallet level
    vr.params[MdVelAmt1] = 1.0f;
    vr.params[MdMalletAmp] = 0.0f;  // routes add amount x velocity
    expect(hit(vr, 60, 0.2f).peak < hit(vr, 60, 1.0f).peak * 0.8f, "pad: velocity route scales the mallet");
    // Output level.
    PluginState half = s;
    half.params[PdOutput] = 0.25f;
    expect(hit(half).peak < base.peak * 0.4f, "pad: output level");
    // Lo-fi changes the waveform.
    PluginState lf = s;
    lf.params[MdLofi] = 1.0f;
    Out lfo = hit(lf);
    expect(lfo.finite && lfo.peak > 0.0f, "pad: lo-fi renders");
    // Mid repeat (clap): more onsets.
    PluginState clap = s, one = s;
    clap.params[MdRetrig] = 1;
    clap.params[MdMidLevel] = one.params[MdMidLevel] = 1.0f;
    clap.params[MdMidLevelMod] = one.params[MdMidLevelMod] = 1.0f;
    clap.params[MdMidAttack] = one.params[MdMidAttack] = 0.0f;
    Out oc = hit(clap), oo = hit(one);
    double e1 = 0, e2 = 0;
    for (size_t i = 900; i < 1600; ++i) {
      e1 += oc.L[i] * oc.L[i];
      e2 += oo.L[i] * oo.L[i];
    }
    expect(e1 != e2, "pad: mid repeat re-fires the band envelope");
    // Extremes.
    for (float v : {0.0f, 1.0f}) {
      PluginState x = s;
      for (int k = 0; k < MdCount; ++k)
        if (k < MdVelSrc1 || k > MdVelSrc4) x.params[static_cast<size_t>(k)] = v;
      x.params[MdVelSrc1] = v > 0.5f ? 26 : 0;
      Out o = hit(x, 60, 1.0f);
      expect(o.finite && o.peak < 50.0f, v > 0.5f ? "pad: all controls max -> finite" : "pad: all controls min -> finite");
    }
    // Block-size invariance.
    Out a = render(s, {{100, true, 1, 60, 0.9f}}, 8192, 64), b = render(s, {{100, true, 1, 60, 0.9f}}, 8192, 1000);
    double diff = 0;
    for (size_t i = 0; i < a.L.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(a.L[i] - b.L[i])));
    expect(diff < 1e-5, "pad: block-size invariant");
    // Short param vector normalizes.
    PluginState shortS;
    shortS.type = "gl.membranepad";
    shortS.params = {0, 0, 0.5f};
    normalizePluginState(shortS);
    expect(shortS.params.size() == PdCount, "pad: short state normalized");
  }

  // ---------------------------------------------------------- Membrane Kit behaviours
  {
    PluginState s = fresh("gl.membranekit");
    const PluginInfo* p = findPlugin("gl.membranekit");
    for (const auto& [k, v] : p->presets[0].values) s.params[static_cast<size_t>(k)] = v;
    Out o = hit(s, 60);
    expect(o.finite && o.peak > 0.01f, "kit: channel 1 key sounds");
    Out none = hit(s, 100);
    expect(none.peak == 0.0f, "kit: unmapped key is silent");
    PluginState muted = s;
    muted.params[static_cast<size_t>(kitPad(0, KpMute))] = 1;
    expect(hit(muted, 60).peak == 0.0f, "kit: mute");
    PluginState solo = s;
    solo.params[static_cast<size_t>(kitPad(1, KpSolo))] = 1;
    expect(hit(solo, 60).peak == 0.0f && hit(solo, 61).peak > 0.0f, "kit: solo silences the others");
    PluginState left = s;
    left.params[static_cast<size_t>(kitPad(0, KpPan))] = 0.0f;
    Out ol = hit(left, 60);
    double l = 0, r = 0;
    for (size_t i = 0; i < ol.L.size(); ++i) {
      l += std::fabs(ol.L[i]);
      r += std::fabs(ol.R[i]);
    }
    expect(l > 10 * r, "kit: pan hard left");
    PluginState quiet = s;
    quiet.params[KmLevel] = 0.1f;
    expect(hit(quiet, 60).peak < o.peak * 0.3f, "kit: master level");
    // Sequencer: a pattern on key 60 plays channel 1 on steps 1 and 5 at 120 BPM.
    KitData d;
    for (auto& P : d.pattern)
      for (auto& row : P.cell) std::fill(std::begin(row), std::end(row), uint8_t{0x7F});
    d.pattern[0].length = 8;
    d.pattern[0].key = 60;
    d.pattern[0].cell[0][0] = 0xFF;
    d.pattern[0].cell[0][4] = 0xFF;
    PluginState seq = s;
    seq.data = encodeKitData(d);
    seq.params[static_cast<size_t>(kitPad(0, KpModel + MdHold))] = 0.4f;  // short gate: steps stay separate
    KitData back;
    expect(decodeKitData(seq.data, back) && back.pattern[0].key == 60 && back.pattern[0].cell[0][4] == 0xFF, "kit: pattern data round trip");
    Out os = render(seq, {{0, true, 7, 60, 1.0f}, {44100, false, 7, 60, 0}}, 44100);
    // Step 5 starts 4 sixteenths = 0.5 s later at 120 BPM.
    float early = 0, late = 0;
    for (size_t i = 0; i < 2000; ++i) early = std::max(early, std::fabs(os.L[i]));
    for (size_t i = 22050; i < 24050; ++i) late = std::max(late, std::fabs(os.L[i]));
    expect(early > 0.01f && late > 0.01f, "kit: pattern key plays its steps in tempo");
    Out stopped = render(seq, {{0, true, 7, 60, 1.0f}, {2000, false, 7, 60, 0}}, 44100);
    float after = 0;
    for (size_t i = 22050; i < 24050; ++i) after = std::max(after, std::fabs(stopped.L[i]));
    expect(after < late * 0.2f, "kit: releasing the trigger key stops the pattern");
    // EQ / limiter / swing finite at extremes.
    PluginState x = s;
    x.params[KmEqOn] = 1;
    x.params[KmLimiter] = 1;
    for (int k = 0; k < 8; ++k) x.params[static_cast<size_t>(KmEq1 + k)] = 1.0f;
    x.params[KmLimGain] = 1.0f;
    Out ox = hit(x, 60);
    expect(ox.finite && ox.peak <= 1.0f, "kit: EQ max + limiter stays below full scale");
    PluginState shortS;
    shortS.type = "gl.membranekit";
    normalizePluginState(shortS);
    expect(shortS.params.size() == KmCount, "kit: empty state normalized");
  }

  // ---------------------------------------------------------- Kick Sculptor behaviours
  {
    PluginState s = fresh("gl.kicksculpt");
    Out base = hit(s);
    expect(base.finite && base.peak > 0.05f, "kick: sounds");
    PluginState shortD = s, longD = s;
    shortD.params[KsDuration] = 0.1f;
    longD.params[KsDuration] = 0.9f;
    expect(ringLength(hit(longD)) > ringLength(hit(shortD)), "kick: duration");
    PluginState lowB = s, highB = s;
    lowB.params[KsBase] = 0.0f;
    highB.params[KsBase] = 1.0f;
    lowB.params[KsNoiseMix] = highB.params[KsNoiseMix] = 0;
    lowB.params[KsClickLevel] = highB.params[KsClickLevel] = 0;
    lowB.params[KsDuration] = highB.params[KsDuration] = 0.9f;
    expect(zcr(hit(highB), 6000, 8000) > zcr(hit(lowB), 6000, 8000), "kick: base frequency");
    PluginState slow = lowB, fast = lowB;
    slow.params[KsPeak] = fast.params[KsPeak] = 0.6f;
    slow.params[KsSlide] = 0.7f;
    fast.params[KsSlide] = 0.1f;
    expect(zcr(hit(slow), 1000, 3000) > zcr(hit(fast), 1000, 3000), "kick: slide time holds the peak pitch longer");
    PluginState noisy = s;
    noisy.params[KsNoiseMix] = 1.0f;
    noisy.params[KsNoiseS] = 1.0f;
    Out on = hit(noisy);
    expect(on.finite, "kick: noise layer finite");
    expect(brightness(on) > brightness(base), "kick: noise sample layer adds highs");
    PluginState lvl = s;
    lvl.params[KsLevel] = 0.1f;
    expect(hit(lvl).peak < base.peak * 0.4f, "kick: output level");
    for (float v : {0.0f, 1.0f}) {
      PluginState x = s;
      for (int k = 0; k < KsNoiseSample; ++k) x.params[static_cast<size_t>(k)] = v;
      Out o = hit(x);
      expect(o.finite && o.peak < 20.0f, "kick: extremes finite");
    }
  }

  // ---------------------------------------------------------- Sweep Kick behaviours
  {
    PluginState s = fresh("gl.sweepkick");
    Out base = hit(s);
    expect(base.finite && base.peak > 0.05f, "sweep: sounds");
    PluginState shortD = s, longD = s;
    shortD.params[SkAmpDecay] = 20;
    longD.params[SkAmpDecay] = 240;
    expect(ringLength(hit(longD)) > ringLength(hit(shortD)), "sweep: amp decay");
    PluginState hiMax = s, loMax = s;
    hiMax.params[SkMaxFreq] = 3600;
    loMax.params[SkMaxFreq] = -2400;
    expect(zcr(hit(hiMax), 0, 1500) > zcr(hit(loMax), 0, 1500), "sweep: max frequency sets the start");
    PluginState dist = s;
    dist.params[SkDist] = 128;
    expect(brightness(hit(dist)) > brightness(base), "sweep: distortion adds harmonics");
    for (float v : {0.0f, 1.0f}) {
      PluginState x = s;
      const PluginInfo* p = findPlugin("gl.sweepkick");
      for (int k = 0; k < SkCount; ++k) x.params[static_cast<size_t>(k)] = static_cast<float>(v > 0.5f ? p->params[static_cast<size_t>(k)].max : p->params[static_cast<size_t>(k)].min);
      Out o = hit(x);
      expect(o.finite && o.peak < 20.0f, "sweep: extremes finite");
    }
  }

  // ---------------------------------------------------------- malformed states
  {
    bool ok = true;
    std::vector<uint8_t> junk(300, 0xAB);
    stateFromBytes("Drumpad", junk, ok);
    expect(!ok, "malformed Drumpad state rejected");
    std::vector<uint8_t> trunc = {'S', 'S', 'L', 'F', 0x20, 1, 0, 0, '3', 'P', 'M', 'D'};
    stateFromBytes("Drumpad", trunc, ok);
    expect(!ok, "truncated Drumpad state rejected");
    stateFromBytes("Drumaxx", std::vector<uint8_t>(21644, 0), ok);
    expect(!ok, "zeroed Drumaxx state rejected");
    stateFromBytes("BassDrum", std::vector<uint8_t>(100, 1), ok);
    expect(!ok, "short BassDrum state rejected");
    std::vector<uint8_t> fk(32, 0);
    fk[4] = 0xFF;
    fk[5] = 0xFF;
    fk[6] = 0xFF;
    fk[7] = 0x7F;  // max freq out of range
    stateFromBytes("Fruit Kick", fk, ok);
    expect(!ok, "out-of-range Fruit Kick state rejected");
    std::vector<uint8_t> fk0(32, 0);
    PluginState s = stateFromBytes("Fruit Kick", fk0, ok);
    expect(ok && s.type == "gl.sweepkick", "zero Fruit Kick state (all minimum) decodes");
  }

  // ---------------------------------------------------------- shipped FL content
  corpus("Drumpad", "Drumpad", "gl.membranepad", 961, 10);
  corpus("DrumPad", "__no preset folder__", "gl.membranepad", 0, 1);
  corpus("Drumaxx", "Drumaxx", "gl.membranekit", 131, 1);
  corpus("BassDrum", "Bassdrum", "gl.kicksculpt", 52, 5);
  corpus("Fruit Kick", "Fruit Kick", "gl.sweepkick", 8, 3);
  return finish();
}
