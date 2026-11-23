// Gridline - K-321: Seltmann in a Track Receipt pass that only measures. Under tempo automation
// an offline Seltmann stretches its whole warped region again at every tempo it settles on; a
// receipt pass that only measures (its sound matters to nothing) skips the stretching, and its
// warped voices still read where they would. The fast pass gives exactly what the full-mix
// reference pass gives (ranges, play seconds, pitch, segments), and a 60 s project with the
// tempo moving every bar measures in well under a second.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include "../../src/core/Params.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/Song.h"
#include "../../src/core/TrackReceipt.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/SampleTap.h"
#include "../../src/engine/TrackReceiptPass.h"
#include "../Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr int kRate = 44100;

// Clicks on every beat at 100 BPM over a soft tone: material with a clear grid to warp.
std::string writeLoop(const std::string& dir, const std::string& name, double seconds) {
  fs::create_directories(dir);
  const auto n = static_cast<size_t>(seconds * kRate);
  AudioBuffer b(2, n);
  const double beat = 60.0 / 100.0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const double inBeat = std::fmod(t, beat);
    const double click = inBeat < 0.03 ? std::exp(-inBeat * 120.0) * std::sin(2 * 3.141592653589793 * 1800.0 * t) : 0.0;
    b.at(0, i) = b.at(1, i) = static_cast<float>(0.6 * click + 0.15 * std::sin(2 * 3.141592653589793 * 220.0 * t));
  }
  const std::string path = (fs::path(dir) / name).generic_string();
  writeWav(path, b, kRate, 16);
  return path;
}

Tick at(const Song& s, double sec) { return static_cast<Tick>(std::llround(sec * static_cast<double>(s.beatTicks()) * s.tempo / 60.0)); }

// A Seltmann channel playing its slices (keys from the base note) over `bars` bars, with the
// song tempo held for a bar, then stepping to the next value.
Song project(const std::string& file, int bars) {
  Song s = makeEmptySong();
  s.tempo = 120.0;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "seltmann";
  ch.plugin = defaultPluginState("gl.seltmann");
  ch.plugin.data = file;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  Pattern p;
  p.id = s.allocateId();
  p.bars = 1;
  for (int k = 0; k < 4; ++k) {
    Note n;
    n.start = at(s, 0.5 * k), n.length = at(s, 0.4), n.pitch = 60 + k;
    p.notes[ch.id].push_back(n);
  }
  s.patterns.push_back(p);
  for (int b = 0; b < bars; ++b) {
    Clip k;
    k.id = s.allocateId(), k.kind = ClipKind::Pattern, k.pattern = p.id, k.track = 0;
    k.start = static_cast<Tick>(b) * s.barTicks(), k.length = s.barTicks();
    s.playlist.push_back(k);
  }
  AutomationClip a;
  const ParamId tempo = songParam(SongParam::Tempo);
  a.id = s.allocateId(), a.owner = tempo.owner, a.index = tempo.index;
  for (int b = 0; b < bars; ++b) {
    AutoPoint pt;
    pt.pos = static_cast<Tick>(b) * s.barTicks();
    pt.value = 0.40f + 0.02f * static_cast<float>(b % 6);
    pt.shape = AutoShape::Hold;
    a.points.push_back(pt);
  }
  s.automations.push_back(a);
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Automation, k.source = a.id, k.track = 1;
  k.start = 0, k.length = static_cast<Tick>(bars) * s.barTicks();
  s.playlist.push_back(k);
  return s;
}

std::string text(const receipt::Measurement& m, const std::vector<tap::Use>& t) {
  std::ostringstream o;
  o << std::setprecision(17);
  for (const receipt::Use& u : m.uses) {
    o << "use " << u.owner << " plays " << u.plays << " bars " << u.bar0 << "-" << u.bar1 << " |";
    for (const receipt::Range& r : u.ranges) o << " " << r.a << "-" << r.b;
    o << "\n";
  }
  for (const std::string& w : m.warnings) o << "warning " << w << "\n";
  for (const tap::Use& u : t) {
    o << "pair " << u.owner << " play " << u.outSeconds << " jumps " << u.jumps << " pitch " << u.pitched << " " << u.semisMin << " "
      << u.semisMax << " |";
    for (const tap::Segment& g : u.segments) o << " " << g.outA << "," << g.outB << ">" << g.srcA << "," << g.srcB;
    o << "\n";
  }
  return o.str();
}

double since(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

}  // namespace

int main() {
  if (!findPlugin("gl.seltmann")) {
    std::printf("# skipped: no Seltmann in this build\n");
    return finish();
  }
  const std::string dir = (fs::temp_directory_path() / "gl_track_receipt_seltmann").generic_string();
  fs::remove_all(dir);
  const std::string file = writeLoop(dir, "Seltmann Test-Step Song-Break--100.00bpm-Am-[Clearance Required].wav", 9.6);
  {  // fast == reference, to the last bit
    const Song s = project(file, 8);
    receipt::PassOptions o;
    o.sampleRate = kRate;
    std::vector<tap::Use> tf, tr;
    o.tapUses = &tf;
    auto t0 = std::chrono::steady_clock::now();
    const receipt::Measurement mf = receipt::measure(s, o);
    const double fastSec = since(t0);
    o.reference = true;
    o.tapUses = &tr;
    t0 = std::chrono::steady_clock::now();
    const receipt::Measurement mr = receipt::measure(s, o);
    const double refSec = since(t0);
    std::printf("# 16 s under tempo steps: reference %.3f s, fast %.3f s\n", refSec, fastSec);
    const std::string a = text(mf, tf), b = text(mr, tr);
    expect(!mf.uses.empty() && !tf.empty(), "seltmann: its reads are measured");
    expect(a == b, "seltmann: under tempo automation the fast pass reads exactly what the reference reads");
    if (a != b) std::printf("--- reference\n%s--- fast\n%s", b.c_str(), a.c_str());
    double used = 0;
    for (const receipt::Use& u : mf.uses)
      for (const receipt::Range& r : u.ranges) used += r.b - r.a;
    expect(used > 0.5, "seltmann: the slices it plays are counted");
  }
  {  // a 60 s project with the tempo stepping every bar: well under a second
    const Song s = project(file, 30);
    receipt::PassOptions o;
    o.sampleRate = kRate;
    const auto t0 = std::chrono::steady_clock::now();
    const receipt::Measurement m = receipt::measure(s, o);
    const double sec = since(t0);
    std::printf("# 60 s under tempo steps: fast %.3f s\n", sec);
    expect(!m.uses.empty() && sec < 1.0, "seltmann: a 60 s project under tempo automation measures in under 1 s");
  }
  fs::remove_all(dir);
  return finish();
}
