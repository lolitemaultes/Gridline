// Gridline — chord tools (J-31). See ChordTools.h.
#include "ChordTools.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include "NoteTools.h"

namespace gl::chord {

namespace {

int mod12(int x) { return ((x % 12) + 12) % 12; }
int clampPitch(int p) { return std::clamp(p, 0, 127); }

uint32_t nextRand(uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}
double rand01(uint32_t& s) { return static_cast<double>(nextRand(s) & 0xFFFFFF) / static_cast<double>(0x1000000); }

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

const Quality& qual(int i) {
  const auto& q = qualities();
  return q[static_cast<size_t>(std::clamp(i, 0, static_cast<int>(q.size()) - 1))];
}

// The progression's scale, as a seven-note list (major / natural minor stand-ins).
std::vector<int> sevenSteps(int scale) {
  static const std::vector<int> major = {0, 2, 4, 5, 7, 9, 11}, minor = {0, 2, 3, 5, 7, 8, 10};
  const auto& sc = tools::scales();
  if (scale > 0 && scale < static_cast<int>(sc.size())) {
    const auto& st = sc[static_cast<size_t>(scale)].steps;
    if (st.size() == 7) return st;
    const bool hasMin = std::find(st.begin(), st.end(), 3) != st.end();
    const bool hasMaj = std::find(st.begin(), st.end(), 4) != st.end();
    return hasMin && !hasMaj ? minor : major;
  }
  return major;
}

bool isMinorKey(int scale) { return sevenSteps(scale)[2] == 3; }

int qualityIndex(const char* symbol) { return findQuality(symbol); }

}  // namespace

// ------------------------------------------------------------- qualities ----

const std::vector<Quality>& qualities() {
  static const std::vector<Quality> q = {
      // 0..20: the piano roll chord stamps (order kept from J-11).
      {"Major", "", {0, 4, 7}},
      {"Minor", "m", {0, 3, 7}},
      {"Diminished", "dim", {0, 3, 6}},
      {"Augmented", "aug", {0, 4, 8}},
      {"Suspended 2nd", "sus2", {0, 2, 7}},
      {"Suspended 4th", "sus4", {0, 5, 7}},
      {"Power (5th)", "5", {0, 7, 12}},
      {"Major 6th", "6", {0, 4, 7, 9}},
      {"Minor 6th", "m6", {0, 3, 7, 9}},
      {"Dominant 7th", "7", {0, 4, 7, 10}},
      {"Major 7th", "maj7", {0, 4, 7, 11}},
      {"Minor 7th", "m7", {0, 3, 7, 10}},
      {"Half-diminished 7th", "m7b5", {0, 3, 6, 10}},
      {"Diminished 7th", "dim7", {0, 3, 6, 9}},
      {"Minor-major 7th", "m(maj7)", {0, 3, 7, 11}},
      {"Added 9th", "add9", {0, 4, 7, 14}},
      {"Major 9th", "maj9", {0, 4, 7, 11, 14}},
      {"Minor 9th", "m9", {0, 3, 7, 10, 14}},
      {"Dominant 9th", "9", {0, 4, 7, 10, 14}},
      {"Minor 11th", "m11", {0, 3, 7, 10, 14, 17}},
      {"Octaves", "(8va)", {0, 12}},
      // J-31 additions.
      {"Dominant 7th sus4", "7sus4", {0, 5, 7, 10}},
      {"Minor added 9th", "madd9", {0, 3, 7, 14}},
      {"Six-nine", "69", {0, 4, 7, 9, 14}},
      {"Augmented 7th", "aug7", {0, 4, 8, 10}},
      {"Dominant 13th", "13", {0, 4, 7, 10, 14, 21}},
      {"Major 11th (#11)", "maj7#11", {0, 4, 7, 11, 18}},
  };
  return q;
}

int findQuality(const std::string& symbol) {
  const auto& q = qualities();
  for (size_t i = 0; i < q.size(); ++i)
    if (symbol == q[i].symbol) return static_cast<int>(i);
  return -1;
}

int qualityOfClasses(const std::vector<int>& pcs) {
  std::set<int> want;
  for (int p : pcs) want.insert(mod12(p));
  const auto& q = qualities();
  for (size_t i = 0; i < q.size(); ++i) {
    if (q[i].steps.size() < 2 || std::string(q[i].symbol) == "(8va)") continue;
    std::set<int> have;
    for (int s : q[i].steps) have.insert(mod12(s));
    if (have == want) return static_cast<int>(i);
  }
  return -1;
}

std::string noteName(int pc, bool flats) {
  static const char* const sh[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  static const char* const fl[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
  return (flats ? fl : sh)[mod12(pc)];
}

const char* voicingName(Voicing v) {
  static const char* const n[] = {"Close", "Open", "Drop 2", "Spread", "Smooth"};
  const int i = static_cast<int>(v);
  return i >= 0 && i < static_cast<int>(Voicing::Count) ? n[i] : "";
}

// ---------------------------------------------------------- scale degrees ----

int degreeOffset(int scale, int degree) {
  const auto st = sevenSteps(scale);
  const int oct = degree >= 0 ? degree / 7 : -((-degree + 6) / 7);
  const int k = degree - oct * 7;
  return st[static_cast<size_t>(k)] + 12 * oct;
}

Chord diatonicChord(int key, int scale, int degree, bool sevenths) {
  const int d0 = std::clamp(degree, 1, 7) - 1;
  const int base = degreeOffset(scale, d0);
  std::vector<int> rel;
  for (int k = 0; k < (sevenths ? 4 : 3); ++k) rel.push_back(degreeOffset(scale, d0 + 2 * k) - base);
  Chord c;
  c.root = mod12(key + base);
  int q = qualityOfClasses(rel);
  if (q < 0) q = rel[1] == 3 ? 1 : 0;
  c.quality = q;
  return c;
}

int degreeOf(int key, int scale, int rootPc) {
  const auto st = sevenSteps(scale);
  const int rel = mod12(rootPc - key);
  for (size_t i = 0; i < st.size(); ++i)
    if (st[i] == rel) return static_cast<int>(i) + 1;
  return 0;
}

// ---------------------------------------------------------------- names ----

std::string chordName(const Chord& c, bool flats) {
  std::string n = noteName(c.root, flats) + qual(c.quality).symbol;
  if (c.fixedBass >= 0 && mod12(c.fixedBass) != mod12(c.root)) n += "/" + noteName(c.fixedBass, flats);
  return n;
}

std::string romanName(const Chord& c, int key, int scale) {
  static const char* const up[] = {"I", "II", "III", "IV", "V", "VI", "VII"};
  static const char* const lo[] = {"i", "ii", "iii", "iv", "v", "vi", "vii"};
  std::string acc;
  int deg = degreeOf(key, scale, c.root);
  if (deg == 0) {
    if (int d = degreeOf(key, scale, c.root + 1); d > 0) {
      acc = "b";
      deg = d;
    } else if (int e = degreeOf(key, scale, c.root - 1); e > 0) {
      acc = "#";
      deg = e;
    } else {
      return chordName(c);
    }
  }
  const auto& steps = qual(c.quality).steps;
  const bool minor3 = std::find(steps.begin(), steps.end(), 3) != steps.end() &&
                      std::find(steps.begin(), steps.end(), 4) == steps.end();
  std::string sym = qual(c.quality).symbol, suffix;
  if (minor3) {
    if (sym == "dim") suffix = "°";
    else if (sym == "dim7") suffix = "°7";
    else if (sym == "m7b5") suffix = "ø7";
    else if (!sym.empty() && sym[0] == 'm') suffix = sym.substr(1);
    else suffix = sym;
  } else {
    suffix = sym == "aug" ? "+" : sym == "(8va)" ? "" : sym;
  }
  std::string n = acc + (minor3 ? lo : up)[deg - 1] + suffix;
  if (c.fixedBass >= 0 && mod12(c.fixedBass) != mod12(c.root)) n += "/" + noteName(c.fixedBass);
  return n;
}

namespace {

// Symbol aliases people type.
std::string normalizeSymbol(std::string s) {
  static const std::vector<std::pair<std::string, std::string>> alias = {
      {"maj", ""}, {"M", ""}, {"min", "m"}, {"mi", "m"}, {"M7", "maj7"}, {"Maj7", "maj7"}, {"^7", "maj7"},
      {"min7", "m7"}, {"mi7", "m7"}, {"o", "dim"}, {"°", "dim"}, {"o7", "dim7"}, {"°7", "dim7"},
      {"ø", "m7b5"}, {"ø7", "m7b5"}, {"m7-5", "m7b5"}, {"+", "aug"}, {"+7", "aug7"}, {"7+", "aug7"},
      {"sus", "sus4"}, {"7sus", "7sus4"}, {"mmaj7", "m(maj7)"}, {"mM7", "m(maj7)"}, {"6/9", "69"},
      {"add2", "add9"}, {"madd2", "madd9"}, {"M9", "maj9"}, {"min9", "m9"}, {"min11", "m11"}, {"min6", "m6"},
  };
  for (const auto& [from, to] : alias)
    if (s == from) return to;
  return s;
}

int letterPc(char c) {
  switch (std::toupper(static_cast<unsigned char>(c))) {
    case 'C': return 0;
    case 'D': return 2;
    case 'E': return 4;
    case 'F': return 5;
    case 'G': return 7;
    case 'A': return 9;
    case 'B': return 11;
    default: return -1;
  }
}

// "C", "F#", "Bb" at s[i]; advances i. -1 = no note.
int readNote(const std::string& s, size_t& i) {
  if (i >= s.size()) return -1;
  int pc = letterPc(s[i]);
  if (pc < 0) return -1;
  ++i;
  while (i < s.size() && (s[i] == '#' || s[i] == 'b')) {
    // "Cb" is a note; "Cbm" too. A 'b' is an accidental right after the letter only.
    pc += s[i] == '#' ? 1 : -1;
    ++i;
    break;
  }
  return mod12(pc);
}

bool parseName(const std::string& t, Chord& out) {
  size_t i = 0;
  const int root = readNote(t, i);
  if (root < 0) return false;
  std::string rest = t.substr(i), bassText;
  if (size_t sl = rest.rfind('/'); sl != std::string::npos && sl + 1 < rest.size() && letterPc(rest[sl + 1]) >= 0) {
    bassText = rest.substr(sl + 1);
    rest = rest.substr(0, sl);
  }
  const int q = findQuality(normalizeSymbol(rest));
  if (q < 0) return false;
  out.root = root;
  out.quality = q;
  out.fixedBass = -1;
  if (!bassText.empty()) {
    size_t j = 0;
    const int b = readNote(bassText, j);
    if (b < 0 || j != bassText.size()) return false;
    if (b != root) out.fixedBass = b;
  }
  return true;
}

bool parseRoman(const std::string& t, int key, int scale, Chord& out) {
  size_t i = 0;
  int acc = 0;
  if (i < t.size() && (t[i] == 'b' || t[i] == '#')) {
    acc = t[i] == 'b' ? -1 : 1;
    ++i;
  }
  size_t j = i;
  while (j < t.size() && std::string("IViv").find(t[j]) != std::string::npos) ++j;
  if (j == i) return false;
  std::string num = t.substr(i, j - i), upNum;
  for (char c : num) upNum += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  static const char* const nums[] = {"I", "II", "III", "IV", "V", "VI", "VII"};
  int deg = 0;
  for (int k = 0; k < 7; ++k)
    if (upNum == nums[k]) deg = k + 1;
  if (deg == 0) return false;
  const bool lower = std::islower(static_cast<unsigned char>(num[0])) != 0;
  std::string suffix = t.substr(j), bassText;
  if (size_t sl = suffix.rfind('/'); sl != std::string::npos) {
    bassText = suffix.substr(sl + 1);
    suffix = suffix.substr(0, sl);
  }
  std::string sym;
  if (suffix == "°" || suffix == "o" || suffix == "dim") sym = "dim";
  else if (suffix == "°7" || suffix == "o7" || suffix == "dim7") sym = "dim7";
  else if (suffix == "ø" || suffix == "ø7" || suffix == "m7b5") sym = "m7b5";
  else if (suffix == "+" || suffix == "aug") sym = "aug";
  else if (lower) sym = suffix == "add9" ? "madd9" : suffix == "maj7" ? "m(maj7)" : "m" + suffix;
  else sym = normalizeSymbol(suffix);
  const int q = findQuality(sym);
  if (q < 0) return false;
  out.root = mod12(key + degreeOffset(scale, deg - 1) + acc);
  out.quality = q;
  out.fixedBass = -1;
  if (!bassText.empty()) {
    Chord b;
    size_t k = 0;
    int pc = readNote(bassText, k);
    if (pc < 0 && parseRoman(bassText, key, scale, b)) pc = b.root;
    if (pc < 0) return false;
    if (pc != out.root) out.fixedBass = pc;
  }
  return true;
}

}  // namespace

bool parseChord(const std::string& text, int key, int scale, Chord& out) {
  const std::string t = trim(text);
  if (t.empty()) return false;
  Chord c = out;
  const char f = t[0];
  const bool flatRoman = f == 'b' && t.size() > 1 && std::string("IViv").find(t[1]) != std::string::npos;
  const bool roman = flatRoman || f == '#' || f == 'I' || f == 'V' || f == 'i' || f == 'v';
  if (roman ? parseRoman(t, key, scale, c) : parseName(t, c)) {
    out = c;
    return true;
  }
  return false;
}

bool detect(const std::vector<int>& pitches, Chord& out) {
  if (pitches.empty()) return false;
  std::set<int> pcs;
  int lowest = 1000;
  for (int p : pitches) {
    pcs.insert(mod12(p));
    lowest = std::min(lowest, p);
  }
  if (pcs.size() < 2) return false;
  const int bass = mod12(lowest);
  std::vector<int> roots = {bass};
  for (int pc : pcs)
    if (pc != bass) roots.push_back(pc);
  for (int root : roots) {
    std::vector<int> rel;
    for (int pc : pcs) rel.push_back(mod12(pc - root));
    const int q = qualityOfClasses(rel);
    if (q < 0) continue;
    Chord c = out;
    c.root = root;
    c.quality = q;
    c.fixedBass = -1;
    c.inversion = 0;
    if (root != bass) {  // the bass's position among the chord tones = inversion
      const auto& st = qual(q).steps;
      for (size_t k = 0; k < st.size(); ++k)
        if (mod12(root + st[k]) == bass) {
          c.inversion = static_cast<int>(k);
          break;
        }
    }
    out = c;
    return true;
  }
  return false;
}

// ------------------------------------------------------------ performance ----

const std::vector<Rhythm>& rhythms() {
  using P = Hit::Part;
  static const std::vector<Rhythm> r = [] {
    std::vector<Rhythm> v;
    v.push_back({"Held", {{0, 1, P::All, 0, 1.0}}});
    v.push_back({"Two pushes", {{0, 0.5, P::All, 0, 1.0}, {0.5, 0.5, P::All, 0, 0.85}}});
    v.push_back({"Quarter pulse", {{0, 0.25, P::All, 0, 1.0}, {0.25, 0.25, P::All, 0, 0.8},
                                   {0.5, 0.25, P::All, 0, 0.9}, {0.75, 0.25, P::All, 0, 0.8}}});
    {
      Rhythm e{"Eighth pulse", {{0, 0.5, P::Bass, 0, 1.0}, {0.5, 0.5, P::Bass, 0, 0.9}}};
      for (int k = 0; k < 8; ++k) e.hits.push_back({k / 8.0, 1.0 / 8.0, P::Main, 0, k % 2 == 0 ? 0.95 : 0.75});
      v.push_back(e);
    }
    {
      Rhythm o{"Offbeat stabs", {{0, 1, P::Bass, 0, 1.0}}};
      for (int k = 0; k < 4; ++k) o.hits.push_back({k / 4.0 + 0.125, 1.0 / 16.0, P::Main, 0, 0.9});
      v.push_back(o);
    }
    v.push_back({"Push and hold", {{0, 0.375, P::All, 0, 1.0}, {0.375, 0.625, P::All, 0, 0.9}}});
    {
      Rhythm a{"Broken up", {{0, 1, P::Bass, 0, 1.0}}};
      for (int k = 0; k < 8; ++k) a.hits.push_back({k / 8.0, 1.0 / 8.0, P::Arp, k, k % 2 == 0 ? 0.9 : 0.75});
      v.push_back(a);
    }
    {
      Rhythm a{"Broken up and down", {{0, 1, P::Bass, 0, 1.0}}};
      static const int idx[] = {0, 1, 2, 3, 2, 1, 0, 1};
      for (int k = 0; k < 8; ++k) a.hits.push_back({k / 8.0, 1.0 / 8.0, P::Arp, idx[k], k % 2 == 0 ? 0.9 : 0.75});
      v.push_back(a);
    }
    v.push_back({"Bass then chord", {{0, 0.25, P::Bass, 0, 1.0}, {0.25, 0.75, P::Main, 0, 0.85}}});
    v.push_back({"Three-step", {{0, 1.0 / 3, P::Bass, 0, 1.0}, {1.0 / 3, 1.0 / 3, P::Main, 0, 0.8},
                                {2.0 / 3, 1.0 / 3, P::Main, 0, 0.8}}});
    return v;
  }();
  return r;
}

// ------------------------------------------------------------ progression ----

Tick chordLength(const ChordProgression& p, size_t i) {
  if (i >= p.chords.size()) return 0;
  const Tick l = p.chords[i].length > 0 ? p.chords[i].length : p.chordLength;
  return std::max<Tick>(1, l);
}

Tick chordStart(const ChordProgression& p, size_t i) {
  Tick t = 0;
  for (size_t k = 0; k < i && k < p.chords.size(); ++k) t += chordLength(p, k);
  return t;
}

Tick totalLength(const ChordProgression& p) { return chordStart(p, p.chords.size()); }

int chordAt(const ChordProgression& p, Tick t) {
  if (t < 0) return -1;
  Tick a = 0;
  for (size_t k = 0; k < p.chords.size(); ++k) {
    const Tick l = chordLength(p, k);
    if (t < a + l) return static_cast<int>(k);
    a += l;
  }
  return -1;
}

namespace {

// Main notes of chord i before the voicing.
std::vector<int> rawTones(const ChordProgression& p, const Chord& c, int& rootPitch) {
  const int rel = mod12(c.root - p.key);
  rootPitch = 12 * (p.octave + c.octave) + p.key + rel;
  std::vector<int> st = qual(c.quality).steps;
  if (c.extra > 0) {  // extensions: the scale's 9th / 11th / 13th when the root is in the key
    std::vector<int> ext;
    const int deg = degreeOf(p.key, p.scale, c.root);
    if (deg > 0) {
      const int base = degreeOffset(p.scale, deg - 1);
      for (int k : {8, 10, 12}) ext.push_back(degreeOffset(p.scale, deg - 1 + k) - base);
    } else {
      ext = {14, 17, 21};
    }
    int added = 0;
    for (int e : ext) {
      if (added >= c.extra) break;
      bool have = false;
      for (int s : st) have = have || mod12(s) == mod12(e);
      if (have) continue;
      st.push_back(e);
      ++added;
    }
  } else if (c.extra < 0 && st.size() > 2) {  // one note left out: the fifth, else the top note
    auto five = std::find(st.begin(), st.end(), 7);
    if (five != st.end())
      st.erase(five);
    else
      st.pop_back();
  }
  std::vector<int> out;
  for (int s : st) out.push_back(rootPitch + s);
  std::sort(out.begin(), out.end());
  return out;
}

void applyInversion(std::vector<int>& v, int inversion) {
  if (v.empty()) return;
  for (int k = 0; k < inversion; ++k) {
    std::sort(v.begin(), v.end());
    v.front() += 12;
  }
  for (int k = 0; k > inversion; --k) {
    std::sort(v.begin(), v.end());
    v.back() -= 12;
  }
  std::sort(v.begin(), v.end());
}

double centre(const std::vector<int>& v) {
  double s = 0;
  for (int x : v) s += x;
  return v.empty() ? 0 : s / static_cast<double>(v.size());
}

std::vector<int> voiceMain(const ChordProgression& p, size_t i, int depth) {
  const Chord& c = p.chords[i];
  int rp = 0;
  std::vector<int> v = rawTones(p, c, rp);
  const int n = static_cast<int>(v.size());
  switch (c.voicing) {
    case Voicing::Close:
      applyInversion(v, c.inversion);
      break;
    case Voicing::Open:
      applyInversion(v, c.inversion);
      if (n >= 3) v[1] += 12;
      break;
    case Voicing::Drop2:
      applyInversion(v, c.inversion);
      if (n >= 3) v[static_cast<size_t>(n - 2)] -= 12;
      break;
    case Voicing::Spread:
      for (size_t k = 1; k < v.size(); ++k)
        if (v[k] - rp != 7) v[k] += 12;
      applyInversion(v, c.inversion);
      break;
    case Voicing::Smooth: {
      if (i == 0 || depth > 64) {
        applyInversion(v, c.inversion);
        break;
      }
      const std::vector<int> prev = voiceMain(p, i - 1, depth + 1);
      const double target = centre(prev);
      std::vector<int> best;
      double bestCost = 1e9;
      for (int inv = -n; inv <= n; ++inv) {
        std::vector<int> cand = v;
        applyInversion(cand, inv);
        // Movement: each note to its nearest previous note, plus the distance of the centres.
        double cost = std::fabs(centre(cand) - target);
        for (int x : cand) {
          int d = 128;
          for (int y : prev) d = std::min(d, std::abs(x - y));
          cost += d * 0.25;
        }
        if (cost < bestCost - 1e-9) {
          bestCost = cost;
          best = cand;
        }
      }
      v = best;
      break;
    }
    default:
      break;
  }
  std::sort(v.begin(), v.end());
  for (int& x : v) x = clampPitch(x);
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

}  // namespace

Voiced voice(const ChordProgression& p, size_t i) {
  Voiced out;
  if (i >= p.chords.size()) return out;
  const Chord& c = p.chords[i];
  out.main = voiceMain(p, i, 0);
  int rp = 0;
  rawTones(p, c, rp);
  const int pc = c.fixedBass >= 0 ? mod12(c.fixedBass) : c.root;
  int b = rp - 12 + mod12(pc - c.root);
  const int lowest = out.main.empty() ? rp : out.main.front();
  while (b >= lowest) b -= 12;
  b += 12 * c.bassOctave;
  out.bass = clampPitch(b);
  return out;
}

std::vector<int> chordClasses(const ChordProgression& p, size_t i) {
  std::set<int> s;
  const Voiced v = voice(p, i);
  for (int x : v.main) s.insert(mod12(x));
  if (i < p.chords.size() && p.chords[i].bass && v.bass >= 0) s.insert(mod12(v.bass));
  return {s.begin(), s.end()};
}

std::vector<Note> renderChord(const ChordProgression& p, size_t i, Tick start, bool withMain, bool withBass) {
  std::vector<Note> out;
  if (i >= p.chords.size()) return out;
  const Chord& c = p.chords[i];
  const Voiced v = voice(p, i);
  const bool mainOn = withMain && c.main && !v.main.empty();
  const bool bassOn = withBass && c.bass && v.bass >= 0;
  const Tick L = chordLength(p, i);
  const Tick cycle = p.perf.cycle > 0 ? p.perf.cycle : L;
  const auto& rs = rhythms();
  const Rhythm& r = rs[static_cast<size_t>(std::clamp(p.perf.rhythm, 0, static_cast<int>(rs.size()) - 1))];
  const double gate = std::clamp(p.perf.gate, 0.05, 1.0);
  for (Tick cs = 0; cs < L; cs += cycle) {
    for (const Hit& h : r.hits) {
      const Tick at = cs + static_cast<Tick>(std::llround(h.at * static_cast<double>(cycle)));
      if (at >= L) continue;
      Tick len = static_cast<Tick>(std::llround(h.len * static_cast<double>(cycle) * gate));
      len = std::clamp<Tick>(len, 1, L - at);
      std::vector<int> pitches;
      switch (h.part) {
        case Hit::All:
          if (mainOn) pitches = v.main;
          if (bassOn) pitches.insert(pitches.begin(), v.bass);
          break;
        case Hit::Main:
          if (mainOn) pitches = v.main;
          break;
        case Hit::Bass:
          if (bassOn) pitches.push_back(v.bass);
          break;
        case Hit::Arp:
          if (mainOn) {
            // Index past the top note continues an octave up.
            const int n = static_cast<int>(v.main.size());
            const int k = std::max(0, h.index);
            pitches.push_back(clampPitch(v.main[static_cast<size_t>(k % n)] + 12 * (k / n)));
          }
          break;
      }
      std::sort(pitches.begin(), pitches.end());
      pitches.erase(std::unique(pitches.begin(), pitches.end()), pitches.end());
      const int n = static_cast<int>(pitches.size());
      for (int k = 0; k < n; ++k) {
        const int order = p.perf.strumDown ? n - 1 - k : k;
        const Tick off = n > 1 && p.perf.strum > 0 ? p.perf.strum * order / (n - 1) : 0;
        if (off >= len) continue;
        Note note(start + chordStart(p, i) + at + off, len - off, pitches[static_cast<size_t>(k)],
                  std::clamp(static_cast<int>(std::lround(p.perf.velocity * h.velocity)), 1, 127));
        note.color = std::clamp(c.color, 0, kNoteColors - 1);
        out.push_back(note);
      }
    }
  }
  return out;
}

namespace {
void sortNotes(std::vector<Note>& v) {
  std::stable_sort(v.begin(), v.end(), [](const Note& a, const Note& b) {
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
  });
}
}  // namespace

std::vector<Note> render(const ChordProgression& p, Tick start, bool withMain, bool withBass) {
  std::vector<Note> out;
  for (size_t i = 0; i < p.chords.size(); ++i) {
    auto n = renderChord(p, i, start, withMain, withBass);
    out.insert(out.end(), n.begin(), n.end());
  }
  sortNotes(out);
  return out;
}

const char* nonChordModeName(NonChordMode m) {
  static const char* const n[] = {"Replace all notes", "Replace clashing notes", "Keep notes", "Snap notes to the chords"};
  const int i = static_cast<int>(m);
  return i >= 0 && i < static_cast<int>(NonChordMode::Count) ? n[i] : "";
}

namespace {
int nearestTone(int pitch, const std::vector<int>& classes) {
  if (classes.empty()) return pitch;
  for (int d = 0; d <= 6; ++d) {
    for (int s : {-d, d}) {
      const int q = pitch + s;
      if (q < 0 || q > 127) continue;
      if (std::find(classes.begin(), classes.end(), mod12(q)) != classes.end()) return q;
    }
  }
  return pitch;
}
}  // namespace

std::vector<Note> rebase(const std::vector<Note>& base, const std::vector<Note>& written, const std::vector<Note>& current) {
  if (current == written) return base;
  // Multiset differences: what left `written` and what is new in `current`.
  std::vector<Note> gone = written, added;
  for (const Note& n : current) {
    auto it = std::find(gone.begin(), gone.end(), n);
    if (it != gone.end())
      gone.erase(it);
    else
      added.push_back(n);
  }
  std::vector<Note> out = base;
  for (const Note& n : gone)
    if (auto it = std::find(out.begin(), out.end(), n); it != out.end()) out.erase(it);
  out.insert(out.end(), added.begin(), added.end());
  return out;
}

std::vector<Note> write(const std::vector<Note>& existing, const ChordProgression& p, const WriteOpts& o,
                        std::vector<Note>* written) {
  const Tick total = totalLength(p);
  auto linkedAt = [&](Tick t) {  // the linked chord over song tick t (-1: none / unlinked)
    const int i = chordAt(p, t - o.start);
    return i >= 0 && p.chords[static_cast<size_t>(i)].linked ? i : -1;
  };
  // Notes the last write produced (still unchanged) go, unless an unlinked chord now owns them.
  std::vector<Note> prev = o.previous;
  std::vector<Note> keep;
  for (const Note& n : existing) {
    auto it = std::find_if(prev.begin(), prev.end(), [&](const Note& q) {
      return q.start == n.start && q.pitch == n.pitch && q.length == n.length;
    });
    const int under = chordAt(p, n.start - o.start);
    const bool unlinked = under >= 0 && !p.chords[static_cast<size_t>(under)].linked;
    if (it != prev.end() && !unlinked) {
      prev.erase(it);
      continue;
    }
    keep.push_back(n);
  }
  std::vector<Note> out;
  for (Note n : keep) {
    const bool inSpan = n.start >= o.start && n.start < o.start + total;
    const int i = inSpan ? linkedAt(n.start) : -1;
    if (i < 0) {
      out.push_back(n);
      continue;
    }
    const auto classes = chordClasses(p, static_cast<size_t>(i));
    switch (o.mode) {
      case NonChordMode::ReplaceAll: continue;
      case NonChordMode::ReplaceClashing:
        if (std::find(classes.begin(), classes.end(), mod12(n.pitch)) == classes.end()) continue;
        break;
      case NonChordMode::Snap: n.pitch = nearestTone(n.pitch, classes); break;
      default: break;
    }
    out.push_back(n);
  }
  std::vector<Note> add;
  for (size_t i = 0; i < p.chords.size(); ++i) {
    if (!p.chords[i].linked) continue;
    auto n = renderChord(p, i, o.start, o.withMain, o.withBass);
    add.insert(add.end(), n.begin(), n.end());
  }
  // A new note replaces an old one with the same start and pitch.
  out.erase(std::remove_if(out.begin(), out.end(), [&](const Note& n) {
              return std::any_of(add.begin(), add.end(), [&](const Note& a) { return a.start == n.start && a.pitch == n.pitch; });
            }),
            out.end());
  out.insert(out.end(), add.begin(), add.end());
  sortNotes(out);
  if (written) {
    sortNotes(add);
    *written = add;
  }
  return out;
}

int snapNotes(std::vector<Note>& notes, const ChordProgression& p, Tick start) {
  int moved = 0;
  for (Note& n : notes) {
    const int i = chordAt(p, n.start - start);
    if (i < 0) continue;
    const int q = nearestTone(n.pitch, chordClasses(p, static_cast<size_t>(i)));
    if (q != n.pitch) {
      n.pitch = q;
      ++moved;
    }
  }
  sortNotes(notes);
  return moved;
}

// -------------------------------------------------------------- editing ----

void invert(Chord& c, int dir) {
  const int n = static_cast<int>(qual(c.quality).steps.size()) + std::max(0, c.extra);
  c.inversion = std::clamp(c.inversion + (dir > 0 ? 1 : dir < 0 ? -1 : 0), -2 * n, 2 * n);
}

void transpose(Chord& c, int semitones) {
  // The root is placed relative to the key, so count octaves from the root's
  // pitch class: B + 1 = C of the next octave.
  const int r = c.root + semitones;
  const int oct = r >= 0 ? r / 12 : -((-r + 11) / 12);
  c.root = mod12(r);
  c.octave += oct;
  if (c.fixedBass >= 0) c.fixedBass = mod12(c.fixedBass + semitones);
}

void setRoot(Chord& c, int pitchClass) { c.root = mod12(pitchClass); }

bool swapChords(ChordProgression& p, size_t i, size_t j) {
  if (i >= p.chords.size() || j >= p.chords.size() || i == j) return false;
  // The chords trade places; each slot keeps its length (the rhythm of the progression stays).
  const Tick li = p.chords[i].length, lj = p.chords[j].length;
  std::swap(p.chords[i], p.chords[j]);
  p.chords[i].length = li;
  p.chords[j].length = lj;
  return true;
}

int addPassingChord(ChordProgression& p, size_t i, Tick beat) {
  if (i >= p.chords.size()) return -1;
  beat = std::max<Tick>(1, beat);
  const Tick L = chordLength(p, i);
  if (L < 2 * beat) return -1;
  Tick first = (L / 2) / beat * beat;
  first = std::clamp<Tick>(first, beat, L - beat);
  const Chord& next = p.chords[(i + 1) % p.chords.size()];
  Chord pass = p.chords[i];
  pass.root = mod12(next.root + 7);  // the dominant of the next chord
  pass.quality = qualityIndex("7");
  pass.inversion = 0;
  pass.fixedBass = -1;
  pass.extra = 0;
  pass.locked = false;
  pass.length = L - first;
  p.chords[i].length = first;
  p.chords.insert(p.chords.begin() + static_cast<std::ptrdiff_t>(i) + 1, pass);
  return static_cast<int>(i) + 1;
}

size_t addChord(ChordProgression& p, int at) {
  Chord c = diatonicChord(p.key, p.scale, 1);
  return addChord(p, c, at);
}

size_t addChord(ChordProgression& p, const Chord& like, int at) {
  size_t pos = at < 0 || at > static_cast<int>(p.chords.size()) ? p.chords.size() : static_cast<size_t>(at);
  p.chords.insert(p.chords.begin() + static_cast<std::ptrdiff_t>(pos), like);
  return pos;
}

bool removeChord(ChordProgression& p, size_t i) {
  if (i >= p.chords.size()) return false;
  p.chords.erase(p.chords.begin() + static_cast<std::ptrdiff_t>(i));
  return true;
}

namespace {

// Borrowed / colour chords of a key: (semitones above the key, quality symbol).
const std::vector<std::pair<int, const char*>>& borrowed(bool minorKey) {
  static const std::vector<std::pair<int, const char*>> maj = {{10, ""}, {8, ""}, {5, "m"}, {3, ""}, {2, "7"}, {9, "7"}, {4, "7"}},
                                                         min = {{7, ""}, {7, "7"}, {5, ""}, {2, "m"}, {1, ""}, {0, ""}, {11, "dim"}};
  return minorKey ? min : maj;
}

// Common moves between scale degrees (from, to) → weight.
double moveWeight(int from, int to) {
  static const double w[8][8] = {
      // to: -  1  2  3  4  5  6  7
      {0, 6, 2, 1, 4, 3, 4, 0},  // start
      {0, 1, 3, 2, 6, 6, 5, 1},  // from I
      {0, 2, 1, 1, 3, 7, 1, 1},  // ii
      {0, 2, 1, 1, 4, 1, 5, 0},  // iii
      {0, 5, 3, 1, 1, 6, 2, 1},  // IV
      {0, 7, 1, 1, 3, 1, 5, 0},  // V
      {0, 2, 4, 2, 6, 4, 1, 0},  // vi
      {0, 6, 1, 3, 1, 1, 2, 1},  // vii
  };
  return w[std::clamp(from, 0, 7)][std::clamp(to, 0, 7)];
}

}  // namespace

std::vector<Chord> alternatives(const ChordProgression& p, size_t i, size_t max) {
  std::vector<Chord> out;
  if (i >= p.chords.size()) return out;
  const Chord& cur = p.chords[i];
  auto classesOf = [&](const Chord& c) {
    std::set<int> s;
    for (int st : qual(c.quality).steps) s.insert(mod12(c.root + st));
    return s;
  };
  const std::set<int> have = classesOf(cur);
  std::vector<std::pair<Chord, int>> cand;  // chord, rank (lower = more usual)
  int rank = 0;
  auto push = [&](Chord c) {
    Chord k = cur;
    k.root = c.root;
    k.quality = c.quality;
    k.fixedBass = -1;
    k.locked = false;
    if (k.root == cur.root && k.quality == cur.quality) return;
    for (const auto& e : cand)
      if (e.first.root == k.root && e.first.quality == k.quality) return;
    cand.push_back({k, rank++});
  };
  for (int d = 1; d <= 7; ++d) push(diatonicChord(p.key, p.scale, d));
  for (int d = 1; d <= 7; ++d) push(diatonicChord(p.key, p.scale, d, true));
  for (const char* s : {"", "m", "sus2", "sus4", "7", "maj7", "m7", "add9", "6"}) {
    Chord c;
    c.root = cur.root;
    c.quality = qualityIndex(s);
    if (c.quality >= 0) push(c);
  }
  for (const auto& [off, sym] : borrowed(isMinorKey(p.scale))) {
    Chord c;
    c.root = mod12(p.key + off);
    c.quality = qualityIndex(sym);
    if (c.quality >= 0) push(c);
  }
  std::vector<std::pair<int, std::pair<int, Chord>>> scored;
  for (const auto& [c, r] : cand) {
    int shared = 0;
    for (int pc : classesOf(c)) shared += have.count(pc) ? 1 : 0;
    if (shared == 0) continue;
    scored.push_back({shared, {r, c}});
  }
  std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first > b.first : a.second.first < b.second.first;
  });
  for (const auto& s : scored) {
    if (out.size() >= max) break;
    out.push_back(s.second.second);
  }
  return out;
}

void regenerate(ChordProgression& p, const std::vector<size_t>& which, double adventure, uint32_t seed) {
  adventure = std::clamp(adventure, 0.0, 1.0);
  uint32_t st = seed ? seed : 1;
  for (int k = 0; k < 3; ++k) nextRand(st);
  std::set<size_t> pick(which.begin(), which.end());
  const bool minorKey = isMinorKey(p.scale);
  for (size_t i = 0; i < p.chords.size(); ++i) {
    if ((!pick.empty() && !pick.count(i)) || p.chords[i].locked) continue;
    Chord& c = p.chords[i];
    const int prevDeg = i == 0 ? 0 : degreeOf(p.key, p.scale, p.chords[i - 1].root);
    Chord next;
    if (rand01(st) < adventure * 0.45) {  // a borrowed / colour chord
      const auto& b = borrowed(minorKey);
      const auto& e = b[nextRand(st) % b.size()];
      next.root = mod12(p.key + e.first);
      next.quality = std::max(0, qualityIndex(e.second));
    } else {
      // Weighted diatonic move; adventure flattens the weights.
      double w[8] = {0, 0, 0, 0, 0, 0, 0, 0}, sum = 0;
      for (int d = 1; d <= 7; ++d) {
        w[d] = moveWeight(prevDeg, d) * (1.0 - adventure) + adventure * 2.0;
        if (d == 7 && adventure < 0.3) w[d] *= 0.3;  // the diminished chord stays rare
        if (i > 0 && d == prevDeg) w[d] *= 0.3;       // repeats are rare
        sum += w[d];
      }
      double r = rand01(st) * sum;
      int deg = 1;
      for (int d = 1; d <= 7; ++d) {
        if (r < w[d]) {
          deg = d;
          break;
        }
        r -= w[d];
        deg = d;
      }
      next = diatonicChord(p.key, p.scale, deg, rand01(st) < adventure * 0.5);
    }
    c.root = next.root;
    c.quality = next.quality;
    c.fixedBass = -1;
    c.inversion = 0;
  }
}

ChordProgression generate(int key, int scale, int count, Tick chordLen, double adventure, uint32_t seed) {
  ChordProgression p;
  p.key = mod12(key);
  p.scale = scale;
  p.chordLength = std::max<Tick>(1, chordLen);
  for (int k = 0; k < std::clamp(count, 1, 64); ++k) addChord(p);
  regenerate(p, {}, adventure, seed);
  return p;
}

ChordProgression analyze(const std::vector<Note>& notes, int key, int scale, Tick start, Tick len, int count) {
  ChordProgression p;
  p.key = mod12(key);
  p.scale = scale;
  p.chordLength = std::max<Tick>(1, len);
  Chord prev = diatonicChord(p.key, p.scale, 1);
  for (int k = 0; k < std::clamp(count, 1, 256); ++k) {
    const Tick a = start + p.chordLength * k, b = a + p.chordLength;
    std::vector<int> pitches;
    for (const Note& n : notes)
      if (n.start < b && n.start + n.length > a && !n.muted) pitches.push_back(n.pitch);
    Chord c = prev;
    c.inversion = 0;
    if (detect(pitches, c)) {
      c.inversion = 0;  // the editor voices it again
      c.length = 0;
      prev = c;
    }
    p.chords.push_back(prev);
  }
  return p;
}

void rescale(ChordProgression& p, Tick total, Tick grid) {
  const Tick cur = totalLength(p);
  if (p.chords.empty() || total <= 0 || cur <= 0) return;
  grid = std::max<Tick>(1, grid);
  const double f = static_cast<double>(total) / static_cast<double>(cur);
  Tick sum = 0;
  for (size_t i = 0; i < p.chords.size(); ++i) {
    Tick l = static_cast<Tick>(std::llround(static_cast<double>(chordLength(p, i)) * f / static_cast<double>(grid))) * grid;
    p.chords[i].length = std::max(grid, l);
    sum += p.chords[i].length;
  }
  Chord& last = p.chords.back();
  last.length = std::max(grid, last.length + (total - sum));
}

// ------------------------------------------------------------------ text ----

bool parseProgression(const std::string& text, int key, int scale, Tick len, ChordProgression& out, int* bad) {
  ChordProgression p = out;
  p.key = mod12(key);
  p.scale = scale;
  p.chordLength = std::max<Tick>(1, len);
  p.chords.clear();
  int nbad = 0;
  std::string tok;
  auto flush = [&] {
    if (tok.empty()) return;
    Chord c;
    if (parseChord(tok, p.key, p.scale, c))
      p.chords.push_back(c);
    else
      ++nbad;
    tok.clear();
  };
  for (size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    // "m7-5": a dash right after a digit inside a chord is part of the name.
    const bool dashInName = ch == '-' && !tok.empty() && std::isdigit(static_cast<unsigned char>(tok.back())) &&
                            i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]));
    if (!dashInName && (ch == ',' || ch == '-' || ch == ' ' || ch == '|' || ch == '\t' || ch == '\n' || ch == ';'))
      flush();
    else
      tok += ch;
  }
  flush();
  if (bad) *bad = nbad;
  if (p.chords.empty()) return false;
  out = p;
  return true;
}

std::string progressionText(const ChordProgression& p, bool roman) {
  std::string s;
  for (size_t i = 0; i < p.chords.size(); ++i) {
    if (i) s += " - ";
    s += roman ? romanName(p.chords[i], p.key, p.scale) : chordName(p.chords[i]);
  }
  return s;
}

const std::vector<Preset>& presets() {
  static const std::vector<Preset> v = {
      {"Anthem", "I V vi IV", false},
      {"Fifties", "I vi IV V", false},
      {"Ballad", "vi IV I V", false},
      {"Turnaround", "ii7 V7 Imaj7 Imaj7", false},
      {"Twelve-bar seed", "I IV I V", false},
      {"Rising", "I iii IV V", false},
      {"Soul loop", "Imaj7 vi7 ii7 V7", false},
      {"Borrowed four", "I bVII IV I", false},
      {"Long walk", "I V vi iii IV I IV V", false},
      {"Night drive", "i VI III VII", true},
      {"Lament", "i iv VI v", true},
      {"Pulse", "i VII VI VII", true},
      {"Stairs down", "i VII VI V", true},
      {"Two-chord vamp", "i IV i IV", true},
  };
  return v;
}

}  // namespace gl::chord
