// Gridline — K-107: a small formant speech synthesizer. See Speech.h. Formant frequencies are
// textbook averages of English vowels and consonants; the rules and the synthesis are Gridline's.
#include "Speech.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <random>

namespace gl::speech {

namespace {

constexpr double kPi = 3.14159265358979323846;

enum class Kind { Vowel, Sonorant, Fricative, VoicedFricative, Plosive, VoicedPlosive, Aspirate, Pause };

struct Ph {
  Kind kind;
  float f1, f2, f3;  // formants (Hz); for fricatives f1 is the noise band's centre
  float dur;         // seconds at speed 1
  float f1b = 0, f2b = 0, f3b = 0;  // diphthongs: where the formants end (0 = steady)
};

const std::map<std::string, Ph>& table() {
  static const std::map<std::string, Ph> t = {
      {"IY", {Kind::Vowel, 270, 2290, 3010, 0.12f}},  {"IH", {Kind::Vowel, 390, 1990, 2550, 0.09f}},
      {"EH", {Kind::Vowel, 530, 1840, 2480, 0.10f}},  {"AE", {Kind::Vowel, 660, 1720, 2410, 0.12f}},
      {"AA", {Kind::Vowel, 730, 1090, 2440, 0.12f}},  {"AO", {Kind::Vowel, 570, 840, 2410, 0.12f}},
      {"UH", {Kind::Vowel, 440, 1020, 2240, 0.09f}},  {"UW", {Kind::Vowel, 300, 870, 2240, 0.12f}},
      {"AH", {Kind::Vowel, 520, 1190, 2390, 0.09f}},  {"ER", {Kind::Vowel, 490, 1350, 1690, 0.12f}},
      {"EY", {Kind::Vowel, 530, 1840, 2480, 0.17f, 300, 2200, 2900}}, {"AY", {Kind::Vowel, 730, 1090, 2440, 0.19f, 300, 2100, 2800}},
      {"OW", {Kind::Vowel, 570, 840, 2410, 0.17f, 320, 870, 2240}},   {"AW", {Kind::Vowel, 730, 1090, 2440, 0.19f, 320, 870, 2240}},
      {"OY", {Kind::Vowel, 570, 840, 2410, 0.19f, 300, 2100, 2800}},
      {"M", {Kind::Sonorant, 280, 900, 2200, 0.07f}},  {"N", {Kind::Sonorant, 280, 1700, 2600, 0.07f}},
      {"NG", {Kind::Sonorant, 280, 2300, 2750, 0.07f}}, {"L", {Kind::Sonorant, 360, 1300, 2700, 0.07f}},
      {"R", {Kind::Sonorant, 420, 1300, 1600, 0.07f}},  {"W", {Kind::Sonorant, 300, 610, 2200, 0.06f}},
      {"Y", {Kind::Sonorant, 260, 2070, 3020, 0.06f}},
      {"S", {Kind::Fricative, 5500, 0, 0, 0.10f}},  {"SH", {Kind::Fricative, 2600, 0, 0, 0.10f}},
      {"F", {Kind::Fricative, 4500, 0, 0, 0.09f}},  {"TH", {Kind::Fricative, 4800, 0, 0, 0.08f}},
      {"Z", {Kind::VoicedFricative, 5000, 0, 0, 0.08f}}, {"ZH", {Kind::VoicedFricative, 2600, 0, 0, 0.08f}},
      {"V", {Kind::VoicedFricative, 4000, 0, 0, 0.07f}}, {"DH", {Kind::VoicedFricative, 4500, 0, 0, 0.06f}},
      {"H", {Kind::Aspirate, 0, 0, 0, 0.06f}},
      {"P", {Kind::Plosive, 900, 0, 0, 0.07f}},  {"T", {Kind::Plosive, 4200, 0, 0, 0.07f}},
      {"K", {Kind::Plosive, 2000, 0, 0, 0.08f}}, {"B", {Kind::VoicedPlosive, 900, 0, 0, 0.05f}},
      {"D", {Kind::VoicedPlosive, 3500, 0, 0, 0.05f}}, {"G", {Kind::VoicedPlosive, 1800, 0, 0, 0.06f}},
      {"_", {Kind::Pause, 0, 0, 0, 0.18f}},  {".", {Kind::Pause, 0, 0, 0, 0.35f}}, {"|", {Kind::Pause, 0, 0, 0, 0.02f}},
  };
  return t;
}

bool vowelLetter(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }

const char* const kDigits[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};

void word(const std::string& w, std::vector<std::string>& out) {
  const size_t n = w.size();
  // Silent final e makes the vowel before it long ("make", "time", "home", "cute").
  int longAt = -1;
  if (n >= 3 && w[n - 1] == 'e' && !vowelLetter(w[n - 2]) && vowelLetter(w[n - 3])) longAt = static_cast<int>(n - 3);
  auto at = [&](size_t i) { return i < n ? w[i] : '\0'; };
  auto starts = [&](size_t i, const char* s) { return w.compare(i, std::char_traits<char>::length(s), s) == 0; };
  for (size_t i = 0; i < n;) {
    const char c = w[i];
    struct Rule {
      const char* letters;
      std::vector<const char*> ph;
    };
    static const Rule rules[] = {
        {"igh", {"AY"}}, {"tch", {"T", "SH"}}, {"th", {"TH"}}, {"sh", {"SH"}}, {"ch", {"T", "SH"}}, {"ph", {"F"}},
        {"ng", {"NG"}},  {"ck", {"K"}},        {"qu", {"K", "W"}}, {"wh", {"W"}}, {"ee", {"IY"}}, {"ea", {"IY"}},
        {"oo", {"UW"}},  {"ou", {"AW"}},       {"ow", {"OW"}}, {"ai", {"EY"}}, {"ay", {"EY"}}, {"oi", {"OY"}},
        {"oy", {"OY"}},  {"er", {"ER"}},       {"ir", {"ER"}}, {"ur", {"ER"}}, {"ar", {"AA", "R"}}, {"or", {"AO", "R"}},
        {"oa", {"OW"}},  {"ie", {"IY"}},       {"au", {"AO"}}, {"aw", {"AO"}}, {"ew", {"UW"}},
    };
    bool done = false;
    for (const Rule& r : rules)
      if (starts(i, r.letters)) {
        for (const char* p : r.ph) out.push_back(p);
        i += std::char_traits<char>::length(r.letters);
        done = true;
        break;
      }
    if (done) continue;
    if (static_cast<int>(i) == longAt) {
      static const std::map<char, const char*> lng = {{'a', "EY"}, {'e', "IY"}, {'i', "AY"}, {'o', "OW"}, {'u', "UW"}};
      out.push_back(lng.at(c));
      ++i;
      continue;
    }
    if (c == 'e' && i == n - 1 && n > 2) {  // silent
      ++i;
      continue;
    }
    switch (c) {
      case 'a': out.push_back("AE"); break;
      case 'e': out.push_back(n <= 2 ? "IY" : "EH"); break;
      case 'i': out.push_back("IH"); break;
      case 'o': out.push_back(n <= 2 ? "OW" : "AA"); break;
      case 'u': out.push_back("AH"); break;
      case 'y':
        if (i == 0) out.push_back("Y");
        else out.push_back(n <= 3 ? "AY" : "IY");
        break;
      case 'c': out.push_back(at(i + 1) == 'e' || at(i + 1) == 'i' || at(i + 1) == 'y' ? "S" : "K"); break;
      case 'g': out.push_back("G"); break;
      case 'j': out.push_back("D"), out.push_back("ZH"); break;
      case 'q': out.push_back("K"); break;
      case 's': out.push_back(i > 0 && i + 1 < n && vowelLetter(w[i - 1]) && vowelLetter(w[i + 1]) ? "Z" : "S"); break;
      case 'x': out.push_back("K"), out.push_back("S"); break;
      case 'b': out.push_back("B"); break;
      case 'd': out.push_back("D"); break;
      case 'f': out.push_back("F"); break;
      case 'h': out.push_back("H"); break;
      case 'k': out.push_back("K"); break;
      case 'l': out.push_back("L"); break;
      case 'm': out.push_back("M"); break;
      case 'n': out.push_back("N"); break;
      case 'p': out.push_back("P"); break;
      case 'r': out.push_back("R"); break;
      case 't': out.push_back("T"); break;
      case 'v': out.push_back("V"); break;
      case 'w': out.push_back("W"); break;
      case 'z': out.push_back("Z"); break;
      default: break;
    }
    // A doubled consonant is said once ("hello", "little").
    if (i + 1 < n && w[i + 1] == c && !vowelLetter(c)) ++i;
    ++i;
  }
}

struct Reso {
  double a = 0, b = 0, c = 0, y1 = 0, y2 = 0;
  void set(double f, double bw, double sr) {
    f = std::clamp(f, 50.0, sr * 0.45);
    const double r = std::exp(-kPi * bw / sr);
    c = -r * r;
    b = 2 * r * std::cos(2 * kPi * f / sr);
    a = 1 - b - c;
  }
  double run(double x) {
    const double y = a * x + b * y1 + c * y2;
    y2 = y1, y1 = y;
    return y;
  }
};

}  // namespace

const std::vector<Voice>& voices() {
  static const std::vector<Voice> v = {
      {"Narrator", 115, 0.6f, 1.0f, 1.0f, 0.05f, 0, 0, 50},
      {"Deep", 82, 0.5f, 0.9f, 0.86f, 0.04f, 0, 0, 50},
      {"Bright", 210, 0.75f, 1.05f, 1.18f, 0.06f, 0, 0, 50},
      {"Child", 290, 0.85f, 1.1f, 1.3f, 0.05f, 0.1f, 0, 50},
      {"Whisper", 140, 0.4f, 0.95f, 1.05f, 1.0f, 0, 0, 50},
      {"Singer", 160, 0.3f, 0.85f, 1.05f, 0.03f, 0.4f, 0, 50},
      {"Robot", 100, 0.0f, 0.95f, 1.0f, 0.0f, 0, 1.0f, 60},
      {"Old robot", 70, 0.0f, 0.8f, 0.92f, 0.0f, 0, 1.0f, 30},
      {"Toy robot", 220, 0.1f, 1.2f, 1.2f, 0.0f, 0, 0.8f, 110},
  };
  return v;
}

const Voice* findVoice(const std::string& name) {
  for (const auto& v : voices())
    if (v.name == name) return &v;
  return nullptr;
}

std::vector<std::string> phonemes(const std::string& text) {
  std::vector<std::string> out;
  std::string w;
  auto flush = [&] {
    if (w.empty()) return;
    if (!out.empty() && out.back() != "_" && out.back() != ".") out.push_back("|");
    word(w, out);
    w.clear();
  };
  for (char ch : text) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (std::isalpha(u)) {
      w += static_cast<char>(std::tolower(u));
    } else if (std::isdigit(u)) {
      flush();
      w = kDigits[ch - '0'];
      flush();
    } else {
      flush();
      if (ch == ',' || ch == ';' || ch == ':' || ch == '-') out.push_back("_");
      else if (ch == '.' || ch == '!' || ch == '?' || ch == '\n') out.push_back(".");
    }
  }
  flush();
  while (!out.empty() && (out.back() == "|" || out.back() == "_")) out.pop_back();
  return out;
}

