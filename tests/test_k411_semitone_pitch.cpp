// Gridline — K-411: Semitone pitch available on every plugin.
//  1. Per-channel Transpose (-48..+48 st) + Fine (cents): JSON round-trip, undo/redo, automation.
//  2. Acoustic frequency audit table across native instruments AND hosted CLAP / VST3 / VST2 synths.
//  3. Hosted plugin mid-note transpose change: active note -> transposed MIDI key tracking guarantees
//     ZERO stuck notes when transpose changes while notes are held.
//  4. Exhaustive audit of every native instrument's coarse pitch ("st") parameter: snaps cleanly to
//     whole semitones by default, fine control when Shift/Alt modifier is held.
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
#include "core/Params.h"
#include "core/PluginInfo.h"
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

std::string makeSineWav(double hz) {
  AudioBuffer b(2, static_cast<size_t>(kSr * 4));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * kPi * hz * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  const std::string path = (std::filesystem::temp_directory_path() / "test_k411_a440.wav").string();
  writeWav(path, b, kSr, 24);
  return path;
}

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

double peakHz(const AudioBuffer& b, double guess, double t0, double t1, double span = 60) {
  double best = guess, bm = -1;
  for (double c = -span; c <= span; c += 2.0) {
    const double f = guess * std::pow(2.0, c / 1200.0), m = mag(b, f, t0, t1);
    if (m > bm) bm = m, best = f;
  }
  double lo = best * std::pow(2.0, -2.0 / 1200.0), hi = best * std::pow(2.0, 2.0 / 1200.0);
  for (int k = 0; k < 40; ++k) {
    const double g = 0.381966, x1 = lo + g * (hi - lo), x2 = hi - g * (hi - lo);
    if (mag(b, x1, t0, t1) > mag(b, x2, t0, t1)) hi = x2;
    else lo = x1;
  }
  return 0.5 * (lo + hi);
}

double centsOff(double hz, double want) { return 1200.0 * std::log2(hz / want); }

struct InstSpec {
  std::string label, type, data;
  int key = 69;
  double baseHz = 440.0;
  bool fromTracker = false;
  double tolCents = 8.0;
};

Song toneSong(const InstSpec& in, ObjectId& ch) {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.patterns[0].bars = 1;
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
    x.plugin = packs::trackerpack::fromSamplerState(smp, kSr * 4, kSr, "a440");
  } else {
    x.plugin = defaultPluginState(in.type);
    if (!in.data.empty()) x.plugin.data = in.data;
  }
  x.mixerTrack = s.masterId();
  s.channels.push_back(x);
  ch = x.id;
  s.patterns[0].notes[ch] = {Note(0, 320, in.key, 100)};
  return s;
}

AudioBuffer renderBuf(const Song& s) {
  RenderOptions o;
  o.sampleRate = kSr;
  return renderSong(s, o);
}

