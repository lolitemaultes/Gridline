// Gridline — J-30: render determinism across block sizes.
//  * Offline renders (renderSong / renderAudio) are bit-identical for any
//    caller block size 1…2048, odd sizes included: the engine renders its own
//    fixed blocks (AudioEngine::setFixedBlock).
//  * The realtime path (engine fed the host's blocks as they come) is
//    bit-identical too for the test song and engine features that used to
//    depend on block boundaries (note ids, cut groups, mono / porta,
//    polyphony limits, note processors).
//  * Every built-in instrument and effect (defaults and factory presets),
//    fed odd block sizes, with and without parameters moving on the engine's
//    control grid, stays within -120 dBFS of its 512-frame render.
//  * FL Studio's own demo projects (when FL Studio is installed): offline
//    renders bit-identical for any block size.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../fixtures/TestSong.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/Flp.h"
#include "../../src/engine/AudioEngine.h"
#include "../../src/engine/Effects.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Render.h"
#include "../../src/packs/Packs.h"
#include "../Check.h"
#include "../Hermetic.h"

using namespace gl;

namespace {

double maxDiff(const AudioBuffer& a, const AudioBuffer& b) {
  if (a.data.size() != b.data.size()) return 1e9;
  double d = 0;
  for (size_t i = 0; i < a.data.size(); ++i) d = std::max(d, std::fabs(static_cast<double>(a.data[i]) - b.data[i]));
  return d;
}
double peakOf(const AudioBuffer& a) {
  double p = 0;
  for (float v : a.data) p = std::max(p, static_cast<double>(std::fabs(v)));
  return p;
}

// The engine fed the caller's blocks as they are (what a realtime host does):
// renderSong without the fixed engine blocks.
AudioBuffer renderDirect(const Song& song, int block, Tick tail) {
  ProgramBuilder builder;
  builder.setSampleRate(44100);
  auto prog = builder.build(song);
  const Tick end = prog->songEnd + tail;
  const int64_t total = static_cast<int64_t>(std::ceil(prog->tempoMap.samplesBetween(0.0, static_cast<double>(end), 44100))) + 1;
  AudioEngine eng(44100);
  eng.setProgram(prog);
  eng.setLooping(false);
  eng.setOffline(true);
  eng.setMode(PlayMode::Song);
  eng.play();
  const int64_t lat = std::max(0, prog->outputLatency), span = total + lat;
  std::vector<float> L(static_cast<size_t>(span)), R(static_cast<size_t>(span));
  for (int64_t pos = 0; pos < span; pos += block) {
    const int n = static_cast<int>(std::min<int64_t>(block, span - pos));
    eng.process(L.data() + pos, R.data() + pos, n);
  }
  AudioBuffer out(2, static_cast<size_t>(total));
  std::copy(L.begin() + lat, L.end(), out.plane(0));
  std::copy(R.begin() + lat, R.end(), out.plane(1));
  return out;
}

AudioBuffer renderFixed(const Song& song, int block, Tick tail) {
  RenderOptions o;
  o.blockSize = block;
  o.tail = tail;
  return renderSong(song, o);
}

// Keeps the arrangement window [fromBar, fromBar + bars) and moves it to the start.
Song window(const Song& s0, int fromBar, int bars) {
  Song s = s0;
  const Tick a = static_cast<Tick>(fromBar) * s.barTicks(), b = a + static_cast<Tick>(bars) * s.barTicks();
  std::vector<Clip> keep;
  for (Clip c : s.playlist) {
    const Tick len = s.clipLength(c);
    if (c.start + len <= a || c.start >= b) continue;
    if (c.start < a) continue;  // keep whole clips only (no partial-clip offsets)
    if (c.start + len > b) c.length = b - c.start;
    c.start -= a;
    keep.push_back(c);
  }
  s.playlist = keep;
  return s;
}

// ------------------------------------------------------------- songs

void testDemoSong() {
  const Song demo = makeTestSong();
  const Tick tail = demo.ppq * 2;
  // Offline: bit-identical for any caller block size (2048 is clamped to the engine maximum).
  const AudioBuffer ref = renderFixed(demo, 512, tail);
  expect(peakOf(ref) > 0.1, "test song renders sound");
  for (int b : {1, 7, 32, 64, 333, 1000, 1024, 2048}) {
    const double d = maxDiff(ref, renderFixed(demo, b, tail));
    if (d > 0) std::printf("# demo offline block %d: max diff %g\n", b, d);
    expect(d == 0, ("test song offline render identical at block size " + std::to_string(b)).c_str());
  }
  // Realtime path: the engine sees the host's blocks.
  const AudioBuffer dref = renderDirect(demo, 512, tail);
  expect(maxDiff(ref, dref) == 0, "test song: realtime path at 512 = offline render");
  for (int b : {37, 64, 333, 1024}) {
    const double d = maxDiff(dref, renderDirect(demo, b, tail));
    if (d > 0) std::printf("# demo realtime block %d: max diff %g\n", b, d);
    expect(d == 0, ("test song realtime path identical at block size " + std::to_string(b)).c_str());
  }
}

// Engine features that used to depend on where block boundaries fall.
void testEngineFeatures() {
  Song s = window(makeTestSong(), 7, 5);  // fill bar, bass and chords
  auto chan = [&](const char* name) -> Channel* {
    for (auto& c : s.channels)
      if (c.name == name) return &c;
    return nullptr;
  };
  if (Channel* h = chan("Hat")) h->cutGroup = 1;
  if (Channel* o = chan("Open hat")) o->cutBy = 1;
  if (Channel* b = chan("Bass")) b->mono = true, b->porta = true, b->portaTime = 0.05f;
  if (Channel* c = chan("Chords")) c->polyphony = 2;
  const Tick tail = s.ppq;
  const AudioBuffer a = renderDirect(s, 512, tail);
  expect(peakOf(a) > 0.05, "feature song renders sound");
  double d = 0;
  for (int b : {37, 333, 1000}) d = std::max(d, maxDiff(a, renderDirect(s, b, tail)));
  if (d > 0) std::printf("# features max diff %g\n", d);
  expect(d == 0, "cut groups, mono / porta, polyphony: realtime path identical at block sizes 37 / 333 / 1000");

  // Note processors: every type, spread over the channels (two layouts cover them all).
  std::vector<std::string> nfx;
  for (const PluginInfo& info : pluginCatalog())
    if (info.kind == PluginKind::NoteFx) nfx.push_back(info.type);
  expect(!nfx.empty(), "note processors in the catalogue");
  for (size_t layout = 0; layout * s.channels.size() < nfx.size(); ++layout) {
    Song v = s;
    std::string names;
    for (size_t k = 0; k < v.channels.size(); ++k) {
      const std::string& t = nfx[(k + layout * v.channels.size()) % nfx.size()];
      v.channels[k].noteFx = {defaultPluginState(t)};
      names += (k ? " " : "") + t;
    }
    const AudioBuffer x = renderDirect(v, 512, tail);
    double dd = 0;
    for (int b : {37, 333}) dd = std::max(dd, maxDiff(x, renderDirect(v, b, tail)));
    if (dd > 0) std::printf("# note processors (%s) max diff %g\n", names.c_str(), dd);
    expect(dd <= 1e-6, ("note processors realtime path within -120 dB at block sizes 37 / 333 (layout " + std::to_string(layout) + ")").c_str());
  }
}

// FL Studio's own demo projects (only when FL Studio is installed).
void testFlDemos() {
  const std::string root = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Demo projects/";
  const char* demos[] = {"Product demos/Olbaid - Harmless Essentials.flp", "Product demos/nucleon - Fruity Delay 3.flp",
                         "Product demos/Vocodex demo.flp"};
  std::error_code ec;
  int done = 0;
  for (const char* d : demos) {
    const std::string path = root + d;
    if (!std::filesystem::exists(path, ec)) continue;
    Song s;
    flp::Report rep;
    std::string err;
    if (!importFlp(path, s, rep, &err)) {
      expect(false, ("import " + std::string(d)).c_str());
      continue;
    }
    // The first 6 seconds (at the start tempo).
    const Tick limit = static_cast<Tick>(6.0 * s.tempo / 60.0 * s.ppq);
    s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [&](const Clip& c) { return c.start >= limit; }), s.playlist.end());
    for (auto& c : s.playlist)
      if (c.start + s.clipLength(c) > limit) c.length = limit - c.start;
    const AudioBuffer a = renderFixed(s, 512, s.ppq);
    double diff = 0;
    for (int b : {333, 2048, 32}) diff = std::max(diff, maxDiff(a, renderFixed(s, b, s.ppq)));
    if (diff > 0) std::printf("# %s: max diff %g\n", d, diff);
    expect(peakOf(a) > 1e-3 && diff == 0, (std::string(d) + ": offline render identical at block sizes 512 / 333 / 2048 / 32").c_str());
    ++done;
  }
  if (done == 0) std::printf("# FL Studio demo projects not installed: skipped\n");
}

