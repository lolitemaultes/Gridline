// Gridline tests - the channel pitch knob (the strip of every instrument window), in cents.
// Renders a held note at several channel pitches on the Sampler, built-in synths, the Tracker
// Sampler and hosted plugins (the in-tree CLAP, VST3 and VST2 test synths: note tuning, the
// plugin's pitch-bend parameter, plain MIDI pitch bend) and measures the played frequency against
// the expected one (within 1 cent). Then: a held note follows the knob and its automation, the
// knob's pitch adds to a note's bend line, old projects (whole semitones) load as cents, undo.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Render.h"
#include "host/HostedPlugins.h"
#include "host/PluginPrefs.h"
#include "host/PluginScan.h"
#include "host/vst3/Vst3Scan.h"
#include "packs/tracker/tracker.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;
constexpr double kPi = 3.14159265358979323846;

const std::string kClapSynth = "clap:org.gridline.test.synth";
const std::string kVst2Synth = "vst2:476C5432";
const std::string kVst3Synth = "vst3:47524944" "4C494E45" "54535931" "00000001";

std::string sineWav(double hz) {
  AudioBuffer b(2, static_cast<size_t>(kSr * 6));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * kPi * hz * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  const std::string path = (std::filesystem::temp_directory_path() / "test_channel_pitch_a440.wav").string();
  writeWav(path, b, kSr, 24);
  return path;
}

// Hann-windowed spectrum magnitude of the left channel at `hz` over [t0, t1) seconds.
double mag(const AudioBuffer& b, double hz, double t0, double t1) {
  const size_t a = static_cast<size_t>(t0 * kSr), e = std::min(b.frames, static_cast<size_t>(t1 * kSr));
  if (e <= a + 2) return 0;
  const double n = static_cast<double>(e - a), w = 2 * kPi * hz / kSr;
  double re = 0, im = 0;
  for (size_t i = a; i < e; ++i) {
    const double k = static_cast<double>(i - a), win = 0.5 - 0.5 * std::cos(2 * kPi * k / n);
    const double x = (b.at(0, i) + b.at(1, i)) * win;
    re += x * std::cos(w * k), im -= x * std::sin(w * k);
  }
  return std::sqrt(re * re + im * im) / n;
}

// The strongest frequency within +-`span` cents of `guess` (a search on the windowed spectrum,
// refined to well under a cent): the fundamental when `guess` is near it.
double peakHz(const AudioBuffer& b, double guess, double t0, double t1, double span = 60) {
  double best = guess, bm = -1;
  for (double c = -span; c <= span; c += 2.0) {
    const double f = guess * std::pow(2.0, c / 1200.0), m = mag(b, f, t0, t1);
    if (m > bm) bm = m, best = f;
  }
  double lo = best * std::pow(2.0, -2.0 / 1200.0), hi = best * std::pow(2.0, 2.0 / 1200.0);
  for (int k = 0; k < 40; ++k) {  // golden section on the peak
    const double g = 0.381966, x1 = lo + g * (hi - lo), x2 = hi - g * (hi - lo);
    if (mag(b, x1, t0, t1) > mag(b, x2, t0, t1)) hi = x2;
    else lo = x1;
  }
  return 0.5 * (lo + hi);
}

double centsOff(double hz, double want) { return 1200.0 * std::log2(hz / want); }

// The knob's own unit: cents (0.7.7 on) or whole semitones (before).
double knobValue(const Song& s, ObjectId ch, double cents) {
  auto d = describeParam(s, channelParam(ch, ChannelParam::Pitch));
  return d && d->unit == "st" ? cents / 100.0 : cents;
}

struct Inst {
  std::string label, type, data;
  int key = 69;         // the note played
  double hz = 440;      // its frequency at pitch 0
  bool fromTracker = false;
};

// 120 BPM, one 2-bar pattern (4 s) placed once, one channel holding `key` for 3.5 s.
Song toneSong(const Inst& in, ObjectId& ch) {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.patterns[0].bars = 2;
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  s.playlist.push_back(c);
  Channel x;
  x.id = s.allocateId();
  x.name = in.label;
  if (in.fromTracker) {
    PluginState smp = defaultPluginState("gl.sampler");
    smp.data = in.data;
    x.plugin = packs::trackerpack::fromSamplerState(smp, kSr * 6, kSr, "a440");
  } else {
    x.plugin = defaultPluginState(in.type);
    x.plugin.data = in.data.empty() ? x.plugin.data : in.data;
  }
  x.mixerTrack = s.masterId();
  s.channels.push_back(x);
  ch = x.id;
  s.patterns[0].notes[ch] = {Note(0, 672, in.key, 100)};
  return s;
}

