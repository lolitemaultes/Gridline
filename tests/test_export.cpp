// Gridline tests — J-20 export & interop: render scopes (sample-exact), loop
// counts, tail modes, dither statistics, sample rate conversion, stems and
// playlist-track splits, WAV metadata, external encoders, MIDI import/export
// (SMF 0/1, round trip), zipped projects / data files / bones (path
// rewriting), missing-sample relinking.
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Automation.h"
#include "core/MidiFile.h"
#include "core/MidiImport.h"
#include "core/Params.h"
#include "core/PatternAutomation.h"
#include "core/Project.h"
#include "core/ProjectInterop.h"
#include "core/Zip.h"
#include "engine/Export.h"
#include "engine/ExportJob.h"
#include "engine/Render.h"
#include "engine/Resample.h"
#include "engine/Sample.h"
#include "packs/edison/edison.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {

std::string scratch;

std::string writeTestWav(const std::string& name, const std::vector<float>& mono, int rate = 44100) {
  AudioBuffer b(2, mono.size());
  std::copy(mono.begin(), mono.end(), b.plane(0));
  std::copy(mono.begin(), mono.end(), b.plane(1));
  const std::string p = (fs::path(scratch) / name).string();
  fs::create_directories(fs::path(p).parent_path());
  writeWavFile(p, b, rate, 32);
  return p;
}

// One-sample click (then silence).
std::string impulseWav() {
  static std::string p;
  if (p.empty()) {
    std::vector<float> v(2000, 0.0f);
    v[0] = 1.0f;
    p = writeTestWav("impulse.wav", v);
  }
  return p;
}

ObjectId addSampler(Song& s, const std::string& name, const std::string& path, ObjectId mixer = kNoId) {
  Channel c;
  c.id = s.allocateId();
  c.name = name;
  c.plugin = defaultPluginState("gl.sampler");
  c.plugin.data = path;
  c.mixerTrack = mixer == kNoId ? s.masterId() : mixer;
  s.channels.push_back(c);
  return c.id;
}

void addNote(Song& s, ObjectId pat, ObjectId ch, Tick start, Tick len = 24, int pitch = 60) {
  s.pattern(pat)->notes[ch].push_back(Note(start, len, pitch, 100));
}

ObjectId addClip(Song& s, ObjectId pat, Tick start, int track = 0, Tick len = 0) {
  Clip c;
  c.id = s.allocateId();
  c.pattern = pat;
  c.track = track;
  c.start = start;
  c.length = len;
  s.playlist.push_back(c);
  return c.id;
}

// Song at 120 BPM / 96 PPQ: 1 beat = 0.5 s = 22050 frames.
Song baseSong() {
  Song s = makeEmptySong();
  s.tempo = 120.0;
  s.ppq = 96;
  return s;
}

std::vector<int64_t> onsets(const AudioBuffer& b, float thr = 0.005f) {
  std::vector<int64_t> o;
  bool on = false;
  for (size_t i = 0; i < b.frames; ++i) {
    const bool loud = std::fabs(b.at(0, i)) > thr;
    if (loud && !on) o.push_back(static_cast<int64_t>(i));
    on = loud;
  }
  return o;
}