// ------------------------------------------------------------- built-in plugins

constexpr double kSr = 44100;
constexpr int kLen = 33075;  // 0.75 s

struct NoteE {
  int at;
  bool on;
  uint32_t id;
  double pitch;
};
const NoteE kNotes[] = {{300, true, 1, 60},    {2777, true, 2, 64},   {2777, true, 3, 67},  {9000, false, 1, 0},
                        {12011, true, 4, 48},  {15011, false, 4, 0},  {17777, false, 2, 0}, {17777, false, 3, 0},
                        {20000, true, 5, 72},  {20001, true, 6, 36},  {26000, false, 5, 0}, {30000, false, 6, 0}};

std::vector<float> input(int ch) {
  std::vector<float> v(kLen);
  uint32_t s = 1234u + static_cast<uint32_t>(ch) * 77u;
  for (int i = 0; i < kLen; ++i) {
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    const double nz = static_cast<double>(static_cast<int32_t>(s)) / 2147483648.0;
    double x = 0;
    if (i < 8000) x = 0.4 * nz;
    else if (i < 20000) x = 0.5 * std::sin(2 * 3.141592653589793 * 220 * i / kSr) * (i % 4000 < 2000 ? 1.0 : 0.2);
    else if (i < 26000) x = 0.7 * nz * std::exp(-(i - 20000) / 1500.0);
    v[static_cast<size_t>(i)] = static_cast<float>(x);
  }
  return v;
}

