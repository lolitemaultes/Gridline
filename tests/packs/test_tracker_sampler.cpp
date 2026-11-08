// Gridline - K-120 tests: the Tracker Sampler played as an ordinary sampler.
//   * a fresh instance is silent (K-129); a built-in sound plays in tune and releases; every built-in sound and
//     preset sounds, stays finite and is the same for any block size;
//   * states saved before the built-in sounds keep playing their own samples only (and nothing
//     when they have none);
//   * the instrument edits: adding samples (16 at most), removing (keys move to a neighbour),
//     reordering, the key map, envelope points with sustain and loop indices, the text round trip
//     (old texts read back unchanged);
//   * sample start and end, stereo files, the note's fine pitch, pan and the channel's pitch bend.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/TrackerData.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Sample.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/tracker/tracker.h"
#include "../Check.h"

using namespace gl;
namespace trk = gl::tracker;
namespace tp = gl::packs::trackerpack;
namespace fs = std::filesystem;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

struct Out {
  std::vector<float> L, R;
};

// Plays notes through the generator: `on` at frame 0 (pitch, props), off at `offAt` (-1 = never).
Out play(const PluginState& st, double pitch, int total, int block, int offAt = -1, NoteProps np = {}, int bendAt = -1, int bendValue = 8192) {
  Out o;
  o.L.assign(static_cast<size_t>(total), 0.0f);
  o.R.assign(static_cast<size_t>(total), 0.0f);
  auto g = packs::createGenerator(st.type);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 125;
  ctx.ppq = 96;
  ctx.playing = false;
  double tick = 0;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    if (pos == 0) g->noteOn(1, pitch, 100.0f / 127.0f, 0, np);
    if (offAt >= pos && offAt < pos + n) g->noteOff(1, offAt - pos);
    if (bendAt >= pos && bendAt < pos + n) g->midi(0xE0, static_cast<uint8_t>(bendValue & 0x7F), static_cast<uint8_t>((bendValue >> 7) & 0x7F), bendAt - pos);
    ctx.tick = tick;
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
    tick += n * ctx.tempo * ctx.ppq / (60.0 * ctx.sampleRate);
  }
  return o;
}

double peak(const std::vector<float>& v, size_t a, size_t b) {
  double m = 0;
  for (size_t i = a; i < std::min(b, v.size()); ++i) m = std::max(m, static_cast<double>(std::abs(v[i])));
  return m;
}
bool finite(const Out& o) {
  for (size_t i = 0; i < o.L.size(); ++i)
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) return false;
  return true;
}
// The strongest period between lo and hi frames (autocorrelation), as a frequency.
double pitchOf(const std::vector<float>& v, size_t from, size_t len, double rate = 44100) {
  int best = 0;
  double bestC = -1e30;
  for (int lag = 40; lag < 1200; ++lag) {
    double c = 0;
    for (size_t i = from; i < from + len && i + static_cast<size_t>(lag) < v.size(); ++i) c += static_cast<double>(v[i]) * v[i + static_cast<size_t>(lag)];
    if (c > bestC) {
      bestC = c;
      best = lag;
    }
  }
  // Refine between neighbours (parabola).
  auto corr = [&](int lag) {
    double c = 0;
    for (size_t i = from; i < from + len && i + static_cast<size_t>(lag) < v.size(); ++i) c += static_cast<double>(v[i]) * v[i + static_cast<size_t>(lag)];
    return c;
  };
  const double a = corr(best - 1), b = corr(best), c = corr(best + 1);
  const double den = a - 2 * b + c;
  const double shift = den != 0 ? 0.5 * (a - c) / den : 0.0;
  return rate / (best + shift);
}

std::string tempDir(const std::string& tag) {
  const fs::path d = fs::temp_directory_path() / ("gridline-k120-" + tag);
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d.u8string();
}

std::string writeFile(const std::string& dir, const std::string& name, const std::vector<float>& left, const std::vector<float>* right, int rate) {
  AudioBuffer b(right ? 2 : 1, left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    b.data[i] = left[i];
    if (right) b.data[left.size() + i] = (*right)[i];
  }
  const std::string p = (fs::u8path(dir) / name).u8string();
  writeWav(p, b, rate, 24);
  return p;
}

PluginState withData(const std::string& data, int sound = 0) {
  PluginState st = defaultPluginState(trk::kInstrumentType);
  st.params[trk::PSound] = static_cast<float>(sound);
  st.params[trk::PInterp] = trk::InterpNone;
  st.params[trk::PRamp] = 0;
  st.data = data;
  return st;
}