void setupHostedPlugins() {
  host::ScanResult clap = host::findFormat("clap")->scanInProcess(GRIDLINE_CLAP_TESTPLUG);
  host::registerScanResult(clap);
  host::ScanResult vst2 = host::findFormat("vst2")->scanInProcess(GRIDLINE_VST2_TESTPLUG);
  host::registerScanResult(vst2);
  host::ScanResult vst3 = vst3::scanBundleInProcess(GRIDLINE_TESTPLUG_BUNDLE);
  host::registerScanResult(vst3);
  expect(clap.ok && vst2.ok && vst3.ok, "hosted test plugins (CLAP, VST2, VST3) scan cleanly");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k411_semitone_pitch");
  setupHostedPlugins();

  // ---- 1. Core model, Params, JSON round-trip, undo/redo, automation ----
  {
    ObjectId cid = kNoId;
    Document doc(toneSong({"Tercet", "gl.synth", "", 69, 440.0, false, 8.0}, cid));
    const ParamId trPid = channelParam(cid, ChannelParam::Transpose);
    const auto d = describeParam(doc.song(), trPid);
    expect(d.has_value() && d->unit == "st" && d->min == -48.0 && d->max == 48.0 && d->def == 0.0,
           "ChannelParam::Transpose descriptor is -48..+48 st, default 0");
    expect(isSemitonePitchParam(*d), "ChannelParam::Transpose is recognized as a semitone pitch param");

    // Default 0 is omitted from JSON.
    const Json j0 = songToJson(doc.song());
    expect(!j0.object.at("channels").array[0].object.count("transpose"), "default transpose=0 omitted from JSON");

    // Undo / redo via Document::setParam.
    doc.setParam(trPid, 7.0, false);
    expect(doc.song().channel(cid)->transpose == 7, "setParam sets Channel::transpose to +7");
    expect(*getParam(doc.song(), trPid) == 7.0, "getParam returns +7");
    doc.setParam(trPid, -12.0, false);
    expect(doc.song().channel(cid)->transpose == -12, "setParam sets Channel::transpose to -12");
    doc.undo();
    expect(doc.song().channel(cid)->transpose == 7, "undo restores +7");
    doc.redo();
    expect(doc.song().channel(cid)->transpose == -12, "redo restores -12");

    // JSON round-trip (covers project save/load AND collaboration sync).
    Song sWithFine = doc.song();
    sWithFine.channel(cid)->fine = 35;
    const Json saved = songToJson(sWithFine);
    expect(saved.object.at("channels").array[0].num("transpose", 0) == -12, "non-zero transpose written to JSON");
    Song loaded;
    expect(songFromJson(saved, loaded), "songFromJson succeeds");
    expect(loaded.channel(cid)->transpose == -12 && loaded.channel(cid)->fine == 35,
           "transpose (-12) and fine (+35 ct) round-trip cleanly");

    // Clamping to +-48 st.
    doc.setParam(trPid, 99.0, false);
    expect(doc.song().channel(cid)->transpose == 48, "transpose clamps at +48 st");
    doc.setParam(trPid, -99.0, false);
    expect(doc.song().channel(cid)->transpose == -48, "transpose clamps at -48 st");

    // listParams includes ChannelParam::Transpose.
    const auto params = listParams(doc.song());
    expect(std::find(params.begin(), params.end(), trPid) != params.end(),
           "listParams exposes ChannelParam::Transpose for automation");
  }

  // ---- 2. Acoustic frequency audit table (native instruments + hosted CLAP/VST3/VST2) ----
  const std::string wav = makeSineWav(440.0);
  const std::vector<InstSpec> insts = {
      {"Tercet (gl.synth)", "gl.synth", "", 69, 440.0, false, 8.0},
      {"Sampler (gl.sampler)", "gl.sampler", wav, 60, 440.0, false, 5.0},
      {"Tracker Sampler", "gl.tracker", wav, 60, 440.0, true, 5.0},
      {"Analog (gl.analog)", "gl.analog", "", 69, 440.0, false, 10.0},
      {"Isoline (gl.wavetable)", "gl.wavetable", "", 69, 440.0, false, 10.0},
      {"FM (gl.fm)", "gl.fm", "", 69, 440.0, false, 10.0},
      {"Keys (gl.keys)", "gl.keys", "", 69, 440.0, false, 15.0},
      {"Acid (gl.acid)", "gl.acid", "", 69, 440.0, false, 15.0},
      {"Alloy (gl.alloy)", "gl.alloy", "", 69, 440.0, false, 10.0},
      {"Facet (gl.additive)", "gl.additive", "", 69, 440.0, false, 10.0},
      {"Halftone (gl.harmor)", "gl.harmor", "", 69, 440.0, false, 10.0},
      {"Plasma (gl.plasma)", "gl.plasma", "", 69, 440.0, false, 12.0},
      {"Hosted CLAP Synth", kClapSynth, "", 69, 440.0, false, 5.0},
      {"Hosted VST3 Synth", kVst3Synth, "", 69, 440.0, false, 5.0},
      {"Hosted VST2 Synth", kVst2Synth, "", 69, 440.0, false, 5.0},
  };

  std::printf("\n# K-411 Semitone Transpose Audit Table\n");
  std::printf("# %-24s | %8s | %8s | %8s | %10s\n", "Instrument", "0 st", "+7 st", "+12 st", "+7st+35ct");
  for (const InstSpec& in : insts) {
    if (!findPlugin(in.type)) {
      std::printf("# skip %s (not available)\n", in.label.c_str());
      continue;
    }
    auto measure = [&](int tr, int fine) -> double {
      ObjectId ch = kNoId;
      Song s = toneSong(in, ch);
      s.channel(ch)->transpose = tr;
      s.channel(ch)->fine = fine;
      const AudioBuffer b = renderBuf(s);
      const double want = in.baseHz * std::pow(2.0, (tr + fine / 100.0) / 12.0);
      return peakHz(b, want, 0.25, 1.15);
    };
    const double f0 = measure(0, 0);
    const double f7 = measure(7, 0);
    const double f12 = measure(12, 0);
    const double f7f = measure(7, 35);
    const double e7 = std::fabs(centsOff(f7, f0 * std::pow(2.0, 7.0 / 12.0)));
    const double e12 = std::fabs(centsOff(f12, f0 * 2.0));
    const double e7f = std::fabs(centsOff(f7f, f0 * std::pow(2.0, 7.35 / 12.0)));
    std::printf("# %-24s | %6.1fHz | %6.1fHz | %6.1fHz | %8.1fHz  (err %.2f / %.2f / %.2f ct)\n",
                in.label.c_str(), f0, f7, f12, f7f, e7, e12, e7f);
    expect(e7 <= in.tolCents, (in.label + ": +7 st within tolerance").c_str());
    expect(e12 <= in.tolCents, (in.label + ": +12 st within tolerance").c_str());
    expect(e7f <= in.tolCents, (in.label + ": +7 st + 35 ct fine within tolerance").c_str());
  }

  // ---- 3. Automation of ChannelParam::Transpose ----
  {
    ObjectId ch = kNoId;
    Song s = toneSong({"Tercet", "gl.synth", "", 69, 440.0, false, 8.0}, ch);
    s.patterns[0].bars = 2;
    s.patterns[0].notes[ch] = {Note(0, 144, 69, 100), Note(192, 144, 69, 100)};
    AutomationClip ac;
    ac.id = s.allocateId();
    ac.owner = ch;
    ac.index = static_cast<uint32_t>(ChannelParam::Transpose);
    // Normalized value for 0 st in [-48..+48] is 0.5; for +12 st is (12+48)/96 = 0.625.
    ac.points = {{0, 0.5, 0.0f, AutoShape::Hold}, {192, 0.625, 0.0f, AutoShape::Hold}};
    s.automations.push_back(ac);
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Automation;
    c.source = ac.id;
    c.track = 1;
    c.start = 0;
    c.length = 384;
    s.playlist.push_back(c);
    const AudioBuffer b = renderBuf(s);
    const double fFirst = peakHz(b, 440.0, 0.15, 0.60);
    const double fSecond = peakHz(b, 880.0, 1.15, 1.60);
    expect(std::fabs(centsOff(fFirst, 440.0)) < 5.0 && std::fabs(centsOff(fSecond, 880.0)) < 5.0,
           "automated ChannelParam::Transpose shifts second note cleanly from 0 st to +12 st");
  }

  // ---- 4. Hosted plugin mid-note transpose change -> ZERO stuck notes ----
  {
    for (const std::string& hostedType : {kVst2Synth, kClapSynth, kVst3Synth}) {
      if (!findPlugin(hostedType)) continue;
      ObjectId cid = kNoId;
      Song s = toneSong({hostedType, hostedType, "", 69, 440.0, false, 5.0}, cid);
      Channel& ch = *s.channel(cid);
      ch.transpose = 12;

      ProgramBuilder pb;
      AudioEngine eng(kSr);
      eng.setProgram(pb.build(s));
      std::vector<float> L(256), R(256);
      auto pump = [&](int blocks) {
        double maxAbs = 0;
        for (int b = 0; b < blocks; ++b) {
          eng.process(L.data(), R.data(), 256);
          for (int i = 0; i < 256; ++i) maxAbs = std::max(maxAbs, static_cast<double>(std::fabs(L[static_cast<size_t>(i)])));
        }
        return maxAbs;
      };
      // Start C4 (60) while transpose == +12 (hosted plugin receives MIDI note 72).
      eng.noteOn(ch.id, 60, 0.9f);
      const double sounding1 = pump(12);
      expect(sounding1 > 1e-3, ("hosted mid-note test sounding (" + hostedType + ")").c_str());

      // Change transpose MID-NOTE from +12 to -5 while note 60 is still held!
      ch.transpose = -5;
      eng.setProgram(pb.build(s));
      pump(6);

      // Start a second note (E4 = 64 -> transposed to 59) and then release BOTH.
      eng.noteOn(ch.id, 64, 0.9f);
      pump(8);
      eng.noteOff(ch.id, 60);
      eng.noteOff(ch.id, 64);

      // Drain release tail (2 seconds) and verify complete silence -> ZERO stuck notes!
      pump(180);
      const double tailPeak = pump(20);
      expect(tailPeak < 1e-4, ("hosted mid-note transpose leaves ZERO stuck notes (" + hostedType + ")").c_str());
    }
  }

  // ---- 5. Audit EVERY native instrument's coarse pitch ("st") parameter stepping ----
  {
    int semiParamCount = 0;
    for (const PluginInfo& p : pluginCatalog()) {
      if (p.kind != PluginKind::Generator || isHostedType(p.type)) continue;
      for (const ParamDesc& d : p.params) {
        if (d.kind == ParamKind::Continuous && d.scale == ParamScale::Linear && d.unit == "st" && (d.max - d.min) >= 4.0) {
          ++semiParamCount;
          expect(isSemitonePitchParam(d), (p.type + " " + d.name + ": detected as semitone pitch param").c_str());
          const double probe = std::clamp(3.42, d.min + 0.5, d.max - 0.5);
          const double snapped = snapSemitonePitch(d, probe, false);
          const double fine = snapSemitonePitch(d, probe, true);
          expect(std::fabs(snapped - std::round(probe)) < 1e-9,
                 (p.type + " " + d.name + ": default drag snaps to whole semitone").c_str());
          expect(std::fabs(fine - probe) < 1e-9,
                 (p.type + " " + d.name + ": fine modifier preserves fractional value").c_str());
        }
      }
    }
    std::printf("# Audited %d native instrument semitone pitch parameters: all snap cleanly to semitones\n", semiParamCount);
    expect(semiParamCount >= 20, "audited at least 20 native instrument semitone pitch controls");
  }

  std::error_code ec;
  std::filesystem::remove(wav, ec);
  return finish();
}
