#include "Sample.h"
#include <thread>
#include <set>
#include <atomic>
#include "../core/PluginParams.h"
#include "Generators.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include "WavPack.h"

#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include "../../third_party/dr_libs/dr_flac.h"
#include "../../third_party/dr_libs/dr_mp3.h"
#include "../../third_party/dr_libs/dr_wav.h"

namespace gl {

namespace {
std::string lowerExt(const std::string& path) {
  auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return "";
  std::string e = path.substr(dot);
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e;
}

SamplePtr fromInterleaved(const std::string& path, float* data, uint64_t frames, unsigned channels, unsigned rate) {
  auto s = std::make_shared<Sample>();
  s->path = path;
  s->sampleRate = rate;
  s->left.resize(frames);
  for (uint64_t i = 0; i < frames; ++i) s->left[i] = data[i * channels];
  // J-30: mono files — and stereo files whose two channels are identical —
  // keep one copy (the right channel shares the left one).
  bool dual = channels <= 1;
  if (!dual) {
    dual = true;
    for (uint64_t i = 0; i < frames && dual; ++i) dual = data[i * channels + 1] == data[i * channels];
  }
  if (dual) {
    s->setMono();
  } else {
    std::vector<float>& r = s->right.mut();
    r.resize(frames);
    for (uint64_t i = 0; i < frames; ++i) r[i] = data[i * channels + 1];
  }
  return s;
}
}  // namespace

bool isAudioFile(const std::string& path) {
  std::string e = lowerExt(path);
  return e == ".wav" || e == ".wave" || e == ".flac" || e == ".mp3" || e == ".ogg" || e == ".wv";
}

namespace {
SamplePtr fromDecoded(const std::string& path, DecodedAudio& d) {
  if (d.channels <= 0 || d.sampleRate <= 0) return nullptr;
  return fromInterleaved(path, d.interleaved.data(), d.frames(), static_cast<unsigned>(d.channels), static_cast<unsigned>(d.sampleRate));
}

// RIFF/WAVE whose "fmt " tag says Ogg Vorbis (0x674f…0x6771): the data chunk
// holds a plain Ogg stream. Returns the stream, or {nullptr, 0}.
std::pair<const uint8_t*, size_t> riffVorbisPayload(const std::vector<uint8_t>& b) {
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) return {nullptr, 0};
  size_t i = 12;
  bool vorbis = false;
  while (i + 8 <= b.size()) {
    uint32_t sz = b[i + 4] | (b[i + 5] << 8) | (b[i + 6] << 16) | (static_cast<uint32_t>(b[i + 7]) << 24);
    if (std::memcmp(b.data() + i, "fmt ", 4) == 0 && i + 10 <= b.size()) {
      unsigned tag = b[i + 8] | (b[i + 9] << 8);
      vorbis = (tag >= 0x674f && tag <= 0x6751) || (tag >= 0x676f && tag <= 0x6771);
    } else if (std::memcmp(b.data() + i, "data", 4) == 0) {
      size_t n = std::min<size_t>(sz, b.size() - (i + 8));
      if (vorbis && n >= 4 && std::memcmp(b.data() + i + 8, "OggS", 4) == 0) return {b.data() + i + 8, n};
      return {nullptr, 0};
    }
    i += 8 + static_cast<size_t>(sz) + (sz & 1u);
  }
  return {nullptr, 0};
}
}  // namespace

namespace {
thread_local const std::atomic<bool>* tCancel = nullptr;

// J-51: cancellable decodes (a DecodeCancelScope is alive): dr_libs streams
// decoded block by block, the flag checked between blocks. The same frames
// as the one-call readers below.
constexpr uint64_t kBlockFrames = 1 << 16;
float* cancellableWav(const std::vector<uint8_t>& bytes, unsigned& ch, unsigned& sr, uint64_t& n) {
  drwav w;
  if (!drwav_init_memory(&w, bytes.data(), bytes.size(), nullptr)) return nullptr;
  float* out = nullptr;
  const uint64_t total = w.totalPCMFrameCount;
  if (w.channels > 0 && total <= (static_cast<uint64_t>(SIZE_MAX) / w.channels / sizeof(float))) {
    out = static_cast<float*>(std::malloc(static_cast<size_t>(total * w.channels * sizeof(float))));
    uint64_t done = 0;
    while (out && done < total && !decodeCancelled()) {
      const uint64_t got = drwav_read_pcm_frames_f32(&w, std::min(kBlockFrames, total - done), out + done * w.channels);
      if (got == 0) break;
      done += got;
    }
    if (out && done != total) drwav_free(out, nullptr), out = nullptr;
    ch = w.channels, sr = w.sampleRate, n = total;
  }
  drwav_uninit(&w);
  return out;
}
// FLAC / MP3: frames appended block by block until the stream ends.
template <class Read>
float* cancellableStream(unsigned channels, Read read, uint64_t& n) {
  std::vector<float> all;
  std::vector<float> block(static_cast<size_t>(kBlockFrames) * std::max(1u, channels));
  while (!decodeCancelled()) {
    const uint64_t got = read(block.data(), kBlockFrames);
    if (got == 0) break;
    all.insert(all.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(got * channels));
  }
  if (decodeCancelled() || all.empty() || channels == 0) return nullptr;
  n = all.size() / channels;
  float* out = static_cast<float*>(std::malloc(all.size() * sizeof(float)));
  if (out) std::memcpy(out, all.data(), all.size() * sizeof(float));
  return out;
}
}  // namespace

