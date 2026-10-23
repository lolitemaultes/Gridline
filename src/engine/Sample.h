// Gridline — audio sample data and loading (WAV, FLAC, MP3, Ogg Vorbis, WavPack) with a cache.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace gl {

// Right channel of a Sample (J-30): its own data, or — mono and dual-mono
// files — the sample's left channel itself, so mono material is stored once.
// Reads behave like a const std::vector<float>; writes (non-const access)
// first give the channel its own copy.
class SampleRight {
 public:
  using value_type = float;
  const std::vector<float>& vec() const { return alias_ ? *alias_ : own_; }
  operator const std::vector<float>&() const { return vec(); }
  size_t size() const { return vec().size(); }
  bool empty() const { return vec().empty(); }
  const float* data() const { return vec().data(); }
  const float& operator[](size_t i) const { return vec()[i]; }
  std::vector<float>::const_iterator begin() const { return vec().begin(); }
  std::vector<float>::const_iterator end() const { return vec().end(); }
  // The channel's own storage (copies the left channel when it was shared).
  std::vector<float>& mut() {
    if (alias_) {
      own_ = *alias_;
      alias_ = nullptr;
    }
    return own_;
  }
  float& operator[](size_t i) { return mut()[i]; }
  float* data() { return mut().data(); }
  void resize(size_t n) { mut().resize(n); }
  void resize(size_t n, float v) { mut().resize(n, v); }
  void assign(size_t n, float v) {
    alias_ = nullptr;
    own_.assign(n, v);
  }
  template <class It>
  void assign(It a, It b) {
    alias_ = nullptr;
    own_.assign(a, b);
  }
  void swap(std::vector<float>& v) { mut().swap(v); }
  SampleRight& operator=(const std::vector<float>& v) {
    alias_ = nullptr;
    own_ = v;
    return *this;
  }
  SampleRight& operator=(std::vector<float>&& v) {
    alias_ = nullptr;
    own_ = std::move(v);
    return *this;
  }
  bool shared() const { return alias_ != nullptr; }

 private:
  friend struct Sample;
  std::vector<float> own_;
  const std::vector<float>* alias_ = nullptr;  // the owning Sample's left channel
};
inline bool operator==(const std::vector<float>& a, const SampleRight& b) { return &a == &b.vec() || a == b.vec(); }
inline bool operator!=(const std::vector<float>& a, const SampleRight& b) { return !(a == b); }

struct Sample {
  std::string path;
  double sampleRate = 44100.0;
  std::vector<float> left;  // equal length; mono files share the left channel (setMono)
  SampleRight right;
  size_t frames() const { return left.size(); }
  double seconds() const { return sampleRate > 0 ? static_cast<double>(frames()) / sampleRate : 0.0; }
  // The right channel becomes the left one (no second copy). Call once the left
  // channel holds its final data; writing the right channel later splits them.
  void setMono() {
    right.own_.clear();
    right.own_.shrink_to_fit();
    right.alias_ = &left;
  }
  bool mono() const { return right.shared(); }

  Sample() = default;
  Sample(const Sample& o) : path(o.path), sampleRate(o.sampleRate), left(o.left), right(o.right) { rebind(o); }
  Sample(Sample&& o) noexcept : path(std::move(o.path)), sampleRate(o.sampleRate), left(std::move(o.left)), right(std::move(o.right)) {
    rebind(o);
  }
  Sample& operator=(const Sample& o) {
    if (this != &o) {
      path = o.path, sampleRate = o.sampleRate, left = o.left, right = o.right;
      rebind(o);
    }
    return *this;
  }
  Sample& operator=(Sample&& o) noexcept {
    if (this != &o) {
      path = std::move(o.path), sampleRate = o.sampleRate, left = std::move(o.left), right = std::move(o.right);
      rebind(o);
    }
    return *this;
  }

 private:
  void rebind(const Sample& o) { right.alias_ = o.right.alias_ ? &left : nullptr; }
};

using SamplePtr = std::shared_ptr<const Sample>;

// Decodes a file by content (WAV, FLAC, Ogg Vorbis also inside WAV, WavPack), else by
// extension (.mp3). nullptr on failure.
SamplePtr loadSample(const std::string& path, std::string* err = nullptr);
bool isAudioFile(const std::string& path);

// J-51: while a scope is alive, loading on its thread gives up as soon as
// *flag is true (loadSample returns nullptr, err "cancelled"): the file is read
// and decoded in blocks, the flag checked between them. The browser indexer
// stops within a block of a long file instead of after it. Scopes nest.
class DecodeCancelScope {
 public:
  explicit DecodeCancelScope(const std::atomic<bool>* flag);
  ~DecodeCancelScope();
  DecodeCancelScope(const DecodeCancelScope&) = delete;
  DecodeCancelScope& operator=(const DecodeCancelScope&) = delete;

 private:
  const std::atomic<bool>* prev_;
};
// True when the calling thread's scope flag is set (decoders check it).
bool decodeCancelled();

// Thread-safe path → sample cache.
//
// get() decodes on the calling thread (or waits for a background decode of the
// same file already running). J-30: request() never blocks — it returns the
// sample once it is decoded and otherwise queues it for the background
// decoders; generation() changes whenever one finishes, so the app can rebuild
// its program (sounds that are not ready yet play silence) and show a loading
// state while pending() > 0. Offline renders use get() and so wait.
class SampleCache {
 public:
  ~SampleCache();
  SamplePtr get(const std::string& path);
  SamplePtr request(const std::string& path);
  // Decodes the files not cached yet on up to `threads` worker threads and
  // waits for them (J-25: projects reference hundreds of samples; decoding
  // them one by one dominated project load and the first render).
  void prefetch(const std::vector<std::string>& paths, int threads = 0);
  void clear();
  // Drops the samples nothing else holds any more (e.g. after a program swap
  // or a project change); returns how many were released.
  size_t trim();
  // Background decodes queued or running.
  int pending() const;
  bool isPending(const std::string& path) const;
  // Changes every time a background decode finishes.
  uint64_t generation() const { return gen_.load(std::memory_order_acquire); }
  // Blocks until no background decode is queued or running.
  void waitIdle();
  // Holds queued decodes back (tests: a deterministic "not decoded yet" state).
  void pauseBackground(bool paused);
  // J-103 app quit: drops queued decodes and joins the background decoders (a running
  // decode finishes), before main() returns: none runs into static destruction. get()
  // still decodes on the calling thread; request() queues nothing any more. Idempotent.
  void shutdown();

  // While an instance lives on a thread, get() on that thread behaves like
  // request() (program builds in the app: nothing waits for the disk).
  class NonBlocking {
   public:
    NonBlocking();
    ~NonBlocking();
    NonBlocking(const NonBlocking&) = delete;
    NonBlocking& operator=(const NonBlocking&) = delete;

   private:
    bool prev_;
  };

 private:
  SamplePtr decodeLocked(std::unique_lock<std::mutex>& lk, const std::string& path);
  void enqueueLocked(const std::string& path);
  void worker();
  mutable std::mutex mu_;
  std::condition_variable done_;  // a decode finished
  std::condition_variable work_;  // the queue has work
  std::map<std::string, SamplePtr> cache_;
  std::map<std::string, bool> failed_;
  std::set<std::string> inflight_;  // being decoded right now
  std::deque<std::string> queue_;   // waiting for a background decoder
  std::vector<std::thread> workers_;
  bool stop_ = false, paused_ = false;
  std::atomic<uint64_t> gen_{0};
};

SampleCache& sampleCache();

}  // namespace gl
