// Gridline - J-186: play lights. The channel activity the engine publishes
// (note-start counter, held notes, the last block's output peak) for a
// synth, a sampler, an audio clip and a muted channel; the channel rack's
// light model; Kerf's sounding regions (classic markers and the region
// engine: which region, polyphony, release, a region shorter than a frame).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "app/PlayLights.h"
#include "core/AudioBuffer.h"
#include "core/PluginParams.h"
#include "core/WavWriter.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/NoteActivity.h"
#include "engine/Program.h"
#include "engine/RegionActivity.h"
#include "engine/Sample.h"
#include "engine/SlicexEngine.h"
#include "../src/core/Platform.h"

using namespace gl;

namespace {

constexpr double kSr = 44100;

// A stereo WAV: four regions of a sine at 220 / 330 / 440 / 550 Hz, 0.25 s each.
std::string writeTones() {
  const std::string path = std::filesystem::absolute("j186/tones.wav").string();
  const size_t frames = static_cast<size_t>(kSr);
  AudioBuffer b(2, frames);
  for (size_t i = 0; i < frames; ++i) {
    const double hz = 220.0 + 110.0 * static_cast<double>(i / (frames / 4));
    b.at(0, i) = b.at(1, i) = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * hz * static_cast<double>(i) / kSr));
  }
  writeWav(path, b, static_cast<int>(kSr), 16);
  sampleCache().clear();
  return path;
}

// ------------------------------------------------------------------ note-start counter
void testCounter() {
  auto na = std::make_unique<NoteActivity>();
  expect(na->starts() == 0 && !na->anyHeld(), "nothing started");
  na->on(1, 60, 0.5f);
  expect(na->starts() == 1 && na->anyHeld() && std::fabs(na->lastVelocity() - 0.5f) < 0.01f, "a note start: counted, held, its velocity");
  na->on(2, 300, 1.0f);  // out of the key range: no key lights, the channel still played
  expect(na->starts() == 2 && std::fabs(na->lastVelocity() - 1.0f) < 0.01f, "out-of-range pitch still counts for the channel");
  na->off(1);
  expect(!na->anyHeld(), "released");
  na->clear();
  expect(na->starts() == 2, "panic keeps the counter");
}

// ------------------------------------------------------------------ the rack light model
void testLightModel() {
  using namespace playlight;
  expect(levelLight(0) == 0 && levelLight(1e-4f) == 0, "silence and -80 dB stay dark");
  expect(levelLight(0.5f) == 1.0f && levelLight(0.01f) > kLevelMin && levelLight(0.01f) < 0.7f, "loud is full, -40 dB dim");
  ChannelLight L;
  L.update(0, 0, false, 0, false, 0.016f);
  expect(!L.lit(), "idle: dark");
  L.update(1, 1.0f, true, 0, false, 0.016f);
  expect(L.alpha() >= 0.99f, "a note starts: full at once");
  for (int i = 0; i < 30; ++i) L.update(1, 1.0f, true, 0, false, 0.016f);
  expect(L.value >= kHeldFloor - 1e-6f && L.lit(), "held (silent synth): stays lit");
  L.update(1, 1.0f, false, 0.3f, false, 0.016f);
  expect(L.value > 0.9f, "sounding loud: bright");
  L.update(1, 1.0f, false, 0.0f, false, 0.016f);
  expect(L.value > 0.85f && L.value < 1.0f, "sound ends: fades, not off at once");
  for (int i = 0; i < 20; ++i) L.update(1, 1.0f, false, 0.0f, false, 0.016f);
  expect(!L.lit(), "dark again after the release (0.22 s)");
  L.update(2, 0.1f, false, 0.0f, false, 0.016f);
  const float soft = L.value;
  expect(soft > 0.3f && soft < 0.5f, "a short soft note between two frames still blinks (velocity-scaled)");
  L.update(3, 1.0f, false, 0.0f, false, 0.016f, false);
  L.update(3, 1.0f, false, 0.0f, false, 0.016f, false);
  expect(!L.lit(), "animations off: off at once");
  L.update(4, 1.0f, true, 0.5f, true, 0.016f);
  expect(!L.lit(), "muted: dark whatever plays");
  L.reprime();
  L.update(9, 1.0f, false, 0.0f, false, 0.016f);
  expect(!L.lit(), "a row scrolled back into view does not flash for old notes");
}

// ------------------------------------------------------------------ engine: channel signals
struct Rig {
  Song s;
  ProgramBuilder builder;
  std::unique_ptr<AudioEngine> eng;
  std::vector<float> L = std::vector<float>(256), R = std::vector<float>(256);
  void start() {
    builder.setSampleRate(kSr);
    eng = std::make_unique<AudioEngine>(kSr);
    eng->setProgram(builder.build(s));
  }
  void run(int frames) {
    for (int d = 0; d < frames; d += 256) eng->process(L.data(), R.data(), 256);
  }
};

ObjectId addChannel(Song& s, const std::string& type, const std::string& data = "") {
  Channel c;
  c.id = s.allocateId();
  c.name = type;
  c.plugin = defaultPluginState(type);
  c.plugin.data = data;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  return c.id;
}

