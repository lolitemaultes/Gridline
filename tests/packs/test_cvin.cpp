// Gridline — K-170 tests: control voltage in and out. Voltage In (gl.cvin) on synthetic device
// input buffers (gate hysteresis, pitch quantising, notes exact to the sample at any block size,
// links to built-in and hosted parameters, clock tempo), the calibration fits and store (never in
// the project), Voltage Out's upgrades (old states render bit for bit as before, direct outputs,
// value / run / reset sources) and the fractional pitch on the plugin note route.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/Document.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "packs/Packs.h"
#include "packs/edison/edison.h"
#include "packs/modulation/cv.h"
#include "packs/modulation/modulation.h"
#include "packs/modulation/modulation_dsp.h"

using namespace gl;
namespace mod = gl::packs::modulation;
namespace ed = gl::packs::edison;

namespace {

constexpr double kSr = 44100.0;
bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

Document baseDoc() {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  return d;
}

// Synthetic device input: `ch` channels of `frames` samples (digital, 1.0 = full scale).
struct Input {
  std::vector<std::vector<float>> c;
  Input(int ch, int64_t frames) : c(static_cast<size_t>(ch), std::vector<float>(static_cast<size_t>(frames), 0.f)) {}
  void set(int ch, int64_t from, int64_t to, float v) {
    for (int64_t i = from; i < to && i < static_cast<int64_t>(c[0].size()); ++i) c[static_cast<size_t>(ch)][static_cast<size_t>(i)] = v;
  }
};

// One routed note event at an absolute sample.
struct Ev {
  int64_t at;
  int s, a, b;
  float pitch;
};

// Runs a Voltage In generator directly over the input in blocks; returns every note event.
struct Direct {
  std::unique_ptr<Generator> g = createGenerator("gl.cvin");
  PluginState st = defaultPluginState("gl.cvin");
  ChannelAux aux;
  std::vector<Ev> ev;
  std::vector<std::vector<float>> outs;  // controller outputs after each block
  void run(const Input& in, int block, double tempo = 120) {
    packs::prepareAux(st, aux);
    const int64_t total = static_cast<int64_t>(in.c[0].size());
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
    for (int64_t pos = 0; pos < total; pos += block) {
      const int n = static_cast<int>(std::min<int64_t>(block, total - pos));
      const float* ptrs[16] = {};
      for (size_t k = 0; k < in.c.size() && k < 16; ++k) ptrs[k] = in.c[k].data() + pos;
      ProcessContext ctx;
      ctx.sampleRate = kSr;
      ctx.tempo = tempo;
      ctx.deviceIn = ptrs;
      ctx.deviceInCount = static_cast<int>(in.c.size());
      std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
      g->process(st, aux, ctx, L.data(), R.data(), n);
      Generator::MidiOutEvent mo[256];
      const int m = g->takeMidiOut(mo, 256);
      for (int k = 0; k < m; ++k) ev.push_back({pos + mo[k].offset, mo[k].s, mo[k].a, mo[k].b, mo[k].pitch});
      float co[16];
      const int c = g->controlOutputs(co, 16);
      outs.emplace_back(co, co + c);
      for (int i = 0; i < n; ++i)
        if (L[static_cast<size_t>(i)] != 0.f || R[static_cast<size_t>(i)] != 0.f) silent = false;
    }
  }
  bool silent = true;
  const mod::CvInMonitor* monitor() const { return dynamic_cast<const mod::CvInMonitor*>(g.get()); }
};

bool sameEvents(const std::vector<Ev>& a, const std::vector<Ev>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].at != b[i].at || a[i].s != b[i].s || a[i].a != b[i].a || a[i].b != b[i].b || a[i].pitch != b[i].pitch) return false;
  return true;
}

