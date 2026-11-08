// Gridline: K-132, imported channels that follow the tempo and the channel pitch automation.
// A sampler channel stretched to a time keeps its length in beats when the tempo moves (a stretch
// follows the tempo live, a resampled note takes its rate from the tempo where it starts).
// Automation on an imported channel's pitch knob replaces the knob with a smooth value, on sampler
// and audio clip channels alike; Gridline's own channels keep their whole-semitone steps.
// Every file here is synthetic.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/core/AudioBuffer.h"
#include "../../src/core/Params.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Project.h"
#include "../../src/core/WavWriter.h"
#include "../../src/core/flp/Flp.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Render.h"
#include "../../src/engine/Sample.h"
#include "../../src/engine/TimeStretch.h"
#include "../Check.h"
#include "../Hermetic.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100.0;
constexpr int kPpq = 96;

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

struct Builder {
  std::vector<uint8_t> ev;
  void b(uint8_t id, uint8_t v) { ev.push_back(id), ev.push_back(v); }
  void w(uint8_t id, uint16_t v) { ev.push_back(id), ev.push_back(v & 0xFF), ev.push_back(v >> 8); }
  void d(uint8_t id, uint32_t v) {
    ev.push_back(id);
    for (int i = 0; i < 4; ++i) ev.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void data(uint8_t id, const std::vector<uint8_t>& p) {
    ev.push_back(id);
    size_t n = p.size();
    do {
      uint8_t x = n & 0x7F;
      n >>= 7;
      ev.push_back(static_cast<uint8_t>(x | (n ? 0x80 : 0)));
    } while (n);
    ev.insert(ev.end(), p.begin(), p.end());
  }
  void text(uint8_t id, const std::string& s) {
    std::vector<uint8_t> p;
    for (char ch : s) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    data(id, p);
  }
  std::vector<uint8_t> file() const {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, static_cast<uint8_t>(kPpq), 0, 'F', 'L', 'd', 't'};
    uint32_t n = static_cast<uint32_t>(ev.size());
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(n >> (8 * i)));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
  }
};

struct Bytes {
  std::vector<uint8_t> v;
  explicit Bytes(size_t n) : v(n, 0) {}
  void u16(size_t at, uint16_t x) { v[at] = x & 0xFF, v[at + 1] = x >> 8; }
  void u32(size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i));
  }
  void f32(size_t at, float x) {
    uint32_t b;
    std::memcpy(&b, &x, 4);
    u32(at, b);
  }
  void f64(size_t at, double x) {
    uint64_t b;
    std::memcpy(&b, &x, 8);
    u32(at, static_cast<uint32_t>(b)), u32(at + 4, static_cast<uint32_t>(b >> 32));
  }
};

void levels(Builder& B, int knobCents) {
  Bytes lv(24);
  lv.u32(0, 6400), lv.u32(4, 10000), lv.u32(8, static_cast<uint32_t>(knobCents));
  B.data(219, lv.v);
}

// Channel time settings (215): the time in 1/192 beats, FL's method (0 resample, -1 stretch).
void timeSettings(Builder& B, float timeUnits, int mode) {
  Bytes pr(168);
  pr.f32(96, timeUnits);
  pr.u32(108, static_cast<uint32_t>(mode));
  pr.f64(136, 1.0);
  B.data(215, pr.v);
}

void samplerChannel(Builder& B, int index, const std::string& sample, int knobCents, int fineCents = 0) {
  B.w(64, static_cast<uint16_t>(index)), B.b(21, 0), B.text(192, "Smp");
  B.text(196, sample);
  levels(B, knobCents);
  if (fineCents) B.d(142, static_cast<uint32_t>(fineCents));
}

void audioChannel(Builder& B, int index, const std::string& sample, int knobCents) {
  B.w(64, static_cast<uint16_t>(index)), B.b(21, 4), B.text(203, "Bed");
  B.text(196, sample);
  levels(B, knobCents);
  timeSettings(B, 0, 0);
}

void automationChannel(Builder& B, int index, const char* name) {
  B.w(64, static_cast<uint16_t>(index)), B.b(21, 5), B.text(203, name);
  Bytes lv(24);
  lv.u32(0, 4800), lv.u32(4, 8000);
  B.data(219, lv.v);
  Bytes a(21 + 24 * 2 + 112);
  a.u32(0, 1), a.u32(4, 64), a.u32(9, 4), a.u32(13, 3), a.u32(17, 2);
  a.f64(21, 0), a.f64(29, 0.5), a.f64(45, 8), a.f64(53, 0.5);
  B.data(234, a.v);
}

