// Gridline - J-219: sample coverage tap (see SampleTap.h).
#include "SampleTap.h"
#include <algorithm>
#include <cmath>
#include "Sample.h"

namespace gl::tap {

struct Slot {
  std::atomic<uint64_t> key{0};  // (file + 1) << 32 | owner; 0 = free
  int file = -1;
  ObjectId owner = kNoId;
  // Written by the thread that renders the owner: each owner (a channel, an
  // audio clip within its channel, a mixer track) is one task of the mixer
  // graph, so one thread at a time even when K-319 passes spread the graph
  // over several; the block barriers order them. Read after the pass ended.
  int64_t plays = 0;
  double tick0 = 1e300, tick1 = -1e300;
  Range ranges[Pass::kMaxRanges];
  int count = 0;
  bool overflow = false;
  // K-321
  int64_t outFrames = 0;  // (whole frames: the same sum however the blocks split)
  double outRate = 0;
  int64_t jumps = 0;
  int64_t pitched = 0;
  double semisMin = 0, semisMax = 0, ratioMin = 0, ratioMax = 0;
  static constexpr int kRecent = 4;  // segments a new one may extend (voices of one owner interleave)
  int recent[kRecent] = {};
  int nRecent = 0;
};

struct SegEntry {
  Segment g;
  int slot = -1;
};

struct PassData {
  std::vector<WatchFile> files;
  std::vector<uint64_t> hashes;
  std::vector<int> index;  // open addressing: file + 1, 0 = empty
  size_t indexMask = 0;
  std::vector<std::unique_ptr<std::atomic<uint64_t>[]>> bits;
  std::vector<size_t> words;
  std::unique_ptr<Slot[]> slots;
  std::atomic<bool> full{false};
  std::atomic<uint64_t> marks{0};       // K-319: reads marked (the receipt render's tail follows it)
  std::atomic<bool> readsOnly{false};  // K-319
  // K-321: the segment log (Pass::Options::segments): entries claimed with fetch_add; an entry
  // belongs to its slot's thread, which extends it in place.
  bool segOn = false;
  size_t segCap = 0;
  std::unique_ptr<SegEntry[]> segs;
  std::atomic<size_t> segN{0};
  std::atomic<bool> segFull{false};
};

namespace {

thread_local PassData* t_pass = nullptr;  // K-322: bound per thread (tap::Bind), never process-wide
thread_local ObjectId t_owner = kNoId;
thread_local double t_tick = 0;
thread_local int64_t t_outFrame = 0;  // K-321: the engine frame the block starts at (OwnerScope)
thread_local double t_rate = 0;       // K-321: the engine's rate (0: no output clock)
thread_local int t_sub = 0;           // K-321: blockOffset

uint64_t hashOf(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

int fileOf(const PassData& d, const std::string& path) {
  if (d.index.empty()) return -1;
  const uint64_t h = hashOf(path);
  for (size_t i = h & d.indexMask, n = 0; n <= d.indexMask; i = (i + 1) & d.indexMask, ++n) {
    const int f = d.index[i];
    if (f == 0) return -1;
    if (d.hashes[static_cast<size_t>(f - 1)] == h && d.files[static_cast<size_t>(f - 1)].path == path) return f - 1;
  }
  return -1;
}

Slot* claim(PassData& d, int file, ObjectId owner) {
  const uint64_t key = (static_cast<uint64_t>(file) + 1) << 32 | owner;
  uint64_t h = key * 0x9E3779B97F4A7C15ull;
  h ^= h >> 29;
  for (int n = 0; n < Pass::kMaxPairs; ++n) {
    Slot& s = d.slots[static_cast<size_t>((h + static_cast<uint64_t>(n)) % Pass::kMaxPairs)];
    uint64_t k = s.key.load(std::memory_order_acquire);
    if (k == key) return &s;
    if (k == 0) {
      uint64_t expect = 0;
      if (s.key.compare_exchange_strong(expect, key, std::memory_order_acq_rel)) {
        s.file = file, s.owner = owner;
        return &s;
      }
      if (expect == key) return &s;
    }
  }
  d.full.store(true, std::memory_order_relaxed);
  return nullptr;
}

void addRange(Slot& s, double a, double b, bool cue) {
  // Seconds: ranges this close become one (K-321: a cue block that touches another read is no
  // longer only a cue block).
  constexpr double kJoin = 0.002;
  // Find the insertion point (sorted by start), merge with neighbours.
  int i = 0;
  while (i < s.count && s.ranges[i].b + kJoin < a) ++i;
  if (i < s.count && s.ranges[i].a <= b + kJoin) {  // overlaps range i (and maybe more)
    s.ranges[i].a = std::min(s.ranges[i].a, a);
    s.ranges[i].b = std::max(s.ranges[i].b, b);
    s.ranges[i].cue = s.ranges[i].cue && cue;
    int j = i + 1;
    while (j < s.count && s.ranges[j].a <= s.ranges[i].b + kJoin) {
      s.ranges[i].b = std::max(s.ranges[i].b, s.ranges[j].b);
      s.ranges[i].cue = s.ranges[i].cue && s.ranges[j].cue;
      ++j;
    }
    const int gone = j - i - 1;
    if (gone > 0) {
      for (int k = i + 1; k + gone < s.count; ++k) s.ranges[k] = s.ranges[k + gone];
      s.count -= gone;
    }
    return;
  }
  if (s.count == Pass::kMaxRanges) {  // full: merge the two neighbours closest together
    int best = 0;
    double gap = 1e300;
    for (int k = 0; k + 1 < s.count; ++k)
      if (s.ranges[k + 1].a - s.ranges[k].b < gap) gap = s.ranges[k + 1].a - s.ranges[k].b, best = k;
    s.ranges[best].b = s.ranges[best + 1].b;
    s.ranges[best].cue = s.ranges[best].cue && s.ranges[best + 1].cue;
    for (int k = best + 1; k + 1 < s.count; ++k) s.ranges[k] = s.ranges[k + 1];
    --s.count;
    s.overflow = true;
    addRange(s, a, b, cue);  // (room for it now)
    return;
  }
  for (int k = s.count; k > i; --k) s.ranges[k] = s.ranges[k - 1];
  s.ranges[i] = Range{a, b, cue};
  ++s.count;
}

// K-321: q continues p at the same speed (within half a millisecond at both of q's ends).
bool continues(const Segment& p, const Segment& q) {
  constexpr double kTol = 0.0005;
  const double dp = p.outB - p.outA;
  if (dp <= 0 || q.outB <= q.outA || std::fabs(q.outA - p.outB) > kTol) return false;
  const double slope = (p.srcB - p.srcA) / dp;
  const double atA = p.srcA + slope * (q.outA - p.outA), atB = p.srcA + slope * (q.outB - p.outA);
  return std::fabs(atA - q.srcA) <= kTol && std::fabs(atB - q.srcB) <= kTol;
}

// K-321: logs a segment of `s` (on its own thread): extends a recent one it continues, else
// claims a new entry.
void logSegment(PassData& d, Slot& s, const Segment& q) {
  if (!d.segOn || !(q.outB > q.outA)) return;
  for (int k = 0; k < s.nRecent; ++k) {
    Segment& p = d.segs[static_cast<size_t>(s.recent[k])].g;
    if (continues(p, q)) {
      p.outB = q.outB, p.srcB = q.srcB;
      const int idx = s.recent[k];
      for (int m = k; m > 0; --m) s.recent[m] = s.recent[m - 1];
      s.recent[0] = idx;
      return;
    }
  }
  const size_t idx = d.segN.fetch_add(1, std::memory_order_relaxed);
  if (idx >= d.segCap) {
    d.segFull.store(true, std::memory_order_relaxed);
    return;
  }
  d.segs[idx].g = q;
  d.segs[idx].slot = static_cast<int>(&s - d.slots.get());
  if (s.nRecent < Slot::kRecent) ++s.nRecent;
  for (int m = s.nRecent - 1; m > 0; --m) s.recent[m] = s.recent[m - 1];
  s.recent[0] = static_cast<int>(idx);
}

void markRange(Slot* slot, double a, double b, bool cue) {
  PassData* d = t_pass;
  if (!slot || !d) return;
  const WatchFile& w = d->files[static_cast<size_t>(slot->file)];
  const double len = w.seconds > 0 ? w.seconds : 1e9;
  if (!(a == a) || !(b == b)) return;  // NaN
  a = std::clamp(a, 0.0, len), b = std::clamp(b, 0.0, len);
  if (b < a) std::swap(a, b);
  if (b - a <= 0) return;
  d->marks.fetch_add(1, std::memory_order_relaxed);
  addRange(*slot, a, b, cue);
  slot->tick0 = std::min(slot->tick0, t_tick);
  slot->tick1 = std::max(slot->tick1, t_tick);
  const size_t nw = d->words[static_cast<size_t>(slot->file)];
  if (nw == 0) return;
  const auto g0 = static_cast<int64_t>(std::floor(a / Pass::kGrain + 1e-9));
  auto g1 = static_cast<int64_t>(std::ceil(b / Pass::kGrain - 1e-9));  // exclusive
  g1 = std::min<int64_t>(g1, static_cast<int64_t>(nw * 64));
  std::atomic<uint64_t>* bits = d->bits[static_cast<size_t>(slot->file)].get();
  for (int64_t g = g0; g < g1;) {
    const auto wi = static_cast<size_t>(g >> 6);
    const int bit = static_cast<int>(g & 63);
    const int64_t n = std::min<int64_t>(64 - bit, g1 - g);
    const uint64_t m = n >= 64 ? ~0ull : (((1ull << n) - 1) << bit);
    bits[wi].fetch_or(m, std::memory_order_relaxed);
    g += n;
  }
}

}  // namespace

bool active() { return t_pass != nullptr; }

Slot* slotFor(const Sample* s) {
  PassData* d = t_pass;
  if (!d || !s || s->path.empty()) return nullptr;
  const int f = fileOf(*d, s->path);
  if (f < 0) return nullptr;
  return claim(*d, f, t_owner);
}

void play(Slot* slot) {
  if (!slot) return;
  ++slot->plays;
  slot->tick0 = std::min(slot->tick0, t_tick);
  slot->tick1 = std::max(slot->tick1, t_tick);
}

void play(const Sample* s) {
  if (!active()) return;
  play(slotFor(s));
}

void mark(Slot* slot, double a, double b) { markRange(slot, a, b, false); }

void mark(Slot* slot, double a, double b, int outFrom, int outTo, bool reverse) {
  PassData* d = t_pass;
  if (!slot || !d) return;
  markRange(slot, a, b, false);
  if (t_rate <= 0 || outTo <= outFrom) return;
  slot->outFrames += outTo - outFrom, slot->outRate = t_rate;
  const double o0 = static_cast<double>(t_outFrame + t_sub + outFrom) / t_rate, o1 = static_cast<double>(t_outFrame + t_sub + outTo) / t_rate;
  logSegment(*d, *slot, reverse ? Segment{o0, o1, b, a} : Segment{o0, o1, a, b});
}

void blockOffset(int frames) {
  if (t_pass) t_sub = frames;
}

void pitch(Slot* slot, double semis, double timeRatio) {
  if (!slot || !t_pass || !(semis == semis)) return;
  const bool first = slot->pitched++ == 0;
  slot->semisMin = first ? semis : std::min(slot->semisMin, semis);
  slot->semisMax = first ? semis : std::max(slot->semisMax, semis);
  if (timeRatio > 0) {
    slot->ratioMin = slot->ratioMin > 0 ? std::min(slot->ratioMin, timeRatio) : timeRatio;
    slot->ratioMax = std::max(slot->ratioMax, timeRatio);
  }
}

void pitch(const Sample* s, double semis, double timeRatio) {
  if (!active()) return;
  pitch(slotFor(s), semis, timeRatio);
}

uint64_t marks() {
  const PassData* d = t_pass;
  return d ? d->marks.load(std::memory_order_relaxed) : 0;
}

bool readsOnly() {
  const PassData* d = t_pass;
  return d && d->readsOnly.load(std::memory_order_relaxed);
}

void markShare(Slot* slot, double f0, double f1) {
  PassData* d = t_pass;
  if (!slot || !d) return;
  const double len = d->files[static_cast<size_t>(slot->file)].seconds;
  mark(slot, f0 * len, f1 * len);
}

void markFrames(const Sample* s, double f0, double f1) {
  if (!active() || !s || s->sampleRate <= 0) return;
  if (f1 < f0) std::swap(f0, f1);
  mark(slotFor(s), f0 / s->sampleRate, (f1 + 1) / s->sampleRate);
}

OwnerScope::OwnerScope(ObjectId owner, double tick) : prevOwner_(t_owner), prevTick_(t_tick), on_(active()) {
  if (!on_) return;
  prevOut_ = t_outFrame, prevRate_ = t_rate, prevSub_ = t_sub;
  t_owner = owner;
  t_tick = tick;
}

OwnerScope::OwnerScope(ObjectId owner, double tick, int64_t outFrame, double rate) : OwnerScope(owner, tick) {
  if (!on_) return;
  t_outFrame = outFrame;
  t_sub = 0;
  t_rate = rate;
}

OwnerScope::~OwnerScope() {
  if (!on_) return;
  t_owner = prevOwner_;
  t_tick = prevTick_;
  t_outFrame = prevOut_, t_rate = prevRate_, t_sub = prevSub_;
}

void Run::begin(const Sample* s) {
  flush();
  slot_ = active() ? slotFor(s) : nullptr;
  toSec_ = s && s->sampleRate > 0 ? 1.0 / s->sampleRate : 0;
  if (toSec_ <= 0) slot_ = nullptr;
  n_ = i0_ = 0;
  base_ = t_outFrame + t_sub;
  flushed_ = 0;
}

void Run::flushOpen(bool jump) {
  open_ = false;
  PassData* d = t_pass;
  if (!slot_ || !d) return;
  // K-321: the first range of a run that ends in a jump is a cue block (see SampleTap.h).
  markRange(slot_, lo_ * toSec_, (hi_ + 1) * toSec_, jump && flushed_ == 0);
  ++flushed_;
  if (jump) ++slot_->jumps;
  const int64_t end = jump ? n_ - 1 : n_;  // (the call that jumped starts the next range)
  const int64_t frames = end - i0_;
  if (t_rate <= 0 || frames <= 0) return;
  slot_->outFrames += frames, slot_->outRate = t_rate;
  if (!d->segOn) return;
  // From the first frame read to where the next one would be (the step so far).
  const double step = frames > 1 ? (last_ - first_) / static_cast<double>(frames - 1) : 1.0;
  const double o0 = static_cast<double>(base_ + i0_) / t_rate, o1 = static_cast<double>(base_ + end) / t_rate;
  logSegment(*d, *slot_, Segment{o0, o1, first_ * toSec_, (last_ + step) * toSec_});
}

void Cover::covered(const Sample* s, double lo, double hi, int frames, bool jump) {
  PassData* d = t_pass;
  if (!d || !s || s->sampleRate <= 0) return;
  Slot* slot = slotFor(s);
  if (!slot) return;
  markRange(slot, lo / s->sampleRate, (hi + 1) / s->sampleRate, false);
  if (jump) ++slot->jumps;
  if (t_rate > 0 && frames > 0) slot->outFrames += frames, slot->outRate = t_rate;
}

// ---- Pass

Pass::Pass(std::vector<WatchFile> files) : Pass(std::move(files), Options{}) {}

Pass::Pass(std::vector<WatchFile> files, const Options& o) : d_(std::make_unique<PassData>()) {
  PassData& d = *d_;
  if (o.segments && o.segmentCapacity > 0) {
    d.segOn = true;
    d.segCap = o.segmentCapacity;
    d.segs = std::make_unique<SegEntry[]>(d.segCap);
  }
  d.files = std::move(files);
  size_t cap = 4;
  while (cap < d.files.size() * 2 + 2) cap <<= 1;
  d.index.assign(cap, 0);
  d.indexMask = cap - 1;
  d.hashes.resize(d.files.size());
  d.bits.resize(d.files.size());
  d.words.resize(d.files.size());
  for (size_t f = 0; f < d.files.size(); ++f) {
    const uint64_t h = hashOf(d.files[f].path);
    d.hashes[f] = h;
    if (fileOf(d, d.files[f].path) < 0) {
      size_t i = h & d.indexMask;
      while (d.index[i] != 0) i = (i + 1) & d.indexMask;
      d.index[i] = static_cast<int>(f) + 1;
    }
    const double secs = std::max(0.0, d.files[f].seconds);
    const auto grains = static_cast<size_t>(std::ceil(secs / kGrain)) + 1;
    d.words[f] = (grains + 63) / 64;
    d.bits[f] = std::make_unique<std::atomic<uint64_t>[]>(d.words[f]);
    for (size_t w = 0; w < d.words[f]; ++w) d.bits[f][w].store(0, std::memory_order_relaxed);
  }
  d.slots = std::make_unique<Slot[]>(kMaxPairs);
}

Pass::~Pass() = default;

uint64_t Pass::marks() const { return d_->marks.load(std::memory_order_relaxed); }

Bind::Bind(const Pass* pass) : on_(pass != nullptr) {
  if (!on_) {
    return;
  }
  prev_ = t_pass;
  t_pass = pass->d_.get();
}

Bind::~Bind() {
  if (on_) {
    t_pass = prev_;
  }
}

void Pass::setReadsOnly(bool on) { d_->readsOnly.store(on, std::memory_order_relaxed); }

const std::vector<WatchFile>& Pass::files() const { return d_->files; }

bool Pass::pairsFull() const { return d_->full.load(std::memory_order_relaxed); }

bool Pass::segmentsFull() const { return d_->segFull.load(std::memory_order_relaxed); }

std::vector<Range> Pass::fileRanges(int i) const {
  std::vector<Range> out;
  if (i < 0 || static_cast<size_t>(i) >= d_->files.size()) return out;
  const size_t nw = d_->words[static_cast<size_t>(i)];
  const std::atomic<uint64_t>* bits = d_->bits[static_cast<size_t>(i)].get();
  const double len = d_->files[static_cast<size_t>(i)].seconds;
  int64_t start = -1;
  const auto total = static_cast<int64_t>(nw * 64);
  for (int64_t g = 0; g <= total; ++g) {
    const bool on = g < total && (bits[static_cast<size_t>(g >> 6)].load(std::memory_order_relaxed) >> (g & 63) & 1ull);
    if (on && start < 0) start = g;
    if (!on && start >= 0) {
      double a = static_cast<double>(start) * kGrain, b = static_cast<double>(g) * kGrain;
      if (len > 0) b = std::min(b, len);
      if (b > a) out.push_back(Range{a, b});
      start = -1;
    }
  }
  return out;
}

std::vector<Use> Pass::uses() const {
  std::vector<Use> out;
  // K-321: each slot's segments in the order they were logged (one thread per slot: output order).
  std::vector<std::vector<Segment>> segsOf;
  if (d_->segOn) {
    segsOf.resize(kMaxPairs);
    const size_t n = std::min(d_->segN.load(std::memory_order_relaxed), d_->segCap);
    for (size_t i = 0; i < n; ++i)
      if (d_->segs[i].slot >= 0) segsOf[static_cast<size_t>(d_->segs[i].slot)].push_back(d_->segs[i].g);
  }
  for (int k = 0; k < kMaxPairs; ++k) {
    const Slot& s = d_->slots[static_cast<size_t>(k)];
    if (s.key.load(std::memory_order_acquire) == 0) continue;
    if (s.count == 0 && s.plays == 0) continue;
    Use u;
    u.file = s.file;
    u.owner = s.owner;
    u.plays = s.plays;
    u.tick0 = s.tick0, u.tick1 = s.tick1;
    u.ranges.assign(s.ranges, s.ranges + s.count);
    u.overflow = s.overflow;
    if (s.overflow) {  // K-321: merged neighbours: only what the exact bitmap says was read
      const std::vector<Range> exact = fileRanges(s.file);
      std::vector<Range> cut;
      for (const Range& r : u.ranges)
        for (const Range& e : exact) {
          const double a = std::max(r.a, e.a), b = std::min(r.b, e.b);
          if (b > a) cut.push_back(Range{a, b, r.cue});
        }
      u.ranges = std::move(cut);
    }
    u.outSeconds = s.outRate > 0 ? static_cast<double>(s.outFrames) / s.outRate : 0.0;
    u.jumps = s.jumps;
    u.pitched = s.pitched;
    u.semisMin = s.semisMin, u.semisMax = s.semisMax, u.ratioMin = s.ratioMin, u.ratioMax = s.ratioMax;
    if (!segsOf.empty()) u.segments = std::move(segsOf[static_cast<size_t>(k)]);
    out.push_back(std::move(u));
  }
  std::sort(out.begin(), out.end(), [](const Use& a, const Use& b) { return a.file != b.file ? a.file < b.file : a.owner < b.owner; });
  return out;
}

}  // namespace gl::tap