// The old Voltage Out render loop (before K-170), kept as the reference: old states must still
// produce exactly these samples.
struct OldVoltageOut {
  int held = 0;
  double heldPitch[8] = {};
  double pitch = 60, target = 60, glideCoef = 1;
  float velocity = 0;
  int pulse = 441, trigLeft = 0, clockLeft = 0;
  bool hasPitch = false;
  void noteOn(double p, float v) {
    heldPitch[held++] = p;
    target = p;
    if (!hasPitch) pitch = p;
    hasPitch = true;
    velocity = v;
    trigLeft = pulse;
  }
  // Ends the newest note; the pitch falls back to the note still held.
  void noteOff() {
    held = std::max(0, held - 1);
    if (held > 0) target = heldPitch[held - 1];
  }
  void render(const std::vector<float>& s, const ProcessContext& ctx, float* L, float* R, int n) {
    static const int kPpq[] = {2, 4, 8, 12, 16, 24, 32, 48, 64, 96};
    auto prm = [&](int k, float d) { return static_cast<size_t>(k) < s.size() ? s[static_cast<size_t>(k)] : d; };
    pulse = std::max(1, static_cast<int>(prm(mod::CvPulse, 0.01f) * ctx.sampleRate));
    const double glide = prm(mod::CvGlide, 0);
    glideCoef = glide > 0 ? 1.0 - std::exp(-1.0 / (glide * ctx.sampleRate)) : 1.0;
    const int range = static_cast<int>(prm(mod::CvRange, 0) + 0.5f);
    const double vMax = range == 0 ? 5.0 : 10.0, vMin = range == 2 ? 0.0 : -vMax;
    const double pitchScale = prm(mod::CvPitchScale, 0) >= 0.5f ? 1.2 : 1.0;
    const int ppqIdx = std::clamp(static_cast<int>(prm(mod::CvClockPpq, 5) + 0.5f), 0, 9);
    const double spt = ctx.sampleRate * 60.0 / (ctx.tempo * std::max(1, ctx.ppq));
    const double clockTicks = static_cast<double>(std::max(1, ctx.ppq)) / kPpq[ppqIdx];
    for (int i = 0; i < n; ++i) {
      pitch += (target - pitch) * glideCoef;
      if (ctx.playing) {
        double t0 = ctx.tick + (i - 1) / spt, t1 = ctx.tick + i / spt;
        if (i == 0 || std::floor(t1 / clockTicks) != std::floor(t0 / clockTicks))
          if (i != 0 || std::fmod(t1, clockTicks) < 1.0 / spt) clockLeft = pulse;
      }
      const double gateV = held > 0 ? vMax : 0.0;
      double v[2];
      for (int side = 0; side < 2; ++side) {
        const int src = static_cast<int>(prm(side ? mod::CvRightSrc : mod::CvLeftSrc, 0) + 0.5f);
        double volts = 0;
        switch (src) {
          case 1: volts = (pitch - 60.0) / 12.0 * pitchScale; break;
          case 2: volts = gateV; break;
          case 3: volts = held > 0 ? velocity * vMax : 0.0; break;
          case 4: volts = clockLeft > 0 ? vMax : 0.0; break;
          case 5: volts = trigLeft > 0 ? vMax : 0.0; break;
          default: break;
        }
        const double ofs = prm(side ? mod::CvRightOfs : mod::CvLeftOfs, 0);
        const bool bip = prm(side ? mod::CvRightBipolar : mod::CvLeftBipolar, 0) >= 0.5f;
        const double offV = bip ? ofs * vMax : (ofs * 0.5 + 0.5) * vMax * (ofs != 0 ? 1.0 : 0.0);
        const double scale = prm(side ? mod::CvRightScale : mod::CvLeftScale, 1);
        volts = std::clamp(volts * scale + offV, vMin, vMax);
        v[side] = volts / 10.0;
      }
      if (trigLeft > 0) --trigLeft;
      if (clockLeft > 0) --clockLeft;
      if (prm(mod::CvEnable, 0) >= 0.5f) {
        L[i] += static_cast<float>(v[0]);
        R[i] += static_cast<float>(v[1]);
      }
    }
  }
};

}  // namespace

