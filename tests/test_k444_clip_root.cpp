// Gridline: K-444, an audio clip's root note and fine tune set its pitch on the playlist.
// A playlist audio clip plays its sound as its channel plays the reference key (C5, note 60):
// the channel's root note says which key plays the sound at its own pitch, so a root a whole
// tone up plays the clip a whole tone down, exactly as the channel rack's notes on C5 do.
//  * Resample, Stretch and Rubber Band clips, tempo-synced clips and launched live clips
//    follow the root note and the fine tune; the clip keeps its length (pitch never moves timing);
//  * the edit through the document (the clip editor's root knob and keyboard, the sampler
//    editor) is one undo step and the undo puts the pitch back;
//  * a channel rack note on C5 and the playlist clip agree;
//  * in a collaboration session the root change and its undo reach the other person, whose
//    render follows.
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "collab/CollabSim.h"
#include "core/ClipRate.h"
#include "core/Document.h"
#include "core/Params.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/WavWriter.h"
#include "engine/ClipOps.h"
#include "engine/Render.h"
#include "engine/Sample.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTone = 220.0;

std::string writeSource() {
  const size_t n = static_cast<size_t>(2.0 * kSr);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) b.at(0, i) = b.at(1, i) = static_cast<float>(0.3 * std::sin(2 * kPi * kTone * static_cast<double>(i) / kSr));
  const std::string path = (fs::temp_directory_path() / "gl_k444_src.wav").string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  sampleCache().clear();
  return path;
}

