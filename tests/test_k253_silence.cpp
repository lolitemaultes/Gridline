// Gridline: K-253, Trim silence at the ends and Remove silence (engine/Silence.h), one engine for
// the playlist, Loupe and the sample instruments. The test audio: phrases of tone over a -72 dB
// noise floor, a short pause inside a phrase, a breath (a short quiet noise) between two
// phrases, a click in a long silence, silence at both ends. Checks:
//  * detection: the automatic threshold sits between the noise and the sound; the parts are the
//    phrases with their margins; a pause shorter than the shortest gap stays; a click is silence;
//    a breath stays with Keep breaths and goes without; a fixed threshold works too;
//  * a trim keeps exactly what a remove keeps at the two ends (one engine);
//  * audio (Loupe, instruments): the trim and the remove keep the sound sample for sample (away
//    from the fades), the markers move with the audio, fades at the cuts, all silence unchanged;
//  * playlist clips: Trim moves the clip in to its sound and it still plays the same; Remove cuts
//    it into the phrases (the first keeps the clip's id), fades at the cuts, Close the gaps packs
//    them and pulls the later clips on the track; a reversed clip, a tempo-following clip; one
//    undo step each; a looping clip is refused;
//  * a channel's sample (Sampler, Kerf): a new file, the start / loop points and Kerf's markers
//    moved with the audio, the slice names kept.
#include <cmath>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Song.h"
#include "engine/ClipOps.h"
#include "engine/KerfMarks.h"
#include "engine/Render.h"
#include "engine/Sample.h"
#include "engine/Silence.h"
#include "packs/edison/edison.h"

using namespace gl;
namespace ed = gl::packs::edison;
namespace fs = std::filesystem;

