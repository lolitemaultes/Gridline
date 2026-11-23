// Gridline — "seltmann" pack: Seltmann runtime. Pack entry points, the data the
// audio thread reads (SeltmannAux, built on the UI thread), the warp service
// (the region time-stretched to the project tempo on a worker thread, handed
// over through an atomic pointer), the instrument (slices on keys, the
// pattern on the octave below, voices with declick fades, ADSR, choke groups,
// mono, stutter, swing) and the offline renders (loops, slices, spans, notes).
//
// Audio thread: no allocation, no locks, no I/O. Voices are a fixed array; the
// aux data never changes once built; warp renders are retired with a grace
// period long after the audio thread could still read them.
//
// GitHub #80: a voice reads a render, or the file itself, for as long as it sounds, which can
// be long after the data it started under was replaced (a layout, Analyse, another file) and
// freed. A sounding voice counts itself as a reader of what it reads (Render::readers,
// WarpFamily::srcReaders, which holds the file); the worker never frees a render or a family
// while it has readers, whatever its age.
#include "../Packs.h"
#include "../../core/ThreadName.h"
#include "../../engine/SampleTap.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include "../../core/AudioBuffer.h"
#include "../../core/Platform.h"
#include "../../core/MidiFile.h"
#include "../../core/Song.h"
#include "../../core/WavWriter.h"
#include "../../engine/TimeStretch.h"
#include "seltmann.h"
#include "seltmann_editor.h"

namespace gl::packs::seltmann {

namespace {
std::atomic<bool> gWholeOffline{false};
}  // namespace

void setWholeOfflineRenders(bool on) { gWholeOffline.store(on, std::memory_order_relaxed); }
bool wholeOfflineRenders() { return gWholeOffline.load(std::memory_order_relaxed); }

namespace {

float par(const PluginState& s, int i, float def) { return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : def; }
int ipar(const PluginState& s, int i, int def) { return static_cast<int>(std::lround(par(s, i, static_cast<float>(def)))); }
double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

// ================================================================== warp service
// 0.7.8: a stretched render covers a span of the source's beats. Every instance playing the same
// file on the same grid shares a family of renders, so a new sequence (a layout, a typed sequence,
// undo, a collaborator's edit) whose region lies inside a render the family already has plays it
// at once, at the right pitch: nothing waits for the worker. A slot renders its region first, then
// the region grown to kSpanBars bars (the longest layout), so that every layout keeping the
// region's first bar finds its slices ready.
inline constexpr int kSpanBars = 8;

// 0.7.8: while the tempo moves (a ramp, a dragged tempo), the worker makes a render once the nearest
// one is more than this far from the tempo, aimed this far ahead along a ramp, so renders come about
// 1% apart and a voice never plays one further off than this (the voices play the nearest render
// that much faster or slower). Before, it waited for the tempo to hold still and a slice that started
// meanwhile played the plain resampled source (a ramp: up to several semitones off).
inline constexpr double kLiveCents = 8;
inline constexpr double kMaxAheadCents = 300;  // (how far ahead a render is aimed at most)
// 0.7.8: a moving tempo's renders for one file are rate limited: they may take this share of the
// worker's time (a budget of stretching time that fills at that rate, up to kMovingBudgetSecs: a
// short burst is free), so a tempo dragged back and forth keeps the worker stretching for it about
// 40% of the time instead of a whole core. A steady ramp (each render aimed about as far ahead as the one
// before) may take more, so that its renders stay close enough together for every slice to play in
// tune.
inline constexpr double kMovingDuty = 0.4;
inline constexpr double kRampDuty = 0.7;
inline constexpr double kMovingBudgetSecs = 0.3;

using Clock = std::chrono::steady_clock;

// 0.7.8: the bytes of the renders the warp service holds (made, in use, or waiting out their grace
// period) and the most it held, the renders made while the tempo moves, those skipped for the memory
// cap, and the worker's time spent stretching.
std::atomic<int64_t> gWarpBytes{0}, gWarpPeak{0}, gWarpWorkNs{0};
std::atomic<int64_t> gWarpEvicted{0};  // (of them, those evicted for the memory cap, still in their grace period)
std::atomic<int> gWarpRenders{0}, gWarpMoving{0}, gWarpSkipped{0}, gWarpThrottled{0};

struct WarpSlot;

struct Render {
  Render() = default;
  Render(const Render&) = delete;
  Render& operator=(const Render&) = delete;
  ~Render() {
    if (counted) gWarpBytes.fetch_sub(counted, std::memory_order_relaxed);
    if (counted && evicted) gWarpEvicted.fetch_sub(counted, std::memory_order_relaxed);
  }
  double tempo = 0;
  double rate = 44100;
  double beatA = 0, beatB = 0;  // the source beats it covers (frame 0 = beatA)
  std::vector<float> L, R;
  // K-321: a Track Receipt pass that only measures (tap::readsOnly) needs where a voice reads, not
  // the stretched sound: a render with only its length (no audio, nothing stretched).
  bool lengthOnly = false;
  int64_t length = 0;  // frames (also before a render made in pieces has any)
  int64_t frames() const { return length; }
  // GitHub #80: the voices reading it now (audio thread); never freed while above zero
  mutable std::atomic<int> readers{0};
  bool covers(double b0, double b1) const { return b0 >= beatA - 1e-6 && b1 <= beatB + 1e-6; }
  // 0.7.8: an offline render (an export) is made only as far as its voices read it (a tempo
  // automation meets many tempos, and the voices at each read a slice or two): `from` is the region
  // it stretches (null: made whole). L and R are sized on its first read and `pieces` makes them up
  // to the frame a voice needs, the same frames a whole render has. Never on the audio thread.
  std::shared_ptr<const WarpSlot> from;
  std::unique_ptr<stretch::MappedStream> pieces;
  bool partial() const { return from && (L.empty() || pieces); }
  int64_t ready() const { return !from ? frames() : L.empty() ? 0 : pieces ? pieces->done() : frames(); }
  // 0.7.8 (live renders, the warp service): a voice started on it since the worker last looked
  // (audio thread sets it); when that last was (worker only: the render read least recently leaves
  // first when the memory cap is reached); its bytes once the service holds it; evicted for the cap
  // (freed after a short grace period instead of the long one, still never while a voice reads it).
  mutable std::atomic<bool> touched{false};
  Clock::time_point lastUse{};
  int64_t counted = 0;
  bool evicted = false;
  int64_t bytes() const { return static_cast<int64_t>((L.size() + R.size()) * sizeof(float)); }
  void evict() {
    if (evicted) return;
    evicted = true;
    gWarpEvicted.fetch_add(counted, std::memory_order_relaxed);
  }
  void count() {
    if (counted) return;
    counted = std::max<int64_t>(1, bytes());
    const int64_t now = gWarpBytes.fetch_add(counted, std::memory_order_relaxed) + counted;
    int64_t peak = gWarpPeak.load(std::memory_order_relaxed);
    while (now > peak && !gWarpPeak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
    }
  }
};

// A source span to stretch: frames [a, b), beats [beatA, beatB) of the grid.
struct Span {
  int64_t a = 0, b = 0;
  double beatA = 0, beatB = 0;
};

// Output frame -> frame of the span (from its start) of a span's render.
struct SpanMap {
  const Analysis* g = nullptr;
  double beatA = 0, outBeat = 1;
  int64_t a = 0, n = 1;
  double operator()(int64_t j) const {
    return std::clamp(frameAt(*g, beatA + static_cast<double>(j) / outBeat) - static_cast<double>(a), 0.0, static_cast<double>(n - 1));
  }
};

std::unique_ptr<Render> makeRender(const SamplePtr& src, const Analysis& g, const Span& sp, double tempo, bool lengthOnly = false) {
  if (!src || sp.b <= sp.a || tempo <= 0) return nullptr;
  auto r = std::make_unique<Render>();
  r->tempo = tempo;
  r->rate = src->sampleRate;
  r->beatA = sp.beatA;
  r->beatB = sp.beatB;
  const double outBeat = 60.0 / tempo * r->rate;  // output frames per beat
  const int64_t outLen = std::max<int64_t>(1, static_cast<int64_t>(std::llround((sp.beatB - sp.beatA) * outBeat)));
  if (outLen > static_cast<int64_t>(r->rate * 60 * 12)) return nullptr;  // 12 minutes: refuse
  r->length = outLen;
  if (lengthOnly) {  // K-321 (see Render::lengthOnly)
    r->lengthOnly = true;
    return r;
  }
  r->L.assign(static_cast<size_t>(outLen), 0.f);
  r->R.assign(static_cast<size_t>(outLen), 0.f);
  const float* in[2] = {src->left.data() + sp.a, src->right.data() + sp.a};
  float* out[2] = {r->L.data(), r->R.data()};
  stretch::Settings st;
  st.mode = stretch::kStretch;  // J-153 / 0.7.8: one stretch for every stored mode
  st.pitch = 1;
  stretch::processMapped(in, 2, sp.b - sp.a, out, outLen, r->rate, st, SpanMap{&g, sp.beatA, outBeat, sp.a, sp.b - sp.a});
  return r;
}

using Retired = std::pair<Clock::time_point, std::unique_ptr<Render>>;

// The renders of one file on one grid, shared by every slot (region) that plays it.
struct WarpFamily {
  static constexpr int kRing = 16;  // (0.7.8: a moving tempo keeps the renders of the tempos it met last)
  // worker -> audio: the renders made so far (null = free). A render leaves the ring before it is
  // retired, and is freed long after (the audio thread reads one when a voice starts).
  std::array<std::atomic<const Render*>, kRing> ring{};
  // the tempo the family's instances asked for last (a new slot starts on it at once)
  std::atomic<double> tempo{0};
  SamplePtr src;
  // GitHub #80: the voices reading `src` itself now (audio thread); the family stays while above zero
  mutable std::atomic<int> srcReaders{0};
  // worker only
  std::vector<std::unique_ptr<Render>> owned;  // oldest first
  std::vector<Retired> retired;
  Clock::time_point idleSince{};
  bool idle = false;
  // 0.7.8 (the rate limit): the stretching time the renders made while the tempo moves may still
  // take (seconds), when it was last filled, the rate it fills at, how far ahead (cents) the last
  // one was aimed
  double movingBudget = kMovingBudgetSecs;
  Clock::time_point movingAt{};
  double movingDuty = kMovingDuty;
  double movingAhead = 0;
  // A finished render at `t` covering beats [b0, b1] (any thread; null none).
  const Render* find(double t, double b0, double b1) const {
    for (const auto& e : ring) {
      const Render* r = e.load(std::memory_order_acquire);
      if (r && std::fabs(r->tempo - t) < 1e-4 && r->covers(b0, b1)) return r;
    }
    return nullptr;
  }
  // 0.7.8: the finished render nearest in tempo to `t` covering beats [b0, b1] (any thread; null none).
  const Render* nearest(double t, double b0, double b1) const {
    const Render* best = nullptr;
    double bd = 0;
    for (const auto& e : ring) {
      const Render* r = e.load(std::memory_order_acquire);
      if (!r || !r->covers(b0, b1)) continue;
      const double d = std::fabs(std::log(r->tempo / t));
      if (!best || d < bd) best = r, bd = d;
    }
    return best;
  }
  // The bytes of its renders (worker only); `kept`: without those evicted (they go soon).
  int64_t bytes(bool kept = false) const {
    int64_t n = 0;
    for (const auto& o : owned) n += o->counted;
    for (const auto& r : retired)
      if (!kept || !r.second->evicted) n += r.second->counted;
    return n;
  }
};

struct WarpSlot {
  // inputs (immutable)
  SamplePtr src;
  Analysis grid;  // beats only
  int64_t a = 0, b = 0;  // the region
  double beatA = 0, beatB = 0;
  Span span;             // what the slot renders once its region is ready: the region grown to kSpanBars bars
  std::shared_ptr<WarpFamily> fam;  // null: a private slot (offline renders)
  // audio -> worker
  std::atomic<double> wanted{0};
  // worker -> audio: a render at the wanted tempo that covers the region
  std::atomic<const Render*> current{nullptr};
  // worker only
  double lastWanted = 0;
  int stable = 0;
  // 0.7.8: the tempo it wanted at the worker's last three wakeups (and when), how long a render of
  // its region takes (seconds)
  double seenPrev2 = 0, seenPrev = 0, seenNow = 0;
  Clock::time_point seenPrevAt{}, seenNowAt{};
  double renderSecs = 0.1;
  Span region() const { return Span{a, b, beatA, beatB}; }
};

// The span a slot renders: the region grown to kSpanBars bars from its first beat (back from the
// end of the file when the file ends sooner), so the layouts that keep the region's first bar fall
// inside it.
Span spanFor(const Analysis& g, int64_t nFrames, const Span& region) {
  Span s = region;
  const double want = kSpanBars * std::max(1, g.beatsPerBar);
  if (region.beatB - region.beatA >= want - 1e-6 || nFrames <= 0) return s;
  const double endBeat = beatAt(g, static_cast<double>(nFrames));
  const double b1 = std::min(endBeat, region.beatA + want);
  const double b0 = std::max(beatAt(g, 0.0), std::min(region.beatA, b1 - want));
  s.a = std::clamp<int64_t>(static_cast<int64_t>(std::floor(frameAt(g, b0))), 0, std::max<int64_t>(0, region.a));
  s.b = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(frameAt(g, b1))), region.b, std::max(region.b, nFrames));
  s.beatA = std::min(region.beatA, beatAt(g, static_cast<double>(s.a)));
  s.beatB = std::max(region.beatB, beatAt(g, static_cast<double>(s.b)));
  return s;
}

