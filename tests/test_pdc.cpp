// Gridline — plugin delay compensation, parallel mixer graph, CPU profiling.
//
//  - latent paths line up exactly with parallel ones at every summing point
//    (sends to the master, sidechain inputs, bus send → return);
//  - offline renders with latent effects are sample-identical to the same
//    song without them;
//  - latency changes while running cross-fade and settle on exact alignment;
//  - any processing thread count gives bit-identical output (demo song and
//    converted FL projects when present locally);
//  - profiling counters fill when enabled.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/core/Demo.h"
#include "../src/core/Document.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/Song.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Effects.h"
#include "../src/engine/Render.h"
#include "Check.h"

using namespace gl;

namespace {

// ---- synthetic effects -------------------------------------------------------

// Pure delay by params[0] samples, reported as its latency.
class TestDelay final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const int d = s.params.empty() ? 0 : static_cast<int>(s.params[0]);
    for (int i = 0; i < n; ++i) {
      l_[w_ & kMask] = L[i];
      r_[w_ & kMask] = R[i];
      L[i] = l_[(w_ - static_cast<size_t>(d)) & kMask];
      R[i] = r_[(w_ - static_cast<size_t>(d)) & kMask];
      ++w_;
    }
  }
  void reset() override {}
  int latencyFor(const PluginState& s, double) const override { return s.params.empty() ? 0 : static_cast<int>(s.params[0]); }

 private:
  static constexpr size_t kMask = (1u << 16) - 1;
  std::vector<float> l_ = std::vector<float>(kMask + 1, 0.f), r_ = std::vector<float>(kMask + 1, 0.f);
  size_t w_ = 0;
};

class TestInvert final : public Effect {
 public:
  void process(const PluginState&, const ProcessContext&, float* L, float* R, int n) override {
    for (int i = 0; i < n; ++i) {
      L[i] = -L[i];
      R[i] = -R[i];
    }
  }
  void reset() override {}
};

// Leaves the audio alone; meter() = onset of the sidechain minus onset of the
// main input (samples), once both were seen.
class TestScProbe final : public Effect {
 public:
  void setSideIo(const SideIo& io) override { io_ = io; }
  void process(const PluginState&, const ProcessContext&, float* L, float* R, int n) override {
    for (int i = 0; i < n; ++i, ++t_) {
      if (main_ < 0 && std::fabs(L[i]) + std::fabs(R[i]) > 1e-4f) main_ = t_;
      if (sc_ < 0 && io_.scL && std::fabs(io_.scL[i]) + std::fabs(io_.scR[i]) > 1e-4f) sc_ = t_;
    }
  }
  void reset() override {}
  float meter() const override { return main_ >= 0 && sc_ >= 0 ? static_cast<float>(sc_ - main_) : -99999.f; }

 private:
  SideIo io_;
  int64_t t_ = 0, main_ = -1, sc_ = -1;
};

std::unique_ptr<Effect> testFactory(const std::string& type) {
  if (type == "test.delay") return std::make_unique<TestDelay>();
  if (type == "test.invert") return std::make_unique<TestInvert>();
  if (type == "test.scprobe") return std::make_unique<TestScProbe>();
  return nullptr;
}

PluginState fx(const std::string& type, std::vector<float> params = {}) {
  PluginState s = type.compare(0, 5, "test.") == 0 ? PluginState{} : defaultPluginState(type);
  s.type = type;
  for (size_t i = 0; i < params.size(); ++i) {
    if (s.params.size() <= i) s.params.resize(i + 1, 0.f);
    s.params[i] = params[i];
  }
  return s;
}

// ---- fixtures ----------------------------------------------------------------

// Two identical kick channels on inserts 1 and 2 (kick on every step: a
// continuous, deterministic signal). Inserts 1..3 send to the master.
Song twinSong() {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  const Song& s = d.song();
  ObjectId pat = s.patterns[0].id;
  ObjectId a = d.addChannel("a", "gl.kick");
  ObjectId b = d.addChannel("b", "gl.kick");
  d.routeChannel(a, s.mixer[1].id);
  d.routeChannel(b, s.mixer[2].id);
  for (int st = 0; st < 16; ++st) {
    d.setStep(pat, a, st, 120);
    d.setStep(pat, b, st, 120);
  }
  d.addClip(pat, 0, 0);
  // Drum voices seed their click noise from the note id, which differs per
  // channel: switch the click off so both kicks are sample-identical.
  Song out = d.song();
  for (auto& c : out.channels)
    if (c.plugin.type == "gl.kick" && c.plugin.params.size() > pp::KickClick) c.plugin.params[pp::KickClick] = 0.f;
  return out;
}

