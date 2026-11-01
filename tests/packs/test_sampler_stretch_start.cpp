// Gridline — J-220: a Sampler in a stretch time mode (Stretch, Rubber Band)
// whose start offset (or, in reverse, end) falls between two frames is not
// silent. The stretch stream used to start on the nearest frame, which could
// lie just outside the region it plays (before a fractional start, past a
// fractional end), and the stream then ended at once: every note silent.
// Checked in song renders (offline) and on the generator itself (the live
// path), for 96 kHz mono and 44.1 kHz stereo files, against the same setup
// with a start point that always worked.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Project.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Render.h"
#include "../../src/engine/Sample.h"
#include "../Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string writeTone(const std::string& name, double sec, int rate, int channels) {
  const auto n = static_cast<size_t>(sec * rate);
  AudioBuffer b(channels, n);
  for (size_t i = 0; i < n; ++i)
    for (int c = 0; c < channels; ++c)
      b.at(c, i) = static_cast<float>(0.4 * std::sin(2 * kPi * (330.0 + 40.0 * c) * static_cast<double>(i) / rate));
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, rate, 16);
  sampleCache().clear();
  return path;
}

PluginState samplerState(const std::string& path, int mode, double start, bool reverse) {
  PluginState s = defaultPluginState("gl.sampler");
  s.data = path;
  s.params[pp::SmpStretchMode] = static_cast<float>(mode);
  s.params[pp::SmpTimeStretch] = 2.0f;
  if (reverse) {
    s.params[pp::SmpReverse] = 1.0f;
    s.params[pp::SmpEnd] = static_cast<float>(start);
  } else {
    s.params[pp::SmpStart] = static_cast<float>(start);
  }
  return s;
}

double rms(const AudioBuffer& a) {
  double sum = 0;
  for (size_t i = 0; i < a.frames; ++i) sum += static_cast<double>(a.at(0, i)) * a.at(0, i);
  return a.frames ? std::sqrt(sum / static_cast<double>(a.frames)) : 0.0;
}

// Three half-second notes in a song render (offline).
double songRms(const std::string& path, int mode, double start, bool reverse) {
  Song s = makeEmptySong();
  s.tempo = 120;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "smp";
  ch.plugin = samplerState(path, mode, start, reverse);
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  const Tick perSec = static_cast<Tick>(s.beatTicks() * 2);  // a beat is 0.5 s
  Pattern p;
  p.id = s.allocateId();
  p.bars = 2;
  for (double at : {0.0, 1.0, 2.0}) {
    Note n;
    n.start = static_cast<Tick>(at * static_cast<double>(perSec));
    n.length = perSec / 2;
    n.pitch = 60;
    p.notes[ch.id].push_back(n);
  }
  s.patterns.push_back(p);
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Pattern, k.pattern = p.id, k.track = 1, k.start = 0;
  k.length = 8 * s.beatTicks();
  s.playlist.push_back(k);
  RenderOptions o;
  return rms(renderSong(s, o));
}

// One note on the generator itself (the live engine's path), 256-frame blocks.
double voiceRms(const std::string& path, int mode, double start, bool reverse) {
  const double sr = 48000;
  PluginState s = samplerState(path, mode, start, reverse);
  ChannelAux aux;
  prepareChannelAux(s, aux);
  auto gen = createGenerator("gl.sampler");
  gen->prepareHost(s, sr);
  ProcessContext ctx;
  ctx.sampleRate = sr;
  AudioBuffer out(2, 48000);
  std::vector<float> L(out.frames, 0.f), R(out.frames, 0.f);
  gen->noteOn(1, 60, 1.0f, 0);
  for (size_t i = 0; i < L.size(); i += 256)
    gen->process(s, aux, ctx, L.data() + i, R.data() + i, static_cast<int>(std::min<size_t>(256, L.size() - i)));
  for (size_t i = 0; i < L.size(); ++i) out.at(0, i) = L[i];
  return rms(out);
}

// The frame position a 0..1 start parameter lands on (the sampler's region start).
double framePos(float param, size_t frames) { return static_cast<double>(param) * static_cast<double>(frames - 1); }
double frac(double x) { return x - std::floor(x); }

}  // namespace

int main() {
  struct File {
    const char* name;
    double sec;
    int rate, channels;
  };
  // The reported setup (178 s at 96 kHz, mono) and a 44.1 kHz stereo file.
  const File files[] = {{"gl_j220_tone96m.wav", 178.0, 96000, 1}, {"gl_j220_tone44s.wav", 30.0, 44100, 2}};
  for (const File& f : files) {
    const std::string path = writeTone(f.name, f.sec, f.rate, f.channels);
    const size_t frames = static_cast<size_t>(f.sec * f.rate);
    // Start points between two frames, below half way (rounding went to the
    // frame before the region) for forwards, at or past it for reverse.
    std::vector<float> fwd, rev;
    for (float st : {0.505f, 0.7f, 0.3f, 0.9f, 0.123f, 0.61f, 0.77f})
      if (fwd.size() < 2 && frac(framePos(st, frames)) > 1e-6 && frac(framePos(st, frames)) < 0.5) fwd.push_back(st);
    for (float st : {0.5f, 0.9f, 0.3f, 0.1f, 0.123f, 0.61f, 0.77f})
      if (rev.size() < 2 && frac(framePos(st, frames)) >= 0.5) rev.push_back(st);
    char name[200];
    std::snprintf(name, sizeof name, "%s: two fractional start and two end points to test (%zu, %zu)", f.name, fwd.size(), rev.size());
    expect(fwd.size() == 2 && rev.size() == 2, name);
    for (int mode : {1, 7}) {  // Stretch, Rubber Band
      const char* mname = mode == 1 ? "Stretch" : "Rubber Band";
      const double ref = songRms(path, mode, 0.1, false), refLive = voiceRms(path, mode, 0.1, false);
      std::snprintf(name, sizeof name, "%s %s: start 0.1 plays (song %.3f, voice %.3f)", f.name, mname, ref, refLive);
      expect(ref > 0.2 && refLive > 0.2, name);
      for (int r = 0; r < 2; ++r)
        for (float st : r ? rev : fwd) {
          const double song = songRms(path, mode, st, r == 1), live = voiceRms(path, mode, st, r == 1);
          std::snprintf(name, sizeof name, "%s %s x2%s %s %.3f: not silent, level like start 0.1 (song %.3f / %.3f, voice %.3f / %.3f)",
                        f.name, mname, r ? " reverse" : "", r ? "end" : "start", static_cast<double>(st), song, ref, live, refLive);
          expect(song > 0.8 * ref && song < 1.25 * ref && live > 0.8 * refLive && live < 1.25 * refLive, name);
        }
    }
    sampleCache().clear();
    std::error_code ec;
    fs::remove(path, ec);
  }
  return finish();
}