// 0.7.8: a render of a slot's region at `tempo` made in pieces, as its voices read it (offline).
std::unique_ptr<Render> renderInPieces(const std::shared_ptr<WarpSlot>& w, double tempo) {
  auto r = makeRender(w->src, w->grid, w->region(), tempo, true);  // (its length: nothing stretched yet)
  if (!r) return nullptr;
  r->lengthOnly = false;
  r->from = w;
  return r;
}

// Makes a render in pieces up to frame `upTo` (its buffers on the first call).
void renderUpTo(Render& r, int64_t upTo) {
  if (!r.partial()) return;
  const WarpSlot& w = *r.from;
  if (r.L.empty()) {
    r.L.assign(static_cast<size_t>(r.length), 0.f);
    r.R.assign(static_cast<size_t>(r.length), 0.f);
    const float* in[2] = {w.src->left.data() + w.a, w.src->right.data() + w.a};
    float* out[2] = {r.L.data(), r.R.data()};
    const double outBeat = 60.0 / r.tempo * r.rate;  // (as makeRender)
    r.pieces = std::make_unique<stretch::MappedStream>(in, 2, w.b - w.a, out, r.length, r.rate, SpanMap{&w.grid, w.beatA, outBeat, w.a, w.b - w.a});
  }
  r.pieces->renderTo(upTo);
  if (r.pieces->done() >= r.length) r.pieces.reset();
}