// Every continuous parameter moves at each 64-sample control-grid point (the
// engine applies links and automation there).
void automate(const PluginInfo& info, const PluginState& base, PluginState& st, int pos) {
  for (size_t k = 0; k < info.params.size() && k < st.params.size(); ++k) {
    const ParamDesc& d = info.params[k];
    if (d.kind != ParamKind::Continuous) continue;
    const double n0 = d.toNormalized(base.params[k]);
    const double n = std::clamp(n0 + 0.12 * std::sin(2 * 3.141592653589793 * pos / (0.5 * kSr) + 0.37 * static_cast<double>(k)), 0.0, 1.0);
    st.params[k] = static_cast<float>(d.clamp(d.fromNormalized(n)));
  }
}

struct Run {
  std::vector<float> L, R;
};

Run run(const PluginInfo& info, const PluginState& st0, int block, bool autoParams) {
  PluginState st = st0;
  Run r;
  const bool gen = info.kind == PluginKind::Generator;
  std::unique_ptr<Generator> g;
  std::unique_ptr<Effect> fx;
  ChannelAux aux;
  if (gen) {
    g = createGenerator(st.type);
    r.L.assign(kLen, 0.f), r.R.assign(kLen, 0.f);
    if (!g) return r;
    prepareChannelAux(st, aux);
    g->prepareHost(st, kSr);
  } else {
    fx = createEffect(st.type);
    r.L = input(0), r.R = input(1);
    if (!fx) return r;
    packs::prepareAux(st, aux);
    fx->prepareHost(st, kSr);
  }
  size_t ei = 0;
  ProcessContext c;
  c.sampleRate = kSr, c.tempo = 128, c.ppq = 96, c.playing = true, c.offline = true;
  for (int pos = 0; pos < kLen;) {
    int n = std::min(block, kLen - pos);
    if (autoParams) {
      n = std::min(n, kControlBlock - pos % kControlBlock);
      if (pos % kControlBlock == 0) automate(info, st0, st, pos);
    }
    c.tick = pos * (c.tempo * c.ppq / 60.0) / kSr;
    c.jumped = pos == 0;
    if (gen) {
      while (ei < std::size(kNotes) && kNotes[ei].at < pos + n) {
        const NoteE& e = kNotes[ei++];
        if (e.on) g->noteOn(e.id, e.pitch, 0.8f, e.at - pos);
        else g->noteOff(e.id, e.at - pos);
      }
      g->process(st, aux, c, r.L.data() + pos, r.R.data() + pos, n);
    } else {
      fx->processAux(st, aux, c, r.L.data() + pos, r.R.data() + pos, n);
    }
    pos += n;
  }
  return r;
}