void testFreshInstance() {
  // K-129: a new instance is silent; choosing a built-in sound makes it play.
  PluginState fresh = defaultPluginState(trk::kInstrumentType);
  expect(fresh.params.size() == trk::PCount && fresh.params[trk::PSound] == 0.0f && fresh.data.empty(), "fresh: no built-in sound, no data");
  expect(peak(play(fresh, 60, 8000, 256).L, 0, 8000) == 0.0, "fresh: silent");
  fresh.params[trk::PSound] = 1;
  const Out o = play(fresh, 60, 66150, 256, 22050);
  const double held = peak(o.L, 4000, 20000), after = peak(o.L, 60000, 66150);
  expect(held > 0.2 && held < 1.0, "fresh: a played note sounds");
  expect(after < 1e-6, "fresh: let go, the note fades to silence");
  const double hz = pitchOf(o.L, 8000, 4000);
  expect(std::abs(hz - 261.63) < 2.0, "fresh: middle C plays middle C (" + std::to_string(hz) + " Hz)");
  const Out o2 = play(fresh, 72, 20000, 256);
  const double hz2 = pitchOf(o2.L, 8000, 4000);
  expect(std::abs(hz2 / hz - 2.0) < 0.02, "fresh: an octave up doubles the pitch");
  // Every built-in sound: sounds, finite, the same for any block size.
  for (int s = 1; s <= tp::kSoundCount; ++s) {
    PluginState st = fresh;
    st.params[trk::PSound] = static_cast<float>(s);
    const Out a = play(st, 48, 30000, 512, 20000);
    const Out b = play(st, 48, 30000, 37, 20000);
    double d = 0;
    for (size_t i = 0; i < a.L.size(); ++i) d = std::max(d, static_cast<double>(std::abs(a.L[i] - b.L[i])) + std::abs(a.R[i] - b.R[i]));
    const std::string nm = tp::soundName(s);
    expect(finite(a) && peak(a.L, 0, 30000) > 0.1, "built-in " + nm + ": sounds");
    expect(d < 1e-6, "built-in " + nm + ": the same for any block size");
    const Out hi = play(st, 127, 8000, 256, 4000), lo = play(st, 0, 8000, 256, 4000);
    expect(finite(hi) && finite(lo), "built-in " + nm + ": highest and lowest keys stay finite");
  }
  // Every factory preset sounds.
  const PluginInfo* info = findPlugin(trk::kInstrumentType);
  expect(info && info->presets.size() >= 2 && info->presets.size() <= 6, "presets: 2 to 6 factory presets");
  if (info)
    for (size_t i = 0; i < info->presets.size(); ++i) {
      const PluginState st = presetState(*info, i);
      const Out a = play(st, 60, 20000, 256, 12000);
      expect(finite(a) && peak(a.L, 0, 20000) > 0.05, "preset " + info->presets[i].name + ": sounds");
    }
  // No sound chosen and no sample: silent.
  PluginState none = fresh;
  none.params[trk::PSound] = 0;
  expect(peak(play(none, 60, 8000, 256).L, 0, 8000) == 0.0, "no built-in sound and no sample: silent");
}

void testOldStates() {
  // A state saved before the built-in sounds (6 parameters) keeps its own samples only.
  PluginState old = defaultPluginState(trk::kInstrumentType);
  old.params.resize(6);
  old.data = "\n#gl-tracker 1\nname Empty\nins 0 0 0 0 0\nmap " + std::string(96, '0') + "\nenv v 0 0 0 0 0\nenv p 0 0 0 0 0\n";
  const PluginState before = old;
  normalizePluginState(old);
  expect(old.params.size() == trk::PCount && old.params[trk::PSound] == 0.0f, "old state: no built-in sound");
  expect(old.data == before.data, "old state: its data stays as it was");
  for (int i = 0; i < 6; ++i) expect(old.params[static_cast<size_t>(i)] == before.params[static_cast<size_t>(i)], "old state: parameter " + std::to_string(i) + " unchanged");
  expect(peak(play(old, 60, 8000, 256).L, 0, 8000) == 0.0, "old empty instrument: still silent");
  // An empty state saved before (no data at all) stays silent too.
  PluginState bare = defaultPluginState(trk::kInstrumentType);
  bare.params.resize(6);
  normalizePluginState(bare);
  expect(peak(play(bare, 60, 8000, 256).L, 0, 8000) == 0.0, "old fresh instance: still silent");
  // An imported instrument's text reads and writes back unchanged.
  const std::string text =
      "a.wav\nfile b.wav\n#gl-tracker 1\nname Lead\nins 12 1 20 4 8\nmap " + std::string(48, '0') + std::string(48, '1') +
      "\nenv v 7 3 1 0 2 0:64 4:40 10:0\nenv p 1 2 0 0 0 0:32 8:20\nsmp 0 0 2000 100 900 1 52 -16 128 3 Bass\nsmp 1 1 500 0 0 0 64 0 200 -2 Hat\n";
  const tp::Edit e = tp::readEdit(text, 0);
  expect(e.body && !e.cuts.any() && tp::writeEdit(e) == text, "instrument text: an imported one writes back byte for byte");
}

