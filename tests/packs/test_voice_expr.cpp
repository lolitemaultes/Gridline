// Gridline - per-note pressure and timbre (MPE) on each voice of Gridline's own instruments: with
// two held notes, expression on one changes that note's sound only. Every instrument of the
// catalogue is played; those that keep the channel-wide response are listed.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "engine/Generators.h"
#include "engine/Render.h"
#include "packs/Packs.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr int kSr = 44100;
constexpr double kPi = 3.14159265358979323846;

fs::path dir() {
  static const fs::path d = [] {
    fs::path p = fs::temp_directory_path() / "gridline_test_voice_expr";
    fs::create_directories(p);
    return p;
  }();
  return d;
}

// A 220 Hz sine, for the sample players (key 60 plays it at 220 Hz).
std::string sineWav() {
  const std::string path = (dir() / "sine220.wav").string();
  AudioBuffer b(2, static_cast<size_t>(kSr * 4));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.4 * std::sin(2 * kPi * 220.0 * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  writeWav(path, b, kSr, 16);
  return path;
}

// Instruments that keep the channel-wide response (no per-note target): drums and kits, the
// scratch and deck players, controllers, MIDI out, instruments that play their own sequence or
// host another engine, and the mono bass with one shared voice.
const std::set<std::string> kGlobal = {
    "gl.plasma", "gl.stepbass", "gl.flex", "gl.seltmann", "gl.tracker", "gl.midiout", "gl.platter", "gl.turntable",
    "gl.splinescratch", "gl.scratch", "gl.patcher",
    // drums and kits: a key picks a drum or pad
    "gl.kick", "gl.snare", "gl.clap", "gl.hat", "gl.membrane", "gl.membranekit", "gl.kicksculpt", "gl.sweepkick", "gl.drumkit", "gl.pads",
};
// Monophonic: a second key takes over the one voice, so two held notes cannot be told apart.
const std::set<std::string> kMono = {"gl.acid"};

struct Run {
  PluginState st;
  ChannelAux aux;
  std::unique_ptr<Generator> g;
  ProcessContext ctx;
  std::vector<float> L, R;
  Run(const std::string& type, const std::string& sample) {
    st = defaultPluginState(type);
    const PluginInfo* info = findPlugin(type);
    if (info && info->usesSample) st.data = sample;
    packs::prepareAux(st, aux);
    if (aux.samples.empty() && info && info->usesSample) aux.samples = {sampleCache().get(sample)};
    g = createGenerator(type);
    ctx.sampleRate = kSr;
    ctx.tempo = 120;
  }
  void run(size_t n, int block = 256) {
    const size_t base = L.size();
    L.resize(base + n, 0.f);
    R.resize(base + n, 0.f);
    for (size_t p = 0; p < n; p += static_cast<size_t>(block)) {
      const int len = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - p));
      g->process(st, aux, ctx, L.data() + base + p, R.data() + base + p, len);
    }
  }
};

// Two notes a tritone apart held; at a quarter second, `pressure` / `timbre` on note 2 only.
std::vector<float> twoNotes(const std::string& type, const std::string& sample, float pressure, float timbre, bool solo = false) {
  Run r(type, sample);
  if (!r.g) return {};
  r.g->noteOn(1, 60, 0.6f, 0);
  if (solo) r.g->noteOff(1, 0);  // note 1 ends at once: note 2 gets the same voice and seed as with both held
  r.g->noteOn(2, 66, 0.6f, 0);  // a tritone: no shared low harmonics
  r.run(kSr / 4);
  if (pressure != 0) r.g->expr(2, ExprDim::Pressure, pressure, 0);
  if (timbre != 0.5f) r.g->expr(2, ExprDim::Timbre, timbre, 0);
  r.run(kSr / 2);
  std::vector<float> m(r.L.size());
  for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (r.L[i] + r.R[i]);
  return m;
}

// Magnitude of the frequency `f` (and its first harmonics) in v[a, b), Hann window.
double tone(const std::vector<float>& v, size_t a, size_t b, double f) {
  double sum = 0;
  for (int h = 1; h <= 3; ++h) {
    double re = 0, im = 0;
    const double w = 2 * kPi * f * h / kSr;
    for (size_t i = a; i < b && i < v.size(); ++i) {
      const double win = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i - a) / static_cast<double>(b - a));
      re += v[i] * win * std::cos(w * static_cast<double>(i));
      im += v[i] * win * std::sin(w * static_cast<double>(i));
    }
    sum += std::sqrt(re * re + im * im);
  }
  return sum;
}

double rms(const std::vector<float>& v, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b && i < v.size(); ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(std::max<size_t>(1, b - a)));
}

bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}

// The note pitches heard: key 60 / 66 (the sample players: the 220 Hz sample transposed).
void pitches(const std::string& type, double& f1, double& f2) {
  const PluginInfo* info = findPlugin(type);
  const double k = info && info->usesSample ? 220.0 : 440.0 * std::pow(2.0, (60 - 69) / 12.0);
  f1 = k, f2 = k * std::pow(2.0, 6 / 12.0);
}

