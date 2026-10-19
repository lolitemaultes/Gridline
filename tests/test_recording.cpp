// Gridline — J-8 audio recording & audio clip workflow tests. The audio
// device is simulated: every block pushes generated input frames into the
// engine's input queue before processing, exactly as the capture callback does.
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/Song.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/AudioRecord.h"
#include "../src/engine/ClipOps.h"
#include "../src/engine/Render.h"
#include "../src/engine/Sample.h"
#include "../src/packs/edison/edison.h"
#include "Check.h"
#include "Hermetic.h"

namespace fs = std::filesystem;
using namespace gl;
namespace ed = gl::packs::edison;

namespace {

std::string gDir;

struct Rig {
  double sr;
  Song song;
  ProgramBuilder builder;
  AudioEngine eng;
  AudioRecorder rec;
  std::shared_ptr<const Program> prog;
  int64_t frame = 0;  // input frames pushed so far
  std::function<float(int64_t, int)> input = [](int64_t, int) { return 0.f; };
  std::vector<float> outL, outR;  // output of the last run

  explicit Rig(double rate = 48000) : sr(rate), eng(rate) {
    song = makeEmptySong();
    builder.setSampleRate(rate);
    eng.setInputChannels(2);
    eng.setMode(PlayMode::Song);
    eng.setLooping(false);
  }
  void build() {
    prog = builder.build(song);
    eng.setProgram(prog);
  }
  void run(int64_t frames, int block = 256) {
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block)), in(static_cast<size_t>(block) * 2);
    outL.clear();
    outR.clear();
    for (int64_t done = 0; done < frames;) {
      int n = static_cast<int>(std::min<int64_t>(block, frames - done));
      for (int i = 0; i < n; ++i) {
        in[static_cast<size_t>(2 * i)] = input(frame + i, 0);
        in[static_cast<size_t>(2 * i + 1)] = input(frame + i, 1);
      }
      eng.pushInput(in.data(), n);
      eng.process(L.data(), R.data(), n);
      outL.insert(outL.end(), L.begin(), L.begin() + n);
      outR.insert(outR.end(), R.begin(), R.begin() + n);
      frame += n;
      done += n;
      rec.pump(eng, ringsOf(*prog));
    }
  }
  double spt() const { return sr * 60.0 / (song.tempo * song.ppq); }  // samples per tick
  // Stops the transport and lets the engine close the takes (latency tail).
  void finish() {
    eng.setRecording(false);
    eng.stop();
    run(eng.recordLatency() + 1024);
  }
};

float peakOf(const std::vector<float>& v, size_t from = 0, size_t to = ~size_t{0}) {
  float p = 0;
  for (size_t i = from; i < std::min(to, v.size()); ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}

// ------------------------------------------------------------------ input & monitoring
void testMonitoring() {
  Rig r;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].inputStereo = true;
  r.input = [](int64_t, int c) { return c == 0 ? 0.25f : -0.5f; };
  r.build();
  r.run(1024);
  expect(peakOf(r.outL, 512) > 0.1f && peakOf(r.outR, 512) > 0.2f, "monitor auto: stopped transport hears the input");
  expect(std::fabs(r.outR.back() / r.outL.back() + 2.0f) < 0.01f, "stereo input pair keeps L/R");
  auto rt = r.builder.mixerRuntime(r.song.mixer[1].id);
  expect(rt && rt->inPeakL.load() > 0.24f && rt->inPeakR.load() > 0.49f, "track input meter");
  expect(r.eng.takeInputPeak(1) > 0.49f && r.eng.takeInputPeak(1) == 0.f, "device input meter reads and resets");

  r.eng.play();
  r.run(1024);
  expect(peakOf(r.outL, 512) == 0.f, "monitor auto: unarmed track is silent while playing");
  r.song.mixer[1].armed = true;
  r.build();
  r.run(1024);
  expect(peakOf(r.outL, 512) > 0.1f, "monitor auto: armed track hears the input while playing");
  r.song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::Off);
  r.build();
  r.run(1024);
  expect(peakOf(r.outL, 512) == 0.f, "monitor off: silent");
  r.song.mixer[1].armed = false;
  r.song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::On);
  r.build();
  r.run(1024);
  expect(peakOf(r.outL, 512) > 0.1f, "monitor on: always heard");
  r.song.mixer[1].inputStereo = false;
  r.build();
  r.run(1024);
  expect(std::fabs(r.outL.back() - r.outR.back()) < 1e-6f && r.outL.back() > 0.1f, "mono input feeds both sides");
  r.song.mixer[1].input = 1;
  r.build();
  r.run(1024);
  expect(r.outL.back() < -0.2f, "input selector picks the device channel (In 2)");
  r.song.mixer[1].input = -1;
  r.build();
  r.run(1024);
  expect(peakOf(r.outL, 512) == 0.f, "no input selected: nothing heard");
}

