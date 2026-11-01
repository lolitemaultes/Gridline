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
  // Written by the thread that renders the owner (receipt passes render on
  // one thread); read after the pass ended.
  int64_t plays = 0;
  double tick0 = 1e300, tick1 = -1e300;
  Range ranges[Pass::kMaxRanges];
  int count = 0;
  bool overflow = false;
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
};

namespace {

std::atomic<PassData*> g_pass{nullptr};
thread_local ObjectId t_owner = kNoId;
thread_local double t_tick = 0;

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

void addRange(Slot& s, double a, double b) {
  constexpr double kJoin = 0.002;  // seconds: ranges this close become one
  // Find the insertion point (sorted by start), merge with neighbours.
  int i = 0;
  while (i < s.count && s.ranges[i].b + kJoin < a) ++i;
  if (i < s.count && s.ranges[i].a <= b + kJoin) {  // overlaps range i (and maybe more)
    s.ranges[i].a = std::min(s.ranges[i].a, a);
    s.ranges[i].b = std::max(s.ranges[i].b, b);
    int j = i + 1;
    while (j < s.count && s.ranges[j].a <= s.ranges[i].b + kJoin) {
      s.ranges[i].b = std::max(s.ranges[i].b, s.ranges[j].b);
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
    for (int k = best + 1; k + 1 < s.count; ++k) s.ranges[k] = s.ranges[k + 1];
    --s.count;
    s.overflow = true;
    addRange(s, a, b);  // (room for it now)
    return;
  }
  for (int k = s.count; k > i; --k) s.ranges[k] = s.ranges[k - 1];
  s.ranges[i] = Range{a, b};
  ++s.count;
}

}  // namespace

bool active() { return g_pass.load(std::memory_order_relaxed) != nullptr; }

Slot* slotFor(const Sample* s) {
  PassData* d = g_pass.load(std::memory_order_acquire);
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

void mark(Slot* slot, double a, double b) {
  PassData* d = g_pass.load(std::memory_order_acquire);
  if (!slot || !d) return;
  const WatchFile& w = d->files[static_cast<size_t>(slot->file)];
  const double len = w.seconds > 0 ? w.seconds : 1e9;
  if (!(a == a) || !(b == b)) return;  // NaN
  a = std::clamp(a, 0.0, len), b = std::clamp(b, 0.0, len);
  if (b < a) std::swap(a, b);
  if (b - a <= 0) return;
  addRange(*slot, a, b);
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

void markShare(Slot* slot, double f0, double f1) {
  PassData* d = g_pass.load(std::memory_order_acquire);
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
  t_owner = owner;
  t_tick = tick;
}

OwnerScope::~OwnerScope() {
  if (!on_) return;
  t_owner = prevOwner_;
  t_tick = prevTick_;
}

void Run::begin(const Sample* s) {
  flush();
  slot_ = active() ? slotFor(s) : nullptr;
  toSec_ = s && s->sampleRate > 0 ? 1.0 / s->sampleRate : 0;
  if (toSec_ <= 0) slot_ = nullptr;
}

void Run::flushOpen() {
  open_ = false;
  mark(slot_, lo_ * toSec_, (hi_ + 1) * toSec_);
}

// ---- Pass

Pass::Pass(std::vector<WatchFile> files) : d_(std::make_unique<PassData>()) {
  PassData& d = *d_;
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

Pass::~Pass() { end(); }

bool Pass::begin() {
  PassData* expect = nullptr;
  return g_pass.compare_exchange_strong(expect, d_.get(), std::memory_order_acq_rel) || expect == d_.get();
}

void Pass::end() {
  PassData* expect = d_.get();
  g_pass.compare_exchange_strong(expect, nullptr, std::memory_order_acq_rel);
}

const std::vector<WatchFile>& Pass::files() const { return d_->files; }

bool Pass::pairsFull() const { return d_->full.load(std::memory_order_relaxed); }

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
    out.push_back(std::move(u));
  }
  std::sort(out.begin(), out.end(), [](const Use& a, const Use& b) { return a.file != b.file ? a.file < b.file : a.owner < b.owner; });
  return out;
}

}  // namespace gl::tap
