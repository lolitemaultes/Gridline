// Gridline — audio sample data and loading (WAV, FLAC, MP3, Ogg Vorbis, WavPack) with a cache.
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gl {

struct Sample {
  std::string path;
  double sampleRate = 44100.0;
  std::vector<float> left, right;  // equal length; mono files duplicate left into right
  size_t frames() const { return left.size(); }
  double seconds() const { return sampleRate > 0 ? static_cast<double>(frames()) / sampleRate : 0.0; }
};

using SamplePtr = std::shared_ptr<const Sample>;

// Decodes a file by content (WAV, FLAC, Ogg Vorbis also inside WAV, WavPack), else by
// extension (.mp3). nullptr on failure.
SamplePtr loadSample(const std::string& path, std::string* err = nullptr);
bool isAudioFile(const std::string& path);

// Thread-safe path → sample cache (loading happens on the calling thread).
class SampleCache {
 public:
  SamplePtr get(const std::string& path);
  void clear();

 private:
  std::mutex mu_;
  std::map<std::string, SamplePtr> cache_;
  std::map<std::string, bool> failed_;
};

SampleCache& sampleCache();

}  // namespace gl
