// Gridline - GitHub #87: slide notes glide the velocity of each voice of the drum kit (gl.drumkit),
// the drum voices (gl.kick, gl.snare, gl.hat, gl.clap) and Seltmann (gl.seltmann) on its own: a
// slide on one note fades that note only, never the other notes still sounding on the channel (the
// whole-output fade used to duck them). Also: a glide to the note's own velocity changes nothing
// (bit for bit), and gliding allocates nothing on the audio thread.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "../Check.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/models/models.h"
#include "../../src/packs/seltmann/seltmann.h"

namespace {
std::atomic<bool> gCounting{false};
std::atomic<long> gAllocs{0};
}  // namespace
void* operator new(std::size_t n) {
  if (gCounting.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gCounting.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;
namespace md = gl::packs::models;
namespace sm = gl::packs::seltmann;

namespace {

constexpr int kSr = 44100, kBlock = 256;

struct Setup {
  std::string type;
  PluginState state;
  ChannelAux aux;
  double keyA = 60, keyB = 62;  // two notes that sound together on separate voices
};

struct Out {
  std::vector<float> L, R;
};

// What happens to note B (A always plays): none, a glide to `vel` at frame kBlock over `secs`.
struct Glide {
  bool b = true, glide = false;
  float vel = 0;
  double secs = 0.02;
};

Out play(const Setup& s, const Glide& g, int64_t frames, bool count = false) {
  auto gen = packs::createGenerator(s.type);
  if (!gen) gen = createGenerator(s.type);
  Out o;
  o.L.assign(static_cast<size_t>(frames), 0.f);
  o.R.assign(static_cast<size_t>(frames), 0.f);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = 120;
  ctx.ppq = 96;
  for (int64_t pos = 0; pos < frames; pos += kBlock) {
    const int n = static_cast<int>(std::min<int64_t>(kBlock, frames - pos));
    ctx.tick = static_cast<double>(pos) / kSr * 2.0 * ctx.ppq;
    ctx.jumped = pos == 0;
    if (pos == 0) {
      gen->noteOn(1, s.keyA, 1.0f, 0);
      if (g.b) gen->noteOn(2, s.keyB, 1.0f, 0);
    }
    if (g.glide && pos == kBlock) gen->glideVelocity(2, g.vel, g.secs, 0);
    if (count && pos > 0) gCounting = true;
    gen->process(s.state, s.aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
    gCounting = false;
  }
  return o;
}

double rms(const Out& o, double t0, double t1) {
  const size_t a = static_cast<size_t>(t0 * kSr), b = std::min(o.L.size(), static_cast<size_t>(t1 * kSr));
  double e = 0;
  for (size_t i = a; i < b; ++i) e += 0.5 * (static_cast<double>(o.L[i]) * o.L[i] + static_cast<double>(o.R[i]) * o.R[i]);
  return b > a ? std::sqrt(e / static_cast<double>(b - a)) : 0;
}

double rmsDiff(const Out& x, const Out& y, double t0, double t1) {
  const size_t a = static_cast<size_t>(t0 * kSr), b = std::min({x.L.size(), y.L.size(), static_cast<size_t>(t1 * kSr)});
  double e = 0;
  for (size_t i = a; i < b; ++i) {
    const double l = static_cast<double>(x.L[i]) - y.L[i], r = static_cast<double>(x.R[i]) - y.R[i];
    e += 0.5 * (l * l + r * r);
  }
  return b > a ? std::sqrt(e / static_cast<double>(b - a)) : 0;
}

bool same(const Out& x, const Out& y) { return x.L == y.L && x.R == y.R; }

void check(const Setup& s, double t0, double t1) {
  const std::string t = s.type + ": ";
  const int64_t frames = static_cast<int64_t>(t1 * kSr) + kBlock;
  const Out a = play(s, {false, false, 0, 0}, frames);
  const Out ab = play(s, {true, false, 0, 0}, frames);
  const Out faded = play(s, {true, true, 0.0f, 0.02}, frames);
  const Out own = play(s, {true, true, 1.0f, 0.02}, frames);
  const double la = rms(a, t0, t1), lab = rms(ab, t0, t1), lf = rms(faded, t0, t1), d = rmsDiff(faded, a, t0, t1);
  std::printf("# %-14s A alone %.5f, A+B %.5f, B faded out %.5f (differs from A alone by %.2e)\n", s.type.c_str(), la, lab, lf, d);
  expect(la > 1e-3 && lab > la * 1.05, (t + "both notes sound after the slide's time (A on its own, and B adds to it)").c_str());
  expect(d < 1e-3 * la, (t + "a slide fading B out leaves A exactly as loud as A alone (it is not ducked)").c_str());
  expect(same(own, ab), (t + "a glide to the note's own velocity renders bit for bit as no glide").c_str());
  gAllocs = 0;
  play(s, {true, true, 0.0f, 0.02}, frames, true);
  const long n = gAllocs.load();
  expect(n == 0, (t + "gliding allocates nothing while rendering (" + std::to_string(n) + ")").c_str());
}

Setup drumkit() {
  Setup s{"gl.drumkit", defaultPluginState("gl.drumkit"), {}, 60, 62};
  s.state.params[md::DmLimiter] = 0;  // the kit's limiter would couple the pads
  for (int pad : {0, 2}) {  // two ringing pads, no choke between them
    s.state.params[static_cast<size_t>(md::drumPad(pad, md::DpMute))] = 0;
    s.state.params[static_cast<size_t>(md::drumPad(pad, md::DpLevel))] = 0.8f;
    s.state.params[static_cast<size_t>(md::drumPad(pad, md::DpDecay))] = 0.6f;
    s.state.params[static_cast<size_t>(md::drumPad(pad, md::DpChoke))] = 0;
  }
  return s;
}

Setup drum(const std::string& type) {
  Setup s{type, defaultPluginState(type), {}, 60, 64};
  if (type == "gl.hat") s.state.params[pp::HatChoke] = 0;  // a new hit chokes the ringing ones otherwise
  return s;
}

Setup seltmann(const std::string& path) {
  const double rate = kSr;
  auto src = std::make_shared<Sample>();
  src->sampleRate = rate;
  std::vector<float> L(static_cast<size_t>(rate * 8));
  for (size_t i = 0; i < L.size(); ++i) L[i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * 220.0 * static_cast<double>(i) / rate));
  src->left = L;
  src->right = L;
  expect(sm::writeLoopWav(path, L, L, rate), "seltmann: write the source");
  sm::Model m;
  m.srcFrames = static_cast<int64_t>(src->frames());
  m.srcRate = rate;
  m.grid = sm::flatGrid(*src, 120);
  m.chop.mode = sm::ChopGrid;
  m.chop.division = sm::Div1Bar;
  m.chop.zeroCross = false;
  sm::setRegionBars(m, 1, 4);
  sm::rechop(m, src.get());
  sm::straightPattern(m);
  Setup s{sm::kType, defaultPluginState(sm::kType), {}, 60, 61};
  s.state.data = sm::formatData(path, m);
  s.state.params[sm::SmSync] = static_cast<float>(sm::SyncResample);
  s.state.params[sm::SmMono] = 0;
  expect(packs::prepareAux(s.state, s.aux) && s.aux.custom, "seltmann: prepared data");
  return s;
}

}  // namespace

int main() {
  check(drumkit(), 0.06, 0.16);
  for (const char* d : {"gl.kick", "gl.snare", "gl.hat", "gl.clap"}) check(drum(d), 0.04, 0.09);
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "gl_gh87_voices";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  check(seltmann((dir / "sine.wav").string()), 0.1, 0.4);
  std::filesystem::remove_all(dir, ec);
  return finish();
}