class WarpService {
 public:
  static WarpService& get() {
    static WarpService s;
    return s;
  }
  // The family of a file on a grid (prepared data without a region holds one too: it keeps the
  // file while a voice reads it).
  std::shared_ptr<WarpFamily> family(const std::string& famKey, const SamplePtr& src) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!thread_.joinable()) thread_ = std::thread([this] { gl::threads::setName("gl-warp"); loop(); });
    return familyLocked(famKey, src);
  }
  // The slot for a region (`key`) of a family (`famKey`): an existing one, else a new one made by
  // `make`. A new slot starts on the tempo its family plays at, so a render that covers it is
  // there at once, or the worker makes one right away (without waiting for the tempo to settle).
  std::shared_ptr<WarpSlot> slot(const std::string& famKey, const std::string& key, const std::function<std::shared_ptr<WarpSlot>()>& make) {
    std::shared_ptr<WarpSlot> s;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = slots_.find(key);
      if (it != slots_.end()) s = it->second.lock();
      if (!s) {
        s = make();
        auto fam = familyLocked(famKey, s->src);
        s->fam = fam;
        const double t = fam->tempo.load(std::memory_order_relaxed);
        if (t > 0) {
          s->wanted.store(t, std::memory_order_relaxed);
          s->seenNow = t;  // (its span follows its region at once)
          if (const Render* r = fam->find(t, s->beatA, s->beatB)) s->current.store(r, std::memory_order_release);
        }
        slots_[key] = s;
      }
      if (!thread_.joinable()) thread_ = std::thread([this] { gl::threads::setName("gl-warp"); loop(); });
    }
    cv_.notify_all();
    return s;
  }
  // How long a family nobody plays keeps its renders (switching back, undo, a layout tried again),
  // and how long a render that left the ring stays in memory at least (it also stays while a voice
  // reads it, GitHub #80). Tests shorten them (setWarpTimesForTest).
  static inline std::atomic<int64_t> lingerMs{120000}, graceMs{60000};
  // 0.7.8: the memory the renders made while the tempo moves may take (bytes, per file and for every
  // file together, counting every render the service holds): past it the renders no voice reads go,
  // least recently read first, after a short grace period (a voice may just be starting on one), and
  // while there is still no room the voices play the nearest render there is (or the plain source).
  // The renders of a tempo that holds still are made as before, whatever the cap.
  static inline std::atomic<int64_t> fileCap{int64_t{128} << 20}, totalCap{int64_t{512} << 20};
  static constexpr int64_t kEvictGraceMs = 2000;
  int familiesForTest() {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(families_.size());
  }
  ~WarpService() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    // at exit: what a voice still reads stays (an instrument may go after this)
    const auto keep = [](std::unique_ptr<Render>& r) {
      if (r && r->readers.load() > 0) (void)r.release();
    };
    for (auto& d : dying_) keep(d.second);
    for (auto& [k, f] : families_) {
      for (auto& o : f->owned) keep(o);
      for (auto& r : f->retired) keep(r.second);
      if (f->srcReaders.load() > 0) (void)new std::shared_ptr<WarpFamily>(f);
    }
  }

 private:
  std::shared_ptr<WarpFamily> familyLocked(const std::string& famKey, const SamplePtr& src) {
    auto& fam = families_[famKey];
    if (!fam) {
      fam = std::make_shared<WarpFamily>();
      fam->src = src;  // (held: no other sample takes its address while the family lingers)
    }
    return fam;
  }

  // Runs a stretch, adding its time to the worker's.
  // (`secs`: a running estimate of how long such a render takes)
  template <class F>
  static std::unique_ptr<Render> timed(F&& make, double* secs = nullptr) {
    const auto t0 = Clock::now();
    auto r = make();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
    gWarpWorkNs.fetch_add(ns, std::memory_order_relaxed);
    if (secs && r) *secs = 0.5 * *secs + 0.5 * static_cast<double>(ns) * 1e-9;
    return r;
  }
  static bool usedBySlot(const WarpFamily& f, const Render* r, const std::vector<std::shared_ptr<WarpSlot>>& live) {
    for (const auto& s : live)
      if (s->fam.get() == &f && s->current.load() == r) return true;
    return false;
  }
  // The render of `f` read least recently that no live slot plays (and, `unread`, no voice reads); -1 none.
  static long leastUsed(const WarpFamily& f, const std::vector<std::shared_ptr<WarpSlot>>& live, bool unread) {
    long best = -1;
    for (size_t k = 0; k < f.owned.size(); ++k) {
      const Render* o = f.owned[k].get();
      if (usedBySlot(f, o, live) || (unread && o->readers.load(std::memory_order_acquire) > 0)) continue;
      if (best < 0 || o->lastUse < f.owned[static_cast<size_t>(best)]->lastUse) best = static_cast<long>(k);
    }
    return best;
  }
  // Takes owned render `k` out of the ring and retires it (it stays while a voice reads it).
  static int retire(WarpFamily& f, long k, Clock::time_point now) {
    const Render* old = f.owned[static_cast<size_t>(k)].get();
    int at = -1;
    for (int i = 0; i < WarpFamily::kRing; ++i)
      if (f.ring[static_cast<size_t>(i)].load() == old) {
        f.ring[static_cast<size_t>(i)].store(nullptr, std::memory_order_release);
        at = i;
      }
    f.retired.push_back({now, std::move(f.owned[static_cast<size_t>(k)])});
    f.owned.erase(f.owned.begin() + k);
    return at;
  }
  // 0.7.8: room for a render of `need` bytes made while the tempo moves: renders no voice reads are
  // evicted (least recently read first) while the family or the service is over its cap. False: no
  // room yet (what was evicted is freed once its short grace period is over).
  static bool roomFor(WarpFamily& f, int64_t need, const std::vector<std::shared_ptr<WarpSlot>>& live) {
    // (kept: once what was evicted is gone)
    const auto over = [&](bool kept) {
      const int64_t all = gWarpBytes.load(std::memory_order_relaxed) - (kept ? gWarpEvicted.load(std::memory_order_relaxed) : 0);
      return f.bytes(kept) + need > fileCap.load(std::memory_order_relaxed) || all + need > totalCap.load(std::memory_order_relaxed);
    };
    if (!over(false)) return true;
    // the renders already out of use first (they go after the short grace period), then the ones
    // read least recently, until there will be room
    for (auto& r : f.retired) r.second->evict();
    const auto now = Clock::now();
    for (long k; over(true) && (k = leastUsed(f, live, true)) >= 0;) {
      f.owned[static_cast<size_t>(k)]->evict();
      (void)retire(f, k, now);
    }
    // (the bytes of what was just evicted are still held until its grace period is over)
    return !over(false);
  }

  // Adds a render to its family's ring; when the ring is full, the renders read least recently that
  // no live slot plays are retired until a place is free.
  static const Render* publish(WarpFamily& f, std::unique_ptr<Render> r, const std::vector<std::shared_ptr<WarpSlot>>& live) {
    const auto now = Clock::now();
    int at = -1;
    for (int i = 0; i < WarpFamily::kRing && at < 0; ++i)
      if (!f.ring[static_cast<size_t>(i)].load()) at = i;
    while (at < 0) {
      const long k = leastUsed(f, live, false);
      if (k < 0) break;
      at = retire(f, k, now);
    }
    r->lastUse = now;
    r->count();
    gWarpRenders.fetch_add(1, std::memory_order_relaxed);
    const Render* p = r.get();
    f.owned.push_back(std::move(r));
    if (at >= 0) f.ring[static_cast<size_t>(at)].store(p, std::memory_order_release);  // (else only its slot plays it)
    return p;
  }

  void loop() {
    // 0.7.8: below normal priority (never the audio thread's: this is the warp worker's own thread),
    // so stretching while the tempo moves never competes with the audio or the interface for a core
    platform::lowerThisThreadPriorityOnTime();
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
      cv_.wait_for(lk, std::chrono::milliseconds(30));
      if (stop_) break;
      gl::threads::woke();  // K-142: CPU report
      std::vector<std::shared_ptr<WarpSlot>> live;
      for (auto it = slots_.begin(); it != slots_.end();) {
        if (auto s = it->second.lock()) {
          live.push_back(std::move(s));
          ++it;
        } else {
          it = slots_.erase(it);
        }
      }
      const auto now = Clock::now();
      // a family nobody plays lingers, then goes (its renders after the grace period)
      for (auto it = families_.begin(); it != families_.end();) {
        WarpFamily& f = *it->second;
        const bool used = it->second.use_count() > 1;
        if (used) {
          f.idle = false;
        } else if (!f.idle) {
          f.idle = true;
          f.idleSince = now;
        }
        if (!used && now - f.idleSince > std::chrono::milliseconds(lingerMs.load(std::memory_order_relaxed)) &&
            f.srcReaders.load(std::memory_order_acquire) == 0) {
          for (auto& e : f.ring) e.store(nullptr);
          for (auto& o : f.owned) dying_.push_back({now, std::move(o)});
          for (auto& r : f.retired) dying_.push_back(std::move(r));
          it = families_.erase(it);
        } else {
          ++it;
        }
      }
      // (a render a slot still plays stays, however long ago it left the ring: a slot made while
      // the worker ran may have taken it from the ring just before)
      const auto old = [&](const Retired& r) {
        // (0.7.8: one evicted for the memory cap goes after the short grace period)
        const int64_t grace = graceMs.load(std::memory_order_relaxed);
        if (now - r.first <= std::chrono::milliseconds(r.second->evicted ? std::min(grace, kEvictGraceMs) : grace)) return false;
        if (r.second->readers.load(std::memory_order_acquire) > 0) return false;  // GitHub #80: a voice still reads it
        for (const auto& s : live)
          if (s->current.load() == r.second.get()) return false;
        return true;
      };
      dying_.erase(std::remove_if(dying_.begin(), dying_.end(), old), dying_.end());
      for (auto& [k, f] : families_) f->retired.erase(std::remove_if(f->retired.begin(), f->retired.end(), old), f->retired.end());
      lk.unlock();
      // 1) every slot gets a render covering its region at its tempo: one its family has, else its
      //    region stretched now
      // (0.7.8: what the voices read since the last wakeup, for the memory cap's least recently read)
      for (auto& s : live)
        for (auto& o : s->fam->owned)
          if (o->touched.exchange(false, std::memory_order_relaxed) || o->readers.load(std::memory_order_relaxed) > 0 ||
              s->current.load(std::memory_order_relaxed) == o.get())
            o->lastUse = now;
      for (auto& s : live) {
        WarpFamily& f = *s->fam;
        const double w = s->wanted.load(std::memory_order_relaxed);
        s->seenPrev2 = s->seenPrev;
        s->seenPrev = s->seenNow;
        s->seenPrevAt = s->seenNowAt;
        s->seenNow = w;
        s->seenNowAt = now;
        if (w <= 0) continue;
        const Render* cur = s->current.load(std::memory_order_acquire);
        if (cur && std::fabs(cur->tempo - w) < 1e-4) {
          f.tempo.store(w, std::memory_order_relaxed);
          continue;
        }
        if (w == s->lastWanted) ++s->stable;
        else s->stable = 0, s->lastWanted = w;
        if (const Render* r = f.find(w, s->beatA, s->beatB)) {
          s->current.store(r, std::memory_order_release);
          f.tempo.store(w, std::memory_order_relaxed);
          continue;
        }
        // a render at this tempo once the tempo settles, or at once when the family already plays
        // at it (a new region)
        const bool known = std::fabs(f.tempo.load(std::memory_order_relaxed) - w) < 1e-4;
        if (known || s->stable >= 2) {
          (void)roomFor(f, 0, live);  // (made whatever the cap: the cap only evicts for it)
          auto r = timed([&] { return makeRender(s->src, s->grid, s->region(), w); }, &s->renderSecs);
          if (!r) {
            s->wanted = 0;
            continue;
          }
          s->current.store(publish(f, std::move(r), live), std::memory_order_release);
          f.tempo.store(w, std::memory_order_relaxed);
          continue;
        }
        // 0.7.8: the tempo moves. The voices play the nearest render a little faster or slower; a
        // new one once that is more than kLiveCents off the tempo (or would be by the time a render
        // is ready, along a ramp), while the memory cap leaves room. Along a ramp it is aimed where
        // the tempo will be when it is ready, kLiveCents further on; a jump gets one at the tempo.
        const Render* near = f.nearest(w, s->beatA, s->beatB);
        if (cur && cur->covers(s->beatA, s->beatB) && (!near || std::fabs(std::log(cur->tempo / w)) < std::fabs(std::log(near->tempo / w)))) near = cur;
        const double moved = s->seenPrev > 0 ? 1200.0 * std::log2(w / s->seenPrev) : 0.0;
        const double before = s->seenPrev2 > 0 && s->seenPrev > 0 ? 1200.0 * std::log2(s->seenPrev / s->seenPrev2) : 0.0;
        const double dt = std::chrono::duration<double>(now - s->seenPrevAt).count();
        double ahead = 0;  // cents the tempo moves on while a render is made
        if (moved * before > 0 && dt > 1e-3) ahead = std::clamp(moved / dt * (s->renderSecs + 0.03), -kMaxAheadCents, kMaxAheadCents);
        const double ready = w * std::exp2(ahead / 1200.0);
        const auto off = [&](double t) { return near ? std::fabs(1200.0 * std::log2(t / near->tempo)) : 1e9; };
        if (off(w) <= kLiveCents && off(ready) <= kLiveCents) continue;
        if (f.movingAt != Clock::time_point{})
          f.movingBudget = std::min(kMovingBudgetSecs, f.movingBudget + std::chrono::duration<double>(now - f.movingAt).count() * f.movingDuty);
        f.movingAt = now;
        if (f.movingBudget <= 0) {  // (rate limited: the worker looks again at its next wakeup)
          gWarpThrottled.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const double target = ahead != 0 ? ready * std::exp2(std::copysign(kLiveCents, ahead) / 1200.0) : w;
        const double outFrames = (s->beatB - s->beatA) * 60.0 / target * s->src->sampleRate;
        if (!roomFor(f, static_cast<int64_t>(outFrames * 2 * sizeof(float)), live)) {
          gWarpSkipped.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const auto t0 = Clock::now();
        auto r = timed([&] { return makeRender(s->src, s->grid, s->region(), target); }, &s->renderSecs);
        f.movingBudget -= std::chrono::duration<double>(Clock::now() - t0).count();
        // (a steady ramp: aimed about as far ahead as the render before, the same way)
        const bool steady = ahead * f.movingAhead > 0 && std::fabs(ahead) <= 2 * std::fabs(f.movingAhead) && std::fabs(f.movingAhead) <= 2 * std::fabs(ahead);
        f.movingDuty = steady ? kRampDuty : kMovingDuty;
        f.movingAhead = ahead;
        if (!r) continue;
        gWarpMoving.fetch_add(1, std::memory_order_relaxed);
        s->current.store(publish(f, std::move(r), live), std::memory_order_release);
      }
      // 2) then the spans, so the next layouts find their regions ready
      for (auto& s : live) {
        WarpFamily& f = *s->fam;
        const double w = s->wanted.load(std::memory_order_relaxed);
        if (w <= 0 || (s->span.a == s->a && s->span.b == s->b)) continue;
        const Render* cur = s->current.load(std::memory_order_acquire);
        if (!cur || std::fabs(cur->tempo - w) >= 1e-4 || cur->covers(s->span.beatA, s->span.beatB)) continue;
        if (const Render* r = f.find(w, s->span.beatA, s->span.beatB)) {
          s->current.store(r, std::memory_order_release);
          continue;
        }
        // (0.7.8: not while the tempo moves: a moving tempo plays its region's renders)
        if (s->seenPrev != s->seenNow || w != s->seenNow) continue;
        (void)roomFor(f, 0, live);
        auto r = timed([&] { return makeRender(s->src, s->grid, s->span, w); });
        if (!r) continue;
        const Render* p = publish(f, std::move(r), live);
        // the slot plays the span from now on (the region render stays for the voices reading it)
        if (std::fabs(s->wanted.load(std::memory_order_relaxed) - w) < 1e-4) s->current.store(p, std::memory_order_release);
      }
      live.clear();
      lk.lock();
    }
  }
  std::mutex mu_;
  std::condition_variable cv_;
  std::map<std::string, std::weak_ptr<WarpSlot>> slots_;
  std::map<std::string, std::shared_ptr<WarpFamily>> families_;
  std::vector<Retired> dying_;
  std::thread thread_;
  bool stop_ = false;
};

// ================================================================== aux
struct RtSlice {
  int64_t a = 0, b = 0;  // playable source frames (offset / length applied)
  double beatA = 0, beatB = 0;
  float gain = 1, pitch = 0;
  bool reverse = false;
  float fadeIn = 0, fadeOut = 0;  // seconds
  int choke = 0;
};
struct RtStep {
  int slice = -1;
  double start = 0;      // beats from the pattern start (unswung)
  bool swings = false;   // starts on an odd unit (swing delays it)
  float pitch = 0, gain = 1, length = 1, offset = 0;
  bool reverse = false;
  int stutter = 1;
};

struct SeltmannAux final : AuxData {
  SamplePtr src;
  Analysis grid;
  double srcBpm = 120;
  std::vector<RtSlice> slices;
  std::vector<RtStep> steps;
  std::array<int, kSlotCount> slots{};
  double unitBeats = 1;
  double stepsBeats = 0;  // J-214: where the last step ends (beats)
  double patBeats = 0;    // one cycle (the rest up to the bar line included)
  std::shared_ptr<WarpSlot> warp;
  std::shared_ptr<WarpFamily> fam;  // the file's family (registered data only): a voice reading the file holds it
};