double rms(const float* x, size_t n) {
  double s = 0;
  for (size_t i = 0; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
  return n ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}

float maxDiff(const AudioBuffer& a, const AudioBuffer& b) {
  if (a.frames != b.frames || a.channels != b.channels) return 1e9f;
  float d = 0;
  for (size_t i = 0; i < a.data.size(); ++i) d = std::max(d, std::fabs(a.data[i] - b.data[i]));
  return d;
}

std::vector<uint8_t> readBytes(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// ---------------------------------------------------------------- render scope

void testScopes() {
  Song s = baseSong();
  ObjectId pat = s.patterns[0].id;
  s.pattern(pat)->bars = 2;
  ObjectId ch = addSampler(s, "Click", impulseWav());
  addNote(s, pat, ch, 0);
  addNote(s, pat, ch, 96 * 2);      // beat 3
  addNote(s, pat, ch, 96 * 5);      // beat 6
  addClip(s, pat, 0);
  RenderJob job;
  job.sampleRate = 44100;

  // Song scope: 8 beats (2 bars) = 4 s.
  RenderResult r = renderAudio(s, job);
  expect(r.error.empty() && r.audio.frames == 176400 && r.bodyFrames == 176400, "song scope: exactly the song length (2 bars = 176400 frames)");
  auto on = onsets(r.audio);
  if (std::getenv("GL_DEBUG")) {
    for (auto x : on) std::printf("onset %lld\n", (long long)x);
    for (size_t i = 0; i < 8; ++i) std::printf("s[%zu]=%g\n", i, r.audio.at(0, i));
  }
  expect(on.size() == 3 && on[0] == 0 && on[1] == 44100 && on[2] == 110250, "song scope: clicks land on their exact frames");

  // Range scope (time selection) [beat 2, beat 6): 4 beats.
  job.scope = RenderScope::Range;
  job.start = 96 * 2;
  job.end = 96 * 6;
  r = renderAudio(s, job);
  on = onsets(r.audio);
  expect(r.audio.frames == 88200, "range scope: length = range (88200 frames)");
  expect(on.size() == 2 && on[0] == 0 && on[1] == 66150, "range scope: a note at the range start is frame 0; later notes keep their offset");

  // Loop count: the range twice.
  job.loops = 2;
  r = renderAudio(s, job);
  on = onsets(r.audio);
  expect(r.audio.frames == 176400 && on.size() == 4 && on[2] == 88200 && on[3] == 88200 + 66150, "loop count 2: the range repeats sample-exactly");
  job.loops = 1;

  // Odd tempo: frame count follows the engine's own wrap point.
  Song odd = s;
  odd.tempo = 133.0;
  job.start = 7;
  job.end = 96 * 3 + 5;
  r = renderAudio(odd, job);
  expect(r.audio.frames == static_cast<size_t>(scopeFrames(odd, job, 7, 96 * 3 + 5, 44100)) && r.audio.frames > 0,
         "range scope at 133 BPM: frames = scopeFrames (ceil of the exact span)");

  // Pattern scope: the pattern's length.
  job = RenderJob{};
  job.scope = RenderScope::Pattern;
  job.pattern = pat;
  r = renderAudio(s, job);
  expect(r.audio.frames == 176400 && onsets(r.audio).size() == 3, "pattern scope: one pattern length");

  // Empty range → error, nothing rendered.
  job.scope = RenderScope::Range;
  job.start = 50;
  job.end = 50;
  r = renderAudio(s, job);
  expect(!r.error.empty() && r.audio.frames == 0, "empty range is refused");

  // Block size invariance + oversampling keeps the body sample-exact.
  job = RenderJob{};
  job.blockSize = 77;
  RenderResult a = renderAudio(s, job);
  job.blockSize = 512;
  RenderResult b = renderAudio(s, job);
  expect(maxDiff(a.audio, b.audio) == 0.0f, "export render is identical for any block size");
  job.oversample = 2;
  RenderResult o2 = renderAudio(s, job);
  expect(o2.audio.frames == 176400 && o2.bodyFrames == 176400, "2x oversampled render: same frame count at the output rate");

  // Progress + cancel.
  std::atomic<bool> cancel{false};
  double lastP = -1;
  bool mono = true;
  job = RenderJob{};
  job.progress = [&](double p) {
    mono = mono && p >= lastP;
    lastP = p;
  };
  renderAudio(s, job);
  expect(mono && std::fabs(lastP - 1.0) < 1e-9, "progress rises to 1");
  job.cancel = &cancel;
  job.progress = [&](double p) {
    if (p > 0.3) cancel = true;
  };
  r = renderAudio(s, job);
  expect(r.cancelled && r.audio.frames == 0, "cancel stops the render");

  // Render switches: master effects off, polyphony limits lifted.
  Song fx = s;
  fx.mixer[0].slots[0].plugin = defaultPluginState("gl.delay");
  fx.channels[0].polyphony = 1;
  job = RenderJob{};
  job.masterEffects = false;
  job.polyphonyLimits = false;
  Song prepared = prepareRenderSong(fx, job);
  expect(!prepared.mixer[0].slots[0].enabled && prepared.channels[0].polyphony == 0, "render switches: master effects off, max polyphony lifted");
  RenderResult noFx = renderAudio(fx, job);
  job.masterEffects = true;
  RenderResult withFx = renderAudio(fx, job);
  expect(maxDiff(noFx.audio, renderAudio(s, RenderJob{}).audio) < 1e-6f && maxDiff(noFx.audio, withFx.audio) > 1e-3f,
         "disabling master effects removes them from the render");
}

// ---------------------------------------------------------------- render quality

void testInterpolation() {
  std::vector<float> sine(44100);
  for (size_t i = 0; i < sine.size(); ++i) sine[i] = static_cast<float>(0.8 * std::sin(2 * 3.14159265358979 * 440.0 * i / 44100.0));
  const std::string path = writeTestWav("sine440.wav", sine);
  Song s = baseSong();
  ObjectId ch = addSampler(s, "Sine", path);
  addNote(s, s.patterns[0].id, ch, 0, 96 * 2, 67);  // a fifth up: the sample is resampled
  addClip(s, s.patterns[0].id, 0);
  RenderJob job;
  std::vector<double> levels;
  std::vector<AudioBuffer> outs;
  for (RenderInterp q : {RenderInterp::Linear, RenderInterp::Hermite, RenderInterp::Sinc64, RenderInterp::Sinc512}) {
    job.interp = q;
    RenderResult r = renderAudio(s, job);
    levels.push_back(rms(r.audio.plane(0) + 4410, 22050));
    outs.push_back(std::move(r.audio));
  }
  bool similar = true;
  for (double l : levels) similar = similar && l > 0.01 && std::fabs(l - levels[0]) / levels[0] < 0.02;
  expect(similar, "sampler interpolation qualities: same level for every quality");
  expect(maxDiff(outs[0], outs[2]) > 1e-6f && maxDiff(outs[0], outs[2]) < 0.05f, "sinc interpolation differs slightly from linear");
  // A 15 kHz tone a semitone down: linear interpolation dulls it, sinc keeps its level.
  std::vector<float> hiTone(44100);
  for (size_t i = 0; i < hiTone.size(); ++i) hiTone[i] = static_cast<float>(0.8 * std::sin(2 * 3.14159265358979 * 15000.0 * i / 44100.0));
  Song h = baseSong();
  ObjectId hc = addSampler(h, "Hi", writeTestWav("sine15k.wav", hiTone));
  addNote(h, h.patterns[0].id, hc, 0, 96 * 2, 59);
  addClip(h, h.patterns[0].id, 0);
  job.interp = RenderInterp::Linear;
  RenderResult lin = renderAudio(h, job);
  job.interp = RenderInterp::Sinc64;
  RenderResult snc = renderAudio(h, job);
  const double lr = rms(lin.audio.plane(0) + 4410, 22050), sr = rms(snc.audio.plane(0) + 4410, 22050);
  if (std::getenv("GL_DEBUG")) std::printf("15k: linear %g sinc %g\n", lr, sr);
  expect(sr > lr * 1.15, "sinc interpolation keeps high frequencies that linear interpolation dulls");
}

// ---------------------------------------------------------------- tails

void testTails() {
  Song s = baseSong();
  ObjectId pat = s.patterns[0].id;
  ObjectId ch = addSampler(s, "Click", impulseWav());
  addNote(s, pat, ch, 96 * 3);  // last beat of the bar
  addClip(s, pat, 0);
  s.mixer[0].slots[0].plugin = defaultPluginState("gl.delay");  // echoes run past the end
  RenderJob job;
  job.maxTailSeconds = 8;
  job.tail = TailMode::Cut;
  RenderResult cut = renderAudio(s, job);
  job.tail = TailMode::Leave;
  RenderResult leave = renderAudio(s, job);
  job.tail = TailMode::Wrap;
  RenderResult wrap = renderAudio(s, job);
  const size_t body = 88200;
  expect(cut.audio.frames == body, "cut remainder: ends exactly at the song end");
  expect(leave.audio.frames > body + 4410 && leave.bodyFrames == static_cast<int64_t>(body), "leave remainder: the tail rings on past the end");
  bool sameBody = true;
  for (size_t i = 0; i < body && sameBody; ++i) sameBody = cut.audio.at(0, i) == leave.audio.at(0, i);
  expect(sameBody, "leave remainder: the body equals the cut render");
  float tailPeak = 0;
  for (size_t i = body; i < leave.audio.frames; ++i) tailPeak = std::max(tailPeak, std::fabs(leave.audio.at(0, i)));
  expect(tailPeak > 1e-3f, "the tail has sound");
  // Wrap: the tail folds into the start.
  bool folded = wrap.audio.frames == body;
  for (size_t i = 0; i < body && folded; ++i) {
    float want = cut.audio.at(0, i);
    for (size_t k = i + body; k < leave.audio.frames; k += body) want += leave.audio.at(0, k);
    folded = std::fabs(wrap.audio.at(0, i) - want) < 1e-6f;
  }
  expect(folded, "wrap remainder: length = song, tail mixed into the start sample-exactly");
}

// ---------------------------------------------------------------- dither

void testDither() {
  const size_t n = 200000;
  // A constant 0.3 LSB: plain rounding loses it, dither keeps it on average.
  AudioBuffer c(1, n);
  for (size_t i = 0; i < n; ++i) c.at(0, i) = 0.3f / 32768.0f;
  AudioBuffer plain = c, tpdf = c, shaped = c;
  quantizeBuffer(plain, 16, DitherMode::None);
  quantizeBuffer(tpdf, 16, DitherMode::Tpdf);
  quantizeBuffer(shaped, 16, DitherMode::NoiseShaped);
  double mean = 0, var = 0, meanPlain = 0;
  bool exact = true;
  for (size_t i = 0; i < n; ++i) {
    const double q = tpdf.at(0, i) * 32768.0;
    exact = exact && q == std::round(q) && std::fabs(q) <= 32768;
    mean += q;
    meanPlain += plain.at(0, i) * 32768.0;
  }
  mean /= n;
  meanPlain /= n;
  for (size_t i = 0; i < n; ++i) {
    const double e = tpdf.at(0, i) * 32768.0 - 0.3;
    var += e * e;
  }
  var /= n;
  expect(exact, "TPDF dither: output is exact 16-bit steps");
  expect(std::fabs(meanPlain) < 1e-9 && std::fabs(mean - 0.3) < 0.01, "TPDF dither: no bias (mean 0.3 LSB kept; rounding gives 0)");
  expect(std::fabs(var - 0.25) < 0.01, "TPDF dither: total error power = 1/4 LSB^2");
  // Noise shaping: error energy moves to high frequencies.
  AudioBuffer sine(1, n);
  for (size_t i = 0; i < n; ++i) sine.at(0, i) = static_cast<float>(0.25 * std::sin(2 * 3.14159265358979 * 997.0 * i / 44100.0));
  AudioBuffer flat = sine, ns = sine;
  quantizeBuffer(flat, 16, DitherMode::Tpdf, 11);
  quantizeBuffer(ns, 16, DitherMode::NoiseShaped, 11);
  // Low band: energy of 32-sample block sums (below ~700 Hz); high band:
  // energy of first differences (rises towards Nyquist). Both per sample.
  auto bands = [&](const AudioBuffer& q, double& lo, double& hi, double& mean2) {
    lo = hi = mean2 = 0;
    double prev = 0, block = 0;
    for (size_t i = 0; i < n; ++i) {
      const double e = (static_cast<double>(q.at(0, i)) - sine.at(0, i)) * 32768.0;
      block += e;
      if (i % 32 == 31) {
        lo += block * block / 32.0;
        block = 0;
      }
      hi += (e - prev) * (e - prev);
      mean2 += e;
      prev = e;
    }
    lo /= static_cast<double>(n / 32);
    hi /= static_cast<double>(n);
    mean2 /= n;
  };
  double fl, fh, fm, sl, sh, sm;
  bands(flat, fl, fh, fm);
  bands(ns, sl, sh, sm);
  if (std::getenv("GL_DEBUG")) std::printf("flat lo %g hi %g  ns lo %g hi %g\n", fl, fh, sl, sh);
  expect(std::fabs(fl - 0.25) < 0.03 && std::fabs(fh - 0.5) < 0.05, "TPDF error is white (low band = total power, differences = 2x)");
  expect(sh > fh * 2.0, "noise-shaped error: more energy at high frequencies");
  expect(sl < fl * 0.3, "noise shaping lowers the low-frequency noise (> 5 dB)");
  expect(std::fabs(sm) < 0.05 && std::fabs(fm) < 0.05, "dithered error has no DC");
  // 24-bit exactness + WAV write → read back the same integers.
  AudioBuffer b24(2, 1000);
  for (size_t i = 0; i < 1000; ++i) b24.at(0, i) = b24.at(1, i) = static_cast<float>(0.5 * std::sin(i * 0.01));
  quantizeBuffer(b24, 24, DitherMode::Tpdf);
  const std::string wav = (fs::path(scratch) / "d24.wav").string();
  writeWavFile(wav, b24, 44100, 24);
  SamplePtr back = loadSample(wav);
  bool same = back && back->frames() == 1000;
  for (size_t i = 0; same && i < 1000; ++i) same = std::fabs(back->left[i] - b24.at(0, i)) < 1e-7f;
  expect(same, "24-bit WAV holds the dithered values exactly");
}

// ---------------------------------------------------------------- SRC + formats

void testResampleAndFormats() {
  const size_t n = 44100;
  AudioBuffer in(1, n);
  for (size_t i = 0; i < n; ++i) in.at(0, i) = static_cast<float>(std::sin(2 * 3.14159265358979 * 1000.0 * i / 44100.0));
  AudioBuffer up = resampleBuffer(in, 44100, 48000);
  expect(up.frames == 48000, "SRC 44.1 → 48 kHz: frame count follows the ratio");
  int zc = 0;
  for (size_t i = 1000; i < 47000; ++i) zc += (up.at(0, i - 1) < 0) != (up.at(0, i) < 0);
  const double f = zc / 2.0 / (46000.0 / 48000.0);
  expect(std::fabs(f - 1000.0) < 3.0, "SRC keeps the pitch (1 kHz)");
  expect(std::fabs(rms(up.plane(0) + 2000, 44000) - std::sqrt(0.5)) < 0.01, "SRC keeps the level");
  AudioBuffer hi(1, n);  // 15 kHz at 44.1k → 22.05 kHz must remove it (anti-aliasing)
  for (size_t i = 0; i < n; ++i) hi.at(0, i) = static_cast<float>(std::sin(2 * 3.14159265358979 * 15000.0 * i / 44100.0));
  AudioBuffer down = resampleBuffer(hi, 44100, 22050);
  expect(down.frames == 22050 && rms(down.plane(0) + 500, 21000) < 0.003, "SRC downsampling filters content above the new Nyquist (< -50 dB)");

  // Channel modes + normalize.
  AudioBuffer st(2, 4);
  st.at(0, 0) = 0.5f;
  st.at(1, 0) = -0.1f;
  AudioBuffer m = applyChannelMode(st, ChannelMode::MonoMerged);
  AudioBuffer r = applyChannelMode(st, ChannelMode::MonoRight);
  expect(m.channels == 1 && std::fabs(m.at(0, 0) - 0.2f) < 1e-7f && r.at(0, 0) == -0.1f, "channel modes: mono merged / right only");
  AudioBuffer nb = st;
  normalizeBuffer(nb, 0.0);
  expect(std::fabs(nb.peak() - 1.0f) < 1e-6f, "normalize to 0 dBFS");

  // WAV metadata: markers (cue + labels) and tempo (acid chunk).
  AudioBuffer a(2, 4410);
  WavMeta meta;
  meta.markers = {{0, "Intro"}, {2205, "Drop"}};
  meta.tempo = 128;
  meta.beats = 8;
  const std::string wav = (fs::path(scratch) / "meta.wav").string();
  expect(writeWavFile(wav, a, 44100, 16, meta), "WAV with markers + tempo writes");
  auto bytes = readBytes(wav);
  std::string sbytes(bytes.begin(), bytes.end());
  expect(sbytes.find("acid") != std::string::npos && sbytes.find("cue ") != std::string::npos && sbytes.find("Drop") != std::string::npos,
         "WAV carries acid (tempo) + cue / label chunks");
  packs::edison::Content content;
  expect(packs::edison::parseWav(bytes.data(), bytes.size(), content) && content.markers.size() == 2 && content.markers[1].pos == 2205 &&
             content.markers[1].name == "Drop",
         "markers read back at their frames");

  // FLAC through the audio editor's encoder, dithered to 16 bit.
  AudioBuffer tone(2, 8000);
  for (size_t i = 0; i < 8000; ++i) tone.at(0, i) = tone.at(1, i) = static_cast<float>(0.3 * std::sin(i * 0.05));
  AudioFileOptions fo;
  fo.format = ExportFormat::Flac;
  fo.bits = 16;
  const std::string flac = (fs::path(scratch) / "t.flac").string();
  std::string err;
  expect(writeAudioFile(flac, tone, 44100, fo, &err), "FLAC export");
  SamplePtr fb = loadSample(flac);
  expect(fb && fb->frames() == 8000 && std::fabs(fb->left[100] - tone.at(0, 100)) < 2.0f / 32768.0f, "FLAC decodes back (within dither)");

  // External encoders: a missing program is reported, never faked.
  setEncoderOverride("ffmpeg", "");
  setEncoderOverride("lame", "");
  fo.format = ExportFormat::Mp3;
  err.clear();
  expect(!writeAudioFile((fs::path(scratch) / "x.mp3").string(), tone, 44100, fo, &err) && err.find("encoder") != std::string::npos,
         "MP3 without an encoder program fails with a clear message");
  setEncoderOverride("ffmpeg", "/usr/local/bin/ffmpeg");
  setEncoderOverride("lame", "/usr/local/bin/lame");
  if (::access("/usr/local/bin/ffmpeg", X_OK) == 0) {
    err.clear();
    const std::string mp3 = (fs::path(scratch) / "t.mp3").string(), ogg = (fs::path(scratch) / "t.ogg").string();
    AudioBuffer longer(2, 44100);
    for (size_t i = 0; i < 44100; ++i) longer.at(0, i) = longer.at(1, i) = static_cast<float>(0.3 * std::sin(i * 0.05));
    bool okMp3 = writeAudioFile(mp3, longer, 44100, fo, &err);
    fo.format = ExportFormat::Ogg;
    bool okOgg = writeAudioFile(ogg, longer, 44100, fo, &err);
    SamplePtr m3 = okMp3 ? loadSample(mp3) : nullptr;
    SamplePtr og = okOgg ? loadSample(ogg) : nullptr;
    expect(m3 && std::fabs(m3->seconds() - 1.0) < 0.1 && og && std::fabs(og->seconds() - 1.0) < 0.1,
           "MP3 / Ogg through ffmpeg decode back to the same length");
  } else {
    expect(true, "ffmpeg not installed: external encoder round trip skipped");
  }
  setEncoderOverride("ffmpeg", "");
  setEncoderOverride("lame", "");
  setEncoderOverride("oggenc", "");
  expect(encoderFor(ExportFormat::Ogg).empty() && encoderStatus(ExportFormat::Ogg).find("not installed") == 0, "encoder status reports missing tools");

  expect(expandNamePattern("%P - %N %T", {{'P', "My/Song"}, {'N', "03"}, {'T', "Kick: Sub"}}) == "My_Song - 03 Kick_ Sub",
         "file naming pattern expands tokens and cleans the name");
}

// ---------------------------------------------------------------- stems / tracks

void testStems() {
  Song s = baseSong();
  ObjectId pat = s.patterns[0].id;
  ObjectId pat2 = s.allocateId();
  {
    Pattern p;
    p.id = pat2;
    p.name = "P2";
    s.patterns.push_back(p);
  }
  ObjectId kick = addSampler(s, "Kick", impulseWav(), s.mixer[1].id);
  ObjectId snare = addSampler(s, "Snare", impulseWav(), s.mixer[2].id);
  ObjectId hat = addSampler(s, "Hat", impulseWav(), s.mixer[4].id);
  // Insert 4 → insert 3 (a submix bus) → master.
  s.mixer[4].sends = {Send{s.mixer[3].id, 1.0f, false, false}};
  addNote(s, pat, kick, 0);
  addNote(s, pat, snare, 96);
  addNote(s, pat2, hat, 48);
  addNote(s, pat2, hat, 96 * 2 + 48);
  addClip(s, pat, 0, 0);
  addClip(s, pat2, 0, 1);
  s.mixer[1].slots[0].plugin = defaultPluginState("gl.delay");   // insert effect: in the stem
  s.mixer[0].slots[0].plugin = defaultPluginState("gl.reverb");  // master effect: not in stems
  RenderJob job;
  std::vector<AudioBuffer> stems;
  std::vector<std::string> names;
  auto used = usedInserts(s);
  expect(used.size() == 4, "used inserts: tracks fed by channels or sends");
  int n = renderStems(s, job, used, false, [&](const SplitPart& p, RenderResult&& r) {
    stems.push_back(std::move(r.audio));
    names.push_back(p.name);
    return true;
  });
  expect(n == 4 && stems.size() == 4, "one stem per used insert");
  RenderJob full = job;
  full.masterEffects = false;
  Song neutral = s;
  RenderResult mix = renderAudio(neutral, full);
  // Insert 3 is a bus fed by insert 4: its stem contains insert 4. Sum the
  // stems of tracks that reach the master directly (1, 2, 3).
  AudioBuffer sum(2, mix.audio.frames);
  for (size_t k = 0; k < stems.size(); ++k) {
    if (used[k] == s.mixer[4].id) continue;
    for (size_t i = 0; i < sum.data.size() && i < stems[k].data.size(); ++i) sum.data[i] += stems[k].data[i];
  }
  expect(stems[0].frames == mix.audio.frames && maxDiff(sum, mix.audio) < 1e-5f, "stems sum to the master (no master effects) within 1e-5");
  // The routed-through insert 4 has its own stem (sent straight to the master).
  const size_t i4 = static_cast<size_t>(std::find(used.begin(), used.end(), s.mixer[4].id) - used.begin());
  expect(i4 < stems.size() && onsets(stems[i4]).size() == 2, "stem of an insert that feeds a bus has its own sound");
  Song st = stemSong(s, s.mixer[2].id, false);
  expect(st.mixer[1].muted && !st.mixer[2].muted && st.mixer[2].sends.size() == 1 && st.mixer[2].sends[0].to == s.masterId() &&
             (st.mixer[0].slots[0].plugin.empty() || !st.mixer[0].slots[0].enabled) && st.channels[0].muted,
         "stem song: other inserts muted, the stem goes straight to a neutral master");
  Song withMaster = stemSong(s, s.mixer[2].id, true);
  expect(withMaster.mixer[0].slots[0].enabled, "stems with master effects keep the master chain");

  // Split playlist tracks: every track alone, summing to the song.
  std::vector<AudioBuffer> parts;
  auto tracks = usedPlaylistTracks(s);
  renderPlaylistTracks(s, full, tracks, [&](const SplitPart&, RenderResult&& r) {
    parts.push_back(std::move(r.audio));
    return true;
  });
  AudioBuffer psum(2, mix.audio.frames);
  for (const auto& p : parts)
    for (size_t i = 0; i < psum.data.size() && i < p.data.size(); ++i) psum.data[i] += p.data[i];
  expect(tracks.size() == 2 && parts.size() == 2 && maxDiff(psum, mix.audio) < 1e-5f, "playlist tracks split and sum to the song");
}

// ---------------------------------------------------------------- MIDI

std::vector<uint8_t> vlq(uint32_t v) {
  std::vector<uint8_t> o;
  uint8_t buf[5];
  int n = 0;
  buf[n++] = v & 0x7F;
  while (v >>= 7) buf[n++] = static_cast<uint8_t>((v & 0x7F) | 0x80);
  while (n) o.push_back(buf[--n]);
  return o;
}
std::vector<uint8_t> chunk(const char* id, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> o(id, id + 4);
  const uint32_t n = static_cast<uint32_t>(body.size());
  o.insert(o.end(), {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)});
  o.insert(o.end(), body.begin(), body.end());
  return o;
}
void ev(std::vector<uint8_t>& t, uint32_t delta, std::initializer_list<uint8_t> b) {
  auto d = vlq(delta);
  t.insert(t.end(), d.begin(), d.end());
  t.insert(t.end(), b);
}

void testSmfParsing() {
  // Format 0, 480 PPQ: tempo 100, 3/4, program, CC 7, two channels, running
  // status and note-on velocity 0 as note-off.
  std::vector<uint8_t> t;
  ev(t, 0, {0xFF, 0x51, 0x03, 0x09, 0x27, 0xC0});  // 600000 us = 100 BPM
  ev(t, 0, {0xFF, 0x58, 0x04, 3, 2, 24, 8});
  ev(t, 0, {0xC0, 33});                             // fingered bass
  ev(t, 0, {0xB0, 7, 100});
  ev(t, 0, {0x90, 40, 90});
  ev(t, 240, {41, 80});        // running status: second note-on
  ev(t, 240, {40, 0});         // velocity 0 = off (480 ticks)
  ev(t, 0, {0x99, 36, 120});   // drums on channel 10
  ev(t, 120, {0x89, 36, 0});
  ev(t, 360, {0x80, 41, 0});
  ev(t, 0, {0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20});   // 500000 us = 120 BPM at tick 960
  ev(t, 0, {0xFF, 0x2F, 0x00});
  std::vector<uint8_t> f = chunk("MThd", {0, 0, 0, 1, 0x01, 0xE0});
  auto tr = chunk("MTrk", t);
  f.insert(f.end(), tr.begin(), tr.end());
  MidiFileData m;
  std::string err;
  expect(parseMidi(f, m, &err) && m.format == 0 && m.division == 480, "SMF type 0 parses");
  expect(m.tracks.size() == 1 && m.tracks[0].notes.size() == 3 && m.tracks[0].program == 33, "type 0: notes (running status, vel-0 off) + program");
  expect(std::fabs(m.tempo - 100.0) < 1e-6 && m.tempos.size() == 2 && m.tempos[1].tick == 960 && std::fabs(m.tempos[1].bpm - 120.0) < 1e-6,
         "tempo map read");
  expect(m.timeSigNum == 3 && m.timeSigDen == 4, "time signature read");
  const auto& n0 = m.tracks[0].notes;
  if (std::getenv("GL_DEBUG"))
    for (const auto& x : n0) std::printf("note %lld len %lld p %d ch %d\n", (long long)x.start, (long long)x.length, x.pitch, x.channel);
  expect(n0[0].start == 0 && n0[0].length == 480 && n0[0].pitch == 40 && n0[1].start == 240 && n0[1].length == 720 && n0[2].start == 480 && n0[2].channel == 9,
         "note timing / length / channel");
  MidiFileData split = m;
  splitTracksByChannel(split);
  expect(split.tracks.size() == 2 && split.tracks[0].notes.size() == 2 && split.tracks[1].notes.size() == 1 && split.tracks[1].channel == 9,
         "type 0 splits into one track per MIDI channel");
  Song s = baseSong();
  MidiImportResult ir = importMidiIntoSong(s, m, "Bassline");
  const Pattern* p = s.pattern(ir.pattern);
  expect(ir.error.empty() && ir.channels.size() == 2 && p && p->notes.at(ir.channels[0]).size() == 2, "import: two channels, one pattern");
  expect(s.channel(ir.channels[0])->name == "Fingered Bass" && s.channel(ir.channels[1])->name == "Drums",
         "program change names the channel (GM); channel 10 is drums");
  expect(p->notes.at(ir.channels[0])[1].start == 48 && p->notes.at(ir.channels[0])[1].length == 144, "480 PPQ ticks scale to the project PPQ (96)");
  expect(std::fabs(s.tempo - 100.0) < 1e-6 && ir.tempoChanges == 1 && s.timeSigNum == 3, "tempo / meter applied; tempo change → automation");
  TempoMap map;
  map.build(s);
  expect(std::fabs(map.bpmAt(10) - 100.0) < 0.01 && std::fabs(map.bpmAt(96 * 2 + 1) - 120.0) < 0.01, "imported tempo map plays (100 → 120 BPM at beat 2)");
  expect(ir.automationLanes == 1 && findPatternLane(*p, ir.channels[0], static_cast<uint32_t>(ChannelParam::Volume)),
         "CC 7 → channel volume pattern automation");

  // Format 1: conductor + named tracks.
  std::vector<uint8_t> c0, c1, c2;
  ev(c0, 0, {0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20});
  ev(c0, 0, {0xFF, 0x06, 0x05, 'V', 'e', 'r', 's', 'e'});
  ev(c0, 0, {0xFF, 0x2F, 0x00});
  ev(c1, 0, {0xFF, 0x03, 0x04, 'L', 'e', 'a', 'd'});
  ev(c1, 0, {0x91, 72, 100});
  ev(c1, 96, {0x81, 72, 64});
  ev(c1, 0, {0xFF, 0x2F, 0x00});
  ev(c2, 0, {0xFF, 0x03, 0x03, 'P', 'a', 'd'});
  ev(c2, 0, {0xB2, 10, 0});
  ev(c2, 0, {0x92, 60, 70});
  ev(c2, 0, {0x92, 64, 70});
  ev(c2, 384, {0x82, 60, 0});
  ev(c2, 0, {0x82, 64, 0});
  ev(c2, 0, {0xFF, 0x2F, 0x00});
  std::vector<uint8_t> f1 = chunk("MThd", {0, 1, 0, 3, 0, 96});
  for (auto* x : {&c0, &c1, &c2}) {
    auto c = chunk("MTrk", *x);
    f1.insert(f1.end(), c.begin(), c.end());
  }
  MidiFileData m1;
  expect(parseMidi(f1, m1) && m1.format == 1 && m1.tracks.size() == 2 && m1.tracks[0].name == "Lead" && m1.tracks[1].name == "Pad",
         "SMF type 1: conductor folded, named tracks");
  expect(m1.markers.size() == 1 && m1.markers[0].text == "Verse" && m1.tracks[1].notes.size() == 2 && m1.tracks[1].ctls.size() == 1,
         "type 1: markers, chords, controllers");
  // RIFF-wrapped (.rmi) and refused variants.
  std::vector<uint8_t> rmi = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'R', 'M', 'I', 'D', 'd', 'a', 't', 'a'};
  const uint32_t dl = static_cast<uint32_t>(f1.size());
  rmi.insert(rmi.end(), {uint8_t(dl), uint8_t(dl >> 8), uint8_t(dl >> 16), uint8_t(dl >> 24)});
  rmi.insert(rmi.end(), f1.begin(), f1.end());
  MidiFileData mr;
  expect(parseMidi(rmi, mr) && mr.tracks.size() == 2, "RIFF MIDI (.rmi) parses");
  std::vector<uint8_t> f2 = f1;
  f2[9] = 2;
  expect(!parseMidi(f2, mr, &err) && err.find("format 2") != std::string::npos, "format 2 is refused with a reason");
  std::vector<uint8_t> smpte = f1;
  smpte[12] = 0xE7;
  expect(!parseMidi(smpte, mr, &err), "SMPTE division is refused");
  std::vector<uint8_t> junk = {'M', 'T', 'h', 'd', 0, 0};
  expect(!parseMidi(junk, mr), "truncated file is refused");

  // Insert at the piano roll cursor.
  Song s2 = baseSong();
  ObjectId ch = addSampler(s2, "Piano", impulseWav());
  int added = insertMidiNotes(s2, s2.patterns[0].id, ch, 96 * 4, m1, 1);
  const auto& notes = s2.patterns[0].notes.at(ch);
  expect(added == 2 && notes[0].start == 96 * 4 && notes[0].length == 384 && s2.patterns[0].bars >= 2, "import at cursor: notes placed from the cursor");
}