void testEveryInstrument(const std::string& sine) {
  int perVoice = 0, global = 0, checked = 0;
  for (const PluginInfo& info : pluginCatalog()) {
    if (info.kind != PluginKind::Generator || isHostedType(info.type) || info.type.rfind("gl.", 0) != 0) continue;
    const std::vector<float> plain = twoNotes(info.type, sine, 0, 0.5f);
    if (plain.empty() || kMono.count(info.type)) continue;
    if (twoNotes(info.type, sine, 0, 0.5f) != plain) {  // grains or noise seeded afresh each time: not comparable
      std::printf("%s: output differs run to run, skipped\n", info.type.c_str());
      continue;
    }
    const std::vector<float> pressed = twoNotes(info.type, sine, 1.0f, 0.5f);
    const std::vector<float> bright = twoNotes(info.type, sine, 0, 1.0f);
    char name[160];
    std::snprintf(name, sizeof name, "%s: expression keeps the output finite", info.type.c_str());
    expect(finite(pressed) && finite(bright), name);
    double f1, f2;
    pitches(info.type, f1, f2);
    const size_t a = kSr / 2, b = kSr * 3 / 4;  // a window well after the change (smoothing done)
    const double base1 = tone(plain, a, b, f1), base2 = tone(plain, a, b, f2);
    if (rms(plain, a, b) < 1e-4) continue;  // silent here (one-shots already over)
    std::vector<float> d(plain.size());
    for (size_t i = 0; i < d.size(); ++i) d[i] = pressed[i] - plain[i];
    ++checked;
    // By pitch: the change sits at note 2's partials, not note 1's.
    const double leak = base1 > 1e-3 ? tone(d, a, b, f1) / base1 : 1, own = base2 > 1e-3 ? tone(d, a, b, f2) / base2 : 0;
    // By difference (any spectrum): the change with both notes held equals the change of note 2
    // alone; a channel-wide response also lifts note 1 and leaves a remainder of note 1's size.
    const std::vector<float> soloPlain = twoNotes(info.type, sine, 0, 0.5f, true), soloPressed = twoNotes(info.type, sine, 1.0f, 0.5f, true);
    std::vector<float> rem(d.size());
    for (size_t i = 0; i < rem.size() && i < soloPlain.size(); ++i) rem[i] = d[i] - (soloPressed[i] - soloPlain[i]);
    const double change = rms(d, a, b), rest = change > 1e-7 ? rms(rem, a, b) / change : 1;
    const bool voice = (leak < 0.15 && own > 0.5) || (change > 1e-5 && rest < 0.1);
    if (kGlobal.count(info.type)) {
      ++global;
      continue;
    }
    std::snprintf(name, sizeof name, "%s: pressure on one of two held notes changes only that note (other %.3f, own %.3f, rest %.3f, change %.2g)",
                  info.type.c_str(), leak, own, rest, change);
    expect(voice, name);
    if (voice) ++perVoice;
  }
  std::printf("voice expression: %d instruments checked, %d per voice, %d channel-wide by design\n", checked, perVoice, global);
  expect(perVoice >= 30, "most built-in instruments answer pressure note by note");
}

