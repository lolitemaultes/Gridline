// Gridline — K-107: the Speech instrument's voice. Text becomes sound with Gridline's own small
// formant speech synthesizer: English letters to phonemes by spelling rules, each phoneme a
// glottal source (or noise) through three formant resonators, with a pitch line, speed and voice
// colour. Robot voices hold a flat pitch on a buzzier source and ring-modulate it. The result is
// a sample (rendered once, off the audio thread) that a sampler channel plays.
#pragma once
#include <string>
#include <vector>

namespace gl::speech {

struct Voice {
  std::string name;
  float pitch = 120.0f;     // Hz at the start of a phrase
  float intonation = 0.6f;  // 0 = flat .. 1 = lively (the pitch falls over a phrase and lifts on stresses)
  float speed = 1.0f;       // 0.5 .. 2
  float formant = 1.0f;     // formant scale: 0.8 = larger voice .. 1.25 = smaller voice
  float breath = 0.05f;     // noise in the voiced source, 0..1 (1 = whispered)
  float vibrato = 0.0f;     // semitones
  float robot = 0.0f;       // 0..1: flat pitch, square-ish source, ring modulation
  float ring = 50.0f;       // ring modulation frequency (Hz) for robot voices
};

// The voices offered by the Speech dialog (the first is the default).
const std::vector<Voice>& voices();
const Voice* findVoice(const std::string& name);

// Phoneme symbols ("AA", "IY", "S", "_" pause, ...) for the text, by spelling rules.
std::vector<std::string> phonemes(const std::string& text);

// The spoken text as mono samples at `sampleRate` (at most `maxSeconds`).
std::vector<float> render(const std::string& text, const Voice& v, int sampleRate = 44100, double maxSeconds = 60.0);

}  // namespace gl::speech