DecodeCancelScope::DecodeCancelScope(const std::atomic<bool>* flag) : prev_(tCancel) { tCancel = flag; }
DecodeCancelScope::~DecodeCancelScope() { tCancel = prev_; }
bool decodeCancelled() { return tCancel && tCancel->load(std::memory_order_relaxed); }

SamplePtr loadSample(const std::string& path, std::string* err) {
  // Formats are recognised by content first (FL Studio content often stores
  // Ogg Vorbis under a .wav name, or inside a RIFF wrapper), then by extension.
  const bool cancellable = tCancel != nullptr;
  auto cancelled = [&] {
    if (err) *err = "cancelled";
    return nullptr;
  };
  std::vector<uint8_t> bytes;
  {
    std::ifstream is(path, std::ios::binary);
    if (!is) {
      if (err) *err = "cannot open " + path;
      return nullptr;
    }
    if (!cancellable) {
      bytes.assign(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
    } else {  // in blocks, the flag checked between them
      constexpr size_t kBlock = 4u << 20;
      for (;;) {
        if (decodeCancelled()) return cancelled();
        const size_t at = bytes.size();
        bytes.resize(at + kBlock);
        is.read(reinterpret_cast<char*>(bytes.data() + at), static_cast<std::streamsize>(kBlock));
        bytes.resize(at + static_cast<size_t>(is.gcount()));
        if (!is) break;
      }
    }
  }
  auto magic = [&](const char* m) { return bytes.size() >= 4 && std::memcmp(bytes.data(), m, 4) == 0; };
  std::string e = lowerExt(path);
  unsigned channels = 0, rate = 0;
  uint64_t frames = 0;
  float* data = nullptr;
  SamplePtr out;
  std::string why;
  if (magic("wvpk")) {
    DecodedAudio d;
    if (decodeWavPack(bytes.data(), bytes.size(), d, &why)) out = fromDecoded(path, d);
  } else if (magic("OggS")) {
    DecodedAudio d;
    if (decodeOggVorbis(bytes.data(), bytes.size(), d, &why)) out = fromDecoded(path, d);
  } else if (auto vorb = riffVorbisPayload(bytes); vorb.first) {
    DecodedAudio d;
    if (decodeOggVorbis(vorb.first, vorb.second, d, &why)) out = fromDecoded(path, d);
  } else if (magic("RIFF") || magic("RF64") || magic("RIFX") || e == ".wav" || e == ".wave") {
    unsigned int ch = 0, sr = 0;
    drwav_uint64 n = 0;
    if (cancellable) {
      uint64_t nn = 0;
      data = cancellableWav(bytes, ch, sr, nn);
      n = nn;
    } else {
      data = drwav_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &ch, &sr, &n, nullptr);
    }
    channels = ch, rate = sr, frames = n;
    if (data) out = fromInterleaved(path, data, frames, channels, rate);
    drwav_free(data, nullptr);
  } else if (magic("fLaC") || e == ".flac") {
    unsigned int ch = 0, sr = 0;
    drflac_uint64 n = 0;
    if (cancellable) {
      if (drflac* f = drflac_open_memory(bytes.data(), bytes.size(), nullptr)) {
        uint64_t nn = 0;
        data = cancellableStream(f->channels, [f](float* out, uint64_t k) { return drflac_read_pcm_frames_f32(f, k, out); }, nn);
        ch = f->channels, sr = f->sampleRate, n = nn;
        drflac_close(f);
      }
    } else {
      data = drflac_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &ch, &sr, &n, nullptr);
    }
    channels = ch, rate = sr, frames = n;
    if (data) out = fromInterleaved(path, data, frames, channels, rate);
    drflac_free(data, nullptr);
  } else if (e == ".mp3" || (bytes.size() > 2 && bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0) ||
             (bytes.size() > 3 && std::memcmp(bytes.data(), "ID3", 3) == 0)) {
    drmp3_config cfg{};
    drmp3_uint64 n = 0;
    if (cancellable) {
      drmp3 m;
      if (drmp3_init_memory(&m, bytes.data(), bytes.size(), nullptr)) {
        uint64_t nn = 0;
        data = cancellableStream(m.channels, [&m](float* out, uint64_t k) { return drmp3_read_pcm_frames_f32(&m, k, out); }, nn);
        cfg.channels = m.channels, cfg.sampleRate = m.sampleRate, n = nn;
        drmp3_uninit(&m);
      }
    } else {
      data = drmp3_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &cfg, &n, nullptr);
    }
    channels = cfg.channels, rate = cfg.sampleRate, frames = n;
    if (data) out = fromInterleaved(path, data, frames, channels, rate);
    drmp3_free(data, nullptr);
  } else {
    if (err) *err = "unsupported audio format: " + path;
    return nullptr;
  }
  if (cancellable && decodeCancelled()) return cancelled();
  if (!out || out->sampleRate <= 0) {
    if (err) *err = "cannot decode " + path + (why.empty() ? "" : ": " + why);
    return nullptr;
  }
  return out;
}