void testInputQueue() {
  Rig r;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::On);
  r.build();
  r.eng.setInputPrefill(256);
  r.input = [](int64_t, int) { return 0.5f; };
  std::vector<float> L(256), R(256), in(512, 0.5f);
  r.eng.pushInput(in.data(), 256);
  r.eng.process(L.data(), R.data(), 256);
  expect(peakOf(L) == 0.f, "jitter buffer: silent until the prefill is queued");
  r.eng.pushInput(in.data(), 256);
  r.eng.pushInput(in.data(), 256);
  r.eng.process(L.data(), R.data(), 256);
  expect(peakOf(L) > 0.3f, "jitter buffer: plays once primed");
  r.eng.process(L.data(), R.data(), 256);
  r.eng.process(L.data(), R.data(), 256);
  r.eng.process(L.data(), R.data(), 256);  // starved
  expect(r.eng.inputUnderruns() >= 1, "underrun counted");
  // A backlog (stream paused) is dropped down to the prefill.
  std::vector<float> big(2 * 20000, 0.25f);
  r.eng.pushInput(big.data(), 20000);
  r.eng.process(L.data(), R.data(), 256);
  r.eng.process(L.data(), R.data(), 256);
  expect(peakOf(L) > 0.2f, "backlog: still plays after dropping old frames");
}

// ------------------------------------------------------------------ placement & latency
void testPlacement() {
  Rig r(48000);
  r.song.tempo = 133;  // non-integer samples per tick
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.build();
  const int lat = 137;
  constexpr int64_t F = 30011;  // input frame carrying the impulse
  r.eng.setRecordLatency(lat);
  r.input = [](int64_t f, int) { return f == F ? 1.0f : (f == F + 1 ? -0.5f : 0.0f); };
  r.rec.begin();
  r.eng.setRecording(true);
  r.eng.play();
  r.run(48000, 256);
  r.finish();
  expect(!r.rec.open(), "take closed after stop + latency tail");
  auto& takes = r.rec.takes();
  expect(takes.size() == 1, "one take");
  if (takes.empty()) return;
  expect(std::fabs(takes[0].startTick) < 1e-9, "take starts at the play position");
  // Device frames arrive `lat` late: the take covers the played music exactly
  // (the stop lets the last `lat` frames in).
  expect(takes[0].frames() == 48000, "take length = played frames");
  expect(takes[0].L.size() > static_cast<size_t>(F - lat) && takes[0].L[static_cast<size_t>(F - lat)] == 1.0f,
         "latency compensation: impulse moved earlier by the latency");
  TakeCommitOptions o;
  o.dir = gDir + "/Recorded";
  o.sampleRate = r.sr;
  TakeCommitResult res = commitTakes(r.song, takes, o);
  expect(res.clips.size() == 1 && res.files.size() == 1 && res.error.empty(), "take committed as one clip + file");
  if (res.clips.empty()) return;
  const Clip* c = r.song.clip(res.clips[0]);
  const Channel* ch = c ? r.song.channel(c->source) : nullptr;
  expect(c && c->kind == ClipKind::Audio && c->track == 0, "audio clip on the insert's playlist track (insert 1 -> track 1)");
  expect(ch && ch->mixerTrack == r.song.mixer[1].id && ch->plugin.type == "gl.sampler", "clip channel plays through the recorded insert");
  expect(fs::exists(res.files[0]) && fs::path(res.files[0]).parent_path() == fs::path(o.dir), "WAV written to <project>/Recorded");
  SamplePtr s = loadSample(res.files[0]);
  expect(s && s->frames() == takes[0].L.size(), "file holds the take");
  if (!s || !c) return;
  // Locate the impulse in the file and map it to the song timeline.
  size_t k = 0;
  for (size_t i = 0; i < s->frames(); ++i)
    if (s->left[i] > 0.9f) k = i;
  const double at = static_cast<double>(c->start) * r.spt() + static_cast<double>(k);
  expect(std::fabs(at - static_cast<double>(F - lat)) <= 1.0, "sample-accurate placement (impulse lands on the frame played)");
  expect(s->left[k + 1] == -0.5f, "32-bit float keeps samples exactly");
  // Playback: the committed clip plays the impulse at the same song frame.
  r.song.mixer[1].armed = false;
  r.song.mixer[1].input = -1;
  RenderOptions ro;
  ro.sampleRate = 48000;
  AudioBuffer out = renderSong(r.song, ro);
  size_t pk = 0;
  float best = 0;
  for (size_t i = 0; i < out.frames; ++i)
    if (std::fabs(out.at(0, i)) > best) best = std::fabs(out.at(0, i)), pk = i;
  expect(best > 0.1f && std::fabs(static_cast<double>(pk) - static_cast<double>(F - lat)) <= 1.0, "rendered song plays the recording in time");

  // 24-bit option.
  o.bits = 24;
  Song s2 = r.song;
  TakeCommitResult res24 = commitTakes(s2, takes, o);
  SamplePtr s24 = res24.files.empty() ? nullptr : loadSample(res24.files[0]);
  expect(s24 && std::fabs(s24->left[k] - 1.0f) < 1e-3f, "24-bit WAV option");
}