uint64_t hashBeats(const Analysis& g) {
  uint64_t h = 1469598103934665603ull;
  for (double b : g.beats) {
    const auto v = static_cast<uint64_t>(std::llround(b * 16));
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

std::string familyKey(const std::string& path, int stretchMode, const Analysis& g, const SamplePtr& src) {
  return path + "|" + std::to_string(stretchMode) + "|" + std::to_string(hashBeats(g)) + "|" + std::to_string(reinterpret_cast<uintptr_t>(src.get()));
}

std::shared_ptr<SeltmannAux> buildAux(const Model& m, SamplePtr src, const std::string& path, int stretchMode, bool registerWarp) {
  auto x = std::make_shared<SeltmannAux>();
  x->src = src;
  x->grid = m.grid;
  x->grid.onsetEnv.clear();
  x->grid.sections.clear();
  x->grid.loops.clear();
  x->srcBpm = m.grid.bpm > 1 ? m.grid.bpm : 120.0;
  const int64_t nFrames = src ? static_cast<int64_t>(src->frames()) : m.srcFrames;
  for (const auto& s : m.slices) {
    RtSlice r;
    const int64_t len = std::max<int64_t>(1, s.end - s.start);
    r.a = std::clamp<int64_t>(s.start + static_cast<int64_t>(len * std::clamp(s.offset, 0.0f, 0.9f)), 0, std::max<int64_t>(0, nFrames - 1));
    r.b = std::clamp<int64_t>(r.a + static_cast<int64_t>(len * std::clamp(s.length, 0.05f, 1.0f)), r.a + 1, std::max<int64_t>(r.a + 1, std::min(s.end, nFrames)));
    r.beatA = beatAt(m.grid, static_cast<double>(r.a));
    r.beatB = beatAt(m.grid, static_cast<double>(r.b));
    r.gain = static_cast<float>(dbToGain(s.gain));
    r.pitch = s.pitch;
    r.reverse = s.reverse;
    r.fadeIn = s.fadeIn * 0.001f;
    r.fadeOut = s.fadeOut * 0.001f;
    r.choke = s.choke;
    x->slices.push_back(r);
  }
  const int n = static_cast<int>(x->slices.size());
  // J-214: every step at its own place in the sequence (own lengths, default lengths in units)
  double units = 0, own = 0;
  for (const auto& st : m.pattern.steps) {
    RtStep r;
    r.slice = stepSlice(m.pattern, st, n);
    r.start = units * m.pattern.unitBeats + own;
    r.swings = swingDelay(m.pattern, r.start, 1.0) > 0;
    if (st.beats > 0) own += st.beats;
    else units += 1 + st.ties;
    r.pitch = st.pitch;
    r.gain = static_cast<float>(dbToGain(st.gain));
    r.length = st.length;
    r.offset = st.offset;
    r.reverse = st.reverse;
    r.stutter = std::clamp(st.stutter, 1, 16);
    x->steps.push_back(r);
  }
  x->unitBeats = std::max(1.0 / 64, m.pattern.unitBeats);
  x->stepsBeats = m.pattern.stepsBeats();
  x->patBeats = cycleBeats(m);
  x->slots.fill(-1);
  for (int k = 0; k < kSlotCount; ++k) {
    const int v = k < static_cast<int>(m.pattern.slots.size()) ? m.pattern.slots[static_cast<size_t>(k)] : -1;
    x->slots[static_cast<size_t>(k)] = v >= 0 && v < n ? v : (n > 0 ? k % n : -1);
  }
  if (src && m.regionEnd > m.regionStart) {
    auto make = [&] {
      auto w = std::make_shared<WarpSlot>();
      w->src = src;
      w->grid = x->grid;
      w->a = std::clamp<int64_t>(m.regionStart, 0, nFrames);
      w->b = std::clamp<int64_t>(m.regionEnd, w->a, nFrames);
      w->beatA = beatAt(m.grid, static_cast<double>(w->a));
      w->beatB = beatAt(m.grid, static_cast<double>(w->b));
      w->span = registerWarp ? spanFor(x->grid, nFrames, w->region()) : w->region();
      return w;
    };
    if (registerWarp) {
      const std::string fam = familyKey(path, stretchMode, m.grid, src);
      x->warp = WarpService::get().slot(fam, fam + "|" + std::to_string(m.regionStart) + "|" + std::to_string(m.regionEnd), make);
    } else {
      x->warp = make();
    }
  }
  // GitHub #80: the file's family, so that a voice reading the file keeps it (with or without a region)
  if (src && registerWarp) x->fam = x->warp ? x->warp->fam : WarpService::get().family(familyKey(path, stretchMode, m.grid, src), src);
  return x;
}

// Model of a state: the stored one, or a default one from the analysis (or a flat grid).
bool modelOf(const PluginState& st, SamplePtr src, std::string& path, Model& m) {
  if (parseData(st.data, path, m) && m.srcFrames > 0) {
    if (src && m.srcFrames != static_cast<int64_t>(src->frames())) {
      // the file changed under the model: keep nothing that points into it
    } else {
      return true;
    }
  }
  path = pathOf(st.data);
  if (!src) return false;
  JobPtr job = requestAnalysis(path);
  if (job->state == 0 && src->seconds() < 40) {
    // short material: wait for the (fast) analysis rather than starting on a guessed grid
    for (int i = 0; i < 4000 && job->state == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  m = defaultModel(*src, job->state == 1 && job->result.frames == static_cast<int64_t>(src->frames()) ? job->result : flatGrid(*src));
  return true;
}

// ================================================================== instrument
class SeltmannGen final : public Generator {
 public:
  bool mayRest() const override { return false; }  // K-277: sound without notes
  SeltmannGen() { (void)stretch::sincTable(stretch::Quality::Realtime); }
  ~SeltmannGen() override {
    for (auto& v : voices_) unhold(v);
  }

  bool active() const override {
    if (patOn_) return true;
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }
  bool handlesNotePan() const override { return false; }
  int displayStep() const override { return display_.load(std::memory_order_relaxed); }
  double liveFrame() const { return liveFrame_.load(std::memory_order_relaxed); }
  double livePattern() const { return livePat_.load(std::memory_order_relaxed); }
  bool patternRunning() const { return patRun_.load(std::memory_order_relaxed); }

  // Offline renders: a private warp render to use instead of the service.
  void forceRender(const Render* r) { forced_ = r; }

 protected:
  struct Voice {
    bool on = false, held = false, pattern = false;
    uint32_t id = 0;
    int slice = -1, choke = 0;
    const float* L = nullptr;
    const float* R = nullptr;
    int64_t n = 0;
    double pos = 0, lo = 0, hi = 0, rate = 1;
    double notePitch = -1, baseRate = 1, semi = 0, semiTo = 0, semiStep = 0;  // K-144: a slide off the played pitch
    int64_t semiLeft = 0;
    int dir = 1;
    bool warped = false;
    double wTempo = 0, wRate = 0;  // J-219: the warped render this voice plays (tempo, source rate)
    Render* pieces = nullptr;      // 0.7.8 (offline): ... while it is made in pieces (renderUpTo ahead of the reads)
    double wBeatA = 0;             // 0.7.8: ... and the source beat its frame 0 is
    uint32_t gen = 0;              // 0.7.8: the prepared data (sequence) it was started under
    // GitHub #80: what it reads, counted as read until the voice has ended (see unhold)
    const Render* rd = nullptr;
    const WarpFamily* hold = nullptr;
    double gain = 1;
    double played = 0;       // output samples since the start
    double left = -1;        // output samples until the gate closes (-1 = while held)
    double fadeInS = 1, fadeOutS = 1;
    int stage = 0;           // 0 attack, 1 decay, 2 sustain, 3 release
    double env = 0;
    double kill = 1, killStep = 0;
    uint64_t age = 0;
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    const auto* nx = dynamic_cast<const SeltmannAux*>(aux.custom.get());
    if (nx != aux_) {
      patPrimed_ = false;  // new chop: find the step again
      ++auxGen_;           // 0.7.8: steps of the sequence before end when the new one plays its first
    }
    aux_ = nx;
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    tempo_ = ctx.tempo > 1 ? ctx.tempo : 120.0;
    ppq_ = std::max(1, ctx.ppq);
    tick_ = ctx.tick;
    playing_ = ctx.playing;
    jumped_ = ctx.jumped;
    blockOff_ = 0;
    vol_ = std::clamp(static_cast<double>(par(s, SmVolume, 0.8f)), 0.0, 1.5);
    const double pan = std::clamp(static_cast<double>(par(s, SmPan, 0)), -1.0, 1.0);
    panL_ = std::cos((pan + 1) * 0.25 * 3.14159265358979) * 1.41421356;
    panR_ = std::sin((pan + 1) * 0.25 * 3.14159265358979) * 1.41421356;
    base_ = std::clamp(ipar(s, SmBaseNote, 60), 0, 127);
    keyMap_ = std::clamp(ipar(s, SmKeyMap, KeySlices), 0, 1);
    sync_ = std::clamp(ipar(s, SmSync, SyncStretch), 0, 2);
    lock_ = par(s, SmLock, 1) >= 0.5f;
    mono_ = par(s, SmMono, 0) >= 0.5f;
    attack_ = std::max(0.0, static_cast<double>(par(s, SmAttack, 0))) * 0.001;
    decay_ = std::max(0.0, static_cast<double>(par(s, SmDecay, 0))) * 0.001;
    sustain_ = std::clamp(static_cast<double>(par(s, SmSustain, 1)), 0.0, 1.0);
    release_ = std::max(0.0, static_cast<double>(par(s, SmRelease, 10))) * 0.001;
    declick_ = std::clamp(static_cast<double>(par(s, SmDeclick, 3)), 0.3, 50.0) * 0.001;
    swing_ = std::clamp(static_cast<double>(par(s, SmSwing, 0)), 0.0, 1.0);
    pitch_ = std::clamp(static_cast<double>(par(s, SmPitch, 0)), -48.0, 48.0);
    velSens_ = std::clamp(static_cast<double>(par(s, SmVelocity, 0.6f)), 0.0, 1.0);
    reverseAll_ = par(s, SmReverse, 0) >= 0.5f;
    gate_ = std::clamp(static_cast<double>(par(s, SmGate, 1)), 0.05, 1.0);
    const int sp = std::clamp(ipar(s, SmSpeed, SpeedNormal), 0, 2);
    speed_ = sp == SpeedHalf ? 0.5 : sp == SpeedDouble ? 2.0 : 1.0;
    const double cut = std::clamp(static_cast<double>(par(s, SmCutoff, 20000)), 20.0, 20000.0);
    reso_ = std::clamp(static_cast<double>(par(s, SmReso, 0)), 0.0, 1.0);
    filterOn_ = cut < 19900.0;
    if (filterOn_) {
      g_ = std::tan(3.14159265358979 * std::min(cut, sr_ * 0.45) / sr_);
      k_ = 2.0 - 1.9 * reso_;
    }
    // warp: ask for this tempo; use the render when it matches
    render_ = nullptr;
    offline_ = ctx.offline;
    if (aux_ && sync_ == SyncStretch) {
      if (forced_) {
        render_ = forced_;
      } else if (ctx.offline && aux_->warp) {
        render_ = offlineRender();
      } else if (aux_->warp) {
        aux_->warp->wanted.store(tempo_, std::memory_order_relaxed);
        const Render* r = aux_->warp->current.load(std::memory_order_acquire);
        // 0.7.8: while the tempo moves, the render nearest this tempo (played a little faster or
        // slower) when that is nearer the pitch than the plain resampled source
        if (!(r && std::fabs(r->tempo - tempo_) < 1e-4) && aux_->warp->fam) {
          const Render* nr = aux_->warp->fam->nearest(tempo_, aux_->warp->beatA, aux_->warp->beatB);
          if (nr && (!r || std::fabs(std::log(nr->tempo / tempo_)) < std::fabs(std::log(r->tempo / tempo_)))) r = nr;
        }
        if (nearerThanSource(r)) render_ = r;
      }
    }
  }

  void onNoteOnEx(uint32_t id, double pitch, float vel, const NoteProps& p) override {
    noteOnImpl(id, pitch, vel, std::round((p.modX - 0.5) * 48.0), p.modY > 0.75f);
  }
  void onNoteOn(uint32_t id, double pitch, float vel) override { noteOnImpl(id, pitch, vel, 0, false); }
  // K-144: a slide plays the slice faster or slower by its interval from the played pitch (the
  // pattern's own steps keep theirs).
  bool supportsGlide() const override { return true; }
  void onGlide(uint32_t id, double pitch, double seconds) override {
    for (auto& v : voices_)
      if (v.on && v.id == id && v.notePitch >= 0) {
        v.semiTo = pitch - v.notePitch;
        v.semiLeft = std::max<int64_t>(1, static_cast<int64_t>(seconds * sr_));
        v.semiStep = (v.semiTo - v.semi) / static_cast<double>(v.semiLeft);
      }
  }

  void onNoteOff(uint32_t id) override {
    if (patOn_ && id == patId_) {
      setPat(false);
      for (auto& v : voices_)
        if (v.on && v.pattern) release(v);
      display_.store(-1, std::memory_order_relaxed);
    }
    for (auto& v : voices_)
      if (v.on && !v.pattern && v.id == id && v.held) {
        v.held = false;
        release(v);
      }
  }
  // J-112: the pattern is the instrument's own playback (latched by its key,
  // from the editor's Play or a keyboard): Play with the transport stopped
  // ends it with a normal release, like the key's release.
  bool ownPlayback() const override { return patOn_; }
  void stopOwnPlayback() override {
    if (!patOn_) return;
    setPat(false);
    for (auto& v : voices_)
      if (v.on && v.pattern) release(v);
    display_.store(-1, std::memory_order_relaxed);
  }
  void onCut(uint32_t id) override {
    if (patOn_ && id == patId_) setPat(false);
    for (auto& v : voices_)
      if (v.on && (v.id == id || v.pattern)) startKill(v);
  }
  void onAllOff() override {
    setPat(false);
    for (auto& v : voices_)
      if (v.on) release(v);
  }
  void onKill() override {
    setPat(false);
    for (auto& v : voices_) {
      v.on = false;
      unhold(v);
    }
    display_.store(-1, std::memory_order_relaxed);
  }

  void render(float* L, float* R, int n) override {
    // J-219: Track Receipt coverage, one run per voice (source frames of the file).
    const bool tapOn = tap::active() && aux_ && aux_->src;
    std::array<tap::Run, kMaxVoices> runs;
    if (tapOn)
      for (auto& rn : runs) rn.begin(aux_->src.get());
    for (int i = 0; i < n; ++i) {
      if (patOn_ && aux_ && !aux_->steps.empty()) patternTick(blockOff_ + i, i);
      double l = 0, r = 0;
      for (auto& v : voices_)
        if (v.on) voiceSample(v, l, r, tapOn ? &runs[static_cast<size_t>(&v - voices_.data())] : nullptr);
      if (filterOn_) {
        l = svf(l, ic1L_, ic2L_);
        r = svf(r, ic1R_, ic2R_);
      }
      L[i] += static_cast<float>(l * vol_ * panL_);
      R[i] += static_cast<float>(r * vol_ * panR_);
    }
    blockOff_ += n;
    publish();
    for (auto& v : voices_)
      if (!v.on) unhold(v);  // GitHub #80: an ended voice reads nothing any more
    if (!patOn_) {
      bool any = false;
      for (const auto& v : voices_) any = any || v.on;
      if (!any) display_.store(-1, std::memory_order_relaxed);
    }
  }

 private:
  // ---- note handling
  void noteOnImpl(uint32_t id, double pitch, float vel, double extraSemis, bool extraReverse) {
    const int note = static_cast<int>(std::lround(pitch));
    const double fine = pitch - note;
    if (note >= base_ - 12 && note < base_) {  // pattern keys (latched even before the prepared data arrives)
      setPat(true);
      patId_ = id;
      patSemis_ = note - (base_ - 12) + fine;
      patVel_ = vel;
      patElapsed_ = 0;
      patPrimed_ = false;
      return;
    }
    if (!aux_) return;
    const int k = note - base_;
    int slice = -1;
    if (keyMap_ == KeySlots) slice = k >= 0 && k < kSlotCount ? aux_->slots[static_cast<size_t>(k)] : -1;
    else slice = k;
    if (slice < 0 || slice >= static_cast<int>(aux_->slices.size())) return;
    startVoice(id, slice, fine + extraSemis, vel, 1.0, extraReverse, 0.0, -1, false);
    for (auto& v : voices_)
      if (v.on && v.id == id && v.age == ageCounter_) v.notePitch = pitch;  // K-144
    display_.store(packDisplay(-1, slice), std::memory_order_relaxed);
  }

  // Source buffer and frame mapping for the current sync mode.
  struct Src {
    const float* L = nullptr;
    const float* R = nullptr;
    int64_t n = 0;
    double rate = 1;  // input frames per output frame before pitch
    bool warped = false;
    bool lengthOnly = false;  // K-321: no audio (L and R null): voices only move through it
  };
  // 0.7.8: the warped render a slice plays: this sequence's own (covering its region), else any
  // render of the same file at this tempo that covers the slice (a sequence switched while playing
  // finds its regions in the render the last one played, so it is in tune from its first step);
  // null: the plain source (resampled) until one is ready.
  const Render* renderFor(const RtSlice& sl) const {
    if (!aux_ || sync_ != SyncStretch) return nullptr;
    const bool exact = render_ && std::fabs(render_->tempo - tempo_) < 1e-4;
    if (render_ && render_->covers(sl.beatA, sl.beatB) && (exact || forced_ || offline_)) return render_;
    if (forced_ || offline_ || !aux_->warp || !aux_->warp->fam) return nullptr;
    // 0.7.8: the render nearest this tempo covering the slice (another region's render may be nearer
    // than this sequence's own), when that is nearer the pitch than the plain resampled source
    const Render* r = aux_->warp->fam->nearest(tempo_, sl.beatA, sl.beatB);
    if (render_ && render_->covers(sl.beatA, sl.beatB) &&
        (!r || std::fabs(std::log(render_->tempo / tempo_)) <= std::fabs(std::log(r->tempo / tempo_))))
      r = render_;
    return nearerThanSource(r) ? r : nullptr;
  }
  // 0.7.8 (live): a render is played (at tempo_ / its tempo: nearSpeed) when it is at this tempo, or
  // when it is nearer the source pitch than the plain resampled source is (never worse than that).
  bool nearerThanSource(const Render* r) const {
    if (!r) return false;
    if (std::fabs(r->tempo - tempo_) < 1e-4) return true;
    return std::fabs(std::log(tempo_ / r->tempo)) < std::fabs(std::log(tempo_ / aux_->srcBpm));
  }
  Src source(const Render* rr) const {
    Src s;
    if (!aux_ || !aux_->src) return s;
    if (rr) {
      s.lengthOnly = rr->lengthOnly;
      s.L = s.lengthOnly ? nullptr : rr->L.data();
      s.R = s.lengthOnly ? nullptr : rr->R.data();
      s.n = rr->frames();
      s.rate = rr->rate / sr_ * nearSpeed(rr);
      s.warped = true;
      return s;
    }
    const Sample& smp = *aux_->src;
    s.L = smp.left.data();
    s.R = smp.right.data();
    s.n = static_cast<int64_t>(smp.frames());
    const double sp = sync_ == SyncOriginal ? 1.0 : tempo_ / aux_->srcBpm;
    s.rate = smp.sampleRate / sr_ * sp;
    return s;
  }
  // 0.7.8 (offline): a render made for a tempo a hair from this one (kNearCents) plays a little
  // faster or slower, so its slices keep their time (1: a render at this tempo).
  double nearSpeed(const Render* rr) const { return !rr || std::fabs(rr->tempo - tempo_) < 1e-4 ? 1.0 : tempo_ / rr->tempo; }
  // Source frame -> buffer frame (warped renders map beats onto the project tempo).
  static double mapFrame(const Render* rr, double beat, double frame) {
    if (!rr) return frame;
    return (beat - rr->beatA) * 60.0 / rr->tempo * rr->rate;
  }

  // Starts a slice. gateSamples < 0: plays while held (keys), else for that long (pattern steps).
  void startVoice(uint32_t id, int slice, double semis, float vel, double stepGain, bool stepReverse, double stepOffset, double gateSamples,
                  bool pattern) {
    const RtSlice& sl = aux_->slices[static_cast<size_t>(slice)];
    const Render* rr = renderFor(sl);
    // 0.7.8: an offline render made in pieces gets its buffers on its first voice
    Render* pieces = rr && rr == own_.get() && own_->partial() ? own_.get() : nullptr;
    if (pieces) renderUpTo(*pieces, 0);
    const Src s = source(rr);
    if ((!s.L && !s.lengthOnly) || s.n <= 0) return;
    // chokes: mono, the slice's group
    for (auto& v : voices_)
      if (v.on && (mono_ || (sl.choke > 0 && v.choke == sl.choke))) startKill(v);
    Voice* pick = nullptr;
    for (auto& v : voices_)
      if (!v.on) {
        pick = &v;
        break;
      }
    if (!pick) {  // steal the oldest
      pick = &voices_[0];
      for (auto& v : voices_)
        if (v.age < pick->age) pick = &v;
    }
    Voice& v = *pick;
    unhold(v);  // (a stolen voice)
    v = Voice{};
    v.on = true;
    v.held = gateSamples < 0;
    v.pattern = pattern;
    v.id = id;
    v.slice = slice;
    v.choke = sl.choke;
    v.L = s.L;
    v.R = s.R;
    v.n = s.n;
    v.warped = s.warped;
    v.gen = auxGen_;
    // GitHub #80: the voice reads this render, or the file, until it ends (the prepared data it
    // started under may be freed long before). Offline and forced renders belong to this instance.
    if (rr && !forced_ && !offline_) {
      rr->readers.fetch_add(1, std::memory_order_acq_rel);
      rr->touched.store(true, std::memory_order_relaxed);  // 0.7.8: read lately (the memory cap)
      v.rd = rr;
    } else if (!rr && aux_->fam) {
      aux_->fam->srcReaders.fetch_add(1, std::memory_order_acq_rel);
      v.hold = aux_->fam.get();
    }
    if (rr) v.wTempo = rr->tempo, v.wRate = rr->rate, v.wBeatA = rr->beatA;  // J-219
    if (aux_ && aux_->src) tap::play(aux_->src.get());
    v.lo = std::clamp(mapFrame(rr, sl.beatA, static_cast<double>(sl.a)), 0.0, static_cast<double>(s.n - 1));
    v.hi = std::clamp(mapFrame(rr, sl.beatB, static_cast<double>(sl.b)), v.lo + 1, static_cast<double>(s.n));
    if (stepOffset > 0) v.lo = std::min(v.hi - 1, v.lo + (v.hi - v.lo) * stepOffset);
    v.rate = s.rate * std::pow(2.0, (sl.pitch + semis + pitch_) / 12.0);
    v.baseRate = v.rate;
    if (pieces) {  // the whole slice now (reversed, it reads from its end), the rest as it plays
      v.pieces = pieces;
      piecesAhead(v, v.hi);
    }
    if (tap::active() && aux_ && aux_->src && aux_->src->sampleRate > 0) {
      // K-321: Track Receipt: a warped slice keeps its pitch at the project tempo (the speed then
      // comes from the reads); a plain one is resampled, its pitch following its speed.
      const double speed = v.rate * sr_ / aux_->src->sampleRate;
      if (s.warped) tap::pitch(aux_->src.get(), sl.pitch + semis + pitch_ + 12.0 * std::log2(nearSpeed(rr)), 0.0);
      else if (speed > 0) tap::pitch(aux_->src.get(), 12.0 * std::log2(speed), speed);
    }
    const bool rev = (sl.reverse != stepReverse) != reverseAll_;
    v.dir = rev ? -1 : 1;
    v.pos = rev ? v.hi - 1 : v.lo;
    const double velGain = 1.0 - velSens_ + velSens_ * std::clamp(static_cast<double>(vel), 0.0, 1.0);
    v.gain = sl.gain * stepGain * velGain;
    v.left = gateSamples;
    v.fadeInS = std::max(1.0, (declick_ + sl.fadeIn) * sr_);
    v.fadeOutS = std::max(1.0, (declick_ + sl.fadeOut) * sr_);
    v.stage = 0;
    v.env = attack_ <= 0 ? 1.0 : 0.0;
    if (attack_ <= 0) v.stage = 1;
    v.age = ++ageCounter_;
  }

  // 0.7.8 (offline): makes the frames of the voice's render a read at `pos` reaches (a little more, so
  // this runs once every 2048 frames at most, and hardly more is stretched than the voices read).
  static void piecesAhead(Voice& v, double pos) {
    Render& r = *v.pieces;
    const int64_t need = static_cast<int64_t>(std::floor(pos)) + pitch::reach(std::fabs(v.rate), stretch::sincTable(stretch::Quality::Realtime)) + 8;
    if (need < r.ready()) return;
    renderUpTo(r, need + 2048);
    if (!r.partial()) v.pieces = nullptr;
  }

  void release(Voice& v) {
    if (v.stage < 3) v.stage = 3;
  }
  static void unhold(Voice& v) {
    if (v.rd) v.rd->readers.fetch_sub(1, std::memory_order_acq_rel);
    if (v.hold) v.hold->srcReaders.fetch_sub(1, std::memory_order_acq_rel);
    v.rd = nullptr;
    v.hold = nullptr;
  }
  void startKill(Voice& v) {
    if (v.killStep <= 0) v.killStep = 1.0 / std::max(1.0, declick_ * sr_);
  }

  // The file frame a voice reads (warped renders map back through the beat grid).
  double sourceFrame(const Voice& v) const {
    if (!v.warped || !aux_ || v.wTempo <= 0 || v.wRate <= 0) return v.pos;
    return frameAt(aux_->grid, v.wBeatA + v.pos / (60.0 / v.wTempo * v.wRate));
  }

  void voiceSample(Voice& v, double& outL, double& outR, tap::Run* run = nullptr) {
    // amplitude envelope
    switch (v.stage) {
      case 0:
        v.env += 1.0 / std::max(1.0, attack_ * sr_);
        if (v.env >= 1) v.env = 1, v.stage = 1;
        break;
      case 1:
        v.env -= (1.0 - sustain_) / std::max(1.0, std::max(decay_, 0.002) * sr_);
        if (v.env <= sustain_) v.env = sustain_, v.stage = 2;
        break;
      case 2: v.env = sustain_; break;
      default: {
        const double rel = std::max(release_, declick_);
        v.env -= 1.0 / std::max(1.0, rel * sr_);
        if (v.env <= 0) {
          v.on = false;
          return;
        }
        break;
      }
    }
    if (v.killStep > 0) {
      v.kill -= v.killStep;
      if (v.kill <= 0) {
        v.on = false;
        return;
      }
    }
    // gate (pattern steps): the release starts so it ends with the step
    if (v.left >= 0) {
      v.left -= 1;
      if (v.left <= 0 && v.stage < 3) v.stage = 3;
    }
    // edge fades: in from the start, out before the slice edge
    const double dist = (v.dir > 0 ? v.hi - v.pos : v.pos - v.lo) / std::max(1e-9, v.rate);
    if (dist <= 0) {
      v.on = false;
      return;
    }
    const double fin = std::min(1.0, (v.played + 1) / v.fadeInS);
    const double fout = std::min(1.0, dist / v.fadeOutS);
    // declick the gate's own end too (the release may be shorter than the declick)
    const double a = v.env * fin * fout * v.kill * v.gain;
    float l, r;
    if (run) run->at(sourceFrame(v));
    if (v.pieces) piecesAhead(v, v.pos);
    if (!v.L) {  // K-321: a length-only render (a receipt pass that only measures): no sound
      l = r = 0.f;
    } else if (std::fabs(v.rate - 1.0) < 1e-9 && std::fabs(v.pos - std::floor(v.pos)) < 1e-9) {
      const auto i = static_cast<int64_t>(v.pos);
      l = i >= 0 && i < v.n ? v.L[i] : 0.f;
      r = i >= 0 && i < v.n ? v.R[i] : 0.f;
    } else {
      stretch::readSinc2(v.L, v.R, v.n, v.pos, v.rate, stretch::sincTable(stretch::Quality::Realtime), l, r);
    }
    outL += l * a;
    outR += r * a;
    v.pos += v.dir * v.rate;
    if (v.semiLeft > 0) {  // K-144: the slide, retuned every 16 samples
      --v.semiLeft;
      v.semi = v.semiLeft == 0 ? v.semiTo : v.semi + v.semiStep;
      if ((v.semiLeft & 15) == 0) v.rate = v.baseRate * std::exp2(v.semi / 12.0);
    }
    v.played += 1;
  }

  double svf(double x, double& ic1, double& ic2) const {
    const double a1 = 1.0 / (1.0 + g_ * (g_ + k_)), a2 = g_ * a1, a3 = g_ * a2;
    const double v3 = x - ic2;
    const double v1 = a1 * ic1 + a2 * v3;
    const double v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2 * v1 - ic1;
    ic2 = 2 * v2 - ic2;
    return v2;
  }

  // ---- pattern sequencer
  double beatsPerSample() const {
    const double bpm = sync_ == SyncOriginal || !aux_ ? (aux_ ? aux_->srcBpm : 120.0) : tempo_;
    return bpm / 60.0 / sr_ * speed_;
  }
  double eventBeat(int step, int sub) const {
    const RtStep& st = aux_->steps[static_cast<size_t>(step)];
    return startOf(step) + stepLen(step) * sub / st.stutter;
  }
  // J-214: a step starts where the one before ends (own lengths included); the last one ends with
  // the steps, not with the cycle (the rest up to the bar line stays silent)
  double startOf(int k) const {
    if (k >= static_cast<int>(aux_->steps.size())) return aux_->stepsBeats;
    const RtStep& s2 = aux_->steps[static_cast<size_t>(k)];
    return s2.start + (swing_ > 0 && s2.swings ? swing_ * 0.5 * aux_->unitBeats : 0.0);
  }
  double stepLen(int step) const { return std::max(1e-6, startOf(step + 1) - startOf(step)); }
  void seek(double p) {
    // first event at or after p
    const int n = static_cast<int>(aux_->steps.size());
    nextStep_ = n;
    nextSub_ = 0;
    for (int k = 0; k < n && nextStep_ == n; ++k)
      for (int s = 0; s < aux_->steps[static_cast<size_t>(k)].stutter; ++s)
        if (eventBeat(k, s) >= p - 1e-9) {
          nextStep_ = k;
          nextSub_ = s;
          break;
        }
  }
  void advance() {
    const int n = static_cast<int>(aux_->steps.size());
    if (++nextSub_ >= aux_->steps[static_cast<size_t>(nextStep_)].stutter) {
      nextSub_ = 0;
      ++nextStep_;
    }
    if (nextStep_ > n) nextStep_ = n;
  }
  void trigger(int step, int sub) {
    const RtStep& st = aux_->steps[static_cast<size_t>(step)];
    if (sub == 0) display_.store(packDisplay(step, st.slice), std::memory_order_relaxed);
    // 0.7.8: the first step (or rest) of a sequence switched in while playing ends the steps of the one before
    // with the declick crossfade (as a mono step would): the two never play over each other
    for (auto& v : voices_)
      if (v.on && v.pattern && v.gen != auxGen_) startKill(v);
    if (st.slice < 0 || st.slice >= static_cast<int>(aux_->slices.size())) return;
    const double lenBeats = stepLen(step) / st.stutter;
    const double gateSamples = std::max(1.0, lenBeats / beatsPerSample() * gate_ * st.length);
    startVoice(0x80000000u | static_cast<uint32_t>(step), st.slice, patSemis_ + st.pitch, patVel_, st.gain, st.reverse, st.offset, gateSamples, true);
  }
  void patternTick(int64_t off, int) {
    const double patBeats = aux_->patBeats;
    if (patBeats <= 0) return;
    const double bps = beatsPerSample();
    double p;
    const bool songLock = lock_ && playing_ && sync_ != SyncOriginal;
    if (songLock) {
      const double songBeat = tick_ / ppq_ + static_cast<double>(off) * tempo_ / 60.0 / sr_;
      p = songBeat * speed_;
    } else {
      p = patElapsed_ * bps;
      patElapsed_ += 1;
    }
    p = std::fmod(p, patBeats);
    if (p < 0) p += patBeats;
    if (!patPrimed_ || (jumped_ && off == 0)) {
      // a note placed on a step starts that step: look a hair before the position
      seek(p - 1e-6);
      patPrimed_ = true;
      lastP_ = p;
    } else if (p + 1e-9 < lastP_) {
      // wrapped (or jumped back): finish the cycle, then start over
      if (lastP_ - p > patBeats * 0.5) {
        const int n = static_cast<int>(aux_->steps.size());
        while (nextStep_ < n) {
          trigger(nextStep_, nextSub_);
          advance();
        }
        seek(0);
      } else {
        seek(p);
      }
    }
    const int n = static_cast<int>(aux_->steps.size());
    while (nextStep_ < n && eventBeat(nextStep_, nextSub_) <= p + 1e-9) {
      trigger(nextStep_, nextSub_);
      advance();
    }
    lastP_ = p;
  }

  const SeltmannAux* aux_ = nullptr;
  const Render* render_ = nullptr;
  const Render* forced_ = nullptr;
  bool offline_ = false;
  uint32_t auxGen_ = 0;
  // Offline renders (export, not the realtime audio thread): a private warp
  // render made here, so the output never depends on when the worker thread
  // finishes (the stretched sound would start at a random point of the render).
  std::unique_ptr<Render> own_;
  std::vector<std::unique_ptr<Render>> ownOld_;  // replaced, still read by voices
  std::shared_ptr<WarpSlot> ownFor_;  // the slot own_ was made from (kept alive: no reused address)
  double ownTempo_ = 0;
  std::array<Voice, kMaxVoices> voices_{};
  uint64_t ageCounter_ = 0;
  double sr_ = 44100, tempo_ = 120, tick_ = 0;
  int ppq_ = 96;
  bool playing_ = false, jumped_ = false;
  int64_t blockOff_ = 0;
  double vol_ = 0.8, panL_ = 1, panR_ = 1;
  int base_ = 60, keyMap_ = 0, sync_ = SyncStretch;
  bool lock_ = true, mono_ = false, reverseAll_ = false;
  double attack_ = 0, decay_ = 0, sustain_ = 1, release_ = 0.01, declick_ = 0.003;
  double swing_ = 0, pitch_ = 0, velSens_ = 0.6, gate_ = 1, speed_ = 1;
  bool filterOn_ = false;
  double g_ = 0, k_ = 2, reso_ = 0;
  double ic1L_ = 0, ic2L_ = 0, ic1R_ = 0, ic2R_ = 0;
  // pattern
  bool patOn_ = false, patPrimed_ = false;
  uint32_t patId_ = 0;
  double patSemis_ = 0;
  float patVel_ = 1;
  double patElapsed_ = 0, lastP_ = 0;
  int nextStep_ = 0, nextSub_ = 0;
  std::atomic<int> display_{-1};
  std::atomic<double> liveFrame_{-1}, livePat_{-1};
  std::atomic<bool> patRun_{false};  // J-110: patOn_ for the editor's Play button

  // The pattern starts / stops (from step 1 on its next start: noteOnImpl).
  void setPat(bool on) {
    patOn_ = on;
    patRun_.store(on, std::memory_order_relaxed);
    if (!on) livePat_.store(-1, std::memory_order_relaxed);
  }

  // The private render for this tempo (offline only: allocates and stretches).
  // 0.7.8: every tempo gets its render from its first block, also while the tempo moves (a ramp
  // meets a new tempo every few blocks; a slice starting on one played the plain resampled source,
  // up to several semitones off): no slice of an export plays off its pitch. A tempo met again
  // plays the render made for it (the same frames): the renders of the last tempos no voice reads stay, up to these.
  static constexpr int kKeepRenders = 16;
  static constexpr int64_t kKeepFrames = 4 * 1024 * 1024;  // (32 MB of buffers per instance)
  static constexpr double kNearCents = 3;  // (a tempo step of a bpm is 12 cents or more: never near)
  const Render* offlineRender() {
    int kept = 0;
    int64_t keptFrames = 0;
    for (size_t k = ownOld_.size(); k-- > 0;) {  // (newest first)
      const Render& r = *ownOld_[k];
      bool keep = false;
      if (!r.lengthOnly) {  // K-321: no voice reads a length-only render
        for (const auto& v : voices_) keep = keep || (v.on && v.L == r.L.data());
        const auto size = static_cast<int64_t>(r.L.size());
        // (one no voice has read holds nothing: it is made again for nothing if its tempo comes back)
        if (!keep && r.from && r.from == ownFor_ && size > 0 && kept < kKeepRenders && keptFrames + size <= kKeepFrames) {
          keep = true;
          ++kept;
          keptFrames += size;
        }
      }
      if (!keep) ownOld_.erase(ownOld_.begin() + static_cast<long>(k));
    }
    const bool mine = ownFor_ == aux_->warp;
    if (mine && std::fabs(ownTempo_ - tempo_) < 1e-4) return own_.get();  // (null: too long to stretch)
    // a ramp moves on by a hair each few blocks: the render it plays stays while the tempo is within
    // kNearCents of the render's (its voices play it that much faster or slower: nearSpeed), so a ramp
    // stretches a few hundred renders, not one for every slice
    if (mine && own_ && std::fabs(1200.0 * std::log2(tempo_ / ownTempo_)) <= kNearCents) return own_.get();
    if (own_) ownOld_.push_back(std::move(own_));
    ownFor_ = aux_->warp;
    ownTempo_ = tempo_;
    if (tap::readsOnly() || wholeOfflineRenders()) {
      // K-321: a Track Receipt pass that only measures stretches nothing (each tempo it meets
      // would stretch the whole region again): the voices read where they would, silently.
      own_ = makeRender(aux_->warp->src, aux_->warp->grid, aux_->warp->region(), tempo_, tap::readsOnly());
      return own_.get();
    }
    // 0.7.8: the render at this tempo, made as far as the voices read it (one made for this tempo
    // before goes on where it stopped)
    for (size_t k = 0; k < ownOld_.size(); ++k)
      if (ownOld_[k]->from == ownFor_ && ownOld_[k]->tempo == tempo_) {
        own_ = std::move(ownOld_[k]);
        ownOld_.erase(ownOld_.begin() + static_cast<long>(k));
        return own_.get();
      }
    own_ = renderInPieces(aux_->warp, tempo_);
    return own_.get();
  }

  // UI side channel: the source frame under the newest voice, the pattern position.
  void publish() {
    const Voice* nv = nullptr;
    for (const auto& v : voices_)
      if (v.on && (!nv || v.age > nv->age)) nv = &v;
    double f = -1;
    if (nv && aux_) {
      f = sourceFrame(*nv);
    }
    liveFrame_.store(f, std::memory_order_relaxed);
    double pp = -1;
    if (patOn_ && aux_ && aux_->patBeats > 0) pp = lastP_ / aux_->patBeats;
    livePat_.store(pp, std::memory_order_relaxed);
  }
};

// ================================================================== offline helpers
PluginState stateFor(const std::vector<float>& params) {
  PluginState st = defaultPluginState(kType);
  for (size_t i = 0; i < params.size() && i < st.params.size(); ++i) st.params[i] = params[i];
  return st;
}

struct Offline {
  std::shared_ptr<SeltmannAux> aux;
  std::unique_ptr<Render> warp;
  ChannelAux ch;
  PluginState st;
  SeltmannGen gen;
};

std::unique_ptr<Offline> makeOffline(const Model& m, SamplePtr src, const RenderOpts& o) {
  if (!src || m.slices.empty()) return nullptr;
  auto off = std::make_unique<Offline>();
  off->st = stateFor(o.params);
  off->st.params[SmSync] = static_cast<float>(o.sync);
  off->aux = buildAux(m, src, "", o.stretchMode, false);
  if (o.sync == SyncStretch && off->aux->warp) {
    off->warp = makeRender(off->aux->warp->src, off->aux->warp->grid, off->aux->warp->region(), o.tempo);
    off->gen.forceRender(off->warp.get());
  }
  off->ch.custom = off->aux;
  off->ch.samples.assign(1, src);
  return off;
}

void run(Offline& off, const RenderOpts& o, double startTick, int64_t frames, std::vector<float>& L, std::vector<float>& R,
         const std::function<void(Generator&, int64_t)>& events) {
  L.assign(static_cast<size_t>(frames), 0.f);
  R.assign(static_cast<size_t>(frames), 0.f);
  ProcessContext ctx;
  ctx.sampleRate = o.sampleRate;
  ctx.tempo = o.tempo;
  ctx.ppq = 960;
  ctx.playing = true;
  ctx.offline = true;
  const int block = 256;
  for (int64_t pos = 0; pos < frames; pos += block) {
    const int n = static_cast<int>(std::min<int64_t>(block, frames - pos));
    ctx.tick = startTick + static_cast<double>(pos) / o.sampleRate * o.tempo / 60.0 * ctx.ppq;
    ctx.jumped = pos == 0;
    if (events) events(off.gen, pos);
    off.gen.process(off.st, off.ch, ctx, L.data() + pos, R.data() + pos, n);
  }
}

}  // namespace

bool liveView(const Generator* g, LiveView& out) {
  const auto* sg = dynamic_cast<const SeltmannGen*>(g);
  if (!sg) return false;
  const int d = sg->displayStep();
  out.step = displayStepOf(d);
  out.slice = displaySliceOf(d);
  out.srcFrame = sg->liveFrame();
  out.patternPos = sg->livePattern();
  out.patternRunning = sg->patternRunning();
  return true;
}

void setWarpTimesForTest(int lingerMs, int graceMs) {
  WarpService::lingerMs.store(std::max(0, lingerMs));
  WarpService::graceMs.store(std::max(0, graceMs));
}
int warpFamiliesForTest() { return WarpService::get().familiesForTest(); }
void setWarpCapsForTest(int64_t fileBytes, int64_t totalBytes) {
  WarpService::fileCap.store(std::max<int64_t>(0, fileBytes));
  WarpService::totalCap.store(std::max<int64_t>(0, totalBytes));
}
WarpStats warpStatsForTest() {
  WarpStats s;
  s.bytes = gWarpBytes.load();
  s.peakBytes = gWarpPeak.load();
  s.renders = gWarpRenders.load();
  s.moving = gWarpMoving.load();
  s.skipped = gWarpSkipped.load();
  s.throttled = gWarpThrottled.load();
  s.workSeconds = static_cast<double>(gWarpWorkNs.load()) * 1e-9;
  return s;
}
void resetWarpPeakForTest() { gWarpPeak.store(gWarpBytes.load()); }

// ================================================================== pack entry points

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (type == kType) return std::make_unique<SeltmannGen>();
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string&) { return nullptr; }

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != kType) return false;
  const std::string path = pathOf(state.data);
  aux.samples.clear();
  aux.custom.reset();
  if (path.empty()) return true;
  SamplePtr src = sampleCache().get(path);
  aux.samples.assign(1, src);
  if (!src) return true;
  (void)stretch::sincTable(stretch::Quality::Realtime);
  Model m;
  std::string p2;
  if (!modelOf(state, src, p2, m)) return true;
  // 0.7.8: one stretch (Follow: Stretch / Resample / Off). Every stored stretch mode plays it; the
  // Rubber Band choice of older projects (3) plays it too.
  aux.custom = buildAux(m, src, path, 0, true);
  return true;
}