int main() {
  const PluginInfo* in = findPlugin("gl.cvin");
  const PluginInfo* out = findPlugin("gl.cvctl");

  // ------------------------------------------------------------ 1. catalogue and old states
  {
    expect(in && in->params.size() == static_cast<size_t>(mod::CiCount) && in->outputs.size() == 8, "Voltage In: params and 8 outputs");
    expect(in && in->presets.size() >= 2 && in->presets.size() <= 6 && in->editorWidth > 0, "Voltage In: presets and editor width");
    expect(out && out->params.size() == static_cast<size_t>(mod::CvCount), "Voltage Out: new params appended");
    PluginState shortIn;
    shortIn.type = "gl.cvin";
    shortIn.params = {1.0f, 0.0f};
    normalizePluginState(shortIn);
    expect(shortIn.params.size() == static_cast<size_t>(mod::CiCount) && shortIn.params[mod::CiFullScale] == 10.0f &&
               shortIn.params[mod::CiZeroNote] == 60.0f,
           "Voltage In: a short state fills with defaults");
    PluginState oldOut = defaultPluginState("gl.cvctl");
    oldOut.params.resize(mod::CvCountV1);
    normalizePluginState(oldOut);
    expect(oldOut.params.size() == static_cast<size_t>(mod::CvCount) && oldOut.params[mod::CvOutMode] == 0.0f &&
               oldOut.params[mod::CvFullScale] == 10.0f && oldOut.params[mod::CvLeftTrim] == 0.0f && oldOut.params[mod::CvLeftFine] == 0.0f,
           "Voltage Out: an old state gets the neutral new settings");
    for (size_t p = 0; p < in->presets.size(); ++p) {
      PluginState ps = presetState(*in, p);
      expect(ps.params.size() == in->params.size(), "Voltage In preset has every parameter");
    }
  }

  // ------------------------------------------------------------ 2. voltage maths
  {
    const unsigned major = mod::scales()[1].mask;
    expect(mod::quantisePitch(61.4, mod::CqOff, major, 0) == 61.4, "quantise off keeps the pitch");
    expect(mod::quantisePitch(61.4, mod::CqSemitone, major, 0) == 61.0 && mod::quantisePitch(61.6, mod::CqSemitone, major, 0) == 62.0,
           "quantise to the nearest semitone");
    expect(mod::quantisePitch(61.4, mod::CqScale, major, 0) == 62.0 && mod::quantisePitch(61.0, mod::CqScale, major, 0) == 62.0 &&
               mod::quantisePitch(60.9, mod::CqScale, major, 0) == 60.0,
           "quantise to C major (C# goes up to D on a tie, nearest otherwise)");
    expect(mod::quantisePitch(61.0, mod::CqScale, major, 1) == 61.0 && mod::quantisePitch(62.0, mod::CqScale, major, 1) == 63.0,
           "quantise to C# major");
    expect(mod::quantisePitch(std::nan(""), mod::CqScale, major, 0) == 0.0, "quantise: NaN gives a finite pitch");
    // Gate hysteresis: a noisy edge crossing the threshold opens the gate once and closes it once.
    mod::GateDetector g;
    int changes = 0;
    uint32_t seed = 1;
    for (int i = 0; i < 4000; ++i) {
      seed = seed * 1664525u + 1013904223u;
      const double noise = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.4;  // +/-0.2 V
      const double ramp = i < 2000 ? 3.0 * i / 2000.0 : 3.0 * (4000 - i) / 2000.0;  // 0..3 V and back
      changes += g.step(ramp + noise, 1.5, 0.5) ? 1 : 0;
    }
    expect(changes == 2, "gate hysteresis: a noisy edge gives one opening and one closing");
    mod::GateDetector g2;
    int chatter = 0;
    seed = 1;
    for (int i = 0; i < 4000; ++i) {
      seed = seed * 1664525u + 1013904223u;
      const double noise = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.4;
      const double ramp = i < 2000 ? 3.0 * i / 2000.0 : 3.0 * (4000 - i) / 2000.0;
      chatter += g2.step(ramp + noise, 1.5, 0.0) ? 1 : 0;
    }
    expect(chatter > 2, "without hysteresis the same edge chatters (the test can tell)");
    // Scaling both directions.
    mod::CvCal none;
    expect(mod::outputDigital(1.0, 10.0, none, 0, 0) == 1.0 / 10.0, "output: no trims and 10 V full scale = volts / 10 exactly");
    mod::CvCal c{1.02, -0.013, true};
    const double d = mod::outputDigital(2.5, 8.0, c, 12, 1.5);
    expect(near(mod::inputVolts(d, 8.0, mod::CvCal{}, 0, 0) * c.gain + c.offset, (2.5 * 1.015 + 0.012), 1e-12),
           "output trims and calibration: the interface delivers the trimmed volts");
    expect(near(mod::inputVolts(0.25, 10.0, c, 0, 0), (2.5 - c.offset) / c.gain, 1e-12), "input calibration undoes gain and offset");
    expect(near(mod::voltsToPitch(1.0, 1.0, 60), 72, 1e-12) && near(mod::voltsToPitch(1.2, 1.2, 60), 72, 1e-12) &&
               near(mod::pitchToVolts(54, 1.0, 60), -0.5, 1e-12),
           "1 V/oct and 1.2 V/oct, C4 at 0 V");
  }

  // ------------------------------------------------------------ 3. notes: exact samples, any block size
  {
    const int64_t T = 44100;
    Input x(4, T);
    // Pitch on In 1 (digital = volts / 10), gate on In 2 (5 V = 0.5).
    x.set(0, 0, 9000, 0.1f);        // +1 V = C5 (72)
    x.set(0, 9000, T, 0.025f);      // +0.25 V = D#4 (63)
    x.set(1, 1234, 5678, 0.5f);     // note 1
    x.set(1, 10007, 20011, 0.5f);   // note 2
    x.set(1, 30000, 30001, 0.5f);   // a one-sample pulse
    std::vector<Ev> ref;
    bool same = true;
    for (int block : {512, 64, 100, 1024, 7}) {
      Direct r;
      r.run(x, block);
      if (block == 512) ref = r.ev;
      else same = same && sameEvents(ref, r.ev);
      if (block == 512) expect(r.silent, "Voltage In adds no audio");
    }
    expect(same, "note events are the same at every block size");
    expect(ref.size() == 6, "three gates give three notes on and off");
    if (ref.size() == 6) {
      expect(ref[0].s == 0x90 && ref[0].at == 1234 && ref[0].a == 72 && ref[0].pitch == 72.0f, "note on at the gate's first sample, C5 from +1 V");
      expect(ref[1].s == 0x80 && ref[1].at == 5678 && ref[1].a == 72, "note off at the sample the gate falls");
      expect(ref[2].at == 10007 && ref[2].a == 63 && ref[3].at == 20011, "second note: D#4 from +0.25 V, exact edges");
      expect(ref[4].at == 30000 && ref[5].at == 30001, "a one-sample gate still plays");
    }
    // Unquantised pitch travels as a fraction.
    Direct f;
    f.st.params[mod::CiQuantise] = mod::CqOff;
    Input y(2, 4096);
    y.set(0, 0, 4096, 0.0425f);  // 0.425 V = 65.1
    y.set(1, 100, 4000, 0.5f);
    f.run(y, 256);
    expect(!f.ev.empty() && f.ev[0].a == 65 && near(f.ev[0].pitch, 65.1, 1e-4), "quantise off: the note carries its fractional pitch");
    // 1.2 V/oct and a scale.
    Direct s;
    s.st.params[mod::CiPitchScale] = 1;
    s.st.params[mod::CiQuantise] = mod::CqScale;
    Input z(2, 4096);
    z.set(0, 0, 4096, 0.042f);  // 0.42 V at 1.2 V/oct = 4.2 semitones = 64.2 -> C major: E (64); at 1 V/oct it would be F
    z.set(1, 10, 4000, 0.5f);
    s.run(z, 256);
    expect(!s.ev.empty() && s.ev[0].a == 64, "1.2 V/oct, quantised to C major");
  }

  // ------------------------------------------------------------ 4. legato, retrigger, trigger mode
  {
    Input x(2, 20000);
    x.set(0, 0, 5000, 0.0f);
    x.set(0, 5000, 20000, 0.05f);  // pitch moves to +0.5 V (66) while the gate is held
    x.set(1, 1000, 15000, 0.5f);
    Direct leg;
    leg.run(x, 256);
    expect(leg.ev.size() == 3 && leg.ev[1].s == Generator::MidiOutEvent::kGlide && leg.ev[1].at == 5000 && leg.ev[1].a == 60 &&
               leg.ev[1].pitch == 66.0f && leg.ev[2].s == 0x80 && leg.ev[2].a == 60,
           "legato: a pitch change while held glides the note (same key for its note off)");
    Direct re;
    re.st.params[mod::CiRetrigger] = 1;
    re.run(x, 256);
    expect(re.ev.size() == 4 && re.ev[1].s == 0x80 && re.ev[1].at == 5000 && re.ev[2].s == 0x90 && re.ev[2].a == 66 && re.ev[2].at == 5000,
           "retrigger: a pitch change while held starts a new note on that sample");
    Direct tr;
    tr.st.params[mod::CiGateMode] = 1;
    tr.st.params[mod::CiTrigLen] = 0.01f;
    tr.run(x, 256);
    expect(tr.ev.size() == 2 && tr.ev[0].at == 1000 && tr.ev[1].s == 0x80 && tr.ev[1].at == 1000 + 440, "trigger mode: fixed-length notes");
  }

  // ------------------------------------------------------------ 5. extremes and no input
  {
    Input x(6, 8192);
    for (int c = 0; c < 6; ++c)
      for (int i = 0; i < 8192; ++i)
        x.c[static_cast<size_t>(c)][static_cast<size_t>(i)] = i % 3 == 0 ? std::nanf("") : (i % 3 == 1 ? 1e30f : -1e30f);
    Direct r;
    for (int k = 0; k < mod::kCvInputs; ++k) r.st.params[mod::cvInParam(k, mod::CfSource)] = static_cast<float>(k + 1);
    r.st.params[mod::CiClockIn] = 1, r.st.params[mod::CiRunIn] = 2;
    r.run(x, 512);
    bool finite = true;
    for (const auto& o : r.outs)
      for (float v : o) finite = finite && std::isfinite(v) && v >= 0.f && v <= 1.f;
    for (const auto& e : r.ev) finite = finite && std::isfinite(e.pitch);
    expect(finite, "NaN and huge input samples: outputs stay finite and in 0..1");
    const PluginInfo* info = in;
    bool ext = true;
    for (int side = 0; side < 2; ++side) {
      Direct m;
      for (size_t k = 0; k < info->params.size(); ++k) m.st.params[k] = static_cast<float>(side ? info->params[k].max : info->params[k].min);
      m.run(x, 300);
      for (const auto& o : m.outs)
        for (float v : o) ext = ext && std::isfinite(v);
    }
    expect(ext, "every parameter at its minimum / maximum: finite outputs");
    // No device input at all (offline, no input device).
    auto g = createGenerator("gl.cvin");
    PluginState st = defaultPluginState("gl.cvin");
    ChannelAux aux;
    ProcessContext ctx;
    std::vector<float> L(512, 0.f), R(512, 0.f);
    g->process(st, aux, ctx, L.data(), R.data(), 512);
    Generator::MidiOutEvent mo[8];
    float co[16];
    const int m = g->controlOutputs(co, 16);
    expect(g->takeMidiOut(mo, 8) == 0 && m == 8 && co[mod::CoGate] == 0.f && co[mod::CoA] == 0.f, "no device input: no notes, outputs at rest");
  }

  // ------------------------------------------------------------ 6. CV outputs and block-size invariance
  {
    Input x(4, 16384);
    for (int i = 0; i < 16384; ++i) x.c[2][static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(i * 0.001));  // CV A on In 3
    std::vector<float> ref;
    bool same = true;
    for (int block : {64, 128, 256}) {  // controller outputs are compared at the same sample (every 256)
      Direct r;
      r.st.params[mod::cvInParam(mod::CisA, mod::CfSource)] = 3;
      r.st.params[mod::CiSmooth] = 0.0005f;
      r.run(x, block);
      std::vector<float> at;
      const int every = 256 / block;
      for (size_t k = static_cast<size_t>(every - 1); k < r.outs.size(); k += static_cast<size_t>(every)) at.push_back(r.outs[k][mod::CoA]);
      if (block == 64) ref = at;
      else same = same && at == ref;
    }
    expect(same && !ref.empty(), "CV outputs are the same at every block size");
    // +/-5 V maps to 0..1: 0.3 digital = 3 V -> 0.8.
    Input c(4, 8192);
    c.set(2, 0, 8192, 0.3f);
    Direct r;
    r.st.params[mod::cvInParam(mod::CisA, mod::CfSource)] = 3;
    r.run(c, 512);
    expect(near(r.outs.back()[mod::CoA], 0.8, 1e-4), "CV A: +3 V in +/-5 V = 0.8");
    r.st.params[mod::cvInParam(mod::CisA, mod::CfOffset)] = -500;  // trim -0.5 V
    r.st.params[mod::cvInParam(mod::CisA, mod::CfScale)] = 10;      // +10 %
    r.run(c, 512);
    expect(near(r.outs.back()[mod::CoA], (3.0 * 1.1 - 0.5 + 5.0) / 10.0, 1e-4), "input trims: scale then offset");
    expect(r.monitor() && near(r.monitor()->deviceVolts(2), 3.0, 1e-5), "monitor: raw device volts for calibration");
  }

  // ------------------------------------------------------------ 7. clock: tempo from edges, run
  {
    for (int ppqIdx : {0, 3, 5, 9}) {
      const int ppq = mod::kCvPpq[ppqIdx];
      mod::ClockTracker t;
      const double iv = kSr * 60.0 / (123.0 * ppq);
      for (int k = 0; k < 200; ++k) t.edge(static_cast<int64_t>(std::llround(k * iv)));  // rounding = up to half a sample of jitter
      expect(near(t.bpm(kSr, ppq), 123.0, 0.05), "clock tracker: 123 BPM at each pulse rate");
    }
    mod::ClockTracker j;
    for (int k = 0; k < 100; ++k) j.edge(k * 1000);
    for (int k = 0; k < 30; ++k) j.edge(100000 + k * 500);  // tempo doubles
    expect(near(j.bpm(kSr, 24), 60.0 * kSr / (500.0 * 24), 0.01), "clock tracker follows a tempo change");
    // Through Voltage In: 24 PPQ pulses of 5 V at 100 BPM on In 3, run high on In 4.
    const int64_t T = 3 * 44100;
    Input x(4, T);
    const double iv = kSr * 60.0 / (100.0 * 24);
    for (int k = 0; k * iv < T; ++k) {
      const int64_t a = static_cast<int64_t>(std::llround(k * iv));
      x.set(2, a, a + 100, 0.5f);
    }
    x.set(3, 0, T, 0.5f);
    Direct r;
    r.st.params[mod::CiClockIn] = 3;
    r.st.params[mod::CiRunIn] = 4;
    r.run(x, 512);
    expect(r.monitor() && near(r.monitor()->clockBpm(), 100.0, 0.05) && r.monitor()->run() == 1, "Voltage In reads 100 BPM and run");
  }

  // ------------------------------------------------------------ 8. calibration fits and store
  {
    std::vector<double> xs, ys;
    uint32_t seed = 7;
    for (int k = -2; k <= 5; ++k) {
      seed = seed * 1664525u + 1013904223u;
      xs.push_back(k);
      ys.push_back(0.97 * k + 0.031 + (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.002);
    }
    const mod::LineFit lf = mod::fitLine(xs, ys);
    expect(lf.ok && near(lf.gain, 0.97, 0.001) && near(lf.offset, 0.031, 0.001) && lf.rmsError < 0.001, "round trip fit: gain and offset");
    expect(!mod::fitLine({1, 1}, {2, 3}).ok, "fit needs two different voltages");
    std::vector<double> t, held, ac;
    for (int i = 0; i < 150; ++i) {
      const double s = i / 60.0;
      t.push_back(s);
      held.push_back(5.0 + 0.001 * std::sin(i));
      ac.push_back(5.0 * std::exp(-s / 0.3));  // a 0.3 s coupling time constant
    }
    const mod::CouplingResult dc = mod::analyseCoupling(t, held), acr = mod::analyseCoupling(t, ac);
    expect(dc.ok && dc.dcCoupled && near(dc.endVolts, 5.0, 0.01), "coupling check: a held step is DC-coupled");
    expect(acr.ok && !acr.dcCoupled, "coupling check: a decaying step is AC-coupled");
    expect(!mod::analyseCoupling(t, std::vector<double>(t.size(), 0.0)).ok, "coupling check: nothing arriving is not a result");
    // Tuner fit through the pitch detector: an oscillator that tracks 1.03 octaves per volt from 110 Hz.
    std::vector<double> volts = {0, 1, 2, 3}, hz;
    for (double v : volts) {
      const double f = 110.0 * std::pow(2.0, 1.03 * v);
      ed::Audio au = ed::makeAudio(kSr, 1, 16384);
      for (int64_t i = 0; i < au.frames(); ++i) au.L[static_cast<size_t>(i)] = au.R[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * f * i / kSr));
      const ed::PitchInfo pi = ed::detectPitch(au, ed::Range{0, au.frames()});
      hz.push_back(pi.hz);
    }
    const mod::TunerFit tf = mod::fitTuner(volts, hz, 1.0);
    expect(tf.ok && near(tf.octavesPerVolt, 1.03, 0.002) && near(tf.gain, 1.03, 0.002), "tuner fit: 1.03 octaves per volt measured");
    bool after = tf.centsAfter.size() == 4 && std::fabs(tf.centsBefore[3]) > 100;
    for (double c : tf.centsAfter) after = after && std::fabs(c) < 3.0;
    expect(after, "tuner fit: 3 octaves off by over 100 cents before, within 3 cents after");
    // Store: text round trip, per device and channel.
    mod::CalStore s;
    s.set(mod::calKey(true, "Desk interface", 0), mod::CvCal{2.0, 0.1, true});
    s.set(mod::calKey(false, "Desk interface", 1), mod::CvCal{1.01, 0.0, true});
    s.set(mod::calKey(false, "bad", 0), mod::CvCal{std::nan(""), 0, true});
    mod::CalStore back;
    expect(back.fromText(s.toText()) && back.all().size() == 2 && back.get(mod::calKey(true, "Desk interface", 0)).gain == 2.0 &&
               !back.get(mod::calKey(true, "Other", 0)).valid,
           "calibration store: round trip, invalid entries dropped, other devices unaffected");
  }

  // ------------------------------------------------------------ 9. calibration reaches the plugins, never the project
  {
    mod::CalStore s;
    s.set(mod::calKey(true, "test-in", 2), mod::CvCal{2.0, 0.5, true});  // the input reads 2 x + 0.5
    s.set(mod::calKey(false, "test-out", 0), mod::CvCal{1.25, 0.0, true});
    mod::setCalStore(s);
    mod::setCalDevices("test-in", "test-out");
    Input c(4, 4096);
    c.set(2, 0, 4096, 0.25f);  // reads 2.5 V raw -> (2.5 - 0.5) / 2 = 1 V true
    Direct r;
    r.st.params[mod::cvInParam(mod::CisA, mod::CfSource)] = 3;
    r.run(c, 512);
    expect(near(r.outs.back()[mod::CoA], 0.6, 1e-4), "input calibration applies (1 V in +/-5 V = 0.6)");
    Direct nc;
    nc.st.params[mod::cvInParam(mod::CisA, mod::CfSource)] = 3;
    nc.st.params[mod::CiUseCal] = 0;
    nc.run(c, 512);
    expect(near(nc.outs.back()[mod::CoA], 0.75, 1e-4), "Use calibration off: raw volts");
    // Voltage Out with output calibration: 1 V sent as 1 / 1.25.
    Document d = baseDoc();
    d.setPatternLength(d.song().patterns[0].id, 1, 16);
    d.addClip(d.song().patterns[0].id, 0, 0);
    ObjectId cv = d.addChannel("cv", "gl.cvctl");
    d.setParam(channelPluginParam(cv, mod::CvEnable), 1);
    d.addNote(d.song().patterns[0].id, cv, {0, 192, 72, 100});
    ObjectId vin = d.addChannel("cv in", "gl.cvin");
    const std::string before = projectToText(d.song());
    {
      ProgramBuilder b;
      AudioEngine eng{kSr};
      eng.setProgram(b.build(d.song()));
      eng.setMode(PlayMode::Song);
      eng.play();
      std::vector<float> L(4410), R(4410);
      for (int p = 0; p < 4410; p += 441) eng.process(L.data() + p, R.data() + p, 441);
      expect(near(L[4000], 0.1 / 1.25, 1e-4), "output calibration applies to Voltage Out (1 V / 1.25 gain)");
    }
    mod::setCalStore(mod::CalStore{});
    const std::string after = projectToText(d.song());
    expect(before == after && before.find("test-in") == std::string::npos && before.find("test-out") == std::string::npos &&
               before.find("calibration") == std::string::npos,
           "calibration is never saved in the project");
    (void)vin;
    mod::setCalDevices("", "");
  }

  // ------------------------------------------------------------ 10. Voltage Out: old states bit for bit
  {
    bool same = true;
    struct Case {
      int l, r;
      float lofs, rofs, lsc, rsc, lbip, rbip, ps, range, glide;
    };
    const Case cases[] = {{1, 2, 0, 0, 1, 1, 1, 0, 0, 0, 0},       {4, 5, 0.1f, -0.3f, 0.7f, 0.5f, 1, 0, 1, 1, 0.05f},
                          {3, 1, -0.5f, 0.2f, 0.9f, 1, 0, 1, 0, 2, 0}, {5, 4, 0, 0, 1, 0.3f, 0, 0, 1, 1, 0.2f}};
    for (const Case& k : cases) {
      std::vector<float> old(mod::CvCountV1, 0.f);
      const PluginState dflt = defaultPluginState("gl.cvctl");
      for (int i = 0; i < mod::CvCountV1; ++i) old[static_cast<size_t>(i)] = dflt.params[static_cast<size_t>(i)];
      old[mod::CvEnable] = 1, old[mod::CvLeftSrc] = static_cast<float>(k.l), old[mod::CvRightSrc] = static_cast<float>(k.r);
      old[mod::CvLeftOfs] = k.lofs, old[mod::CvRightOfs] = k.rofs, old[mod::CvLeftScale] = k.lsc, old[mod::CvRightScale] = k.rsc;
      old[mod::CvLeftBipolar] = k.lbip, old[mod::CvRightBipolar] = k.rbip, old[mod::CvPitchScale] = k.ps, old[mod::CvRange] = k.range;
      old[mod::CvGlide] = k.glide;
      PluginState st;
      st.type = "gl.cvctl";
      st.params = old;
      normalizePluginState(st);
      auto g = createGenerator("gl.cvctl");
      ChannelAux aux;
      packs::prepareAux(st, aux);
      OldVoltageOut ref;
      std::vector<float> L(512), R(512), rl(512), rr(512);
      for (int b = 0; b < 40; ++b) {
        ProcessContext ctx;
        ctx.sampleRate = kSr;
        ctx.tempo = 128;
        ctx.ppq = 96;
        ctx.playing = true;
        ctx.tick = b * 512 * (128.0 * 96 / 60.0) / kSr;
        // Notes at the start of a block: both start them before rendering it.
        if (b == 3) g->noteOn(1, 67.5, 0.8f, 0), ref.noteOn(67.5, 0.8f);
        if (b == 9) g->noteOn(2, 48, 0.4f, 0), ref.noteOn(48, 0.4f);
        if (b == 20) g->noteOff(2, 0), g->noteOff(1, 0), ref.noteOff(), ref.noteOff();
        std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
        std::fill(rl.begin(), rl.end(), 0.f), std::fill(rr.begin(), rr.end(), 0.f);
        g->process(st, aux, ctx, L.data(), R.data(), 512);
        ref.render(old, ctx, rl.data(), rr.data(), 512);
        same = same && std::equal(L.begin(), L.end(), rl.begin()) && std::equal(R.begin(), R.end(), rr.begin());
      }
    }
    expect(same, "Voltage Out: older states render bit for bit as before");
  }

  // ------------------------------------------------------------ 11. Voltage Out: new sources, trims, direct outputs
  {
    auto render = [](PluginState st, ProcessContext ctx, std::vector<float>& L, std::vector<float>& R, mod::CvAux* cal = nullptr) {
      auto g = createGenerator("gl.cvctl");
      ChannelAux aux;
      if (cal) aux.custom = std::make_shared<mod::CvAux>(*cal);
      L.assign(512, 0.f), R.assign(512, 0.f);
      g->process(st, aux, ctx, L.data(), R.data(), 512);
      return g;
    };
    PluginState st = defaultPluginState("gl.cvctl");
    st.params[mod::CvEnable] = 1;
    st.params[mod::CvLeftSrc] = mod::CvsValue;
    st.params[mod::CvLeftValue] = 0.75f;  // +/-5 V: 2.5 V
    st.params[mod::CvRightSrc] = mod::CvsRun;
    st.params[mod::CvRightBipolar] = 0;
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    ctx.playing = true;
    std::vector<float> L, R;
    render(st, ctx, L, R);
    expect(near(L[100], 0.25, 1e-6) && near(R[100], 0.5, 1e-6), "value source: 75 % = +2.5 V; run: high while playing");
    ctx.playing = false;
    render(st, ctx, L, R);
    expect(near(R[100], 0.0, 1e-9), "run: low when stopped");
    st.params[mod::CvRightSrc] = mod::CvsReset;
    ctx.playing = true;
    render(st, ctx, L, R);
    expect(R[0] > 0.4f && R[511] == 0.f, "reset: a pulse when playback starts");
    st.params[mod::CvFullScale] = 5;  // a 5 V interface: 2.5 V is half scale
    st.params[mod::CvLeftTrim] = 100;
    st.params[mod::CvLeftFine] = -2;
    render(st, ctx, L, R);
    expect(near(L[100], (2.5 * 0.98 + 0.1) / 5.0, 1e-6), "full scale, fine scale and trim");
    // Direct mode: no direct pairs -> through the mixer; with a pair -> straight to it.
    st.params[mod::CvOutMode] = mod::CvmDirect;
    st.params[mod::CvOutPair] = 1;
    ctx.directOutPairs = 0;
    render(st, ctx, L, R);
    expect(L[100] != 0.f, "direct mode without extra outputs stays on the mixer path");
    ctx.directOutPairs = 2;
    auto g = render(st, ctx, L, R);
    int pair = -1;
    const float *dl = nullptr, *dr = nullptr;
    expect(L[100] == 0.f && g->directOutput(pair, dl, dr) && pair == 1 && dl && near(dl[100], (2.5 * 0.98 + 0.1) / 5.0, 1e-6),
           "direct mode: nothing on the mixer path, the voltage on its own pair");
    // The engine adds it to that pair (taps of the ASIO driver).
    Document d = baseDoc();
    ObjectId cv = d.addChannel("cv", "gl.cvctl");
    for (size_t k = 0; k < st.params.size(); ++k) d.setParam(channelPluginParam(cv, static_cast<uint32_t>(k)), st.params[k], false);
    d.setParam(channelPluginParam(cv, mod::CvOutPair), 0, false);
    ProgramBuilder b;
    AudioEngine eng{kSr};
    eng.setProgram(b.build(d.song()));
    std::vector<float> mL(512), mR(512), tL(512), tR(512);
    float* tl[1] = {tL.data()};
    float* tr[1] = {tR.data()};
    const int mix[1] = {-1};
    AudioEngine::OutputTaps taps;
    taps.count = 1, taps.mixer = mix, taps.L = tl, taps.R = tr;
    for (int k = 0; k < 4; ++k) eng.process(mL.data(), mR.data(), 512, &taps);
    expect(mL[300] == 0.f && near(tL[300], (2.5 * 0.98 + 0.1) / 5.0, 1e-6), "engine: a direct Voltage Out reaches its device pair, not the mix");
  }

  // ------------------------------------------------------------ 12. engine: notes into a channel, fractional pitch, links
  {
    // Voltage In plays into a Voltage Out (it shows the pitch it received as a voltage).
    Document d = baseDoc();
    ObjectId vin = d.addChannel("cv in", "gl.cvin");
    ObjectId vout = d.addChannel("cv out", "gl.cvctl");
    ObjectId syn = d.addChannel("lead", "gl.synth");
    d.setParam(channelPluginParam(vin, mod::CiQuantise), mod::CqOff, false);
    d.setParam(channelPluginParam(vin, mod::cvInParam(mod::CisA, mod::CfSource)), 3, false);
    d.setParam(channelPluginParam(vout, mod::CvEnable), 1, false);
    d.editSong("play into", [&](Song& s) {
      s.channel(vin)->midiOutPort = 7;
      s.channel(vout)->midiInPort = 7;
      s.channel(syn)->midiInPort = 7;
    });
    // CV A linked to the synth's volume and to a hosted plugin's parameter.
    PluginInfo hosted;
    hosted.type = "clap:org.gridline.test.cvtarget";
    hosted.name = "CV target";
    hosted.kind = PluginKind::Generator;
    hosted.params = {ParamDesc{"Cutoff", "", 0, 1, 0.5, ParamKind::Continuous, ParamScale::Linear, {}}};
    registerDynamicPlugin(hosted);
    ObjectId hc = d.addChannel("hosted", hosted.type);
    ModLink l1;
    l1.sourceOwner = vin, l1.sourceSlot = -1, l1.sourceOutput = mod::CoA, l1.owner = syn, l1.index = channelParam(syn, ChannelParam::Volume).index;
    expect(d.setModLink(l1), "link Voltage In's CV A to a built-in parameter");
    ModLink l2 = l1;
    l2.owner = hc, l2.index = channelPluginParam(hc, 0).index;
    expect(d.setModLink(l2), "link Voltage In's CV A to a hosted plugin's parameter");
    ProgramBuilder b;
    AudioEngine eng{kSr};
    eng.setProgram(b.build(d.song()));
    eng.setInputChannels(4);
    const int B = 256, T = 172 * B;  // whole blocks (the buffers are T long)
    std::vector<float> L(static_cast<size_t>(T)), R(static_cast<size_t>(T));
    std::vector<float> inter(static_cast<size_t>(B) * 4);
    for (int p = 0; p < T; p += B) {
      for (int i = 0; i < B; ++i) {
        const int at = p + i;
        inter[static_cast<size_t>(i) * 4 + 0] = 0.0425f;                           // pitch 0.425 V = 65.1
        inter[static_cast<size_t>(i) * 4 + 1] = at >= 10000 && at < 30000 ? 0.5f : 0.f;  // gate
        inter[static_cast<size_t>(i) * 4 + 2] = 0.2f;                              // CV A: +2 V = 0.7
        inter[static_cast<size_t>(i) * 4 + 3] = 0.f;
      }
      eng.pushInput(inter.data(), B);
      eng.process(L.data() + p, R.data() + p, B);
    }
    // The Voltage Out target sends the received pitch out: (65.1 - 60) / 12 V = 0.425 V = 0.0425.
    expect(near(b.controllerOutput(vout, -1, 0) * 127.0, 65.1, 0.01), "the fractional pitch reaches the target channel");
    double v = 0;
    const ParamDesc vol = *describeParam(d.song(), channelParam(syn, ChannelParam::Volume));
    expect(b.modulatedValue(channelParam(syn, ChannelParam::Volume), v) && near(v, vol.fromNormalized(0.7), 1e-3),
           "CV A (+2 V) drives a built-in parameter through its link");
    expect(b.modulatedValue(channelPluginParam(hc, 0), v) && near(v, 0.7, 1e-3), "CV A drives a hosted plugin's parameter through its link");
    double before = 0, during = 0;
    for (int i = 2000; i < 9000; ++i) before += std::fabs(L[static_cast<size_t>(i)]);
    for (int i = 12000; i < 28000; ++i) during += std::fabs(L[static_cast<size_t>(i)]);
    expect(before == 0.0 || before < 1e-3 * during, "silence before the gate");
    expect(during > 1.0, "the gate plays the channels Voltage In plays into");
  }

  // ------------------------------------------------------------ 13. fractional pitch fallback for hosted plugins
  {
    // The route clamps and splits: checked through Voltage In's own event (the engine helpers are private);
    // a quantised note carries an integer pitch, so instruments without tuning lose nothing.
    Direct q;
    Input y(2, 2048);
    y.set(0, 0, 2048, 0.0425f);
    y.set(1, 10, 2000, 0.5f);
    q.run(y, 256);
    expect(!q.ev.empty() && q.ev[0].pitch == 65.0f && q.ev[0].a == 65, "quantised notes carry whole semitones");
  }
  return finish();
}