void testEdits() {
  tp::Edit e = tp::readEdit("", 0);
  expect(!e.body && e.text.def.vol.on() && (e.text.def.vol.flags & trk::kEnvSustain), "new instrument: a volume envelope that holds and fades");
  const int a = tp::addSample(e, "/x/kick.wav", 1000, 44100, "kick");
  expect(a == 0 && e.text.files.size() == 1 && e.text.fileOf[0] == 0, "add: the first sample takes slot 1");
  bool all0 = true;
  for (uint8_t k : e.text.def.keymap) all0 = all0 && k == 0;
  expect(all0, "add: the first sample takes every key");
  expect(tp::rootNote(e.text.def.samples[0], 44100) == 49, "add: a file plays at its own rate on middle C");
  const int b = tp::addSample(e, "/x/snare.wav", 500, 22050, "snare");
  expect(b == 1 && e.text.def.keymap[60] == 0, "add: later samples keep the key map");
  tp::mapKeys(e, 70, 60, 1);
  expect(e.text.def.keymap[59] == 0 && e.text.def.keymap[60] == 1 && e.text.def.keymap[70] == 1 && e.text.def.keymap[71] == 0, "key map: a dragged range");
  for (int k = 2; k < trk::kMaxSamples; ++k) tp::addSample(e, "/x/s" + std::to_string(k) + ".wav", 100, 44100, "s");
  expect(tp::addSample(e, "/x/extra.wav", 100, 44100, "x") == -1, "add: 16 samples at most");
  tp::swapSamples(e, 0, 1);
  expect(e.text.sampleNames[0] == "snare" && e.text.def.keymap[65] == 0 && e.text.def.keymap[10] == 1 &&
             e.text.files[static_cast<size_t>(e.text.fileOf[0])] == "/x/snare.wav",
         "reorder: names, files and keys move together");
  tp::removeSample(e, 0);
  expect(!e.text.def.samples[0].present() && e.text.fileOf[0] < 0 && e.text.def.keymap[65] == 1, "remove: its keys go to the sample beside them");
  // Cuts and names survive the text round trip.
  e.cuts.start[1] = 100;
  e.cuts.end[1] = 900;
  e.text.sampleNames[1] = "two\nlines";
  const std::string txt = tp::writeEdit(e);
  const tp::Edit r = tp::readEdit(txt, 0);
  expect(r.cuts.start[1] == 100 && r.cuts.end[1] == 900 && r.text.sampleNames[1] == "two lines" && r.text.def.keymap[65] == 1, "text round trip: cuts, names, key map");
  // Envelope points keep the sustain and loop on their points.
  trk::Envelope env;
  tp::insertEnvPoint(env, 0, 64);
  tp::insertEnvPoint(env, 20, 0);
  env.sustain = 1;
  env.loopStart = 0;
  env.loopEnd = 1;
  expect(tp::insertEnvPoint(env, 10, 32) == 1 && env.sustain == 2 && env.loopEnd == 2 && env.loopStart == 0, "envelope: an inserted point moves the marks after it");
  expect(tp::insertEnvPoint(env, 10, 5) == -1, "envelope: two points never share a tick");
  tp::deleteEnvPoint(env, 1);
  expect(env.count == 2 && env.sustain == 1 && env.x[1] == 20, "envelope: a deleted point moves them back");
  tp::deleteEnvPoint(env, 1);
  expect(env.count == 2, "envelope: two points at least");
  for (int i = 0; i < 20; ++i) tp::insertEnvPoint(env, 1 + i, 10);
  expect(env.count == trk::kEnvPoints, "envelope: 12 points at most");
}