// ================================================================== renders

double patternSeconds(const Model& m, const RenderOpts& o) {
  const int sp = o.params.size() > SmSpeed ? std::clamp(static_cast<int>(std::lround(o.params[SmSpeed])), 0, 2) : SpeedNormal;
  const double speed = sp == SpeedHalf ? 0.5 : sp == SpeedDouble ? 2.0 : 1.0;
  const double bpm = o.sync == SyncOriginal ? (m.grid.bpm > 1 ? m.grid.bpm : 120.0) : o.tempo;
  return cycleBeats(m) / speed * 60.0 / bpm;
}

bool renderPatternLoop(const Model& m, SamplePtr src, const RenderOpts& o, std::vector<float>& L, std::vector<float>& R) {
  auto off = makeOffline(m, src, o);
  if (!off || m.pattern.steps.empty()) return false;
  const int base = std::clamp(static_cast<int>(std::lround(off->st.params[SmBaseNote])), 12, 127);
  const int64_t cycle = static_cast<int64_t>(std::llround(patternSeconds(m, o) * o.sampleRate));
  if (cycle <= 0) return false;
  // free-running (no song lock) from the note: three cycles, keep the middle one,
  // which carries the previous cycle's tails exactly as the loop wraps
  off->st.params[SmLock] = 0;
  std::vector<float> l3, r3;
  run(*off, o, 0, cycle * 3, l3, r3, [&](Generator& g, int64_t pos) {
    if (pos == 0) g.noteOn(1, base - 12, 1.0f, 0);
  });
  L.assign(l3.begin() + cycle, l3.begin() + 2 * cycle);
  R.assign(r3.begin() + cycle, r3.begin() + 2 * cycle);
  return true;
}