void testMidiRoundTrip() {
  Song s = baseSong();
  s.ppq = 96;
  ObjectId pat = s.patterns[0].id;
  s.pattern(pat)->bars = 4;
  ObjectId a = addSampler(s, "Bass", impulseWav());
  ObjectId b = addSampler(s, "Chords", impulseWav());
  addNote(s, pat, a, 0, 48, 36);
  addNote(s, pat, a, 96, 30, 38);
  addNote(s, pat, a, 96 * 5 + 7, 200, 43);
  addNote(s, pat, b, 0, 384, 60);
  addNote(s, pat, b, 0, 384, 64);
  addNote(s, pat, b, 0, 384, 67);
  s.pattern(pat)->notes[a][1].velocity = 77;
  // Volume automation (Hold points) on the bass.
  {
    auto desc = describeParam(s, channelParam(a, ChannelParam::Volume));
    std::vector<AutoPoint> pts;
    for (int k : {0, 2, 5}) {  // CC 20, 60, 120
      AutoPoint p;
      p.pos = 96 * k;
      p.value = static_cast<float>(desc->toNormalized((20 + 20 * k) / 127.0));
      p.shape = AutoShape::Hold;
      pts.push_back(p);
    }
    setPatternLane(*s.pattern(pat), a, static_cast<uint32_t>(ChannelParam::Volume), pts);
  }
  addClip(s, pat, 0);
  // Tempo automation: 120 → 150 at bar 3.
  {
    auto desc = describeParam(s, songParam(SongParam::Tempo));
    AutomationClip ac;
    ac.id = s.allocateId();
    ac.owner = kSongId;
    ac.index = static_cast<uint32_t>(SongParam::Tempo);
    AutoPoint p0, p1;
    p0.pos = 0;
    p0.value = static_cast<float>(desc->toNormalized(120.0));
    p1.pos = 96 * 8;
    p1.value = static_cast<float>(desc->toNormalized(150.0));
    p1.shape = AutoShape::Hold;
    ac.points = {p0, p1};
    s.automations.push_back(ac);
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Automation;
    c.source = ac.id;
    c.track = 3;
    c.start = 0;
    c.length = 96 * 16;
    s.playlist.push_back(c);
  }
  Marker mk;
  mk.pos = 96 * 4;
  mk.name = "B section";
  s.markers.push_back(mk);

  MidiFileData out = songToMidi(s);
  const std::string path = (fs::path(scratch) / "round.mid").string();
  expect(writeMidiFile(path, out), "song exports as MIDI");
  MidiFileData back;
  expect(readMidiFile(path, back) && back.tracks.size() == 2, "exported file reads back: one track per channel");
  Song t = baseSong();
  MidiImportOptions o;
  o.placeInPlaylist = true;
  MidiImportResult ir = importMidiIntoSong(t, back, "Round");
  const Pattern* p = t.pattern(ir.pattern);
  bool notesEqual = p && ir.channels.size() == 2;
  for (size_t k = 0; notesEqual && k < 2; ++k) {
    std::vector<Note> want = s.pattern(pat)->notes.at(k == 0 ? a : b);
    std::vector<Note> got = p->notes.at(ir.channels[k]);
    auto key = [](const Note& x, const Note& y) { return x.start != y.start ? x.start < y.start : x.pitch < y.pitch; };
    std::sort(want.begin(), want.end(), key);
    std::sort(got.begin(), got.end(), key);
    notesEqual = want.size() == got.size();
    for (size_t i = 0; notesEqual && i < want.size(); ++i)
      notesEqual = want[i].start == got[i].start && want[i].length == got[i].length && want[i].pitch == got[i].pitch &&
                   want[i].velocity == got[i].velocity;
  }
  expect(notesEqual, "MIDI round trip: notes equal the source (start, length, pitch, velocity)");
  expect(t.channel(ir.channels[0])->name == "Bass" && t.channel(ir.channels[1])->name == "Chords", "round trip: channel names");
  TempoMap m0, m1;
  m0.build(s);
  m1.build(t);
  expect(std::fabs(t.tempo - 120.0) < 1e-6 && std::fabs(m1.bpmAt(96 * 8 + 1) - 150.0) < 0.01 && std::fabs(m0.secondsAt(96 * 12) - m1.secondsAt(96 * 12)) < 1e-3,
         "round trip: tempo map (120 → 150 at bar 3) and timing in seconds");
  const PatternAutomation* lane = findPatternLane(*p, ir.channels[0], static_cast<uint32_t>(ChannelParam::Volume));
  bool laneOk = lane && lane->points.size() == 3;
  for (size_t k = 0; laneOk && k < 3; ++k) {
    auto d0 = describeParam(s, channelParam(a, ChannelParam::Volume));
    auto d1 = describeParam(t, channelParam(ir.channels[0], ChannelParam::Volume));
    laneOk = lane->points[k].pos == s.pattern(pat)->automation[0].points[k].pos &&
             std::fabs(d1->fromNormalized(lane->points[k].value) - d0->fromNormalized(s.pattern(pat)->automation[0].points[k].value)) < 1e-4;
  }
  expect(laneOk, "round trip: volume automation ↔ CC 7 keeps points and values");
  expect(t.markers.size() == 1 && t.markers[0].pos == 96 * 4 && t.markers[0].name == "B section", "round trip: markers");
  // Format 0 export of the same data imports to the same notes.
  const std::string p0 = (fs::path(scratch) / "round0.mid").string();
  writeMidiFile(p0, out, nullptr, 0);
  MidiFileData back0;
  expect(readMidiFile(p0, back0) && back0.format == 0 && back0.tracks.size() == 1, "format 0 export: one track");
  splitTracksByChannel(back0);
  size_t total = 0;
  for (const auto& tr : back0.tracks) total += tr.notes.size();
  expect(back0.tracks.size() == 2 && total == 6, "format 0 export splits back into the two channels");
  // Range export (time selection) shifts to 0.
  MidiFileData rng = songRangeToMidi(s, 96, 96 * 6);
  expect(rng.tracks.size() == 1 && rng.tracks[0].notes.size() == 2 && rng.tracks[0].notes[0].start == 0 &&
             rng.tracks[0].notes[1].start == 96 * 4 + 7,
         "selection export: notes starting inside the range, shifted to 0");
  // Pattern export with a channel selection.
  MidiExportOptions only;
  only.channels = {b};
  MidiFileData pm = patternToMidi(s, pat, only);
  expect(pm.tracks.size() == 1 && pm.tracks[0].name == "Chords", "pattern export of selected channels");
}