void testPunch() {
  Rig r(48000);
  r.song.tempo = 133;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.build();
  const double a = 2 * 96, b = 4 * 96;  // beats 2..4
  r.eng.setPunch(a, b);
  r.input = [](int64_t f, int) { return static_cast<float>(f % 100000) / 100000.0f; };
  r.rec.begin();
  r.eng.setRecording(true);
  r.eng.play();
  r.run(static_cast<int64_t>(6 * 96 * r.spt()), 100);
  r.finish();
  auto& takes = r.rec.takes();
  expect(takes.size() == 1, "punch: one take");
  if (takes.empty()) return;
  const double spt = r.spt();
  const int64_t first = static_cast<int64_t>(std::ceil(a * spt - 1e-6)), last = static_cast<int64_t>(std::ceil(b * spt - 1e-6));
  expect(std::fabs(takes[0].L[0] - static_cast<float>(first % 100000) / 100000.0f) < 1e-6f, "punch in on the first frame at/after the in point");
  expect(takes[0].frames() == last - first, "punch out: frames end at the out point");
  expect(takes[0].startTick >= a && takes[0].startTick < a + 1.0 / spt + 1e-9, "punch take starts at the in point");
  TakeCommitOptions o;
  o.dir = gDir + "/Recorded";
  o.sampleRate = r.sr;
  auto res = commitTakes(r.song, takes, o);
  const Clip* c = res.clips.empty() ? nullptr : r.song.clip(res.clips[0]);
  expect(c && c->start == static_cast<Tick>(a), "punch clip starts at the in point");
  expect(c && std::llabs(c->length - static_cast<Tick>(b - a)) <= 1, "punch clip spans the punch range");
}

