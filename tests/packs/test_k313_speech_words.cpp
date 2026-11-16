// Gridline - K-313: per-word pitch in the Speech text. Pitch marks in every form (never spoken,
// bad ones left out), joined words, the measured pitch of each word against its mark, the three
// styles, and one slice per word in the made file.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/Speech.h"
#include "engine/LoopFile.h"

using namespace gl;

namespace {
constexpr int kSr = 44100;

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
  return s;
}

// Pitch by autocorrelation (as the K-107 test).
double pitchOf(const std::vector<float>& v, size_t at, size_t win = 2048) {
  if (at + win + 1000 > v.size()) return 0;
  std::vector<double> r(kSr / 50 + 1, 0.0);
  double best = 0;
  for (size_t L = kSr / 500; L < kSr / 50; ++L) {
    double c = 0, e1 = 0, e2 = 0;
    for (size_t i = at; i < at + win; ++i) c += v[i] * v[i + L], e1 += v[i] * v[i], e2 += v[i + L] * v[i + L];
    r[L] = e1 > 0 && e2 > 0 ? c / std::sqrt(e1 * e2) : 0;
    best = std::max(best, r[L]);
  }
  size_t lag = 0;
  for (size_t L = kSr / 500 + 1; L + 1 < kSr / 50 && !lag; ++L)
    if (r[L] >= 0.9 * best && r[L] >= r[L - 1] && r[L] >= r[L + 1]) lag = L;
  return lag ? static_cast<double>(kSr) / static_cast<double>(lag) : 0;
}

// The pitch in the middle of word k.
double wordPitch(const speech::Rendered& r, size_t k) {
  const int64_t a = r.wordStarts[k];
  const int64_t b = k + 1 < r.wordStarts.size() ? r.wordStarts[k + 1] : static_cast<int64_t>(r.samples.size());
  const size_t mid = static_cast<size_t>((a + b) / 2);
  return pitchOf(r.samples, mid > 1024 ? mid - 1024 : 0);
}

speech::Voice plain(speech::Style st) {
  speech::Voice v = *speech::findVoice("Narrator");
  v.breath = 0, v.vibrato = 0, v.style = st;
  return v;
}

double semisBetween(double f, double ref) { return 12.0 * std::log2(f / ref); }
}  // namespace