bool renderSlice(const Model& m, SamplePtr src, int slice, const RenderOpts& o, std::vector<float>& L, std::vector<float>& R) {
  auto off = makeOffline(m, src, o);
  if (!off || slice < 0 || slice >= static_cast<int>(m.slices.size())) return false;
  off->st.params[SmKeyMap] = KeySlices;
  const int base = std::clamp(static_cast<int>(std::lround(off->st.params[SmBaseNote])), 0, 127);
  const auto& sl = m.slices[static_cast<size_t>(slice)];
  const double sp = o.sync == SyncOriginal ? 1.0 : o.tempo / std::max(1.0, m.grid.bpm);
  const double pitch = std::pow(2.0, (sl.pitch + (o.params.size() > SmPitch ? o.params[SmPitch] : 0.0f)) / 12.0);
  const double secs = static_cast<double>(sl.end - sl.start) / src->sampleRate / std::max(0.05, sp * pitch);
  const int64_t frames = static_cast<int64_t>(std::llround(secs * o.sampleRate)) + static_cast<int64_t>(o.sampleRate * 0.01);
  if (base + slice > 127) return false;
  run(*off, o, 0, frames, L, R, [&](Generator& g, int64_t pos) {
    if (pos == 0) g.noteOn(1, base + slice, 1.0f, 0);
  });
  // trim trailing silence
  int64_t end = static_cast<int64_t>(L.size());
  while (end > 1 && std::fabs(L[static_cast<size_t>(end - 1)]) < 1e-7f && std::fabs(R[static_cast<size_t>(end - 1)]) < 1e-7f) --end;
  L.resize(static_cast<size_t>(end));
  R.resize(static_cast<size_t>(end));
  return true;
}