// ---------------------------------------------------------------- interop

void testZipAndInflate() {
  // Fixed-Huffman vector ("hello" ×4) and a dynamic-Huffman vector.
  const std::vector<uint8_t> fixedV = {203, 72, 205, 201, 201, 87, 200, 64, 39, 1};
  std::vector<uint8_t> out;
  expect(inflateRaw(fixedV.data(), fixedV.size(), out) && std::string(out.begin(), out.end()) == "hello hello hello hello",
         "inflate: fixed Huffman block");
  const std::vector<uint8_t> dyn = {
      45,  81,  129, 145, 4,   33,  12,  106, 37,  173, 129, 162, 38,  253, 23,  240, 176, 247, 227, 184, 179, 26,  4,   66,  4,
      53,  112, 160, 11,  17,  144, 64,  28,  225, 130, 221, 23,  247, 126, 103, 108, 249, 115, 247, 25,  250, 170, 233, 83,  25,
      242, 168, 57,  146, 174, 180, 201, 114, 109, 251, 84,  218, 47,  68,  70,  153, 109, 112, 110, 238, 158, 81,  192, 82,  13,
      80,  181, 251, 142, 118, 105, 130, 225, 131, 85,  54,  138, 138, 188, 116, 22,  188, 75,  46,  219, 18,  65,  203, 234, 114,
      109, 211, 30,  61,  19,  153, 175, 167, 240, 147, 55,  71,  199, 38,  141, 150, 165, 237, 236, 124, 63,  124, 66,  47,  75,
      43,  126, 108, 210, 181, 82,  217, 4,   63,  29,  148, 113, 187, 225, 27,  187, 229, 49,  89,  243, 145, 174, 199, 74,  197,
      239, 98,  189, 248, 255, 23,  240, 122, 47,  122, 237, 206, 194, 195, 104, 215, 135, 246, 171, 136, 187, 244, 94,  58,  59,
      39,  122, 109, 152, 163, 37,  22,  183, 120, 181, 156, 110, 30,  204, 141, 243, 147, 140, 146, 0,   226, 189, 114, 30,  7,
      144, 2,   185, 98,  251, 228, 243, 188, 24,  155, 182, 200, 50,  71,  194, 77,  79,  152, 180, 50,  59,  66,  150, 116, 112,
      172, 68,  58,  158, 71,  90,  214, 215, 166, 146, 91,  108, 152, 201, 163, 180, 22,  214, 130, 249, 53,  203, 82,  30,  77,
      229, 109, 98,  89,  104, 239, 199, 80,  229, 213, 202, 44,  61,  90,  131, 201, 221, 110, 193, 51,  30,  174, 32,  211, 135,
      157, 100, 210, 211, 246, 109, 141, 157, 232, 88,  213, 127};
  expect(inflateRaw(dyn.data(), dyn.size(), out) && out.size() == 600 && crc32(out.data(), out.size()) == 2850584272u,
         "inflate: dynamic Huffman block (CRC matches)");
  std::vector<uint8_t> bad = dyn;
  bad[5] ^= 0x55;
  std::vector<uint8_t> o2;
  expect(!inflateRaw(bad.data(), bad.size(), o2) || crc32(o2.data(), o2.size()) != 2850584272u, "inflate: corrupt data is not accepted silently");

  // Write → read a stored archive.
  const std::string zp = (fs::path(scratch) / "t.zip").string();
  ZipWriter z;
  z.open(zp);
  z.add("a.txt", {'h', 'i'});
  z.add("dir/b.bin", std::vector<uint8_t>(1000, 7));
  expect(z.close(), "ZIP writes");
  std::vector<ZipEntry> dir;
  std::vector<uint8_t> b;
  expect(readZipDirectory(zp, dir) && dir.size() == 2 && dir[1].name == "dir/b.bin" && readZipEntry(zp, dir[1], b) && b.size() == 1000 && b[999] == 7,
         "ZIP reads back (directory + data + CRC)");
  // Unsafe entries are refused on extraction.
  const std::string evil = (fs::path(scratch) / "evil.zip").string();
  ZipWriter e;
  e.open(evil);
  e.add("../escape.txt", {'x'});
  e.close();
  std::string err;
  expect(!extractZip(evil, (fs::path(scratch) / "x").string(), nullptr, &err) && !fs::exists(fs::path(scratch) / "escape.txt"),
         "ZIP extraction refuses paths outside the folder");
}