void testLoopTakes() {
  Rig r(44100);
  r.song.tempo = 120;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.build();
  const Tick bar = r.song.barTicks();
  r.eng.setLooping(true);
  r.eng.setLoopRange(0, bar);
  r.input = [](int64_t f, int) { return 0.001f + static_cast<float>(f % 1000000) * 1e-7f; };
  r.rec.begin();
  r.eng.setRecording(true);
  r.eng.play();
  const double barFrames = static_cast<double>(bar) * r.spt();
  r.run(static_cast<int64_t>(barFrames * 2.5), 512);
  r.finish();
  auto takes = r.rec.release();
  expect(takes.size() == 3, "loop recording: one take per pass");
  if (takes.size() != 3) return;
  expect(takes[0].pass == 0 && takes[1].pass == 1 && takes[2].pass == 2, "passes numbered");
  expect(std::llabs(takes[0].frames() - static_cast<int64_t>(barFrames)) <= 1 && std::llabs(takes[1].frames() - static_cast<int64_t>(barFrames)) <= 1,
         "full passes are one loop long");
  expect(takes[1].startTick == 0 && takes[2].startTick == 0, "every pass starts at the loop start");
  expect(std::fabs(takes[1].L[0] - (0.001f + static_cast<float>(takes[0].frames()) * 1e-7f)) < 1e-6f, "no frame lost at the wrap");
  TakeCommitOptions o;
  o.dir = gDir + "/Recorded";
  o.sampleRate = r.sr;
  auto res = commitTakes(r.song, takes, o);
  expect(res.clips.size() == 3, "three take clips");
  if (res.clips.size() != 3) return;
  const Clip* c0 = r.song.clip(res.clips[0]);
  const Clip* c1 = r.song.clip(res.clips[1]);
  const Clip* c2 = r.song.clip(res.clips[2]);
  expect(c0->takeGroup != kNoId && c0->takeGroup == c1->takeGroup && c1->takeGroup == c2->takeGroup, "stacked takes share a group");
  expect(c0->track == 0 && c1->track == 1 && c2->track == 2, "takes stack on the tracks below");
  expect(c0->muted && c1->muted && !c2->muted, "earlier takes muted, the last one plays");
  expect(takesInGroup(r.song, c0->takeGroup) == std::vector<int>({0, 1, 2}), "takes listed");
  // Comp: take 1 for the second beat.
  const ObjectId g = c0->takeGroup;
  const Tick beat = r.song.beatTicks();
  expect(compTakes(r.song, g, 0, beat, 0), "comp applied");
  int inRange = 0, playing = 0;
  for (const auto& c : r.song.playlist) {
    if (c.takeGroup != g) continue;
    if (c.start == 0 && c.start + r.song.clipLength(c) == beat) {
      ++inRange;
      if (!c.muted) playing += c.take == 0 ? 1 : 100;
    }
  }
  expect(inRange == 3 && playing == 1, "comp: in the range only the chosen take plays");
  bool tailOk = false;
  for (const auto& c : r.song.playlist)
    if (c.takeGroup == g && c.take == 2 && c.start == beat) tailOk = !c.muted;
  expect(tailOk, "comp: outside the range the previous choice stays");
  // The split keeps the audio continuous: the piece after the cut reads from the cut point.
  for (const auto& c : r.song.playlist)
    if (c.takeGroup == g && c.take == 0 && c.start == beat) expect(std::fabs(c.audioStart - 0.5) < 1e-6, "split clip reads from the cut");
}

void testEngineFlags() {
  Rig r;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.build();
  r.eng.setRecording(true);
  r.rec.begin();
  r.run(2048);
  expect(r.rec.takes().empty(), "nothing is recorded while the transport is stopped");
  r.eng.playWithCountIn(96 * 4);
  r.run(2048);
  expect(r.rec.takes().empty() && r.eng.countingIn(), "nothing is recorded during the count-in");
  r.run(100000);
  expect(r.rec.takes().size() == 1, "recording starts after the count-in");
  if (!r.rec.takes().empty()) expect(std::fabs(r.rec.takes()[0].startTick) < 1e-9, "count-in take starts at the play position");
  r.song.mixer[1].armed = false;
  r.build();
  r.run(1024);
  expect(!r.rec.open(), "disarming closes the take");
}