bool renderSpan(const Model& m, SamplePtr src, double b0, double b1, const RenderOpts& o, std::vector<float>& L, std::vector<float>& R) {
  if (!src || b1 <= b0 || o.tempo <= 0) return false;
  const double fade = 0.005;  // seam crossfade (s)
  const double bpm = o.sync == SyncOriginal ? std::max(1.0, m.grid.bpm) : o.tempo;
  const int64_t len = static_cast<int64_t>(std::llround((b1 - b0) * 60.0 / bpm * o.sampleRate));
  const int64_t F = std::max<int64_t>(8, static_cast<int64_t>(fade * o.sampleRate));
  const double preBeats = static_cast<double>(F) / o.sampleRate * bpm / 60.0;
  const double a0 = b0 - preBeats;
  std::vector<float> l, r;
  const int64_t total = len + F;
  l.assign(static_cast<size_t>(total), 0.f);
  r.assign(static_cast<size_t>(total), 0.f);
  const Sample& s = *src;
  const int64_t n = static_cast<int64_t>(s.frames());
  const float* in[2] = {s.left.data(), s.right.data()};
  if (o.sync == SyncStretch) {
    WarpSlot w;
    w.src = src;
    w.grid = m.grid;
    w.a = std::clamp<int64_t>(static_cast<int64_t>(std::floor(frameAt(m.grid, a0))) - 1, 0, n - 1);
    w.b = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(frameAt(m.grid, b1))) + 1, w.a + 1, n);
    float* out[2] = {l.data(), r.data()};
    const float* inw[2] = {in[0] + w.a, in[1] + w.a};
    stretch::Settings st;
    st.mode = stretch::kStretch;  // 0.7.8: one stretch for every stored mode
    st.pitch = s.sampleRate / o.sampleRate;  // output frames play at the project rate: keep the musical pitch
    const double outBeat = 60.0 / bpm * o.sampleRate;
    stretch::processMapped(inw, 2, w.b - w.a, out, total, s.sampleRate, st, [&](int64_t j) {
      return std::clamp(frameAt(m.grid, a0 + static_cast<double>(j) / outBeat) - static_cast<double>(w.a), 0.0, static_cast<double>(w.b - w.a - 1));
    });
  } else {
    // resample: read the source along the grid (speed follows the project tempo)
    const auto& tab = stretch::sincTable(stretch::Quality::Offline);
    const double outBeat = 60.0 / bpm * o.sampleRate;
    for (int64_t j = 0; j < total; ++j) {
      const double f = frameAt(m.grid, a0 + static_cast<double>(j) / outBeat);
      const double f2 = frameAt(m.grid, a0 + static_cast<double>(j + 1) / outBeat);
      float x, y;
      stretch::readSinc2(in[0], in[1], n, f, std::max(0.01, f2 - f), tab, x, y);
      l[static_cast<size_t>(j)] = x;
      r[static_cast<size_t>(j)] = y;
    }
  }
  // the loop: [F, F + len); its last F frames fade into the audio that preceded b0
  L.assign(l.begin() + F, l.end());
  R.assign(r.begin() + F, r.end());
  for (int64_t k = 0; k < F && k < len; ++k) {
    const double w = (static_cast<double>(k) + 0.5) / static_cast<double>(F);
    const double c = 0.5 - 0.5 * std::cos(w * 3.14159265358979);
    const size_t d = static_cast<size_t>(len - F + k);
    L[d] = static_cast<float>(L[d] * (1 - c) + l[static_cast<size_t>(k)] * c);
    R[d] = static_cast<float>(R[d] * (1 - c) + r[static_cast<size_t>(k)] * c);
  }
  return true;
}

