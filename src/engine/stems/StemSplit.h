// Gridline - J-191: splitting audio into stems (vocals, drums, bass, other, ...) with the
// Hybrid Transformer Demucs models (HTDemucs.h). The song is cut into overlapping segments of the
// model's training length, each segment separated, and the results cross-faded with triangular
// weights, as the reference implementation does; several models ("bags") are blended per stem.
// Blocking: call from a worker thread. Never touches the audio thread.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "../../core/AudioBuffer.h"

namespace gl::stems {

struct SplitRequest {
  std::vector<std::string> modelFiles;       // one checkpoint per bag member
  std::vector<std::vector<float>> weights;   // per model, per source (empty: all ones)
  int threads = 0;                           // 0: all hardware threads
  double overlap = 0.25;                     // segment overlap
};

struct SplitResult {
  std::vector<std::string> sources;  // the model's order (e.g. drums, bass, other, vocals)
  std::vector<AudioBuffer> stems;    // same rate / length / channel count as the input
};

// progress(fraction 0..1). `cancel` is polled between segments (and models).
using SplitProgress = std::function<void(double)>;

bool separate(const AudioBuffer& in, double rate, const SplitRequest& req, SplitResult& out, const SplitProgress& progress,
              const std::atomic<bool>* cancel, std::string* err);

// Overlap-add weights of one segment (triangle, peak 1), as used by `separate`.
std::vector<float> segmentWeights(int segmentLength);
// Segment start offsets for `frames` samples.
std::vector<long long> segmentOffsets(long long frames, int segmentLength, double overlap);

}  // namespace gl::stems