void link(Builder& B, int source, uint32_t target) {
  Bytes l(20);
  l.u16(2, static_cast<uint16_t>(source)), l.u32(8, target), l.u32(12, 8);
  B.data(227, l.v);
}

std::vector<uint8_t> audioItem(uint32_t pos, uint16_t channel, uint32_t len, int track) {
  Bytes r(88);
  r.u32(0, pos);
  r.u16(4, 0x5000);
  r.u16(6, channel);
  r.u32(8, len);
  r.u16(12, static_cast<uint16_t>(499 - track));
  r.u16(18, 0x40);
  r.f32(24, 0), r.f32(28, 1000);
  r.f32(52, 1.0f);
  r.f64(64, 1.0);
  return r.v;
}

void finishFile(Builder& B, const std::vector<std::vector<uint8_t>>& items) {
  B.w(99, 0);
  std::vector<uint8_t> all;
  for (const auto& i : items) all.insert(all.end(), i.begin(), i.end());
  B.data(233, all);
  for (int ins = 0; ins < 2; ++ins) {
    B.data(236, std::vector<uint8_t>(12, 0));
    for (int k = 0; k < 10; ++k) B.w(98, static_cast<uint16_t>(k));
    B.d(147, 0);
  }
}

bool load(const Builder& B, Song& s) {
  flp::Report rep;
  std::string err;
  return importFlpBytes(B.file(), s, rep, &err, "", "");
}

void writeTone(const std::string& path, double seconds, double hz) {
  const size_t n = static_cast<size_t>(seconds * kSr);
  AudioBuffer b(1, n);
  for (size_t i = 0; i < n; ++i) b.at(0, i) = static_cast<float>(0.5 * std::sin(6.283185307179586 * hz * static_cast<double>(i) / kSr));
  writeWav(path, b, static_cast<int>(kSr), 16);
}

// A held note on a sampler at `tempo`: the output.
std::vector<float> playSampler(const PluginState& st, double tempo, double seconds) {
  ChannelAux aux;
  prepareChannelAux(st, aux);
  auto gen = createGenerator("gl.sampler");
  gen->prepareHost(st, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = tempo;
  const size_t total = static_cast<size_t>(seconds * kSr);
  std::vector<float> L(total, 0.f), R(total, 0.f);
  gen->noteOn(1, 60, 1.0f, 0);
  for (size_t i = 0; i < total; i += 256) {
    const int n = static_cast<int>(std::min<size_t>(256, total - i));
    gen->process(st, aux, ctx, L.data() + i, R.data() + i, n);
  }
  return L;
}

double soundEnd(const std::vector<float>& x) {
  for (size_t i = x.size(); i-- > 0;)
    if (std::fabs(x[i]) > 0.02f) return static_cast<double>(i + 1) / kSr;
  return 0;
}

// Frequency from the rising zero crossings in [a, b) seconds.
double frequency(const std::vector<float>& x, double a, double b) {
  const size_t i0 = static_cast<size_t>(a * kSr), i1 = std::min(x.size(), static_cast<size_t>(b * kSr));
  double first = -1, last = -1;
  int n = 0;
  for (size_t i = i0 + 1; i < i1; ++i)
    if (x[i - 1] < 0 && x[i] >= 0) {
      const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (first < 0) first = t;
      last = t, ++n;
    }
  return n > 1 ? (n - 1) * kSr / (last - first) : 0;
}

// A Gridline song: a sampler channel holding a note for 3 s, its pitch automated to `semis`.
Song pitchSong(const std::string& path, double semis, bool smooth, int pitch, int fine, int knobFine) {
  Song s = makeEmptySong();
  s.tempo = 120;
  Channel ch;
  ch.id = s.allocateId();
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.mixerTrack = s.masterId();
  ch.pitch = pitch, ch.fine = fine;
  ch.pitchSmooth = smooth, ch.knobFine = knobFine;
  s.channels.push_back(ch);
  Pattern p;
  p.id = s.allocateId();
  p.bars = 2;
  Note n;
  n.start = s.beatTicks(), n.length = 6 * s.beatTicks(), n.pitch = 60;  // after the automation starts
  p.notes[ch.id].push_back(n);
  s.patterns.push_back(p);
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Pattern, k.pattern = p.id, k.track = 0, k.start = 0, k.length = 2 * s.barTicks();
  s.playlist.push_back(k);
  const ParamId id = channelParam(ch.id, ChannelParam::Pitch);
  const auto d = describeParam(s, id);
  AutomationClip a;
  a.id = s.allocateId(), a.owner = id.owner, a.index = id.index;
  const float v = d ? static_cast<float>(d->toNormalized(semis)) : 0.5f;
  a.points.push_back(AutoPoint{0, v});
  a.points.push_back(AutoPoint{2 * s.barTicks(), v});
  s.automations.push_back(a);
  Clip c;
  c.id = s.allocateId(), c.kind = ClipKind::Automation, c.source = a.id, c.track = 1, c.start = 0, c.length = 2 * s.barTicks();
  s.playlist.push_back(c);
  return s;
}

double renderedFrequency(const Song& s) {
  RenderOptions o;
  o.sampleRate = static_cast<int>(kSr);
  AudioBuffer b = renderSong(s, o);
  std::vector<float> x(b.frames);
  for (size_t i = 0; i < b.frames; ++i) x[i] = b.at(0, i);
  return frequency(x, 1.0, 3.0);
}

}  // namespace