namespace {

std::string gDir;
constexpr double kRate = 44100;

// Sound in seconds: [0.5, 1.5) phrase A (with a 0.15 s pause at 1.0), breath [1.9, 2.1),
// [2.5, 3.5) phrase B, a click at 4.5, [5.5, 6.0) phrase C, silence to 6.5.
struct Layout {
  double a0 = 0.5, a1 = 1.5, pause0 = 1.0, pause1 = 1.15, br0 = 1.9, br1 = 2.1, b0 = 2.5, b1 = 3.5, click = 4.5, c0 = 5.5, c1 = 6.0, end = 6.5;
};

ed::Audio testAudio() {
  const Layout L;
  ed::Audio a = ed::makeAudio(kRate, 2, static_cast<int64_t>(L.end * kRate));
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  const float noise = static_cast<float>(std::pow(10.0, -72.0 / 20.0));
  for (int64_t i = 0; i < a.frames(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    float v = noise * u(rng);
    const bool phrase = (t >= L.a0 && t < L.a1 && !(t >= L.pause0 && t < L.pause1)) || (t >= L.b0 && t < L.b1) || (t >= L.c0 && t < L.c1);
    if (phrase) v += 0.4f * static_cast<float>(std::sin(2 * M_PI * 220 * t));
    if (t >= L.br0 && t < L.br1) v += 0.012f * u(rng);  // a breath: about -42 dB, far under the phrases
    if (t >= L.click && t < L.click + 0.003) v += 0.5f;
    a.L[static_cast<size_t>(i)] = v;
    a.R[static_cast<size_t>(i)] = 0.8f * v;
  }
  return a;
}

std::string writeWav(const std::string& name, const ed::Audio& a) {
  const std::string p = gDir + "/" + name + ".wav";
  ed::writeWavFile(p, ed::makeContent(a), 32);
  return p;
}

int64_t fr(double s) { return static_cast<int64_t>(std::llround(s * kRate)); }
bool near(int64_t f, double s, double tolSec) { return std::fabs(static_cast<double>(f) / kRate - s) <= tolSec; }

void testDetection() {
  const Layout L;
  const ed::Audio a = testAudio();
  silence::Options o;
  const double thr = silence::thresholdFor(a.L.data(), a.R.data(), a.frames(), kRate, o);
  std::printf("# automatic threshold %.1f dB\n", thr);
  expect(thr > -70 && thr < -45, "the automatic threshold sits between the noise floor and the breath");
  const auto sp = silence::soundSpans(a, o);
  for (const auto& s : sp) std::printf("# part %.3f .. %.3f\n", static_cast<double>(s.a) / kRate, static_cast<double>(s.b) / kRate);
  // Keep breaths: A (pause closed up), the breath, B, C; the click is silence.
  const double m = o.marginMs / 1000.0, tol = 0.012;
  expect(sp.size() == 4, "four parts: phrase A, the breath, phrase B, phrase C (the click is silence)");
  if (sp.size() == 4) {
    expect(near(sp[0].a, L.a0 - m, tol) && near(sp[0].b, L.a1 + m, tol), "phrase A with its margins, its short pause kept");
    expect(near(sp[1].a, L.br0 - m, tol) && near(sp[1].b, L.br1 + m, tol), "the breath stays (Keep breaths)");
    expect(near(sp[2].a, L.b0 - m, tol) && near(sp[3].b, L.c1 + m, tol), "phrases B and C");
  }
  silence::Options nb = o;
  nb.keepBreaths = false;
  const auto sp2 = silence::soundSpans(a, nb);
  expect(sp2.size() == 3, "without Keep breaths the breath goes too");
  silence::Options gap = o;
  gap.minGapMs = 100;
  const auto sp3 = silence::soundSpans(a, gap);
  expect(sp3.size() == 5, "with a shorter shortest gap the pause inside phrase A is a silence too");
  silence::Options fixed = o;
  fixed.autoThreshold = false;
  fixed.thresholdDb = -30;  // above the breath: it is silence
  expect(silence::soundSpans(a, fixed).size() == 3, "a fixed threshold above the breath");
  // One engine: a trim keeps what a remove keeps at the two ends.
  const auto t = silence::trimSpan(a, o);
  expect(!sp.empty() && t.a == sp.front().a && t.b == sp.back().b, "the trim's ends are the remove's first and last part");
  // All silence: nothing.
  ed::Audio quiet = ed::makeAudio(kRate, 2, fr(1.0));
  expect(silence::soundSpans(quiet, o).empty() && silence::trimSpan(quiet, o).len() == 0, "all silence: no parts");
  // mapFrame
  const std::vector<silence::Span> keep = {{10, 20}, {30, 40}};
  expect(silence::mapFrame(keep, 5) == 0 && silence::mapFrame(keep, 15) == 5 && silence::mapFrame(keep, 25) == 10 && silence::mapFrame(keep, 35) == 15 &&
             silence::mapFrame(keep, 99) == 20 && silence::keptFrames(keep) == 20,
         "frames move with the kept parts");
}

void testAudioEdits() {
  const ed::Audio a = testAudio();
  silence::Options o;
  ed::Content c = ed::makeContent(a);
  c.markers.push_back(ed::Marker{fr(2.7), "B"});
  c.markers.push_back(ed::Marker{fr(5.7), "C"});
  // Trim.
  silence::Span k;
  const ed::Content t = silence::trimEnds(c, o, &k);
  expect(t.frames() == k.len() && k.a > 0 && k.b < a.frames(), "trim: the audio from the first sound to the last");
  bool same = t.frames() > 0;
  const int64_t f = fr(o.fadeMs / 1000.0);
  for (int64_t i = f; i < t.frames() - f && same; ++i) same = t.a().L[static_cast<size_t>(i)] == a.L[static_cast<size_t>(i + k.a)];
  expect(same, "trim: the sound kept sample for sample (away from the fades)");
  expect(t.a().L[0] == 0.0f, "trim: a fade in at the cut");
  expect(t.markers.size() == 2 && t.markers[0].pos == fr(2.7) - k.a, "trim: the markers move with the audio");
  // Remove.
  std::vector<silence::Span> keep;
  const ed::Content r = silence::removeSilence(c, o, &keep);
  expect(keep.size() == 4 && r.frames() == silence::keptFrames(keep), "remove: only the parts with sound are left");
  bool kept = keep.size() == 4;
  for (const auto& s : keep) {
    const int64_t at = silence::mapFrame(keep, s.a);
    for (int64_t i = f; i < s.len() - f && kept; ++i) kept = r.a().L[static_cast<size_t>(at + i)] == a.L[static_cast<size_t>(s.a + i)];
  }
  expect(kept, "remove: every part kept sample for sample (away from the fades)");
  expect(r.markers.size() == 2 && r.markers[0].pos == silence::mapFrame(keep, fr(2.7)) && r.markers[1].pos == silence::mapFrame(keep, fr(5.7)),
         "remove: the markers move with the audio");
  if (keep.size() >= 2) {
    const int64_t j = silence::mapFrame(keep, keep[1].a);
    expect(std::fabs(r.a().L[static_cast<size_t>(j)]) < 1e-6f && std::fabs(r.a().L[static_cast<size_t>(j - 1)]) < 1e-6f, "remove: fades at every join");
  }
  // Nothing to take out: unchanged (the same buffer).
  ed::Content loud = ed::makeContent(ed::makeAudio(kRate, 2, fr(0.5)));
  for (auto& v : const_cast<ed::Audio&>(loud.a()).L) v = 0.3f;
  const_cast<ed::Audio&>(loud.a()).R = loud.a().L;
  expect(silence::removeSilence(loud, o).audio == loud.audio && silence::trimEnds(loud, o).audio == loud.audio, "no silence: unchanged");
  const ed::Content q = ed::makeContent(ed::makeAudio(kRate, 2, fr(0.5)));
  expect(silence::trimEnds(q, o).audio == q.audio, "all silence: left as it is");
}

ObjectId addClip(Song& s, const std::string& path, Tick start, Tick length, int track) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = fs::path(path).stem().string();
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.track = track;
  c.start = start;
  c.length = length;
  s.playlist.push_back(c);
  return c.id;
}

AudioBuffer render(const Song& s, Tick end) {
  RenderJob j;
  j.sampleRate = static_cast<int>(kRate);
  j.scope = RenderScope::Range;
  j.start = 0;
  j.end = end;
  j.tail = TailMode::Cut;
  j.masterEffects = false;
  return renderAudio(s, j).audio;
}

// The largest difference, leaving out [skipA, skipB) seconds (where a click was taken out).
double maxDiff(const AudioBuffer& x, const AudioBuffer& y, double skipA = -1, double skipB = -1) {
  double d = x.frames == y.frames ? 0.0 : 1.0;
  const auto sa = static_cast<size_t>(std::max(0.0, skipA * kRate)), sb = static_cast<size_t>(std::max(0.0, skipB * kRate));
  for (size_t c = 0; c < 2; ++c)
    for (size_t i = 0; i < std::min(x.frames, y.frames); ++i)
      if (i < sa || i >= sb) d = std::max(d, static_cast<double>(std::fabs(x.at(c, i) - y.at(c, i))));
  return d;
}

double peakIn(const AudioBuffer& x, double a, double b) {
  double p = 0;
  for (size_t c = 0; c < 2; ++c)
    for (size_t i = static_cast<size_t>(a * kRate); i < std::min(x.frames, static_cast<size_t>(b * kRate)); ++i) p = std::max(p, static_cast<double>(std::fabs(x.at(c, i))));
  return p;
}

void testClips() {
  const Layout L;
  const std::string path = writeWav("phrases", testAudio());
  silence::Options o;
  Song s = makeEmptySong();
  s.tempo = 120;  // 96 ticks a beat: 0.5 s
  const Tick len = static_cast<Tick>(std::llround(L.end / 0.5 * 96));
  const ObjectId id = addClip(s, path, 0, len, 0);
  const ObjectId later = addClip(s, path, len + 96, 96, 0);
  const AudioBuffer before = render(s, len);
  // Trim.
  {
    Document doc(s);
    bool ok = false;
    doc.editSong("trim silence", [&](Song& x) { ok = silence::trimClip(x, id, o); });
    const Clip* c = doc.song().clip(id);
    expect(ok && c, "trim: the clip keeps its id");
    const double t0 = c ? static_cast<double>(c->start) / 96 * 0.5 : 0, t1 = c ? static_cast<double>(c->start + doc.song().clipLength(*c)) / 96 * 0.5 : 0;
    std::printf("# trimmed clip %.3f .. %.3f s\n", t0, t1);
    const double m = o.marginMs / 1000.0;
    expect(c && t0 <= L.a0 - m + 1e-9 && t0 > L.a0 - m - 0.012 && t1 >= L.c1 + m - 1e-9 && t1 < L.c1 + m + 0.012,
           "trim: the clip starts at its first sound and ends after its last (ticks rounded outwards)");
    const double d = maxDiff(render(doc.song(), len), before);
    std::printf("# trimmed: max difference to the original %.3g\n", d);
    expect(d < 1e-3, "trim: the clip plays what it played (only the silence and its noise are gone)");
    expect(doc.undo() && doc.song().clip(id)->start == 0 && doc.song().clipLength(*doc.song().clip(id)) == len, "trim: one undo step");
  }
  // Remove, no ripple.
  {
    Document doc(s);
    std::vector<ObjectId> parts;
    doc.editSong("remove silence", [&](Song& x) { parts = silence::removeClipSilence(x, id, o); });
    expect(parts.size() == 4 && parts.front() == id, "remove: four parts (the first keeps the clip's id)");
    bool fades = parts.size() == 4;
    for (size_t i = 0; i < parts.size() && fades; ++i) {
      const Clip* c = doc.song().clip(parts[i]);
      fades = c && c->fadeInManual && c->fadeOutManual && std::fabs(c->fadeIn - o.fadeMs / 1000) < 1e-9;
    }
    expect(fades, "remove: a fade at every cut");
    const AudioBuffer after = render(doc.song(), len);
    const double d = maxDiff(after, before, L.click - 0.01, L.click + 0.02);
    std::printf("# removed: max difference to the original %.3g (the click left out)\n", d);
    expect(d < 1e-3, "remove: each part plays where it played");
    expect(peakIn(before, L.click, L.click + 0.003) > 0.4 && peakIn(after, L.click - 0.01, L.click + 0.02) < 1e-6, "remove: the click in the long silence is gone");
    expect(doc.song().clip(later)->start == len + 96, "remove: without Close the gaps the clips after stay");
    expect(doc.undo() && doc.song().clip(id) && doc.song().playlist.size() == 2, "remove: one undo step");
  }
  // Remove with Close the gaps.
  {
    silence::Options rp = o;
    rp.ripple = true;
    Song x = s;
    const auto parts = silence::removeClipSilence(x, id, rp);
    bool packed = parts.size() == 4;
    Tick at = 0;
    for (const ObjectId p : parts) {
      const Clip* c = x.clip(p);
      packed = packed && c && c->start == at;
      if (c) at += x.clipLength(*c);
    }
    expect(packed, "Close the gaps: the parts follow each other from the clip's start");
    expect(x.clip(later)->start == len + 96 - (len - at), "Close the gaps: the clip after moves left by the time taken out");
  }
  // A reversed clip.
  {
    Song x = s;
    x.clip(id)->reverse = true;
    const AudioBuffer rb = render(x, len);
    const auto parts = silence::removeClipSilence(x, id, o);
    expect(parts.size() == 4, "a reversed clip: its four parts");
    const double rc = L.end - L.click;  // the click, played backwards
    expect(maxDiff(render(x, len), rb, rc - 0.02, rc + 0.01) < 1e-3, "a reversed clip: each part plays where it played");
  }
  // A clip that follows the tempo.
  {
    Song x = s;
    x.clip(id)->syncBpm = 120;
    x.tempo = 100;  // it plays slower (stretched) and longer
    x.clip(id)->length = static_cast<Tick>(std::llround(L.end / 0.5 * 96));
    const auto parts = silence::removeClipSilence(x, id, o);
    expect(parts.size() == 4, "a tempo-following clip: its four parts");
  }
  // A looping clip is refused.
  {
    Song x = s;
    x.clip(id)->loop = true;
    expect(*silence::clipProblem(x, *x.clip(id)) != 0 && !silence::trimClip(x, id, o) && silence::removeClipSilence(x, id, o).empty(),
           "a looping clip is left alone (turn its loop off first)");
  }
}

void testChannels() {
  const std::string path = writeWav("instrument", testAudio());
  silence::Options o;
  Song s = makeEmptySong();
  Channel smp;
  smp.id = s.allocateId();
  smp.plugin = defaultPluginState("gl.sampler");
  smp.plugin.data = path;
  smp.plugin.params[pp::SmpLoopStart] = static_cast<float>(2.7 / 6.5);
  s.channels.push_back(smp);
  Channel kerf;
  kerf.id = s.allocateId();
  kerf.plugin = defaultPluginState("gl.slicer");
  kerf.plugin.params[pp::SlMode] = pp::SlModeManual;
  kerf.plugin.params[pp::slMarker(0)] = static_cast<float>(0.5 / 6.5);
  kerf.plugin.params[pp::slMarker(1)] = static_cast<float>(5.5 / 6.5);
  kerf.plugin.data = path;
  s.channels.push_back(kerf);
  expect(silence::channelHasSample(smp) && silence::channelHasSample(kerf), "sample channels have a sample to work on");
  std::string why;
  const std::string t = silence::applyToChannel(s, smp.id, false, o, gDir + "/Edited", &why);
  SamplePtr ts = t.empty() ? nullptr : loadSample(t);
  const auto span = silence::trimSpan(testAudio(), o);
  expect(ts && static_cast<int64_t>(ts->frames()) == span.len() && s.channel(smp.id)->plugin.data == t, "Sampler trim: a new file the channel plays");
  const double ls = s.channel(smp.id)->plugin.params[pp::SmpLoopStart] * static_cast<double>(span.len());
  expect(std::fabs(ls - static_cast<double>(fr(2.7) - span.a)) < 2, "Sampler trim: the loop start moves with the audio");
  std::vector<silence::Span> keep = silence::soundSpans(testAudio(), o);
  const std::string r = silence::applyToChannel(s, kerf.id, true, o, gDir + "/Edited", &why);
  SamplePtr rs = r.empty() ? nullptr : loadSample(r);
  expect(rs && static_cast<int64_t>(rs->frames()) == silence::keptFrames(keep), "Kerf remove: a new file without the silences");
  const double m1 = s.channel(kerf.id)->plugin.params[pp::slMarker(1)] * static_cast<double>(silence::keptFrames(keep));
  expect(std::fabs(m1 - static_cast<double>(silence::mapFrame(keep, fr(5.5)))) < 2, "Kerf remove: the slice markers move with the audio");
  // Kerf's slice names stay after the path.
  Song k = s;
  Channel& kc = *k.channel(kerf.id);
  kc.plugin.data = kerfmarks::withNames(path, {{0, "Intro"}});
  const std::string tail = kc.plugin.data.substr(kc.plugin.data.find('\n'));
  const std::string r2 = silence::applyToChannel(k, kerf.id, true, o, gDir + "/Edited", &why);
  expect(kerfmarks::hasNames(kc.plugin.data) && !r2.empty() && k.channel(kerf.id)->plugin.data == r2 + tail &&
             kerfmarks::namesOf(k.channel(kerf.id)->plugin.data).at(0) == "Intro",
         "Kerf: the slice names stay with the new file");
  // Nothing to do: a reason, no change.
  Song q = s;
  const std::string again = silence::applyToChannel(q, smp.id, false, o, gDir + "/Edited", &why);
  expect(again.empty() && !why.empty(), "a sample already trimmed: nothing changes, and the reason is given");
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("k253");
  testDetection();
  testAudioEdits();
  testClips();
  testChannels();
  return finish();
}