Song interopSong(std::string& kickA, std::string& kickB, std::string& ir) {
  kickA = writeTestWav("lib/A/kick.wav", std::vector<float>(100, 0.25f));
  kickB = writeTestWav("lib/B/kick.wav", std::vector<float>(150, -0.5f));  // same name, other content
  ir = writeTestWav("lib/ir/hall.wav", std::vector<float>(50, 0.1f));
  Song s = baseSong();
  addSampler(s, "Kick A", kickA);
  addSampler(s, "Kick B", kickB);
  addSampler(s, "Kick A again", kickA);
  // A JSON plugin state that refers to a file (audio editor style).
  s.mixer[1].slots[0].plugin.type = "gl.edison";
  s.mixer[1].slots[0].plugin.data = "{\"name\": \"take\", \"path\": \"" + ir + "\"}";
  return s;
}

void testInterop() {
  std::string kickA, kickB, ir;
  Song s = interopSong(kickA, kickB, ir);
  auto refs = referencedFiles(s);
  expect(refs.size() == 3 && missingFiles(s).empty(), "file references: bare sample paths and JSON states, deduplicated");

  // Export project data files: copies + relative references.
  const std::string out = (fs::path(scratch) / "export1").string();
  CollectReport rep = exportProjectDataFiles(s, out, "Song");
  expect(rep.error.empty() && rep.copied == 3 && rep.missing.empty(), "data files: every referenced file copied once");
  expect(fs::exists(fs::path(out) / "Samples" / "kick.wav") && fs::exists(fs::path(out) / "Samples" / "kick (2).wav") &&
             fs::exists(fs::path(out) / "Samples" / "hall.wav"),
         "data files: name clashes get unique names");
  {
    std::ifstream in(fs::path(out) / "Song.gridline.json");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    expect(text.find("Samples/kick (2).wav") != std::string::npos && text.find(scratch + "/lib") == std::string::npos,
           "saved project refers to its files relatively");
  }
  Song loaded;
  std::string err;
  expect(loadProject((fs::path(out) / "Song.gridline.json").string(), loaded, &err), "exported project loads");
  bool ok = loaded.channels.size() == 3 && missingFiles(loaded).empty() &&
            readBytes(loaded.channels[1].plugin.data) == readBytes(kickB) && loaded.channels[0].plugin.data == loaded.channels[2].plugin.data &&
            fs::path(loaded.channels[0].plugin.data).parent_path() == fs::path(out) / "Samples";
  expect(ok, "loaded copy: relative paths resolve to the copied files with the right content");
  if (std::getenv("GL_DEBUG")) std::printf("edison data: %s\n", loaded.mixer[1].slots[0].plugin.data.c_str());
  expect(loaded.mixer[1].slots[0].plugin.data.find((fs::path(out) / "Samples" / "hall.wav").string()) != std::string::npos,
         "JSON plugin state rewritten to the copy");

  // Zipped project.
  const std::string zp = (fs::path(scratch) / "Song.zip").string();
  rep = exportZippedProject(s, zp, "Song");
  std::vector<ZipEntry> dir;
  readZipDirectory(zp, dir);
  expect(rep.error.empty() && dir.size() == 4 && dir[0].name == "Song.gridline.json", "zipped project: project + 3 files");
  std::string proj;
  const std::string unz = (fs::path(scratch) / "unzipped").string();
  expect(unpackZippedProject(zp, unz, proj, &err) && fs::path(proj).filename() == "Song.gridline.json", "zipped project unpacks");
  Song z;
  expect(loadProject(proj, z, &err) && missingFiles(z).empty() && readBytes(z.channels[1].plugin.data) == readBytes(kickB) &&
             z.channels[1].plugin.data.rfind(fs::weakly_canonical(unz).string(), 0) == 0,
         "unzipped project plays from its own folder");

  // Bones.
  Song bs = s;
  ObjectId pat = bs.patterns[0].id;
  addNote(bs, pat, bs.channels[0].id, 0);
  BonesReport br = exportProjectBones(bs, (fs::path(scratch) / "bones").string(), "Song");
  expect(br.files.copied == 3 && br.midiFiles == 1 && br.presets == 3 && br.chains == 1 && fs::exists(fs::path(scratch) / "bones" / "MIDI"),
         "project bones: files + pattern MIDI + instrument states + effect chains");

  // Missing samples: relink by name, ranked by folder, by stem.
  Song m = s;
  m.channels[0].plugin.data = "/gone/drive/A/kick.wav";
  m.channels[1].plugin.data = "C:\\Samples\\B\\kick.wav";
  m.channels[2].plugin.data = "/gone/hall.flac";
  m.mixer[1].slots[0].plugin.data = "{\"path\": \"/gone/nothing.wav\"}";
  auto missing = missingFiles(m);
  expect(missing.size() == 4, "missing files listed");
  RelinkReport rr = findReplacements(missing, (fs::path(scratch) / "lib").string());
  expect(rr.found.size() == 3 && rr.found["/gone/drive/A/kick.wav"] == kickA && rr.found["C:\\Samples\\B\\kick.wav"] == kickB &&
             rr.found["/gone/hall.flac"] == ir && rr.notFound.size() == 1,
         "relink: by name (parent folder breaks ties, Windows paths), by stem; the rest reported");
  expect(relinkFiles(m, rr.found) == 3 && missingFiles(m).size() == 1, "relink applies to the song");
  // Size hint breaks a tie when folders do not.
  RelinkOptions ro;
  ro.sizeHints["/x/kick.wav"] = fs::file_size(kickB);
  RelinkReport r2 = findReplacements({"/x/kick.wav"}, (fs::path(scratch) / "lib").string(), ro);
  expect(r2.found["/x/kick.wav"] == kickB, "relink: size hint picks between same-named files");
}

