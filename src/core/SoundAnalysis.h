// Gridline — K-218: what a sound file is, from its audio alone: its tempo (loops), its key
// (tonal material) and its sound kind (kick, snare, hat, bass, vocal ...). Used by the browser's
// background indexer for files whose names and loop metadata say nothing; runs on the indexer's
// worker thread, never on the UI thread, and its results are cached with the index.
#pragma once
#include <string>
#include <vector>

namespace gl::sound {

struct Analysis {
  double bpm = 0;        // 0 = no steady beat found (one-shots, free time)
  int key = -1;          // pitch class 0..11 (C = 0), -1 = no clear key
  bool minor = false;
  std::string kind;      // "kick", "snare", "hat", "cymbal", "perc", "bass", "vocal", "drums", "tonal", "fx"; "" = silence
  double confidenceBpm = 0, confidenceKey = 0;  // 0..1
};

// `mono` at `rate`. Looks at most at the first 30 seconds. `seconds` is the whole file's
// length (a loop's length helps settle its tempo); 0 = the length of `mono`.
Analysis analyse(const std::vector<float>& mono, double rate, double seconds = 0);

// The parts, exposed for tests.
double detectTempo(const std::vector<float>& mono, double rate, double seconds, double* confidence = nullptr);
bool detectKey(const std::vector<float>& mono, double rate, int& pitchClass, bool& minor, double* confidence = nullptr);
std::string detectKind(const std::vector<float>& mono, double rate);

// Whether a kind is a drum or percussion sound (no key is looked for).
bool isPercussive(const std::string& kind);

}  // namespace gl::sound