int main() {
  const std::string dir = gl::test::hermeticTestEnv("k132_import");
  fs::create_directories(fs::path(dir) / "Audio");
  const std::string oneSec = (fs::path(dir) / "Audio" / "one-second.wav").string();
  const std::string fourSec = (fs::path(dir) / "Audio" / "four-seconds.wav").string();
  writeTone(oneSec, 1.0, 441.0);
  writeTone(fourSec, 4.0, 441.0);
  sampleCache().clear();

  // ------------------------------------------------ a sampler fitted to a time follows the tempo
  {
    PluginState st = defaultPluginState("gl.sampler");
    st.data = oneSec;
    st.params[pp::SmpStretchMode] = static_cast<float>(stretch::Mode::Auto);
    st.params[pp::SmpTimeStretch] = 2.0f;  // fitted at 100 BPM: 1 s spans 2 s
    st.params[pp::SmpFitTempo] = 100.0f;
    const double at100 = soundEnd(playSampler(st, 100, 3.5));
    const double at200 = soundEnd(playSampler(st, 200, 3.5));
    expect(near(at100, 2.0, 0.1), "stretch fitted at 100 BPM plays its fitted length at 100 BPM");
    expect(near(at200, 1.0, 0.1), "stretch fitted at 100 BPM plays half as long at 200 BPM");
    st.params[pp::SmpFitTempo] = 0.0f;
    expect(near(soundEnd(playSampler(st, 200, 3.5)), 2.0, 0.1), "without a fit tempo the stretch stays put (Gridline's own channels)");
  }
  {
    PluginState st = defaultPluginState("gl.sampler");
    st.data = oneSec;
    st.params[pp::SmpStretchMode] = static_cast<float>(stretch::Mode::Resample);
    st.params[pp::SmpFitTempo] = 100.0f;
    const double f100 = frequency(playSampler(st, 100, 0.6), 0.05, 0.5);
    const double f200 = frequency(playSampler(st, 200, 0.6), 0.05, 0.5);
    expect(near(f100, 441.0, 2.0), "resampled, fitted at 100 BPM: plays as stored at 100 BPM");
    expect(near(f200, 882.0, 4.0), "resampled, fitted at 100 BPM: twice the rate at 200 BPM");
  }
  // ------------------------------------------------ the FL import marks the fit with the project tempo
  {
    Builder B;
    B.d(156, 150000);
    samplerChannel(B, 0, fourSec, 0);
    timeSettings(B, 1536.0f, -1);  // 8 beats
    samplerChannel(B, 1, fourSec, 0);
    timeSettings(B, 1536.0f, 0);
    samplerChannel(B, 2, fourSec, 0);
    finishFile(B, {});
    Song s;
    expect(load(B, s), "imports sampler channels stretched to a time");
    auto fit = [&](size_t i) {
      return i < s.channels.size() && s.channels[i].plugin.params.size() > pp::SmpFitTempo ? s.channels[i].plugin.params[pp::SmpFitTempo] : -1.f;
    };
    expect(s.channels.size() >= 3 && near(fit(0), 150.0, 1e-3), "stretched to a time: follows the tempo from the project tempo");
    expect(near(fit(1), 150.0, 1e-3), "resampled to a time: follows the tempo from the project tempo");
    expect(near(fit(2), 0.0, 1e-6), "no time: does not follow the tempo");
  }
  // ------------------------------------------------ the pitch knob under automation
  {
    // Knob +150 cents, fine tune +20: the automation replaces the knob, the fine tune stays.
    Builder B;
    B.d(156, 120000);
    samplerChannel(B, 0, oneSec, 150, 20);
    samplerChannel(B, 1, oneSec, 150, 20);
    automationChannel(B, 2, "Pitch");
    link(B, 2, 0u << 16 | 0x04u);
    finishFile(B, {});
    Song s;
    expect(load(B, s), "imports a channel with pitch automation");
    const Channel* a = s.channels.size() > 1 ? &s.channels[0] : nullptr;
    const Channel* b = s.channels.size() > 1 ? &s.channels[1] : nullptr;
    expect(a && a->pitchSmooth && a->pitch == 2 && a->fine == -30 && a->knobFine == -50, "automated pitch knob: smooth, the knob's cents kept apart");
    expect(b && !b->pitchSmooth && b->knobFine == 0 && b->pitch == 2 && b->fine == -30, "no automation: as before");
    Song back;
    std::string err;
    expect(a && songFromJson(songToJson(s), back, &err) && back.channels[0].pitchSmooth && back.channels[0].knobFine == -50, "the smooth pitch survives a save");
  }
  {
    // An audio clip channel: the knob stays on the channel (the automation replaces it).
    Builder B;
    B.d(156, 120000);
    audioChannel(B, 0, oneSec, 150);
    audioChannel(B, 1, oneSec, 150);
    automationChannel(B, 2, "Pitch");
    link(B, 2, 0u << 16 | 0x04u);
    finishFile(B, {audioItem(0, 0, 2 * kPpq, 0), audioItem(0, 1, 2 * kPpq, 1)});
    Song s;
    expect(load(B, s), "imports audio clip channels with a pitch knob");
    const Clip* ca = nullptr;
    const Clip* cb = nullptr;
    for (const auto& c : s.playlist)
      if (c.kind == ClipKind::Audio) (c.track == 0 ? ca : cb) = &c;
    const Channel* ka = ca ? s.channel(ca->source) : nullptr;
    const Channel* kb = cb ? s.channel(cb->source) : nullptr;
    expect(ka && ka->pitchSmooth && ka->pitch == 2 && ka->fine == -50 && ka->knobFine == -50, "automated: the knob is the channel's pitch");
    expect(ca && near(ca->pitch, 0.0, 1e-3) && near(ca->speedPitch, 150.0, 1e-3), "automated: the clip leaves the knob to the channel, its speed keeps it");
    expect(kb && !kb->pitchSmooth && kb->pitch == 0 && kb->fine == 0, "not automated: the channel stays at zero");
    expect(cb && near(cb->pitch, 150.0, 1e-3) && near(cb->speedPitch, 150.0, 1e-3), "not automated: the knob moves the clip as before");
  }
  {
    // What plays: automation at +0.25 semitones.
    const double base = 441.0;
    const double rounded = renderedFrequency(pitchSong(fourSec, 0.25, false, 0, 0, 0));
    const double smooth = renderedFrequency(pitchSong(fourSec, 0.25, true, 0, 0, 0));
    const double knob = renderedFrequency(pitchSong(fourSec, 0.25, true, 2, -30, -50));
    std::printf("# frequencies: rounded %.2f smooth %.2f knob %.2f\n", rounded, smooth, knob);
    expect(near(rounded, base, 1.5), "Gridline's own channel: pitch automation keeps whole semitones");
    expect(near(smooth, base * std::pow(2.0, 0.25 / 12.0), 1.5), "imported channel: pitch automation plays smooth values");
    expect(near(knob, base * std::pow(2.0, 0.45 / 12.0), 1.5), "imported channel: the automation replaces the knob, the fine tune stays on top");
  }
  return finish();
}