int main() {
  gl::test::hermeticTestEnv("k313_speech_words");
  // ---- parsing: every form
  {
    const auto p = speech::parse("up(1) down(-2)");
    expect(p.words.size() == 2 && p.words[0].semis == 1 && p.words[1].semis == -2 && p.words[0].marked, "up(1) down(-2): two words, +1 and -2");
    expect(p.marks.size() == 2 && p.marks[0].valid && p.marks[1].valid, "... two good marks");
    expect(p.words[0].text == "up" && p.words[1].text == "down", "... the words without their marks");
  }
  expect(speech::parse("up (3)").words[0].semis == 3, "a space before the bracket");
  expect(speech::parse("half(0.5)").words[0].semis == 0.5, "a decimal mark");
  expect(speech::parse("plus(+3)").words[0].semis == 3, "a mark with a plus sign");
  expect(speech::parse("pad( -4 )").words[0].semis == -4, "spaces inside the brackets");
  {
    const auto p = speech::parse("bad(abc) next");
    expect(p.marks.size() == 1 && !p.marks[0].valid && !p.words[0].marked, "bad(abc): a bad mark, the word unmarked");
    expect(join(speech::phonemes("bad(abc) next")) == join(speech::phonemes("bad next")), "... and it is not spoken");
  }
  expect(!speech::parse("far(30)").marks[0].valid, "beyond 24 semitones is not a mark");
  expect(!speech::parse("open(2").marks[0].valid, "a bracket never closed is a bad mark");
  expect(join(speech::phonemes("up(1) down(-2)")) == join(speech::phonemes("up down")), "marks are never spoken");
  expect(speech::parse("(4) start").marks.empty(), "a bracket after no word is not a mark");
  {
    const auto p = speech::parse("this_is_one_word(2) and");
    expect(p.words.size() == 2 && p.words[0].text == "this_is_one_word" && p.words[0].semis == 2, "underscores join words into one");
    const auto ph = speech::phonemes("this_is_one");
    expect(join(ph).find('|') == std::string::npos, "... spoken with no gap");
    expect(join(ph) == join(speech::phonemes("thisisone")) || ph.size() >= 6, "... every part spoken");
  }
  expect(speech::parse("one two, three. four").words.size() == 4, "word count across punctuation");
  expect(speech::styleFromId(speech::styleId(speech::Style::Sing)) == speech::Style::Sing &&
             speech::styleFromId(speech::styleId(speech::Style::Random)) == speech::Style::Random && speech::styleFromId("x") == speech::Style::Natural,
         "style ids round trip");

  // ---- the pitch of each word follows its mark (Sing: exactly on the voice's pitch plus the mark)
  {
    const speech::Voice v = plain(speech::Style::Sing);
    const auto r = speech::renderWords("aaaa(0) aaaa(5) aaaa(-3) aaaa(7) aaaa(12)", v, kSr);
    expect(r.wordStarts.size() == 5, "five word starts");
    const double want[] = {0, 5, -3, 7, 12};
    for (size_t k = 0; k < 5; ++k) {
      const double f = wordPitch(r, k);
      const double got = f > 0 ? semisBetween(f, v.pitch) : 99;
      char b[96];
      std::snprintf(b, sizeof b, "sing: word %zu at %+.2f semitones (wanted %+.0f)", k + 1, got, want[k]);
      expect(std::fabs(got - want[k]) < 0.35, b);
    }
  }
  {  // Natural: a marked word sits its offset above the same word unmarked
    const speech::Voice v = plain(speech::Style::Natural);
    const auto a = speech::renderWords("aaaa aaaa", v, kSr), b = speech::renderWords("aaaa aaaa(4)", v, kSr);
    const double fa = wordPitch(a, 1), fb = wordPitch(b, 1);
    const double d = fa > 0 && fb > 0 ? semisBetween(fb, fa) : 99;
    char m[96];
    std::snprintf(m, sizeof m, "natural: the marked word is %+.2f semitones up (wanted +4)", d);
    expect(std::fabs(d - 4) < 0.4, m);
    expect(a.samples == speech::render("aaaa aaaa", v, kSr), "natural without marks: render() unchanged");
  }
  {  // Random: deterministic, each word within a few semitones of the voice's pitch (plus its mark)
    const speech::Voice v = plain(speech::Style::Random);
    const std::string t = "aaaa aaaa aaaa aaaa aaaa aaaa";
    const auto r1 = speech::renderWords(t, v, kSr), r2 = speech::renderWords(t, v, kSr);
    expect(r1.samples == r2.samples, "random: the same text says the same each time");
    bool within = true, varied = false;
    double first = 0;
    for (size_t k = 0; k < r1.wordStarts.size(); ++k) {
      const double f = wordPitch(r1, k);
      const double s = f > 0 ? semisBetween(f, v.pitch) : 99;
      within = within && std::fabs(s) <= 3.6;
      if (k == 0) first = s;
      else if (std::fabs(s - first) > 0.5) varied = true;
    }
    expect(within, "random: every word within three semitones");
    expect(varied, "random: the words differ");
    expect(r1.samples != speech::renderWords(t, plain(speech::Style::Sing), kSr).samples, "random differs from sing");
  }
  {  // A robot voice is flat; its marks still move it
    speech::Voice v = *speech::findVoice("Robot");
    v.ring = 0;  // (ring modulation would blur the measurement)
    const auto r = speech::renderWords("aaaa aaaa(7)", v, kSr);
    const double d = semisBetween(wordPitch(r, 1), wordPitch(r, 0));
    expect(std::fabs(d - 7) < 0.5, "a robot word goes up by its mark");
  }

  // ---- slices: one per word in the made file, read back as the file's own slices
  {
    const std::string t = "Here(2) are five_words(4) said, slowly(-1).";
    const speech::Parsed p = speech::parse(t);
    const auto r = speech::renderWords(t, plain(speech::Style::Natural), kSr);
    expect(p.words.size() == 5 && r.wordStarts.size() == 5, "five words (five_words is one)");
    bool rising = r.wordStarts[0] == 0;
    for (size_t k = 1; k < r.wordStarts.size(); ++k) rising = rising && r.wordStarts[k] > r.wordStarts[k - 1];
    expect(rising, "word starts rise from 0");
    const std::vector<uint8_t> wav = speech::wavWithWords(r, p.words, kSr, "speech: test\n" + t);
    loopfile::Info li;
    expect(loopfile::readBytes(wav.data(), wav.size(), li), "the file states its slices");
    expect(li.slices.size() == p.words.size(), "slice count = word count (" + std::to_string(li.slices.size()) + ")");
    expect(li.frames == static_cast<int64_t>(r.samples.size()) && !li.loop, "... its length, and it is not a loop");
    bool same = li.slices.size() == r.wordStarts.size();
    for (size_t k = 0; same && k < li.slices.size(); ++k) same = li.slices[k].start == r.wordStarts[k];
    expect(same, "each slice starts at its word");
  }
  {  // a file of older Speech channels (no cue points) still reads as an ordinary sample
    const auto r = speech::renderWords("hello", plain(speech::Style::Natural), kSr);
    speech::Rendered none = r;
    none.wordStarts.clear();
    const std::vector<uint8_t> wav = speech::wavWithWords(none, {}, kSr, "");
    loopfile::Info li;
    expect(!loopfile::readBytes(wav.data(), wav.size(), li), "a file without cue points says nothing about itself");
  }
  return finish();
}