void testEngineChannels(const std::string& tones) {
  Rig r;
  r.s = makeEmptySong();
  const ObjectId synth = addChannel(r.s, "gl.synth");
  const ObjectId smp = addChannel(r.s, "gl.sampler", tones);
  const ObjectId muted = addChannel(r.s, "gl.synth");
  r.s.channels.back().muted = true;
  const ObjectId idle = addChannel(r.s, "gl.synth");
  r.start();
  auto rtSynth = r.builder.channelRuntime(synth), rtSmp = r.builder.channelRuntime(smp), rtMuted = r.builder.channelRuntime(muted),
       rtIdle = r.builder.channelRuntime(idle);
  expect(rtSynth && rtSmp && rtMuted && rtIdle, "channel runtimes");
  if (!rtSynth || !rtSmp || !rtMuted || !rtIdle) return;
  r.run(1024);
  expect(rtSynth->peak.load() == 0 && rtSynth->keys.starts() == 0, "before any note: silent, nothing started");
  r.eng->noteOn(synth, 60, 0.8f);
  r.eng->noteOn(smp, 60, 0.8f);
  r.eng->noteOn(muted, 60, 0.8f);
  r.run(2048);
  expect(rtSynth->keys.starts() == 1 && rtSynth->keys.anyHeld() && rtSynth->peak.load() > 0.01f, "synth: started, held, sounding");
  expect(rtSmp->keys.starts() == 1 && rtSmp->peak.load() > 0.01f, "sampler: started and sounding");
  expect(rtMuted->keys.starts() == 1 && rtMuted->peak.load() == 0.0f, "muted channel: the note counts but its output is silent");
  expect(rtIdle->keys.starts() == 0 && rtIdle->peak.load() == 0.0f, "another channel stays idle");
  // The peak is the last block's, readable by any number of readers (the mute LED's level is apart).
  const float p1 = rtSynth->peak.load(), p2 = rtSynth->peak.load();
  expect(p1 == p2 && rtSynth->level.exchange(0) > 0, "peak is not consumed by a read");
  // The UI model fed by these signals: lit rows, a dark muted row.
  playlight::ChannelLight ls, lm;
  ls.update(0, 0, false, 0, false, 0.016f);
  lm.update(0, 0, false, 0, true, 0.016f);
  ls.update(rtSynth->keys.starts(), rtSynth->keys.lastVelocity(), rtSynth->keys.anyHeld(), rtSynth->peak.load(), false, 0.016f);
  lm.update(rtMuted->keys.starts(), rtMuted->keys.lastVelocity(), rtMuted->keys.anyHeld(), rtMuted->peak.load(), true, 0.016f);
  expect(ls.lit() && !lm.lit(), "rack: the playing row lights, the muted row stays dark");
  r.eng->noteOff(synth, 60);
  r.eng->noteOff(smp, 60);
  r.run(static_cast<int>(kSr * 2));
  expect(!rtSynth->keys.anyHeld() && rtSynth->peak.load() < 1e-4f, "synth: released and quiet after its release");
  // Pattern steps play through the same path.
  r.s.patterns[0].steps[synth] = std::vector<uint8_t>(16, 0);
  r.s.patterns[0].steps[synth][0] = 100;
  r.eng->setProgram(r.builder.build(r.s));
  r.eng->setMode(PlayMode::Pattern);
  r.eng->setPattern(r.s.patterns[0].id);
  const uint32_t before = rtSynth->keys.starts();
  r.eng->play();
  r.run(2048);
  expect(rtSynth->keys.starts() == before + 1 && rtSynth->peak.load() > 0.001f, "a pattern step starts a note: counted, sounding");
  r.eng->stop();
  r.run(1024);
}

void testAudioClip(const std::string& tones) {
  Rig r;
  r.s = makeEmptySong();
  const ObjectId ch = addChannel(r.s, "gl.sampler", tones);
  Clip c;
  c.id = r.s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch;
  c.start = 0;
  c.length = r.s.ppq;  // one beat
  r.s.playlist.push_back(c);
  r.start();
  auto rt = r.builder.channelRuntime(ch);
  expect(rt != nullptr, "clip channel runtime");
  if (!rt) return;
  r.eng->setMode(PlayMode::Song);
  r.eng->play();
  r.run(4096);
  expect(rt->peak.load() > 0.01f && rt->keys.starts() == 0, "audio clip: the channel sounds without a note (light follows its level)");
  playlight::ChannelLight l;
  l.update(0, 0, false, 0, false, 0.016f);
  l.update(rt->keys.starts(), 0, false, rt->peak.load(), false, 0.016f);
  expect(l.lit(), "audio clip: the row lights");
  r.eng->stop();
  r.run(4096);
  expect(rt->peak.load() < 1e-4f, "stopped: the clip's channel is quiet");
}

// ------------------------------------------------------------------ Kerf: sounding regions
std::vector<int> sounding(const RegionActivity& ra) {
  std::vector<int> v;
  for (int i = 0; i < RegionActivity::kSlots; ++i)
    if (ra.region(i) >= 0) v.push_back(ra.region(i));
  std::sort(v.begin(), v.end());
  return v;
}