std::vector<float> render(const std::string& text, const Voice& v, int sampleRate, double maxSeconds) {
  const std::vector<std::string> ph = phonemes(text);
  const double sr = sampleRate;
  const double speed = std::clamp(static_cast<double>(v.speed), 0.25, 4.0);
  const size_t cap = static_cast<size_t>(maxSeconds * sr);
  std::vector<float> out;
  out.reserve(static_cast<size_t>(sr * 2));
  std::mt19937 rng(12345);
  std::uniform_real_distribution<double> noise(-1.0, 1.0);
  Reso r1, r2, r3, rn, nasal;
  double f1 = 500, f2 = 1500, f3 = 2500, av = 0, an = 0, nf = 3000, phase = 0, glot = 0, ringPh = 0, vibPh = 0;
  // The phrase's pitch falls from its start to its end; stressed vowels (a word's first) lift.
  size_t phraseStart = 0;
  for (size_t i = 0; i < ph.size() && out.size() < cap; ++i) {
    auto it = table().find(ph[i]);
    if (it == table().end()) continue;
    const Ph& p = it->second;
    if (ph[i] == ".") phraseStart = i + 1;
    size_t phraseLen = 1;
    for (size_t k = phraseStart; k < ph.size() && ph[k] != "."; ++k) ++phraseLen;
    const double where = static_cast<double>(i - phraseStart) / static_cast<double>(phraseLen);
    const bool stressed = p.kind == Kind::Vowel && (i == 0 || ph[i - 1] == "|" || (i >= 2 && ph[i - 2] == "|"));
    // Targets.
    double t1 = f1, t2 = f2, t3 = f3, tav = 0, tan = 0, tnf = nf;
    const double fs = v.formant;
    switch (p.kind) {
      case Kind::Vowel: t1 = p.f1 * fs, t2 = p.f2 * fs, t3 = p.f3 * fs, tav = 1; break;
      case Kind::Sonorant: t1 = p.f1 * fs, t2 = p.f2 * fs, t3 = p.f3 * fs, tav = (ph[i] == "M" || ph[i] == "N" || ph[i] == "NG") ? 0.45 : 0.7; break;
      case Kind::Fricative: tnf = p.f1, tan = ph[i] == "S" || ph[i] == "SH" ? 0.5 : 0.22; break;
      case Kind::VoicedFricative: tnf = p.f1, tan = 0.25, tav = 0.35; break;
      case Kind::Aspirate: tan = 0.3, tnf = 1500; break;
      case Kind::Plosive: tnf = p.f1; break;
      case Kind::VoicedPlosive: tnf = p.f1, tav = 0.25; break;
      case Kind::Pause: break;
    }
    // The next vowel's formants colour an aspirate and a plosive's release.
    if (p.kind == Kind::Aspirate || p.kind == Kind::Plosive || p.kind == Kind::VoicedPlosive)
      for (size_t k = i + 1; k < ph.size(); ++k) {
        auto nx = table().find(ph[k]);
        if (nx == table().end() || nx->second.kind == Kind::Pause) break;
        if (nx->second.kind == Kind::Vowel || nx->second.kind == Kind::Sonorant) {
          t1 = nx->second.f1 * fs, t2 = nx->second.f2 * fs, t3 = nx->second.f3 * fs;
          break;
        }
      }
    const size_t len = static_cast<size_t>(p.dur / speed * sr);
    const bool plosive = p.kind == Kind::Plosive || p.kind == Kind::VoicedPlosive;
    for (size_t s = 0; s < len && out.size() < cap; ++s) {
      const double frac = static_cast<double>(s) / static_cast<double>(std::max<size_t>(1, len));
      double g1 = t1, g2 = t2, g3 = t3;
      if (p.f1b > 0) g1 = t1 + (p.f1b * fs - t1) * frac, g2 = t2 + (p.f2b * fs - t2) * frac, g3 = t3 + (p.f3b * fs - t3) * frac;
      double wantAn = tan, wantAv = tav;
      if (plosive) {  // closure, then a short burst of noise
        const bool burst = frac > 0.6 && frac < 0.8;
        wantAn = burst ? 0.6 : 0.0;
        wantAv = frac > 0.6 ? tav : (p.kind == Kind::VoicedPlosive ? 0.1 : 0.0);
      }
      // Glides between targets (about 12 ms for formants, 4 ms for levels).
      const double kf = 1 - std::exp(-1.0 / (0.012 * sr)), ka = 1 - std::exp(-1.0 / (0.004 * sr));
      f1 += (g1 - f1) * kf, f2 += (g2 - f2) * kf, f3 += (g3 - f3) * kf, nf += (tnf - nf) * kf;
      av += (wantAv - av) * ka, an += (wantAn - an) * ka;
      // Pitch.
      double f0 = v.pitch;
      if (v.robot < 0.5) {
        f0 *= 1.0 + v.intonation * (0.12 - 0.25 * (where + frac / static_cast<double>(phraseLen)));
        if (stressed) f0 *= 1.0 + 0.12 * v.intonation * std::sin(kPi * frac);
      }
      vibPh += 5.5 / sr;
      f0 *= std::pow(2.0, v.vibrato * std::sin(2 * kPi * vibPh) / 12.0);
      phase += f0 / sr;
      if (phase >= 1) phase -= 1;
      // Source: a falling ramp per period, softened (a voice), or a buzzier square (a robot).
      const double saw = 1 - 2 * phase;
      glot += (saw - glot) * 0.25;
      const double sq = phase < 0.5 ? 1.0 : -1.0;
      const double rb = std::clamp(static_cast<double>(v.robot), 0.0, 1.0);
      double src = (1 - rb) * glot + rb * 0.6 * sq;
      const double br = std::clamp(static_cast<double>(v.breath), 0.0, 1.0);
      src = src * (1 - br) + noise(rng) * br * 0.7;
      r1.set(f1, 70, sr), r2.set(f2, 100, sr), r3.set(f3, 140, sr), rn.set(nf, std::max(300.0, nf / 3), sr);
      double voiced = av * (r1.run(src) + 0.6 * r2.run(src) + 0.35 * r3.run(src));
      if (p.kind == Kind::Sonorant && tav < 0.6) {  // nasal murmur
        nasal.set(250, 100, sr);
        voiced = 0.6 * voiced + av * nasal.run(src);
      }
      const double fric = an * rn.run(noise(rng));
      double y = voiced + fric;
      if (rb > 0) {
        ringPh += v.ring / sr;
        y = y * (1 - 0.6 * rb) + y * 0.6 * rb * std::sin(2 * kPi * ringPh);
      }
      out.push_back(static_cast<float>(y));
    }
  }
  // A short tail, then to a peak of -2 dB.
  for (int k = 0; k < static_cast<int>(0.05 * sr) && out.size() < cap; ++k) {
    av *= 0.995, an *= 0.995;
    out.push_back(static_cast<float>(av * r1.run(0) + an * rn.run(0)));
  }
  float peak = 0;
  for (float x : out) peak = std::max(peak, std::fabs(x));
  if (peak > 0)
    for (float& x : out) x *= 0.8f / peak;
  return out;
}

}  // namespace gl::speech
