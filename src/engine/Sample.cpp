#include "Sample.h"
#include "../core/PluginParams.h"
#include "Generators.h"
#include <algorithm>
#include <cctype>
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
  s->right.resize(frames);
  for (uint64_t i = 0; i < frames; ++i) {
    s->left[i] = data[i * channels];
    s->right[i] = channels > 1 ? data[i * channels + 1] : data[i * channels];
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

SamplePtr loadSample(const std::string& path, std::string* err) {
  // Formats are recognised by content first (FL Studio content often stores
  // Ogg Vorbis under a .wav name, or inside a RIFF wrapper), then by extension.
  std::vector<uint8_t> bytes;
  {
    std::ifstream is(path, std::ios::binary);
    if (!is) {
      if (err) *err = "cannot open " + path;
      return nullptr;
    }
    bytes.assign(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
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
    data = drwav_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &ch, &sr, &n, nullptr);
    channels = ch, rate = sr, frames = n;
    if (data) out = fromInterleaved(path, data, frames, channels, rate);
    drwav_free(data, nullptr);
  } else if (magic("fLaC") || e == ".flac") {
    unsigned int ch = 0, sr = 0;
    drflac_uint64 n = 0;
    data = drflac_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &ch, &sr, &n, nullptr);
    channels = ch, rate = sr, frames = n;
    if (data) out = fromInterleaved(path, data, frames, channels, rate);
    drflac_free(data, nullptr);
  } else if (e == ".mp3" || (bytes.size() > 2 && bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0) ||
             (bytes.size() > 3 && std::memcmp(bytes.data(), "ID3", 3) == 0)) {
    drmp3_config cfg{};
    drmp3_uint64 n = 0;
    data = drmp3_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &cfg, &n, nullptr);
    channels = cfg.channels, rate = cfg.sampleRate, frames = n;
    if (data) out = fromInterleaved(path, data, frames, channels, rate);
    drmp3_free(data, nullptr);
  } else {
    if (err) *err = "unsupported audio format: " + path;
    return nullptr;
  }
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
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = cache_.find(path); it != cache_.end()) return it->second;
    if (failed_.count(path)) return nullptr;
  }
  SamplePtr s = loadSample(path);
  std::lock_guard<std::mutex> lk(mu_);
  if (s)
    cache_[path] = s;
  else
    failed_[path] = true;
  return s;
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