void runGen(Generator& g, const PluginState& st, const ChannelAux& aux, int frames) {
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = 120;
  std::vector<float> L(256), R(256);
  for (int d = 0; d < frames; d += 256) {
    std::fill(L.begin(), L.end(), 0.0f), std::fill(R.begin(), R.end(), 0.0f);
    g.process(st, aux, ctx, L.data(), R.data(), 256);
  }
}

void checkKerf(const PluginState& st, const char* what) {
  ChannelAux aux;
  prepareChannelAux(st, aux);
  auto gen = createGenerator("gl.slicer");
  const RegionActivity* ra = gen ? gen->regionActivity() : nullptr;
  expect(ra != nullptr, (std::string(what) + ": Kerf publishes its regions").c_str());
  if (!gen || !ra) return;
  runGen(*gen, st, aux, 512);
  expect(sounding(*ra).empty(), (std::string(what) + ": idle, nothing sounds").c_str());
  gen->noteOn(1, 62, 0.9f, 0);  // region 3 (base C5 = 60)
  runGen(*gen, st, aux, 1024);
  std::vector<int> s = sounding(*ra);
  expect(s.size() == 1 && s[0] == 2, (std::string(what) + ": key 62 plays region 3").c_str());
  int slot = -1;
  for (int i = 0; i < RegionActivity::kSlots; ++i)
    if (ra->region(i) == 2) slot = i;
  const int64_t f = ra->frame(slot);
  expect(slot >= 0 && ra->held(slot) && f >= static_cast<int64_t>(kSr / 2) && f < static_cast<int64_t>(kSr * 3 / 4) && ra->level(slot) > 0,
         (std::string(what) + ": its read position is inside region 3, held, with a level").c_str());
  gen->noteOn(2, 60, 0.9f, 0);  // polyphony: region 1 as well
  runGen(*gen, st, aux, 512);
  s = sounding(*ra);
  expect(s.size() == 2 && s[0] == 0 && s[1] == 2, (std::string(what) + ": two regions sound at once").c_str());
  // The UI side: both lit; region 2 dark.
  playlight::RegionLights lights;
  lights.update(*ra, 4, 0.016f);
  expect(lights.lit(0) && lights.lit(2) && !lights.lit(1) && lights.headCount == 2, (std::string(what) + ": the editor lights both, with two playheads").c_str());
  gen->noteOff(1, 0);
  gen->noteOff(2, 0);
  runGen(*gen, st, aux, static_cast<int>(kSr / 2));
  expect(sounding(*ra).empty(), (std::string(what) + ": released: nothing sounds").c_str());
  for (int i = 0; i < 20; ++i) lights.update(*ra, 4, 0.016f);
  expect(!lights.lit(0) && !lights.lit(2), (std::string(what) + ": the lights fade out after the release").c_str());
  // A region that starts and ends between two frames still flashes.
  gen->noteOn(3, 63, 0.9f, 0);
  gen->noteOff(3, 10);
  runGen(*gen, st, aux, static_cast<int>(kSr / 4));
  expect(sounding(*ra).empty(), (std::string(what) + ": the short note is over").c_str());
  lights.update(*ra, 4, 0.016f);
  expect(lights.lit(3) && !lights.lit(0), (std::string(what) + ": a region over before the frame still flashes").c_str());
  gen->kill();
  runGen(*gen, st, aux, 256);
  expect(sounding(*ra).empty(), (std::string(what) + ": kill clears").c_str());
}

void testKerf(const std::string& tones) {
  PluginState st = defaultPluginState("gl.slicer");
  st.data = tones;
  st.params[static_cast<size_t>(pp::SlEngine)] = 0;
  st.params[static_cast<size_t>(pp::SlMode)] = pp::SlModeManual;
  st.params[static_cast<size_t>(pp::SlRelease)] = 0.01f;
  for (int k = 0; k < pp::kMaxSlices; ++k) st.params[static_cast<size_t>(pp::slMarker(k))] = k < 4 ? static_cast<float>(k) / 4.0f : -1.0f;
  checkKerf(st, "classic");

  slicex::Model md = slicex::defaultModel();
  md.decks[0].file = tones;
  md.decks[0].regions.clear();
  for (int k = 0; k < 4; ++k) {
    slicex::Region rg;
    rg.start = static_cast<int64_t>(k) * static_cast<int64_t>(kSr / 4);
    md.decks[0].regions.push_back(rg);
  }
  PluginState sx = defaultPluginState("gl.slicer");
  sx.params[static_cast<size_t>(pp::SlEngine)] = 1;
  sx.data = slicex::makeData(md);
  checkKerf(sx, "region engine");
}

}  // namespace

int main() {
  std::filesystem::create_directories("j186");
  gl::platform::setEnv("GRIDLINE_CACHE_DIR", std::filesystem::absolute("j186/cache").string().c_str());
  const std::string tones = writeTones();
  testCounter();
  testLightModel();
  testEngineChannels(tones);
  testAudioClip(tones);
  testKerf(tones);
  return finish();
}