using cd = std::complex<double>;
void fft(std::vector<cd>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * kPi / static_cast<double>(len);
    const cd wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      cd w(1);
      for (size_t j = 0; j < len / 2; ++j) {
        const cd u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v, a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}
// The strongest partial between 80 and 1000 Hz in x[a, a + 16384), interpolated.
double peakHz(const std::vector<float>& x, size_t a) {
  const size_t N = 16384;
  if (a + N > x.size()) return 0;
  std::vector<cd> b(N);
  for (size_t i = 0; i < N; ++i) {
    const double w = 2 * kPi * static_cast<double>(i) / static_cast<double>(N);
    b[i] = static_cast<double>(x[a + i]) * (0.35875 - 0.48829 * std::cos(w) + 0.14128 * std::cos(2 * w) - 0.01168 * std::cos(3 * w));
  }
  fft(b);
  const size_t k0 = static_cast<size_t>(80.0 * N / kSr), k1 = static_cast<size_t>(1000.0 * N / kSr);
  size_t best = k0;
  for (size_t k = k0; k <= k1; ++k)
    if (std::abs(b[k]) > std::abs(b[best])) best = k;
  const double ym = std::log(std::abs(b[best - 1]) + 1e-30), y0 = std::log(std::abs(b[best]) + 1e-30),
               yp = std::log(std::abs(b[best + 1]) + 1e-30);
  return (static_cast<double>(best) + 0.5 * (ym - yp) / (ym - 2 * y0 + yp)) * kSr / static_cast<double>(N);
}
double cents(double f, double ref) { return f > 0 ? 1200 * std::log2(f / ref) : 1e9; }

std::vector<float> render(const Song& s) {
  RenderOptions o;
  AudioBuffer b = renderSong(s, o);
  return std::vector<float>(b.plane(0), b.plane(0) + b.frames);
}

// One audio clip channel (time mode `mode`) holding the tone; one clip of its natural length.
Song clipSong(const std::string& path, int mode) {
  Song s = makeEmptySong();
  s.tempo = 120;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Tone";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.plugin.params[pp::SmpStretchMode] = static_cast<float>(mode);
  ch.mixerTrack = s.masterId();
  ch.audioClip = true;
  s.channels.push_back(ch);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.start = 0;
  c.length = audioClipNaturalLength(s, c);
  s.playlist.push_back(c);
  return s;
}

double heard(const Song& s) { return peakHz(render(s), static_cast<size_t>(0.4 * kSr)); }

void rootAndFine(const std::string& path) {
  for (int mode : {0, 1, 7}) {
    const char* mname = mode == 0 ? "Resample" : mode == 1 ? "Stretch" : "Rubber Band";
    for (auto [root, fine] : {std::pair<int, float>{62, 0.0f}, {55, 0.0f}, {48, 0.0f}, {60, 50.0f}, {64, -30.0f}}) {
      Song s = clipSong(path, mode);
      const Tick len = s.playlist[0].length;
      s.channels[0].plugin.params[pp::SmpRoot] = static_cast<float>(root);
      s.channels[0].plugin.params[pp::SmpFine] = fine;
      const double want = kTone * std::pow(2.0, ((60 - root) * 100.0 + fine) / 1200.0);
      const double got = heard(s);
      char name[200];
      std::snprintf(name, sizeof name, "%s clip, root %d, fine %+.0f ct: plays at %.2f Hz (want %.2f)", mname, root, static_cast<double>(fine), got, want);
      expect(std::fabs(cents(got, want)) < 5.0, name);
      std::snprintf(name, sizeof name, "%s clip, root %d: the clip keeps its length", mname, root);
      expect(audioClipNaturalLength(s, s.playlist[0]) == len, name);
    }
  }
  {  // the root on top of the clip's own pitch and the channel pitch knob
    Song s = clipSong(path, 0);
    s.channels[0].plugin.params[pp::SmpRoot] = 62;
    s.playlist[0].pitch = 700;
    const double want = kTone * std::pow(2.0, 500.0 / 1200.0);
    const double got = heard(s);
    char name[160];
    std::snprintf(name, sizeof name, "root 62 with a +700 ct clip pitch: +500 ct in all (%.2f Hz, want %.2f)", got, want);
    expect(std::fabs(cents(got, want)) < 5.0, name);
  }
  {  // tempo-synced (follows the project tempo)
    Song s = clipSong(path, 1);
    s.playlist[0].syncBpm = 120;
    s.channels[0].plugin.params[pp::SmpRoot] = 57;
    const double want = kTone * std::pow(2.0, 300.0 / 1200.0);
    const double got = heard(s);
    char name[160];
    std::snprintf(name, sizeof name, "tempo-synced clip, root 57: three semitones up (%.2f Hz, want %.2f)", got, want);
    expect(std::fabs(cents(got, want)) < 5.0, name);
  }
  {  // a live clip (the Clips layout's grid) launched at the song start
    Song s = clipSong(path, 0);
    s.live.areaEnd = s.playlist[0].length * 4;
    s.playlist[0].launched = true;
    const double before = heard(s);
    s.channels[0].plugin.params[pp::SmpRoot] = 62;
    const double want = kTone * std::pow(2.0, -200.0 / 1200.0);
    const double got = heard(s);
    char name[200];
    std::snprintf(name, sizeof name, "launched live clip: plays (%.2f Hz at root 60) and follows root 62 (%.2f Hz, want %.2f)", before, got, want);
    expect(std::fabs(cents(before, kTone)) < 5.0 && std::fabs(cents(got, want)) < 5.0, name);
  }
}

// The channel rack's note on C5 and the playlist clip of the same channel are at one pitch.
void rackAgrees(const std::string& path) {
  Song s = clipSong(path, 0);
  s.channels[0].plugin.params[pp::SmpRoot] = 62;
  Song n = s;
  n.playlist.clear();
  Pattern p;
  p.id = n.allocateId();
  p.notes[n.channels[0].id].push_back(Note(0, n.ppq * 4, 60, 100));
  n.patterns.push_back(p);
  Clip c;
  c.id = n.allocateId();
  c.pattern = p.id;
  n.playlist.push_back(c);
  const double clip = heard(s), note = heard(n);
  char name[160];
  std::snprintf(name, sizeof name, "root 62: the playlist clip (%.2f Hz) sounds like the rack's C5 note (%.2f Hz)", clip, note);
  expect(std::fabs(cents(clip, note)) < 5.0, name);
}

void documentEdit(const std::string& path) {
  Document d(clipSong(path, 0));
  const ObjectId ch = d.song().channels[0].id;
  expect(std::fabs(cents(heard(d.song()), kTone)) < 5.0, "document: root 60 plays the tone at its own pitch");
  d.setParam(channelPluginParam(ch, pp::SmpRoot), 64, false);
  const double down = heard(d.song());
  char name[160];
  std::snprintf(name, sizeof name, "document: root set to E5 plays four semitones down (%.2f Hz)", down);
  expect(std::fabs(cents(down, kTone * std::pow(2.0, -4.0 / 12.0))) < 5.0, name);
  d.undo();
  expect(d.song().channels[0].plugin.params[pp::SmpRoot] == 60.0f && std::fabs(cents(heard(d.song()), kTone)) < 5.0,
         "document: one undo puts the root and the pitch back");
}

void collaboration(const std::string& path) {
  using namespace gl::collab;
  using namespace sim;
  Clock clock;
  Net net(444);
  Peer a, b;
  a.label = "Ann", b.label = "Ben";
  for (Peer* p : {&a, &b}) p->id = crypto::Identity::generate();
  a.doc.reset(clipSong(path, 0));
  a.folder = tmpDir("k444_a");
  b.folder = tmpDir("k444_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  expect(a.s != nullptr, "collab: a session");
  if (!a.s) return;
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  const ObjectId ch = a.doc.song().channels[0].id;
  a.doc.setParam(channelPluginParam(ch, pp::SmpRoot), 62, false);
  settle(net, {&a, &b}, clock);
  const Channel* cb = b.doc.song().channel(ch);
  expect(cb && cb->plugin.params[pp::SmpRoot] == 62.0f, "collab: Ann's root change reaches Ben");
  if (cb && fs::is_regular_file(cb->plugin.data)) {
    const double got = heard(b.doc.song());
    char name[160];
    std::snprintf(name, sizeof name, "collab: Ben's clip plays the new pitch (%.2f Hz)", got);
    expect(std::fabs(cents(got, kTone * std::pow(2.0, -2.0 / 12.0))) < 5.0, name);
  }
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  cb = b.doc.song().channel(ch);
  expect(cb && cb->plugin.params[pp::SmpRoot] == 60.0f, "collab: Ann's undo reaches Ben");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k444");
  const std::string path = writeSource();
  rootAndFine(path);
  rackAgrees(path);
  documentEdit(path);
  collaboration(path);
  return finish();
}