// ------------------------------------------------------------------ clip workflow
std::string writeTestWav(const std::string& name, double sr, double secs, const std::function<float(double)>& f) {
  ed::Audio a = ed::makeAudio(sr, 2, static_cast<int64_t>(std::llround(secs * sr)));
  for (int64_t i = 0; i < a.frames(); ++i) a.L[static_cast<size_t>(i)] = a.R[static_cast<size_t>(i)] = f(static_cast<double>(i) / sr);
  std::string p = gDir + "/" + name + ".wav";
  ed::writeWavFile(p, ed::makeContent(a), 32);
  return p;
}

ObjectId addAudioClip(Song& s, const std::string& path, Tick start, Tick length, int track = 0) {
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

void testClipWorkflow() {
  const double sr = 44100;
  Song s = makeEmptySong();
  s.tempo = 120;
  const std::string tone = writeTestWav("tone", sr, 2.1, [](double t) { return 0.25f * static_cast<float>(std::sin(2 * M_PI * 220 * t)); });
  ObjectId id = addAudioClip(s, tone, 0, 96);
  expect(audioClipNaturalLength(s, *s.clip(id)) == 403, "natural length: 2.1 s at 120 BPM = 4.2 beats");
  // Fit to tempo.
  int beats = fitClipToTempo(s, id);
  expect(beats == 4 && s.clip(id)->length == 4 * 96, "fit to tempo: 4 beats");
  expect(std::fabs(s.clip(id)->syncBpm - 4 * 60.0 / 2.1) < 1e-9, "fit to tempo: clip follows the tempo at its own BPM");
  {
    Song q = s;
    q.tempo = 60;  // half tempo: the fitted clip still spans 4 beats
    RenderOptions ro;
    ro.sampleRate = static_cast<int>(sr);
    AudioBuffer out = renderSong(q, ro);
    size_t lastLoud = 0;
    for (size_t i = 0; i < out.frames; ++i)
      if (std::fabs(out.at(0, i)) > 0.01f) lastLoud = i;
    expect(std::fabs(static_cast<double>(lastLoud) / sr - 4.0) < 0.05, "fitted clip plays 4 beats at another tempo");
  }
  // Normalize.
  float g = normalizeClip(s, id, 0.0);
  expect(std::fabs(g - 4.0f) < 0.01f, "normalize: gain brings the peak to 0 dB");
  // Stretch: realtime modes change the clip length.
  s.clip(id)->syncBpm = 0;
  expect(setClipStretch(s, id, 2.0, true), "stretch set");
  expect(std::llabs(s.clip(id)->length - 806) <= 1, "stretch x2 (pitch-preserving): clip twice as long");
  const Channel* ch = s.channel(s.clip(id)->source);
  expect(ch && ch->plugin.params[pp::SmpStretchMode] >= 0.5f, "pitch-preserving mode on the clip channel");
  setClipStretch(s, id, 0.5, false);
  expect(std::llabs(s.clip(id)->length - 202) <= 1, "stretch x0.5 (resample): half as long");
  // Rendered stretch (audio editor stretch).
  setClipStretch(s, id, 1.0, false);
  std::string out = renderClipStretch(s, id, 1.5, 0, 0, gDir + "/Rendered");
  SamplePtr st = out.empty() ? nullptr : loadSample(out);
  expect(st && std::fabs(st->seconds() - 3.15) < 0.03, "rendered stretch: 1.5x longer audio");
  expect(s.clip(id)->stretch == 1.0 && std::llabs(s.clip(id)->length - 605) <= 2, "rendered stretch: clip follows the new file");

  // Slicing at the grid.
  Song s2 = makeEmptySong();
  s2.tempo = 120;
  ObjectId c2 = addAudioClip(s2, tone, 96, 4 * 96);
  auto pieces = sliceClipAtGrid(s2, c2, 96);
  expect(pieces.size() == 4, "slice at grid: 4 pieces");
  bool contiguous = true;
  for (size_t i = 0; i < pieces.size(); ++i) {
    const Clip* p = s2.clip(pieces[i]);
    contiguous = contiguous && p->start == 96 + static_cast<Tick>(i) * 96 && p->length == 96 && std::fabs(p->audioStart - 0.5 * static_cast<double>(i)) < 1e-9;
  }
  expect(contiguous, "slices are contiguous and read consecutive audio");
  // Slicing at transients: clicks every half second.
  const std::string clicks = writeTestWav("clicks", sr, 2.0, [](double t) {
    double ph = std::fmod(t, 0.5);
    return ph < 0.02 ? 0.8f * static_cast<float>(std::exp(-ph * 200) * std::sin(2 * M_PI * 1000 * t)) : 0.0f;
  });
  Song s3 = makeEmptySong();
  s3.tempo = 120;
  ObjectId c3 = addAudioClip(s3, clicks, 0, 4 * 96);
  auto tr = sliceClipAtTransients(s3, c3, 0.5);
  expect(tr.size() == 4, "slice at transients: one slice per click");
  if (tr.size() == 4) {
    bool onBeats = true;
    for (size_t i = 1; i < tr.size(); ++i) onBeats = onBeats && std::llabs(s3.clip(tr[i])->start - static_cast<Tick>(i) * 96) <= 2;
    expect(onBeats, "transient slices start at the clicks");
  }

  // Consolidate: two clips → one rendered clip.
  Song s4 = makeEmptySong();
  s4.tempo = 120;
  ObjectId a1 = addAudioClip(s4, tone, 96, 96, 2);
  ObjectId a2 = addAudioClip(s4, clicks, 4 * 96, 2 * 96, 3);
  RenderOptions ro;
  ro.sampleRate = static_cast<int>(sr);
  AudioBuffer before = renderSong(s4, ro);
  ObjectId cons = consolidateClips(s4, {a1, a2}, gDir + "/Rendered", sr, true);
  expect(cons != kNoId && !s4.clip(a1) && !s4.clip(a2), "consolidate replaces the clips");
  const Clip* cc = cons ? s4.clip(cons) : nullptr;
  expect(cc && cc->start == 96 && cc->length == 5 * 96 && cc->track == 2, "consolidated clip spans the selection on the first clip's track");
  AudioBuffer after = renderSong(s4, ro);
  double err = 0;
  for (size_t i = 0; i < std::min(before.frames, after.frames); ++i) err = std::max(err, static_cast<double>(std::fabs(before.at(0, i) - after.at(0, i))));
  expect(after.frames > 0 && err < 1e-3, "consolidated audio sounds like the clips it replaced");
  // Bounce a track.
  Song s5 = makeEmptySong();
  s5.tempo = 120;
  ObjectId b1 = addAudioClip(s5, tone, 0, 96, 1);
  ObjectId b2 = addAudioClip(s5, tone, 2 * 96, 96, 1);
  ObjectId bounced = bounceTrack(s5, 1, gDir + "/Rendered", sr);
  expect(bounced != kNoId && s5.clip(b1)->muted && s5.clip(b2)->muted, "bounce mutes the track's clips");
  expect(bounced != kNoId && s5.clip(bounced)->track == 2 && s5.clip(bounced)->start == 0 && s5.clip(bounced)->length == 3 * 96,
         "bounced audio on the next empty track");
}

void testProjectRoundTrip() {
  Song s = makeEmptySong();
  s.mixer[2].input = 3;
  s.mixer[2].inputStereo = false;
  s.mixer[2].monitor = static_cast<uint8_t>(InputMonitor::Off);
  s.mixer[2].armed = true;
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  c.takeGroup = 777;
  c.take = 2;
  s.playlist.push_back(c);
  Song back;
  std::string err;
  expect(songFromJson(songToJson(s), back, &err), "project parses");
  expect(back.mixer[2].input == 3 && !back.mixer[2].inputStereo && back.mixer[2].monitor == 2 && back.mixer[2].armed,
         "mixer input, monitoring and arm saved");
  expect(!back.playlist.empty() && back.playlist.back().takeGroup == 777 && back.playlist.back().take == 2, "take group saved");
  expect(back.mixer[1].input == -1 && back.mixer[1].inputStereo, "defaults stay (no input)");
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("recording");
  testMonitoring();
  testInputQueue();
  testPlacement();
  testPunch();
  testLoopTakes();
  testEngineFlags();
  testClipWorkflow();
  testProjectRoundTrip();
  return finish();
}