double diffOf(const Run& a, const Run& b) {
  double d = 0;
  for (size_t i = 0; i < a.L.size(); ++i) {
    const double e = std::max(std::fabs(static_cast<double>(a.L[i]) - b.L[i]), std::fabs(static_cast<double>(a.R[i]) - b.R[i]));
    if (!(e <= d)) d = std::isfinite(e) ? e : 1e9;
  }
  return d;
}

void testPlugins() {
  struct Job {
    const PluginInfo* info;
    PluginState st;
    std::string name;
    double plain = 0, automated = 0;
  };
  std::vector<Job> jobs;
  for (const PluginInfo& info : pluginCatalog()) {
    if (info.kind == PluginKind::NoteFx || isHostedType(info.type)) continue;
    if (const char* only = std::getenv("GL_DET_TYPE"); only && info.type != only) continue;  // debugging aid
    for (size_t p = 0; p <= info.presets.size(); ++p) {
      Job j{&info, p == 0 ? defaultPluginState(info.type) : presetState(info, p - 1), info.type, 0, 0};
      if (p) j.name += " / " + info.presets[p - 1].name;
      if (info.usesSample && j.st.data.empty()) j.st.data = "builtin:snare";
      jobs.push_back(std::move(j));
    }
  }
  // Warm-up on this thread (lazily built tables) before the parallel runs.
  for (const PluginInfo& info : pluginCatalog())
    if (info.kind != PluginKind::NoteFx && !isHostedType(info.type)) {
      PluginState st = defaultPluginState(info.type);
      if (info.usesSample) st.data = "builtin:snare";
      (void)run(info, st, 512, false);
    }
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (size_t k; (k = next.fetch_add(1)) < jobs.size();) {
      Job& j = jobs[k];
      const Run ref = run(*j.info, j.st, 512, false);
      for (int b : {333, 37, 1024}) j.plain = std::max(j.plain, diffOf(ref, run(*j.info, j.st, b, false)));
      const Run aref = run(*j.info, j.st, 512, true);
      for (int b : {333, 37}) j.automated = std::max(j.automated, diffOf(aref, run(*j.info, j.st, b, true)));
    }
  };
  const unsigned nt = std::getenv("GL_DET_SERIAL") ? 1u : std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < nt; ++t) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
  int bad = 0;
  for (const Job& j : jobs) {
    const bool ok = j.plain <= 1e-6 && j.automated <= 1e-6;
    if (!ok) {
      ++bad;
      std::printf("# %s: max diff %g (automated %g)\n", j.name.c_str(), j.plain, j.automated);
      expect(false, (j.name + ": block-size invariant within -120 dB").c_str());
    }
  }
  std::printf("# %zu instrument / effect states checked at block sizes 333 / 37 / 1024 (+ automated 333 / 37)\n", jobs.size());
  expect(jobs.size() > 300 && bad == 0, "every built-in instrument and effect state is block-size invariant within -120 dB");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("determinism");
  if (std::getenv("GL_DET_TYPE")) {  // debugging aid: one plugin type only
    testPlugins();
    return finish();
  }
  testDemoSong();
  testEngineFeatures();
  testPlugins();
  testFlDemos();
  return finish();
}
