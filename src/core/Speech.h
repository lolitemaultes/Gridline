// Gridline — K-107: the Speech instrument's voice. Text becomes sound with Gridline's own small
// formant speech synthesizer: English letters to phonemes by spelling rules, each phoneme a
// glottal source (or noise) through three formant resonators, with a pitch line, speed and voice
// colour. Robot voices hold a flat pitch on a buzzier source and ring-modulate it. The result is
// a sample (rendered once, off the audio thread) that a sampler channel plays.
//
// K-313: a word can carry a pitch mark, a semitone offset in brackets right after it ("up(1)
// down(-2)", "a (0.5)", "b(+3)"); the mark is never spoken. Underscores join words into one
// ("this_is_one"). The style decides the pitch line: Natural (the voice's own, plus the offsets),
// Sing (every word on the voice's pitch plus its offset, for melodies) or Random (a few semitones
// up or down per word).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gl::speech {

enum class Style { Natural, Sing, Random };
const char* styleId(Style s);           // "natural" | "sing" | "random"
Style styleFromId(const std::string& id);  // unknown = Natural

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
  Style style = Style::Natural;  // K-313
};

// K-313: the words of a text (joined words are one) and its pitch marks.
struct Word {
  std::string text;      // as written, underscores kept
  double semis = 0;      // its pitch mark (0 without one)
  bool marked = false;
  size_t begin = 0, end = 0;  // bytes of the text
};
struct Mark {
  size_t begin = 0, end = 0;  // bytes of the text, brackets included
  bool valid = false;         // false: not a number (or beyond 24 semitones); left out, not spoken
  double semis = 0;
};
struct Parsed {
  std::vector<Word> words;
  std::vector<Mark> marks;
};
Parsed parse(const std::string& text);

// The spoken text and where each word starts (samples; one entry per word of parse()).
struct Rendered {
  std::vector<float> samples;
  std::vector<int64_t> wordStarts;
};
Rendered renderWords(const std::string& text, const Voice& v, int sampleRate = 44100, double maxSeconds = 60.0);

// A 16-bit mono WAV file of the rendered speech with a cue point (a slice) at every word,
// labelled with the word, and the text in its comment ("speech: <voice settings>" line, then the text).
std::vector<uint8_t> wavWithWords(const Rendered& r, const std::vector<Word>& words, int sampleRate, const std::string& comment);

// The voices offered by the Speech dialog (the first is the default).
const std::vector<Voice>& voices();
const Voice* findVoice(const std::string& name);

// Phoneme symbols ("AA", "IY", "S", "_" pause, ...) for the text, by spelling rules.
std::vector<std::string> phonemes(const std::string& text);

// The spoken text as mono samples at `sampleRate` (at most `maxSeconds`).
std::vector<float> render(const std::string& text, const Voice& v, int sampleRate = 44100, double maxSeconds = 60.0);

}  // namespace gl::speech