AudioBuffer render(const Song& s) {
  RenderOptions o;
  o.sampleRate = kSr;
  return renderSong(s, o);
}

struct Row {
  std::string label;
  double cents, want, got;
};
std::vector<Row> gRows;

// Renders `in` at each channel pitch and checks the played frequency (1 cent).
void measure(Inst in, bool required = true) {
  if (in.hz <= 0) {  // its own tuning: the frequency it plays at pitch 0 (strongest peak near A4)
    ObjectId ch = kNoId;
    const AudioBuffer b = render(toneSong(in, ch));
    in.hz = peakHz(b, 440.0, 1.0, 2.5, 100);
  }
  for (double cents : {0.0, 50.0, 100.0, -100.0, 1200.0}) {
    ObjectId ch = kNoId;
    Document d(toneSong(in, ch));
    d.setParam(channelParam(ch, ChannelParam::Pitch), knobValue(d.song(), ch, cents), false);
    const AudioBuffer b = render(d.song());
    const double want = in.hz * std::pow(2.0, cents / 1200.0);
    const double got = peakHz(b, want, 1.0, 2.5);
    gRows.push_back({in.label, cents, want, got});
    std::printf("# %-28s %+6.0f ct: want %8.2f Hz, got %8.2f Hz (%+.2f ct, level %.4f)\n", in.label.c_str(), cents, want, got,
                centsOff(got, want), mag(b, got, 1.0, 2.5));
    if (required)
      expect(std::fabs(centsOff(got, want)) <= 1.0 && mag(b, got, 1.0, 2.5) > 1e-3,
             (in.label + ": the channel pitch " + std::to_string(static_cast<int>(cents)) + " ct plays at the expected frequency").c_str());
  }
}

// A held note (pattern playback in the engine) follows the knob: the pitch changes, then a new
// Program arrives (as the interface does after an edit).
double liveFollow(const Inst& in, double cents) {
  ObjectId ch = kNoId;
  Document d(toneSong(in, ch));
  ProgramBuilder pb;
  AudioEngine eng(kSr);
  eng.setProgram(pb.build(d.song()));
  eng.setMode(PlayMode::Pattern);
  eng.setPattern(d.song().patterns[0].id);
  eng.play();
  AudioBuffer b(2, static_cast<size_t>(kSr * 3));
  std::vector<float> L(512), R(512);
  size_t at = 0;
  auto run = [&](double secs) {
    for (int k = 0; k < static_cast<int>(secs * kSr / 512); ++k) {
      eng.process(L.data(), R.data(), 512);
      for (int i = 0; i < 512 && at < b.frames; ++i, ++at) b.at(0, at) = L[static_cast<size_t>(i)], b.at(1, at) = R[static_cast<size_t>(i)];
    }
  };
  run(1.0);
  d.setParam(channelParam(ch, ChannelParam::Pitch), cents, false);
  eng.setProgram(pb.build(d.song()));
  run(1.5);
  eng.stop();
  const double want = in.hz * std::pow(2.0, cents / 1200.0);
  const double got = peakHz(b, want, 1.5, 2.4);
  std::printf("# %-28s held note, knob to %+.0f ct: want %.2f Hz, got %.2f Hz (%+.2f ct)\n", in.label.c_str(), cents, want, got, centsOff(got, want));
  return centsOff(got, want);
}