std::vector<PatNote> patternNotes(const Model& m, int baseNote, float swing, float gate) {
  std::vector<PatNote> v;
  const int n = static_cast<int>(m.slices.size());
  for (int i = 0; i < static_cast<int>(m.pattern.steps.size()); ++i) {
    const Step& st = m.pattern.steps[static_cast<size_t>(i)];
    const int k = stepSlice(m.pattern, st, n);
    if (k < 0 || baseNote + k > 127) continue;
    const double t0 = stepStart(m.pattern, i, swing);
    // J-214: the last step ends with the steps (the rest up to the bar line stays silent)
    const double t1 = i + 1 < static_cast<int>(m.pattern.steps.size()) ? stepStart(m.pattern, i + 1, swing) : m.pattern.stepsBeats();
    const int rep = std::clamp(st.stutter, 1, 16);
    const double sub = (t1 - t0) / rep;
    for (int r = 0; r < rep; ++r) {
      PatNote pn;
      pn.start = t0 + r * sub;
      pn.length = std::max(1.0 / 96, sub * std::clamp(static_cast<double>(gate), 0.05, 1.0) * st.length);
      pn.key = baseNote + k;
      pn.velocity = std::clamp(static_cast<int>(std::lround(100 * dbToGain(st.gain))), 1, 127);
      pn.modX = std::clamp(128 + static_cast<int>(std::lround(st.pitch * 256.0 / 48.0)), 0, 255);
      pn.modY = st.reverse ? 255 : 128;
      v.push_back(pn);
    }
  }
  return v;
}

bool writeLoopWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R, double rate) {
  AudioBuffer b(2, L.size());
  for (size_t i = 0; i < L.size(); ++i) {
    b.at(0, i) = L[i];
    b.at(1, i) = i < R.size() ? R[i] : L[i];
  }
  return writeWav(path, b, static_cast<int>(std::lround(rate)), 24);
}

bool writePatternMidi(const std::string& path, const std::vector<PatNote>& notes, double tempo) {
  MidiFileData d;
  d.division = 96;
  d.tempo = tempo;
  MidiTrack t;
  t.name = "Seltmann";
  for (const auto& n : notes) {
    MidiNote mn;
    mn.start = static_cast<int64_t>(std::llround(n.start * 96));
    mn.length = std::max<int64_t>(1, static_cast<int64_t>(std::llround(n.length * 96)));
    mn.pitch = n.key;
    mn.velocity = n.velocity;
    t.notes.push_back(mn);
  }
  d.tracks.push_back(t);
  return writeMidiFile(path, d);
}

double patternBars(const Model& m, const std::vector<float>& params, int beatsPerBar) {
  const int sp = params.size() > SmSpeed ? std::clamp(static_cast<int>(std::lround(params[SmSpeed])), 0, 2) : SpeedNormal;
  const double speed = sp == SpeedHalf ? 0.5 : sp == SpeedDouble ? 2.0 : 1.0;
  return cycleBeats(m) / speed / std::max(1, beatsPerBar);
}

ObjectId sendPatternToSong(Song& s, ObjectId channel, const Model& m, const std::vector<float>& params, int cycles, int track, Tick at, bool link) {
  if (m.pattern.steps.empty() || !s.channel(channel)) return kNoId;
  auto prm = [&](int k, float d) { return k < static_cast<int>(params.size()) ? params[static_cast<size_t>(k)] : d; };
  const int sp = std::clamp(static_cast<int>(std::lround(prm(SmSpeed, SpeedNormal))), 0, 2);
  const double speed = sp == SpeedHalf ? 0.5 : sp == SpeedDouble ? 2.0 : 1.0;
  const double cycleB = cycleBeats(m) / speed;
  cycles = std::clamp(cycles, 1, 64);
  gl::Pattern p;
  p.id = s.allocateId();
  p.name = "Seltmann " + m.pattern.name;
  p.color = 0xE0A04A;
  writePatternNotes(s, p, channel, m, params, cycles);  // 0.7.8: the same notes a linked pattern follows
  if (link) {
    p.linkChannel = channel;
    p.linkCycles = cycles;
    p.linkHash = notesHash(p.notes[channel]);
  }
  s.patterns.push_back(p);
  if (track >= 0) {
    Clip c;
    c.id = s.allocateId();
    c.pattern = p.id;
    c.track = std::max(0, track);
    c.start = std::max<Tick>(0, at);
    c.length = static_cast<Tick>(std::llround(cycleB * cycles * s.ppq));
    s.playlist.push_back(c);
  }
  return p.id;
}

}  // namespace gl::packs::seltmann