void testPlayback() {
  const std::string dir = tempDir("play");
  // A ramp file at 44.1 kHz.
  std::vector<float> ramp(20000);
  for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>((static_cast<int>(i) % 2000) - 1000) / 1200.0f;
  const std::string rampPath = writeFile(dir, "ramp.wav", ramp, nullptr, 44100);
  SamplePtr sp = sampleCache().get(rampPath);
  expect(sp && sp->frames() == ramp.size(), "playback: the test file reads back");
  if (!sp) return;
  tp::Edit e = tp::readEdit("", 0);
  e.text.def.vol.flags = 0;  // plain gate: the sample as it is
  tp::addSample(e, rampPath, static_cast<int32_t>(sp->frames()), sp->sampleRate, "ramp");
  e.cuts.start[0] = 1000;
  e.cuts.end[0] = 5000;
  const PluginState st = withData(tp::writeEdit(e));
  const Out o = play(st, 60, 8000, 256);
  const float g = static_cast<float>(std::lround(65536.0 * std::sqrt(0.5)) / 65536.0) * (100.0f / 127.0f) * (127.0f / 100.0f);
  // (the tuning that plays a file at its own rate is exact to 1/256 semitone: allow one frame)
  double err = 0;
  for (int i = 0; i < 900; ++i) err = std::max(err, static_cast<double>(std::abs(o.L[static_cast<size_t>(i)] - sp->left[static_cast<size_t>(1000 + i)] * g)));
  expect(std::abs(o.L[0] - sp->left[1000] * g) < 1e-5 && err < 2.5e-3, "sample start: plays from the start mark at its own rate");
  expect(peak(o.L, 3000, 3990) > 0.1 && peak(o.L, 4050, 8000) == 0.0, "sample end: stops at the end mark");
  // A loop inside the cut, in file frames.
  e.text.def.samples[0].loop = trk::kLoopForward;
  e.text.def.samples[0].loopStart = 3000;
  e.text.def.samples[0].loopLength = 1000;
  const Out lp = play(withData(tp::writeEdit(e)), 60, 12000, 256);
  // Past the end mark the loop (file frames 3000..3999, one rising stretch of the ramp) repeats.
  const double lo = std::min(sp->left[3000], sp->left[3999]) * g - 1e-4, hi = std::max(sp->left[3000], sp->left[3999]) * g + 1e-4;
  bool inLoop = true;
  for (int i = 5000; i < 12000; ++i) inLoop = inLoop && lp.L[static_cast<size_t>(i)] >= lo && lp.L[static_cast<size_t>(i)] <= hi;
  expect(inLoop && peak(lp.L, 5000, 12000) > 0.3, "loop: in file frames, inside the cut");

  // A stereo file plays in stereo.
  std::vector<float> l(8000, 0.5f), r(8000, -0.25f);
  const std::string stPath = writeFile(dir, "stereo.wav", l, &r, 44100);
  SamplePtr ss = sampleCache().get(stPath);
  tp::Edit es = tp::readEdit("", 0);
  es.text.def.vol.flags = 0;
  tp::addSample(es, stPath, static_cast<int32_t>(ss ? ss->frames() : 0), ss ? ss->sampleRate : 44100, "stereo");
  const Out so = play(withData(tp::writeEdit(es)), 60, 4000, 256);
  expect(so.L[2000] > 0.3f && so.R[2000] < -0.1f, "stereo file: left and right apart");

  // Fine pitch, note pan and pitch bend on a played note.
  const PluginState pure = withData(tp::writeEdit(es), 0);
  PluginState tone = defaultPluginState(trk::kInstrumentType);
  tone.params[trk::PSound] = 4;  // a built-in sound without vibrato
  const Out base = play(tone, 60, 20000, 256);
  NoteProps fine;
  fine.detune = 0.5f;
  const Out up = play(tone, 60.5, 20000, 256, -1, fine);
  const double ratio = pitchOf(up.L, 8000, 4000) / pitchOf(base.L, 8000, 4000);
  expect(std::abs(ratio - std::exp2(0.5 / 12.0)) < 0.004, "fine pitch: half a semitone up (" + std::to_string(ratio) + ")");
  const Out bent = play(tone, 60, 20000, 256, -1, {}, 100, 16383);
  const double bratio = pitchOf(bent.L, 8000, 4000) / pitchOf(base.L, 8000, 4000);
  expect(std::abs(bratio - std::exp2(2.0 * 8191.0 / 8192.0 / 12.0)) < 0.01, "pitch bend: up to two semitones (" + std::to_string(bratio) + ")");
  NoteProps left;
  left.pan = -1.0f;
  const Out pl = play(pure, 60, 4000, 256, -1, left);
  expect(pl.L[2000] > 0.3f && std::abs(pl.R[2000]) < 0.02f, "note pan: a note panned left plays on the left");
  std::error_code ec;
  fs::remove_all(fs::u8path(dir), ec);
}

}  // namespace

int main() {
  testFreshInstance();
  testOldStates();
  testEdits();
  testPlayback();
  return finish();
}