// Renders one of the built-in drum sounds with Gridline's own drum synths.
static SamplePtr renderBuiltin(const std::string& name) {
  struct Spec { const char* type; std::vector<std::pair<int, float>> params; double pitch; double seconds; };
  using namespace pp;
  Spec sp;
  if (name == "kick") sp = {"gl.kick", {}, 60, 0.9};
  else if (name == "snare") sp = {"gl.snare", {}, 60, 0.6};
  else if (name == "clap") sp = {"gl.clap", {}, 60, 0.7};
  else if (name == "hat") sp = {"gl.hat", {}, 60, 0.25};
  else if (name == "openhat") sp = {"gl.hat", {{HatOpen, 1.f}, {HatDecay, 0.12f}}, 60, 1.0};
  else if (name == "tomlo") sp = {"gl.kick", {{KickPitch, 260.f}, {KickEndPitch, 95.f}, {KickSweep, 0.12f}, {KickDecay, 0.6f}, {KickClick, 0.1f}}, 60, 0.9};
  else if (name == "tomhi") sp = {"gl.kick", {{KickPitch, 380.f}, {KickEndPitch, 160.f}, {KickSweep, 0.1f}, {KickDecay, 0.45f}, {KickClick, 0.1f}}, 60, 0.7};
  else if (name == "rim") sp = {"gl.snare", {{SnSnappy, 0.15f}, {SnDecay, 0.03f}, {SnNoiseDecay, 0.02f}, {SnTune, 380.f}}, 60, 0.2};
  else return nullptr;
  auto gen = createGenerator(sp.type);
  if (!gen) return nullptr;
  PluginState st = defaultPluginState(sp.type);
  for (auto [k, v] : sp.params) st.params[static_cast<size_t>(k)] = v;
  const double sr = 44100;
  size_t n = static_cast<size_t>(sp.seconds * sr);
  auto s = std::make_shared<Sample>();
  s->path = "builtin:" + name;
  s->sampleRate = sr;
  s->left.assign(n, 0.f);
  s->right.assign(n, 0.f);
  ChannelAux aux;
  ProcessContext ctx;
  ctx.sampleRate = sr;
  gen->noteOn(1, sp.pitch, 1.0f, 0);
  for (size_t pos = 0; pos < n; pos += 512) {
    int len = static_cast<int>(std::min<size_t>(512, n - pos));
    gen->process(st, aux, ctx, s->left.data() + pos, s->right.data() + pos, len);
  }
  return s;
}

namespace {
thread_local bool tNonBlocking = false;  // see SampleCache::NonBlocking
}

SampleCache::NonBlocking::NonBlocking() : prev_(tNonBlocking) { tNonBlocking = true; }
SampleCache::NonBlocking::~NonBlocking() { tNonBlocking = prev_; }

SampleCache::~SampleCache() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
    queue_.clear();
  }
  work_.notify_all();
  for (auto& t : workers_) t.join();
}

// Decodes `path` on this thread (mu_ held on entry and exit; released while decoding).
SamplePtr SampleCache::decodeLocked(std::unique_lock<std::mutex>& lk, const std::string& path) {
  inflight_.insert(path);
  lk.unlock();
  SamplePtr s = loadSample(path);
  lk.lock();
  inflight_.erase(path);
  if (s)
    cache_[path] = s;
  else
    failed_[path] = true;
  done_.notify_all();
  return s;
}

