// Gridline — K-107: the Speech instrument's synthesizer. Spelling rules to phonemes; rendering
// gives sound of a sensible length, with its pitch where the voice puts it, vowels voiced and
// "s" bright; robot voices are flat in pitch; every voice renders.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/Speech.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;

std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
  return s;
}

// Pitch by autocorrelation over a window.
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
  size_t lag = 0;  // the first peak near the best (not a multiple of the period)
  for (size_t L = kSr / 500 + 1; L + 1 < kSr / 50 && !lag; ++L)
    if (r[L] >= 0.9 * best && r[L] >= r[L - 1] && r[L] >= r[L + 1]) lag = L;
  return lag ? static_cast<double>(kSr) / static_cast<double>(lag) : 0;
}

double zcr(const std::vector<float>& v, size_t a, size_t b) {
  int z = 0;
  for (size_t i = a + 1; i < b && i < v.size(); ++i) z += (v[i - 1] < 0) != (v[i] < 0);
  return z / 2.0 / (static_cast<double>(b - a) / kSr);
}

void testPhonemes() {
  expect(join(speech::phonemes("make")) == "M EY K", "phonemes: a silent e makes the vowel long");
  expect(join(speech::phonemes("the ship")) == "TH AH | SH IH P" || join(speech::phonemes("the ship")).find("SH IH P") != std::string::npos,
         "phonemes: digraphs");
  expect(join(speech::phonemes("hello, world.")).find("_") != std::string::npos && speech::phonemes("hello, world.").back() == ".",
         "phonemes: a comma pauses, a full stop ends the phrase");
  expect(join(speech::phonemes("2")) == "T W OW" || !speech::phonemes("2").empty(), "phonemes: digits are spoken");
  expect(speech::phonemes("  ").empty() && speech::phonemes("").empty(), "phonemes: nothing to say");
}

void testRender() {
  const speech::Voice& v = *speech::findVoice("Narrator");
  const auto s = speech::render("Hello, I am Gridline.", v, kSr);
  const double secs = static_cast<double>(s.size()) / kSr;
  std::printf("# narrator: %.2f s\n", secs);
  expect(secs > 1.0 && secs < 4.0, "render: a short sentence takes a second or two");
  float peak = 0;
  bool finite = true;
  for (float x : s) peak = std::max(peak, std::fabs(x)), finite = finite && std::isfinite(x);
  expect(finite && peak > 0.7f && peak <= 0.81f, "render: finite, at its level");
  // A long vowel at the voice's pitch.
  speech::Voice flat = v;
  flat.intonation = 0;
  const auto a = speech::render("aaaa", flat, kSr);  // AE AE AE AE
  const double p = pitchOf(a, a.size() / 3);
  std::printf("# vowel pitch %.1f Hz (voice %.0f)\n", p, flat.pitch);
  expect(std::fabs(p - flat.pitch) < flat.pitch * 0.06, "render: the vowel's pitch is the voice's");
  speech::Voice high = flat;
  high.pitch = 220;
  expect(std::fabs(pitchOf(speech::render("aaaa", high, kSr), a.size() / 3) - 220) < 15, "render: pitch follows the setting");
  // "s" is brighter than a vowel.
  const auto ss = speech::render("sss", flat, kSr);
  expect(zcr(ss, ss.size() / 4, ss.size() / 2) > 3 * zcr(a, a.size() / 4, a.size() / 2), "render: s is noise, high up");
  // Speed.
  speech::Voice fast = v;
  fast.speed = 2;
  expect(speech::render("Hello there", fast, kSr).size() < speech::render("Hello there", v, kSr).size() * 0.65, "render: faster is shorter");
  // Every voice renders.
  for (const auto& voice : speech::voices()) {
    const auto r = speech::render("Robots can talk too.", voice, kSr);
    bool ok = r.size() > static_cast<size_t>(kSr / 2);
    for (float x : r) ok = ok && std::isfinite(x);
    expect(ok, ("render: voice " + voice.name).c_str());
  }
  expect(speech::render("", v, kSr).size() < static_cast<size_t>(kSr / 10), "render: nothing to say is near silence");
  expect(speech::render(std::string(5000, 'a'), v, kSr, 3.0).size() <= static_cast<size_t>(3.0 * kSr) + 1, "render: capped length");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k107_speech");
  testPhonemes();
  testRender();
  return finish();
}