// Automation of the knob on a held note: a hold step to `to` at 1 s, or a ramp 0 -> `to` over 1..2 s.
AudioBuffer automated(const Inst& in, double to, bool ramp, Song* keep = nullptr) {
  ObjectId ch = kNoId;
  Song s = toneSong(in, ch);
  AutomationClip a;
  a.id = s.allocateId();
  a.owner = ch;
  a.index = static_cast<uint32_t>(ChannelParam::Pitch);
  const float v0 = static_cast<float>(0.5), v1 = static_cast<float>((to + kChannelPitchMaxCents) / (2 * kChannelPitchMaxCents));
  if (ramp) a.points = {{192, v0, 0, AutoShape::Curve}, {384, v1, 0, AutoShape::Curve}};
  else a.points = {{0, v0, 0, AutoShape::Hold}, {192, v1, 0, AutoShape::Hold}};
  s.automations.push_back(a);
  Clip c;
  c.id = s.allocateId(), c.kind = ClipKind::Automation, c.source = a.id, c.start = 0, c.length = 768;
  s.playlist.push_back(c);
  if (keep) *keep = s;
  return render(s);
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("channel_pitch");
  const std::string a440 = sineWav(440.0);
  measure({"Sampler", "gl.sampler", a440, 60, 440.0});
  measure({"Tracker Sampler", "gl.tracker", a440, 60, 0.0, true});
  measure({"Plasma", "gl.plasma", "", 69, 440.0});
  measure({"Trio (gl.synth)", "gl.synth", "", 69, 440.0});

  // Hosted plugins (the in-tree test synths).
  host::ScanResult clap = host::findFormat("clap")->scanInProcess(GRIDLINE_CLAP_TESTPLUG);
  host::registerScanResult(clap);
  host::ScanResult vst2 = host::findFormat("vst2")->scanInProcess(GRIDLINE_VST2_TESTPLUG);
  host::registerScanResult(vst2);
  host::ScanResult vst3 = vst3::scanBundleInProcess(GRIDLINE_TESTPLUG_BUNDLE);
  host::registerScanResult(vst3);
  expect(clap.ok && vst2.ok && vst3.ok, "the test plugins scan");
  measure({"CLAP (note tuning)", kClapSynth, "", 69, 440.0});
  measure({"VST3 (note expression)", kVst3Synth, "", 69, 440.0});
  measure({"VST2 (MIDI pitch bend)", kVst2Synth, "", 69, 440.0});
  {
    host::HostOptions o;
    o.notePitch = host::HostOptions::PitchChannelBend;
    host::setHostOptions(kClapSynth, o);
    measure({"CLAP (channel pitch bend)", kClapSynth, "", 69, 440.0});
    host::setHostOptions(kClapSynth, host::HostOptions{});
  }
  // ------------------------------------------------ held notes follow the knob and its automation
  {
    const Inst smp{"Sampler", "gl.sampler", a440, 60, 440.0};
    const Inst pl{"Plasma", "gl.plasma", "", 69, 440.0};
    const Inst vst2i{"VST2 (MIDI pitch bend)", kVst2Synth, "", 69, 440.0};
    const Inst clapi{"CLAP (note tuning)", kClapSynth, "", 69, 440.0};
    for (const Inst* in : {&smp, &pl, &clapi, &vst2i}) {
      expect(std::fabs(liveFollow(*in, 100.0)) <= 1.0, (in->label + ": a held note follows the knob").c_str());
      const AudioBuffer step = automated(*in, 100.0, false);
      const double want = in->hz * std::pow(2.0, 100.0 / 1200.0), got = peakHz(step, want, 1.3, 2.5);
      std::printf("# %-28s automation step to +100 ct on a held note: %.2f Hz (%+.2f ct)\n", in->label.c_str(), got, centsOff(got, want));
      expect(std::fabs(centsOff(got, want)) <= 1.0, (in->label + ": automation moves a held note").c_str());
      const AudioBuffer ramp = automated(*in, 200.0, true);
      // Half way through a 0 -> +200 ct ramp (1..2 s): about +100 ct; the pitch rises steadily.
      const double mid = peakHz(ramp, in->hz * std::pow(2.0, 100.0 / 1200.0), 1.45, 1.55, 40);
      double prev = 0;
      bool rising = true;
      for (int k = 0; k < 8; ++k) {
        const double t = 1.05 + 0.1 * k, f = peakHz(ramp, in->hz * std::pow(2.0, (t - 1.0) * 200.0 / 1200.0), t, t + 0.1, 40);
        rising = rising && f > prev;
        prev = f;
      }
      std::printf("# %-28s ramp 0..+200 ct: %.2f Hz half way (%+.1f ct)\n", in->label.c_str(), mid, centsOff(mid, in->hz));
      expect(rising && std::fabs(centsOff(mid, in->hz) - 100.0) <= 6.0, (in->label + ": a ramp glides a held note smoothly").c_str());
    }
  }
  // ------------------------------------------------ the knob adds to a note's bend line
  {
    ObjectId ch = kNoId;
    const Inst vst2i{"VST2 (MIDI pitch bend)", kVst2Synth, "", 69, 440.0};
    Song s = toneSong(vst2i, ch);
    s.patterns[0].notes[ch][0].bend = {{0, 1.0f, 0}};  // a semitone up from its start
    s.channels.back().pitchCents = 50;
    const double want = 440.0 * std::pow(2.0, 150.0 / 1200.0), got = peakHz(render(s), want, 1.0, 2.5);
    std::printf("# VST2: bend line +1 st with the knob at +50 ct: %.2f Hz (%+.2f ct)\n", got, centsOff(got, want));
    expect(std::fabs(centsOff(got, want)) <= 1.0, "hosted plugin by pitch bend: the knob and a bend line add up");
    const Inst smp{"Sampler", "gl.sampler", a440, 60, 440.0};
    Song t = toneSong(smp, ch);
    t.patterns[0].notes[ch][0].bend = {{0, 1.0f, 0}};
    t.channels.back().pitchCents = 50;
    const double g2 = peakHz(render(t), want, 1.0, 2.5);
    expect(std::fabs(centsOff(g2, want)) <= 1.0, "built-in: the knob and a bend line add up");
  }
  // ------------------------------------------------ projects: semitones before 0.7.7, cents now
  {
    ObjectId ch = kNoId;
    Song s = toneSong({"Sampler", "gl.sampler", a440, 60, 440.0}, ch);
    Json j = songToJson(s);
    for (auto& jc : j.object["channels"].array) {
      jc.object.erase("pitchCents");
      jc.object["pitch"] = Json::makeNumber(3);  // an old project: three semitones
    }
    Song old;
    expect(songFromJson(j, old) && old.channels.back().pitchCents == 300.0f, "an old project's whole semitones load as cents (3 -> 300)");
    old.channels.back().pitchCents = 250;
    old.channels.back().bendRange = 12;
    const Json saved = songToJson(old);
    const Json* jc = &saved.object.at("channels").array.back();
    expect(jc->num("pitchCents", 0) == 250 && jc->num("pitch", 0) == 3 && jc->num("bendRange", 0) == 12,
           "saved: cents, the nearest semitone for older builds, the bend range");
    Song back;
    expect(songFromJson(saved, back) && back.channels.back().pitchCents == 250.0f && back.channels.back().bendRange == 12 &&
               back == old,
           "cents and bend range survive save and load");
    // K-132 (FL import before 0.7.7): an automated knob kept its cents beyond the semitones in knobFine.
    Json k = songToJson(s);
    Json& kc = k.object["channels"].array.back();
    kc.object.erase("pitchCents");
    kc.object["pitch"] = Json::makeNumber(1);
    kc.object["fine"] = Json::makeNumber(35);
    kc.object["pitchSmooth"] = [] { Json b; b.type = Json::Type::Bool; b.boolean = true; return b; }();
    kc.object["knobFine"] = Json::makeNumber(30);
    Song ks;
    expect(songFromJson(k, ks) && ks.channels.back().pitchCents == 130.0f && ks.channels.back().fine == 5,
           "a K-132 project: the knob's cents move from the fine tune to the pitch (same sound)");
  }
  // ------------------------------------------------ undo, the knob's unit
  {
    ObjectId ch = kNoId;
    Document d(toneSong({"Sampler", "gl.sampler", a440, 60, 440.0}, ch));
    d.setParam(channelParam(ch, ChannelParam::Pitch), 37.0, false);
    d.setParam(channelParam(ch, ChannelParam::Pitch), 137.5, false);
    expect(d.song().channel(ch)->pitchCents == 137.5f, "the knob holds fractions of a cent");
    expect(d.undo() && d.song().channel(ch)->pitchCents == 37.0f && d.undo() && d.song().channel(ch)->pitchCents == 0.0f,
           "undo steps the pitch back");
    expect(d.redo() && d.song().channel(ch)->pitchCents == 37.0f, "redo");
    auto desc = describeParam(d.song(), channelParam(ch, ChannelParam::Pitch));
    expect(desc && desc->unit == "ct" && desc->min == -2400 && desc->max == 2400 && desc->format(100) == "+100 ct",
           "the knob reads in cents, -2400..+2400");
    expect(desc && std::fabs(desc->toNormalized(100) - (2500.0 / 4800.0)) < 1e-9, "automation keeps its meaning (+1 st before = +100 ct now)");
  }
  return finish();
}