struct Runner {
  ProgramBuilder builder;
  AudioEngine engine;
  Runner(const Song& s, int threads, bool pdc = true) : engine(44100) {
    builder.setSampleRate(44100);
    builder.setPdc(pdc);
    engine.setThreads(threads);
    engine.setProgram(builder.build(s));
    engine.setMode(PlayMode::Pattern);
    engine.setPattern(s.patterns[0].id);
    engine.play();
  }
  void update(const Song& s) { engine.setProgram(builder.build(s)); }
  // Appends `frames` frames (L only) rendered in `block`-frame calls.
  void run(std::vector<float>& outL, std::vector<float>& outR, int frames, int block = 256) {
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
    for (int done = 0; done < frames; done += block) {
      int n = std::min(block, frames - done);
      engine.process(L.data(), R.data(), n);
      outL.insert(outL.end(), L.begin(), L.begin() + n);
      outR.insert(outR.end(), R.begin(), R.begin() + n);
    }
  }
};

float peak(const std::vector<float>& x, size_t from = 0, size_t to = SIZE_MAX) {
  float p = 0;
  for (size_t i = from; i < x.size() && i < to; ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}

bool sameBuffers(const AudioBuffer& a, const AudioBuffer& b) {
  if (a.channels != b.channels || a.frames != b.frames) return false;
  for (size_t c = 0; c < a.channels; ++c)
    if (std::memcmp(a.plane(c), b.plane(c), sizeof(float) * a.frames) != 0) return false;
  return true;
}
size_t firstDiff(const AudioBuffer& a, const AudioBuffer& b) {
  for (size_t i = 0; i < std::min(a.frames, b.frames); ++i)
    for (size_t c = 0; c < std::min(a.channels, b.channels); ++c)
      if (a.at(c, i) != b.at(c, i)) return i;
  return SIZE_MAX;
}

}  // namespace