// The rest of the checks on Trio Synth and the Sampler in detail.
void testDetail(const std::string& sine) {
  {  // Pressure on note 2: note 2 rises 6 dB, note 1 stays exactly as it was.
    double f1, f2;
    pitches("gl.sampler", f1, f2);
    const std::vector<float> plain = twoNotes("gl.sampler", sine, 0, 0.5f), pressed = twoNotes("gl.sampler", sine, 1.0f, 0.5f);
    const size_t a = kSr / 2, b = kSr * 3 / 4;
    expect(std::fabs(tone(pressed, a, b, f1) / tone(plain, a, b, f1) - 1.0) < 0.01, "sampler: the other note keeps its level");
    expect(std::fabs(tone(pressed, a, b, f2) / tone(plain, a, b, f2) - 2.0) < 0.1, "sampler: the pressed note rises 6 dB");
  }
  {  // Timbre on note 2 brightens that note only: the change with both notes held is the change of
     // note 2 alone, and it lifts the highs (the energy of the second difference) of note 2.
    const std::vector<float> plain = twoNotes("gl.synth", sine, 0, 0.5f), up = twoNotes("gl.synth", sine, 0, 1.0f);
    const std::vector<float> sPlain = twoNotes("gl.synth", sine, 0, 0.5f, true), sUp = twoNotes("gl.synth", sine, 0, 1.0f, true);
    const size_t a = kSr / 2, b = kSr * 3 / 4;
    std::vector<float> rem(plain.size()), d(plain.size());
    for (size_t i = 0; i < d.size(); ++i) d[i] = up[i] - plain[i], rem[i] = d[i] - (sUp[i] - sPlain[i]);
    auto highs = [&](const std::vector<float>& v) {
      std::vector<float> h(v.size());
      for (size_t i = 2; i < v.size(); ++i) h[i] = v[i] - 2 * v[i - 1] + v[i - 2];
      return rms(h, a, b);
    };
    const double lift = highs(sUp) / highs(sPlain), rest = rms(rem, a, b) / rms(d, a, b);
    char msg[160];
    std::snprintf(msg, sizeof msg, "trio synth: timbre brightens its own note only (highs x%.2f, rest %.3f)", lift, rest);
    expect(lift > 1.5 && rest < 0.05, msg);
  }
  {  // Notes without expression sound exactly as before (the response stays off).
    Run a("gl.synth", sine), b("gl.synth", sine);
    a.g->noteOn(1, 60, 0.6f, 0);
    b.g->noteOn(1, 60, 0.6f, 0);
    b.g->expr(1, ExprDim::Pressure, 0, 0);  // neutral values
    b.g->expr(1, ExprDim::Timbre, 0.5f, 0);
    a.run(kSr / 2);
    b.run(kSr / 2);
    bool same = a.L == b.L && a.R == b.R;
    expect(same, "neutral expression leaves the sound bit for bit as it was");
  }
  {  // A new note after a pressed one starts from its own (neutral) values.
    std::vector<float> got[2];
    for (int pass = 0; pass < 2; ++pass) {
      Run r("gl.synth", sine);
      r.g->noteOn(1, 60, 0.6f, 0);
      r.run(kSr / 8);
      if (pass) r.g->expr(1, ExprDim::Pressure, 1.0f, 0);
      r.run(kSr / 8);
      r.g->noteOff(1, 0);
      r.g->noteOn(3, 66, 0.6f, 0);
      r.run(kSr / 2);
      got[pass] = r.L;
    }
    double f1, f2;
    pitches("gl.synth", f1, f2);
    const size_t a = kSr / 2, b = kSr * 3 / 4;
    expect(std::fabs(tone(got[1], a, b, f2) / tone(got[0], a, b, f2) - 1.0) < 0.01, "a new note is not lifted by an earlier note's pressure");
  }
  {  // The channel's Off and amount settings reach each voice.
    ExprSettings s;
    s.pressure = ExprUse::Off;
    Run r("gl.sampler", sine), p("gl.sampler", sine);
    r.g->setExpression(s);
    for (Run* x : {&r, &p}) {
      x->g->noteOn(1, 60, 0.6f, 0);
      x->g->noteOn(2, 61, 0.6f, 0);
      x->run(kSr / 8);
    }
    r.g->expr(2, ExprDim::Pressure, 1.0f, 0);
    r.run(kSr / 4);
    p.run(kSr / 4);
    expect(r.L == p.L, "pressure Off: no voice changes");
  }
  {  // Block size does not change a per-voice response.
    std::vector<float> a, b;
    for (int pass = 0; pass < 2; ++pass) {
      Run r("gl.synth", sine);
      const int blk = pass ? 37 : 512;
      r.g->noteOn(1, 60, 0.6f, 0);
      r.g->noteOn(2, 64, 0.6f, 0);
      r.run(4000, blk);
      r.g->expr(2, ExprDim::Timbre, 0.9f, 0);
      r.g->expr(1, ExprDim::Pressure, 0.7f, 0);
      r.run(8000, blk);
      (pass ? b : a) = r.L;
    }
    double diff = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(a[i] - b[i])));
    expect(diff < 1e-4, "per-voice expression is the same for any block size");
  }
}

// A song: two notes, a pressure lane on one; rendered, only that note's level changes.
void testSongLanes(const std::string& sine) {
  auto song = [&](bool lane) {
    Song s = makeEmptySong();
    s.tempo = 120;
    s.patterns[0].bars = 2;
    Clip c;
    c.id = s.allocateId();
    c.pattern = s.patterns[0].id;
    s.playlist.push_back(c);
    Channel ch;
    ch.id = s.allocateId();
    ch.name = "sampler";
    ch.plugin = defaultPluginState("gl.sampler");
    ch.plugin.data = sine;
    ch.mixerTrack = s.masterId();
    s.channels.push_back(ch);
    Note a(0, 384, 60, 90), b(0, 384, 66, 90);
    if (lane) b.pressure = {{0, 1.0f}};
    s.patterns[0].notes[ch.id] = {a, b};
    return s;
  };
  const AudioBuffer p = renderSong(song(false), RenderOptions{}), q = renderSong(song(true), RenderOptions{});
  std::vector<float> lp(p.frames), lq(q.frames);
  for (size_t i = 0; i < p.frames; ++i) lp[i] = p.at(0, i);
  for (size_t i = 0; i < q.frames; ++i) lq[i] = q.at(0, i);
  double f1, f2;
  pitches("gl.sampler", f1, f2);
  const size_t a = kSr / 4, b = kSr * 3 / 4;
  expect(std::fabs(tone(lq, a, b, f1) / tone(lp, a, b, f1) - 1.0) < 0.01 && std::fabs(tone(lq, a, b, f2) / tone(lp, a, b, f2) - 2.0) < 0.12,
         "a note's pressure lane lifts that note only in a rendered song");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv();
  const std::string sine = sineWav();
  testDetail(sine);
  testSongLanes(sine);
  testEveryInstrument(sine);
  return finish();
}