void testExportJob() {
  Song s = baseSong();
  s.info.title = "Job Song";
  s.mixer[1].name = "Kick";
  s.mixer[2].name = "Snare";
  ObjectId pat = s.patterns[0].id;
  ObjectId kick = addSampler(s, "Kick", impulseWav(), s.mixer[1].id);
  ObjectId snare = addSampler(s, "Snare", impulseWav(), s.mixer[2].id);
  addNote(s, pat, kick, 0);
  addNote(s, pat, snare, 96);
  addClip(s, pat, 0);
  Marker mk;
  mk.pos = 96 * 2;
  mk.name = "Hook";
  s.markers.push_back(mk);
  ExportSettings es;
  es.folder = (fs::path(scratch) / "job").string();
  es.baseName = "Job Song";
  es.projectTitle = "Job Song";
  es.formats = {ExportFormat::Wav, ExportFormat::Flac};
  es.wavBits = 16;
  es.midi = true;
  es.splitMixer = true;
  es.render.tail = TailMode::Cut;
  double last = 0;
  ExportOutcome o = runExport(s, es, [&](double p, const std::string&) { last = p; });
  const fs::path d(es.folder);
  expect(o.errors.empty() && o.files.size() == 7, "export job: mix + 2 stems in WAV and FLAC + MIDI");
  expect(fs::exists(d / "Job Song.wav") && fs::exists(d / "Job Song - 01 Kick.flac") && fs::exists(d / "Job Song - 02 Snare.wav") &&
             fs::exists(d / "Job Song.mid"),
         "export job: file names from the part pattern");
  expect(std::fabs(last - 1.0) < 1e-9 && o.frames == 88200, "export job: progress ends at 1; mix length = song");
  auto bytes = readBytes((d / "Job Song.wav").string());
  packs::edison::Content c;
  expect(packs::edison::parseWav(bytes.data(), bytes.size(), c) && c.markers.size() == 1 && c.markers[0].pos == 44100 && c.markers[0].name == "Hook",
         "export job: playlist markers become WAV cue points at their frame");
  std::string sb(bytes.begin(), bytes.end());
  expect(sb.find("acid") != std::string::npos, "export job: tempo information saved");
  // Mono + normalize.
  ExportSettings m = es;
  m.splitMixer = false;
  m.midi = false;
  m.formats = {ExportFormat::Wav};
  m.wavBits = 32;
  m.channels = ChannelMode::MonoMerged;
  m.normalize = true;
  m.baseName = "Mono";
  o = runExport(s, m);
  SamplePtr mono = loadSample((d / "Mono.wav").string());
  float pk = 0;
  for (float v : mono->left) pk = std::max(pk, std::fabs(v));
  expect(mono && std::fabs(pk - 1.0f) < 1e-5f, "export job: mono merged, normalised to 0 dBFS");
  // Cancel.
  std::atomic<bool> cancel{true};
  o = runExport(s, es, {}, &cancel);
  expect(o.cancelled, "export job: cancel");
}

}  // namespace

int main() {
  scratch = (fs::path(gl::test::hermeticTestEnv("export")) / "work").lexically_normal().string();
  fs::create_directories(scratch);
  testScopes();
  testInterpolation();
  testTails();
  testDither();
  testResampleAndFormats();
  testStems();
  testSmfParsing();
  testMidiRoundTrip();
  testZipAndInflate();
  testInterop();
  testExportJob();
  return finish();
}