int main() {
  setTestEffectFactory(&testFactory);

  // ---- 1. latent insert + parallel path cancel exactly at the master ----
  {
    Song s = twinSong();
    s.mixer[1].slots[0].plugin = fx("test.delay", {300});
    s.mixer[2].invertPolarity = true;  // insert 2 = −(insert 1) once aligned
    std::vector<float> L, R;
    Runner on(s, 1, true);
    on.run(L, R, 44100);
    const Program& p = *on.engine.program();
    expect(p.mixer[1].latOwn == 300 && p.mixer[1].latOut() == 300, "insert latency: own effects summed");
    expect(p.mixer[static_cast<size_t>(p.master)].latIn == 300 && p.outputLatency == 300, "master input latency = slowest path");
    bool tap = false;
    for (const auto& in : p.mixer[static_cast<size_t>(p.master)].inputs)
      if (in.kind == ProgInput::Send && in.src == 2) tap = in.tap.delay == 300;
    expect(tap, "parallel insert compensated by 300 samples");
    std::vector<float> refL, refR;
    Runner ref(twinSong(), 1, true);
    ref.run(refL, refR, 44100);
    expect(peak(refL) > 0.05f, "fixture makes sound");
    expect(peak(L) == 0.f && peak(R) == 0.f, "PDC: latent and parallel paths cancel exactly");
    std::vector<float> offL, offR;
    Runner off(s, 1, false);
    off.run(offL, offR, 44100);
    expect(peak(offL) > 0.01f, "PDC off: paths are misaligned (no cancellation)");
    expect(off.engine.program()->outputLatency == 0 && off.engine.program()->mixer[1].latOwn == 300,
           "PDC off: latency still reported, output not offset");
    // Parallel graph gives the same.
    std::vector<float> pL, pR;
    Runner par(s, 4, true);
    par.run(pL, pR, 44100);
    expect(peak(pL) == 0.f, "PDC with 4 threads: exact cancellation");
  }

  // ---- 2. chains of latency: insert → insert → master, plus a direct path ----
  {
    Song s = twinSong();
    ObjectId m = s.masterId(), i3 = s.mixer[3].id;
    s.mixer[1].sends = {{i3, 1.0f, false}};
    s.mixer[1].slots[0].plugin = fx("test.delay", {100});
    s.mixer[3].slots[2].plugin = fx("test.delay", {57});
    s.mixer[3].volume = s.mixer[2].volume;
    s.mixer[1].volume = 1.0f;  // gain path: insert 1 (1.0 taper) → insert 3 (0.8) vs insert 2 (0.8)
    s.mixer[2].invertPolarity = true;
    (void)m;
    Runner r(s, 1, true);
    const Program& p = *r.engine.program();
    expect(p.mixer[3].latIn == 100 && p.mixer[3].latOut() == 157, "insert 3: 100 in + 57 own");
    expect(p.outputLatency == 157, "master output latency follows the longest chain");
    int d2 = -1;
    for (const auto& in : p.mixer[static_cast<size_t>(p.master)].inputs)
      if (in.src == 2) d2 = in.tap.delay;
    expect(d2 == 157, "direct insert delayed by the whole chain");
  }

  // ---- 3. sidechain inputs are compensated ----
  {
    Song s = twinSong();
    s.mixer[1].slots[0].plugin = fx("test.delay", {64});
    s.mixer[1].sends.push_back({s.mixer[2].id, 1.0f, true});  // insert 1 → insert 2 sidechain
    s.mixer[2].slots[0].plugin = fx("test.scprobe");
    for (bool pdc : {true, false}) {
      Runner r(s, 1, pdc);
      std::vector<float> L, R;
      r.run(L, R, 8192);
      auto rt = r.builder.effectRuntime(s.mixer[2].id, 0);
      float lag = rt ? rt->fx->meter() : -1;
      if (pdc) {
        expect(r.engine.program()->mixer[2].latIn == 64, "sidechain source latency raises the input latency");
        expect(lag == 0.f, "PDC: sidechain arrives aligned with the main input");
      } else {
        expect(lag == 64.f, "PDC off: sidechain lags by the source's latency");
      }
    }
  }

  // ---- 4. bus send → bus return is compensated to the return slot ----
  {
    Song s = twinSong();
    s.mixer[1].slots[0].plugin = fx("test.delay", {200});
    s.mixer[1].slots[1].plugin = fx("gl.bussend", {0, 1, 0});  // bus 0, level 1, dry 0
    s.mixer[2].slots[0].plugin = fx("test.invert");
    s.mixer[2].slots[1].plugin = fx("gl.busreturn", {0, 1, 1});  // −x + bus
    Runner r(s, 1, true);
    std::vector<float> L, R;
    r.run(L, R, 44100);
    const Program& p = *r.engine.program();
    expect(p.mixer[2].latIn == 200 && p.mixer[2].slots[1].auxIn.size() == 1, "bus return input: one sender, 200 samples");
    expect(peak(L) == 0.f, "PDC: bus return sums aligned (exact cancellation)");
    Runner off(s, 1, false);
    std::vector<float> oL, oR;
    off.run(oL, oR, 44100);
    expect(peak(oL) > 0.01f, "PDC off: bus return misaligned");
  }

  // ---- 5. send effect (slot send) mid-chain ----
  {
    Song s = twinSong();
    s.mixer[1].slots[0].plugin = fx("test.delay", {90});
    s.mixer[1].slots[1].plugin = fx("gl.send", {3, 1, 0, 0});  // to insert 3, level 1, pan 0, dry 0
    s.mixer[1].slots[2].plugin = fx("test.delay", {40});          // after the send: does not delay it
    Runner r(s, 1, true);
    const Program& p = *r.engine.program();
    bool found = false;
    for (const auto& in : p.mixer[3].inputs)
      if (in.kind == ProgInput::SlotSend && in.src == 1) found = true;
    expect(found, "slot send becomes an input of its destination");
    expect(p.mixer[3].latIn == 90, "slot send arrives with the latency before it");
    expect(p.mixer[1].latOut() == 130 && p.outputLatency == 130, "track output adds the effects after the send");
  }

  // ---- 6. dynamic latency change: cross-fade, then exact alignment again ----
  {
    Song s = twinSong();
    s.mixer[2].invertPolarity = true;
    s.mixer[1].slots[0].plugin = fx("test.delay", {0});
    Runner r(s, 1, true);
    std::vector<float> L, R;
    r.run(L, R, 22050);
    expect(peak(L) == 0.f, "dynamic: aligned before the change");
    bool ok = true;
    for (int d : {200, 50, 5000, 0}) {
      s.mixer[1].slots[0].plugin.params[0] = static_cast<float>(d);
      r.update(s);
      size_t from = L.size();
      r.run(L, R, 22050 + d);
      // the latent effect itself jumps (its own business); the compensation
      // settles once it has history: d + fade + one block
      size_t settle = from + static_cast<size_t>(d) + PdcDelay::kFade + 512;
      float after = peak(L, settle);
      std::printf("# latency %d: peak after settle %.3g, max %.3g\n", d, after, peak(L, from));
      ok = ok && after == 0.f && peak(L, from) < 4.0f;
      int lineDelay = -1;
      for (const auto& in : r.engine.program()->mixer[static_cast<size_t>(r.engine.program()->master)].inputs)
        if (in.src == 2 && in.tap.line) lineDelay = in.tap.line->current();
      ok = ok && lineDelay == d;
    }
    expect(ok, "dynamic latency (0→200→50→5000 (grow)→0): settles to exact alignment, bounded transition");
  }

  // ---- 7. offline render: latent song is sample-identical to the latency-free one ----
  {
    Song plain = twinSong();
    plain.mixer[3].slots[0].plugin = fx("gl.reverb");  // something stateful on a third path
    Song latent = plain;
    latent.mixer[1].slots[0].plugin = fx("test.delay", {333});
    latent.mixer[static_cast<size_t>(latent.mixerIndex(latent.masterId()))].slots[9].plugin = fx("test.delay", {77});
    RenderOptions o;
    o.blockSize = 512;
    o.tail = 96;
    AudioBuffer a = renderSong(plain, o), b = renderSong(latent, o);
    expect(a.frames == b.frames && a.frames > 44100, "render: same length");
    expect(sameBuffers(a, b), "render: latent song sample-aligned with the latency-free render (bit-exact)");
    o.blockSize = 97;
    AudioBuffer c = renderSong(latent, o);
    expect(sameBuffers(b, c), "render: compensation independent of the block size");
    o.threads = 6;
    o.blockSize = 512;
    AudioBuffer d = renderSong(latent, o);
    expect(sameBuffers(b, d), "render: 6 threads bit-identical");
  }

  // ---- 8. parallel graph is bit-identical to serial ----
  auto checkParallel = [](const Song& song, const char* name, bool bench) {
    RenderOptions o;
    o.blockSize = 512;
    std::vector<int> counts = {1, 2, 4, 8};
    AudioBuffer ref;
    double t1 = 0;
    bool ok = true;
    for (int th : counts) {
      o.threads = th;
      auto t0 = std::chrono::steady_clock::now();
      AudioBuffer out = renderSong(song, o);
      double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (th == 1) {
        ref = std::move(out);
        t1 = sec;
      } else {
        bool same = sameBuffers(ref, out);
        if (!same) std::printf("# %s: %d threads differ at frame %zu\n", name, th, firstDiff(ref, out));
        ok = ok && same;
      }
      if (bench)
        std::printf("# %s: %d thread(s) %.2f s render (%.2fx), %zu frames\n", name, th, sec, t1 > 0 ? t1 / sec : 1.0,
                    ref.frames);
    }
    return ok && ref.frames > 0;
  };
  {
    Song demo = makeDemoSong();
    expect(checkParallel(demo, "demo", true), "demo song: 2/4/8 threads bit-identical to 1");
    // Small blocks (and the 64-sample control grid of linked songs) also agree.
    RenderOptions o;
    o.blockSize = 64;
    o.threads = 1;
    AudioBuffer a = renderSong(demo, o);
    o.threads = 4;
    AudioBuffer b = renderSong(demo, o);
    expect(sameBuffers(a, b), "demo song, 64-frame blocks: 4 threads bit-identical");
  }
  {
    namespace fs = std::filesystem;
    const char* env = std::getenv("GRIDLINE_CONVERTED_DIR");
    fs::path dir = env ? fs::path(env) : fs::path();  // a folder of converted FL projects (optional)
    int tested = 0;
    std::error_code ec;
    if (fs::is_directory(dir, ec))
      for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".json" || e.path().filename().string().find(" - ") == std::string::npos) continue;
        Song song;
        std::string err;
        if (!loadProject(e.path().string(), song, &err)) {
          std::printf("# skip %s: %s\n", e.path().filename().string().c_str(), err.c_str());
          continue;
        }
        const std::string name = e.path().stem().string();
        bool ok = checkParallel(song, name.c_str(), true);
        expect(ok, ("converted FL project bit-identical across thread counts: " + name).c_str());
        ++tested;
        if (!std::getenv("GRIDLINE_PDC_ALL")) break;  // one project by default (renders take a while)
      }
    if (!tested) std::printf("# no converted FL projects found (set GRIDLINE_CONVERTED_DIR)\n");
  }

  // ---- 9. profiling counters ----
  {
    Song s = twinSong();
    s.mixer[1].slots[0].plugin = fx("gl.reverb");
    Runner r(s, 2, true);
    r.engine.setProfiling(true);
    std::vector<float> L, R;
    r.run(L, R, 44100);
    auto ch = r.builder.channelRuntime(s.channels[0].id);
    auto mx = r.builder.mixerRuntime(s.mixer[1].id);
    auto sl = r.builder.effectRuntime(s.mixer[1].id, 0);
    expect(ch && ch->cpu.load() > 0 && mx && mx->cpu.load() > 0 && sl && sl->cpu.load() > 0,
           "profiling: channel, insert and slot CPU time measured");
    uint64_t before = sl->cpu.load();
    r.engine.setProfiling(false);
    r.run(L, R, 4096);
    expect(sl->cpu.load() == before, "profiling off: counters untouched");
  }

  setTestEffectFactory(nullptr);
  return finish();
}