void SampleCache::enqueueLocked(const std::string& path) {
  if (inflight_.count(path) || std::find(queue_.begin(), queue_.end(), path) != queue_.end()) return;
  queue_.push_back(path);
  if (workers_.empty()) {  // background decoders start with the first request
    const int n = static_cast<int>(std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u));
    for (int i = 0; i < n; ++i) workers_.emplace_back([this] { worker(); });
  }
  work_.notify_one();
}

void SampleCache::worker() {
  std::unique_lock<std::mutex> lk(mu_);
  for (;;) {
    work_.wait(lk, [&] { return stop_ || (!paused_ && !queue_.empty()); });
    if (stop_) return;
    const std::string path = queue_.front();
    queue_.pop_front();
    if (cache_.count(path) || failed_.count(path) || inflight_.count(path)) {
      done_.notify_all();  // (waitIdle re-checks the queue)
      continue;
    }
    decodeLocked(lk, path);
    gen_.fetch_add(1, std::memory_order_acq_rel);
  }
}

SamplePtr SampleCache::get(const std::string& path) {
  if (path.empty()) return nullptr;
  if (path.rfind("builtin:", 0) == 0) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(path);
    if (it != cache_.end()) return it->second;
    SamplePtr s = renderBuiltin(path.substr(8));
    if (s) cache_[path] = s;
    return s;
  }
  if (tNonBlocking) return request(path);
  std::unique_lock<std::mutex> lk(mu_);
  // A background decode of this file is running: wait for it instead of decoding twice.
  done_.wait(lk, [&] { return !inflight_.count(path); });
  if (auto it = cache_.find(path); it != cache_.end()) return it->second;
  if (failed_.count(path)) return nullptr;
  if (auto q = std::find(queue_.begin(), queue_.end(), path); q != queue_.end()) queue_.erase(q);  // decode it here, now
  return decodeLocked(lk, path);
}

SamplePtr SampleCache::request(const std::string& path) {
  if (path.empty()) return nullptr;
  if (path.rfind("builtin:", 0) == 0) {
    struct Off {  // built-in drums render in microseconds: synchronous
      bool prev = tNonBlocking;
      Off() { tNonBlocking = false; }
      ~Off() { tNonBlocking = prev; }
    } off;
    return get(path);
  }
  std::lock_guard<std::mutex> lk(mu_);
  if (auto it = cache_.find(path); it != cache_.end()) return it->second;
  if (failed_.count(path)) return nullptr;
  enqueueLocked(path);
  return nullptr;
}

int SampleCache::pending() const {
  std::lock_guard<std::mutex> lk(mu_);
  return static_cast<int>(queue_.size() + inflight_.size());
}

bool SampleCache::isPending(const std::string& path) const {
  std::lock_guard<std::mutex> lk(mu_);
  return inflight_.count(path) || std::find(queue_.begin(), queue_.end(), path) != queue_.end();
}

void SampleCache::pauseBackground(bool paused) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    paused_ = paused;
  }
  work_.notify_all();
}

void SampleCache::waitIdle() {
  std::unique_lock<std::mutex> lk(mu_);
  done_.wait(lk, [&] { return queue_.empty() && inflight_.empty(); });
}

size_t SampleCache::trim() {
  std::lock_guard<std::mutex> lk(mu_);
  size_t n = 0;
  for (auto it = cache_.begin(); it != cache_.end();)
    if (it->second.use_count() == 1) {
      it = cache_.erase(it);
      ++n;
    } else {
      ++it;
    }
  return n;
}

void SampleCache::prefetch(const std::vector<std::string>& paths, int threads) {
  std::vector<std::string> todo;
  {
    std::lock_guard<std::mutex> lk(mu_);
    std::set<std::string> seen;
    for (const auto& p : paths)
      if (!p.empty() && p.rfind("builtin:", 0) != 0 && !cache_.count(p) && !failed_.count(p) && seen.insert(p).second) todo.push_back(p);
  }
  if (todo.empty()) return;
  if (threads <= 0) threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u));
  threads = std::min<int>(threads, static_cast<int>(todo.size()));
  if (threads <= 1) {
    for (const auto& p : todo) get(p);
    return;
  }
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (size_t i; (i = next.fetch_add(1)) < todo.size();) get(todo[i]);
  };
  std::vector<std::thread> pool;
  for (int t = 1; t < threads; ++t) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
}

void SampleCache::clear() {
  std::lock_guard<std::mutex> lk(mu_);
  cache_.clear();
  failed_.clear();
}

SampleCache& sampleCache() {
  static SampleCache c;
  return c;
}

}  // namespace gl
