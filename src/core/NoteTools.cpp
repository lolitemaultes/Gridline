#include "NoteTools.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace gl::tools {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Targeted notes: the selection, or everything when nothing is selected.
std::vector<bool> targets(const std::vector<Note>& notes, NoteSel& sel) {
  sel.resize(notes.size(), false);
  bool any = std::find(sel.begin(), sel.end(), true) != sel.end();
  if (any) return std::vector<bool>(sel.begin(), sel.end());
  return std::vector<bool>(notes.size(), true);
}

// Small deterministic generator (xorshift32) so tools are repeatable per seed.
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  double unit() { return (next() >> 8) * (1.0 / 16777216.0); }        // 0..1
  double bipolar() { return unit() * 2.0 - 1.0; }                      // -1..1
  int range(int lo, int hi) { return hi <= lo ? lo : lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
};

// 0..1 → 0..1 curve bent by a tension in -1..1 (0 = straight).
double bend(double f, double t) {
  f = std::clamp(f, 0.0, 1.0);
  if (t >= 0) return std::pow(f, 1.0 + 3.0 * t);
  return 1.0 - std::pow(1.0 - f, 1.0 - 3.0 * t);
}

Tick roundTick(double v) { return static_cast<Tick>(std::llround(v)); }

void sortSel(std::vector<Note>& notes, NoteSel& sel) { ops::sortNotes(notes, sel); }

// Notes grouped by start (chords), indices into `notes`, targets only.
std::map<Tick, std::vector<size_t>> chordsOf(const std::vector<Note>& notes, const std::vector<bool>& t) {
  std::map<Tick, std::vector<size_t>> out;
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) out[notes[i].start].push_back(i);
  for (auto& [st, idx] : out)
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return notes[a].pitch < notes[b].pitch; });
  return out;
}

int clampPitch(int p) { return std::clamp(p, 0, 127); }

}  // namespace

// ------------------------------------------------------------ note properties ----

const char* notePropName(NoteProp p) {
  static const char* const n[] = {"Velocity", "Pan", "Release velocity", "Fine pitch", "Mod X", "Mod Y"};
  int i = static_cast<int>(p);
  return i >= 0 && i < static_cast<int>(NoteProp::Count) ? n[i] : "";
}

bool notePropBipolar(NoteProp p) { return p == NoteProp::Pan || p == NoteProp::Fine; }

double notePropNorm(const Note& n, NoteProp p) {
  switch (p) {
    case NoteProp::Pan: return 0.5 + 0.5 * n.pan / kNotePanMax;
    case NoteProp::Release: return n.release / 127.0;
    case NoteProp::Fine: return 0.5 + 0.5 * n.fine / kNoteFineMax;
    case NoteProp::ModX: return static_cast<double>(n.modX) / kNoteModMax;
    case NoteProp::ModY: return static_cast<double>(n.modY) / kNoteModMax;
    default: return n.velocity / 127.0;
  }
}

void setNotePropNorm(Note& n, NoteProp p, double v) {
  v = std::clamp(v, 0.0, 1.0);
  switch (p) {
    case NoteProp::Pan: n.pan = static_cast<int>(std::lround((v * 2 - 1) * kNotePanMax)); break;
    case NoteProp::Release: n.release = static_cast<int>(std::lround(v * 127)); break;
    case NoteProp::Fine: n.fine = static_cast<int>(std::lround((v * 2 - 1) * kNoteFineMax)); break;
    case NoteProp::ModX: n.modX = static_cast<int>(std::lround(v * kNoteModMax)); break;
    case NoteProp::ModY: n.modY = static_cast<int>(std::lround(v * kNoteModMax)); break;
    default: n.velocity = std::clamp(static_cast<int>(std::lround(v * 127)), 1, 127); break;
  }
}

// -------------------------------------------------------------- scales / keys ----

const std::vector<ScaleDef>& scales() {
  static const std::vector<ScaleDef> s = {
      {"Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
      {"Major", {0, 2, 4, 5, 7, 9, 11}},
      {"Natural minor", {0, 2, 3, 5, 7, 8, 10}},
      {"Harmonic minor", {0, 2, 3, 5, 7, 8, 11}},
      {"Melodic minor", {0, 2, 3, 5, 7, 9, 11}},
      {"Dorian", {0, 2, 3, 5, 7, 9, 10}},
      {"Phrygian", {0, 1, 3, 5, 7, 8, 10}},
      {"Lydian", {0, 2, 4, 6, 7, 9, 11}},
      {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
      {"Locrian", {0, 1, 3, 5, 6, 8, 10}},
      {"Major pentatonic", {0, 2, 4, 7, 9}},
      {"Minor pentatonic", {0, 3, 5, 7, 10}},
      {"Blues", {0, 3, 5, 6, 7, 10}},
      {"Whole tone", {0, 2, 4, 6, 8, 10}},
      {"Half-whole", {0, 1, 3, 4, 6, 7, 9, 10}},
      {"Phrygian dominant", {0, 1, 4, 5, 7, 8, 10}},
  };
  return s;
}

bool inScale(int pitch, int root, int scale) {
  const auto& sc = scales();
  if (scale <= 0 || scale >= static_cast<int>(sc.size())) return true;
  int k = ((pitch - root) % 12 + 12) % 12;
  const auto& st = sc[static_cast<size_t>(scale)].steps;
  return std::find(st.begin(), st.end(), k) != st.end();
}

int snapPitchToScale(int pitch, int root, int scale) {
  for (int d = 0; d <= 6; ++d) {
    if (inScale(pitch - d, root, scale) && pitch - d >= 0) return pitch - d;
    if (inScale(pitch + d, root, scale) && pitch + d <= 127) return pitch + d;
  }
  return pitch;
}

void snapToScale(std::vector<Note>& notes, NoteSel& sel, int root, int scale) {
  auto t = targets(notes, sel);
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) notes[i].pitch = snapPitchToScale(notes[i].pitch, root, scale);
  sortSel(notes, sel);
}

void selectInScale(const std::vector<Note>& notes, NoteSel& sel, int root, int scale, bool inside) {
  sel.assign(notes.size(), false);
  for (size_t i = 0; i < notes.size(); ++i) sel[i] = inScale(notes[i].pitch, root, scale) == inside;
}

std::string pitchName(int pitch) {
  static const char* const k[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  pitch = clampPitch(pitch);
  return std::string(k[pitch % 12]) + std::to_string(pitch / 12 - 1);
}

// --------------------------------------------------------------- chord stamps ----

const std::vector<ChordDef>& chords() {
  static const std::vector<ChordDef> c = {
      {"Major", {0, 4, 7}},          {"Minor", {0, 3, 7}},           {"Diminished", {0, 3, 6}},
      {"Augmented", {0, 4, 8}},      {"Suspended 2nd", {0, 2, 7}},   {"Suspended 4th", {0, 5, 7}},
      {"Power (5th)", {0, 7, 12}},   {"Major 6th", {0, 4, 7, 9}},    {"Minor 6th", {0, 3, 7, 9}},
      {"Dominant 7th", {0, 4, 7, 10}}, {"Major 7th", {0, 4, 7, 11}}, {"Minor 7th", {0, 3, 7, 10}},
      {"Half-diminished 7th", {0, 3, 6, 10}}, {"Diminished 7th", {0, 3, 6, 9}}, {"Minor-major 7th", {0, 3, 7, 11}},
      {"Added 9th", {0, 4, 7, 14}},  {"Major 9th", {0, 4, 7, 11, 14}}, {"Minor 9th", {0, 3, 7, 10, 14}},
      {"Dominant 9th", {0, 4, 7, 10, 14}}, {"Minor 11th", {0, 3, 7, 10, 14, 17}}, {"Octaves", {0, 12}},
  };
  return c;
}

std::vector<int> chordPitches(int root, int chord, int inversion) {
  const auto& c = chords();
  if (chord < 0 || chord >= static_cast<int>(c.size())) return {clampPitch(root)};
  std::vector<int> p;
  for (int s : c[static_cast<size_t>(chord)].steps) p.push_back(root + s);
  inversion = std::clamp(inversion, 0, static_cast<int>(p.size()) - 1);
  for (int k = 0; k < inversion; ++k) p[static_cast<size_t>(k)] += 12;
  std::sort(p.begin(), p.end());
  for (int& x : p) x = clampPitch(x);
  p.erase(std::unique(p.begin(), p.end()), p.end());
  return p;
}

std::vector<Note> stampChord(const Note& base, int chord, int inversion) {
  std::vector<Note> out;
  for (int p : chordPitches(base.pitch, chord, inversion)) {
    Note n = base;
    n.pitch = p;
    out.push_back(n);
  }
  return out;
}

// ------------------------------------------------------------------- quantize ----

const char* durationModeName(DurationMode m) {
  static const char* const n[] = {"Keep lengths", "Keep end times", "Quantize lengths", "Quantize end times"};
  int i = static_cast<int>(m);
  return i >= 0 && i < static_cast<int>(DurationMode::Count) ? n[i] : "";
}

const std::vector<Groove>& grooves() {
  static const std::vector<Groove> g = {
      {"Straight", {0.0}, {}},
      {"Light swing", {0.0, 0.12}, {}},
      {"Heavy swing", {0.0, 0.26}, {}},
      {"Triplet shuffle", {0.0, 0.333}, {1.0, 0.8}},
      {"Laid back", {0.03, 0.08, 0.04, 0.1}, {}},
      {"Pushed", {-0.03, -0.07, -0.04, -0.08}, {}},
      {"Loose hands", {0.0, 0.09, -0.04, 0.14, 0.02, 0.07, -0.05, 0.12}, {1.0, 0.82, 0.9, 0.78, 0.96, 0.8, 0.88, 0.76}},
      {"Backbeat accents", {0.0}, {0.85, 0.7, 0.85, 0.7, 1.0, 0.7, 0.85, 0.7}},
  };
  return g;
}

namespace {
Tick lineTarget(const QuantizeOpts& o, Tick line, const std::vector<double>* shift) {
  const Tick grid = std::max<Tick>(1, o.grid);
  double pos = static_cast<double>(line) * grid;
  if (line % 2 != 0) pos += o.swing * grid * 0.5;
  if (shift && !shift->empty()) pos += (*shift)[static_cast<size_t>(((line % static_cast<Tick>(shift->size())) + static_cast<Tick>(shift->size())) % static_cast<Tick>(shift->size()))] * grid;
  return std::max<Tick>(0, roundTick(pos));
}

void quantizeImpl(std::vector<Note>& notes, NoteSel& sel, const QuantizeOpts& o, const std::vector<double>* shift,
                  const std::vector<double>* vel) {
  auto t = targets(notes, sel);
  const Tick grid = std::max<Tick>(1, o.grid);
  for (size_t i = 0; i < notes.size(); ++i) {
    if (!t[i]) continue;
    Note& n = notes[i];
    const Tick end = n.start + n.length;
    // Nearest target among the lines around the start.
    Tick base = n.start / grid;
    Tick best = 0, bestLine = 0;
    bool have = false;
    for (Tick l = std::max<Tick>(0, base - 1); l <= base + 2; ++l) {
      Tick tg = lineTarget(o, l, shift);
      if (!have || std::llabs(tg - n.start) < std::llabs(best - n.start)) best = tg, bestLine = l, have = true;
    }
    Tick newStart = n.start;
    const bool inWindow = o.window >= 1.0 || std::llabs(best - n.start) <= roundTick(o.window * grid * 0.5);
    if (inWindow) {
      newStart = n.start + roundTick((best - n.start) * std::clamp(o.startStrength, 0.0, 1.0));
      if (vel && !vel->empty() && o.grooveVelocity > 0) {
        double f = (*vel)[static_cast<size_t>(bestLine % static_cast<Tick>(vel->size()))];
        n.velocity = std::clamp(static_cast<int>(std::lround(n.velocity * (1.0 + o.grooveVelocity * (f - 1.0)))), 1, 127);
      }
    }
    newStart = std::max<Tick>(0, newStart);
    const double ds = std::clamp(o.durationStrength, 0.0, 1.0);
    switch (o.duration) {
      case DurationMode::LeaveEnd: n.length = std::max<Tick>(1, end - newStart); break;
      case DurationMode::QuantizeLength: {
        Tick ql = std::max<Tick>(grid, roundTick(static_cast<double>(n.length) / grid) * grid);
        n.length = std::max<Tick>(1, n.length + roundTick((ql - n.length) * ds));
        break;
      }
      case DurationMode::QuantizeEnd: {
        Tick el = std::max<Tick>(0, roundTick(static_cast<double>(end) / grid));
        Tick te = lineTarget(o, el, shift);
        if (te <= newStart) te = newStart + grid;
        Tick ne = end + roundTick((te - end) * ds);
        n.length = std::max<Tick>(1, ne - newStart);
        break;
      }
      default: break;
    }
    n.start = newStart;
  }
  sortSel(notes, sel);
}
}  // namespace

Tick quantizeTarget(const QuantizeOpts& o, Tick line) {
  const auto& g = grooves();
  const std::vector<double>* sh = o.groove > 0 && o.groove < static_cast<int>(g.size()) ? &g[static_cast<size_t>(o.groove)].shift : nullptr;
  return lineTarget(o, line, sh);
}

void quantizeEx(std::vector<Note>& notes, NoteSel& sel, const QuantizeOpts& o) {
  const auto& g = grooves();
  const Groove* gr = o.groove > 0 && o.groove < static_cast<int>(g.size()) ? &g[static_cast<size_t>(o.groove)] : nullptr;
  quantizeImpl(notes, sel, o, gr ? &gr->shift : nullptr, gr ? &gr->velocity : nullptr);
}

std::vector<double> extractGroove(const std::vector<Note>& notes, Tick grid, int cycle) {
  grid = std::max<Tick>(1, grid);
  cycle = std::max(1, cycle);
  std::vector<double> sum(static_cast<size_t>(cycle), 0.0);
  std::vector<int> cnt(static_cast<size_t>(cycle), 0);
  for (const auto& n : notes) {
    Tick line = (n.start + grid / 2) / grid;
    double off = static_cast<double>(n.start - line * grid) / grid;
    size_t k = static_cast<size_t>(line % cycle);
    sum[k] += off;
    ++cnt[k];
  }
  for (size_t k = 0; k < sum.size(); ++k) sum[k] = cnt[k] ? std::clamp(sum[k] / cnt[k], -0.5, 0.5) : 0.0;
  return sum;
}

void quantizeWithShifts(std::vector<Note>& notes, NoteSel& sel, QuantizeOpts o, const std::vector<double>& shift) {
  o.groove = 0;
  quantizeImpl(notes, sel, o, &shift, nullptr);
}

// ------------------------------------------------------------------------ chop ----

const std::vector<ChopPattern>& chopPatterns() {
  static const std::vector<ChopPattern> c = {
      {"Even steps", {1}, {}},
      {"Even half steps", {0.5}, {}},
      {"Even beats", {4}, {}},
      {"Dotted", {1.5, 1.5, 1}, {}},
      {"Triplets", {4.0 / 3.0}, {}},
      {"Gallop", {2, 1, 1}, {}},
      {"Reverse gallop", {1, 1, 2}, {}},
      {"Roll up", {2, 1, 0.5, 0.5}, {}},
      {"Stutter", {0.5, 0.5, 1, 2}, {}},
      {"Octave bounce", {1}, {0, 12}},
      {"Fifth climb", {1}, {0, 7, 12, 7}},
      {"Triad walk", {1}, {0, 4, 7, 12}},
      {"Minor walk", {1}, {0, 3, 7, 12}},
  };
  return c;
}

void chopByPattern(std::vector<Note>& notes, NoteSel& sel, const ChopPattern& p, Tick stepTicks, double timeMul, double gate) {
  if (p.lengths.empty() || stepTicks <= 0) return;
  auto t = targets(notes, sel);
  timeMul = std::max(0.01, timeMul);
  gate = std::clamp(gate, 0.05, 1.0);
  std::vector<Note> out;
  NoteSel os;
  for (size_t i = 0; i < notes.size(); ++i) {
    const Note& n = notes[i];
    if (!t[i]) {
      out.push_back(n);
      os.push_back(sel[i]);
      continue;
    }
    double pos = 0;
    size_t k = 0;
    while (pos < static_cast<double>(n.length) - 0.5) {
      double piece = std::max(1.0, p.lengths[k % p.lengths.size()] * timeMul * static_cast<double>(stepTicks));
      Note c = n;
      c.start = n.start + roundTick(pos);
      Tick full = std::min<Tick>(roundTick(pos + piece), n.length) - roundTick(pos);
      c.length = std::max<Tick>(1, roundTick(static_cast<double>(full) * gate));
      if (!p.pitch.empty()) c.pitch = clampPitch(n.pitch + p.pitch[k % p.pitch.size()]);
      out.push_back(c);
      os.push_back(sel[i]);
      pos += piece;
      ++k;
      if (k > 4096) break;
    }
  }
  notes.swap(out);
  sel.swap(os);
  sortSel(notes, sel);
}

void chopEvery(std::vector<Note>& notes, NoteSel& sel, Tick len) {
  if (len <= 0) return;
  ChopPattern p{"", {1.0}, {}};
  chopByPattern(notes, sel, p, len, 1.0, 1.0);
}

// ----------------------------------------------------------------------- strum ----

void strumEx(std::vector<Note>& notes, NoteSel& sel, const StrumOpts& o) {
  sortSel(notes, sel);
  auto t = targets(notes, sel);
  int chordNo = 0;
  for (auto& [start, idx] : chordsOf(notes, t)) {
    if (idx.size() < 2) continue;
    bool down = o.direction == 1 || (o.direction == 2 && chordNo % 2 == 1);
    ++chordNo;
    if (down) std::reverse(idx.begin(), idx.end());
    const double last = static_cast<double>(idx.size() - 1);
    for (size_t k = 0; k < idx.size(); ++k) {
      Note& n = notes[idx[k]];
      const double f = k / last;
      const Tick end = n.start + n.length;
      const Tick off = roundTick(static_cast<double>(o.time) * bend(f, o.timeTension));
      const Tick endOff = roundTick(static_cast<double>(o.time) * std::clamp(o.end, 0.0, 1.0) * bend(f, o.endTension));
      n.start = start + off;
      n.length = std::max<Tick>(1, end + endOff - n.start);
      n.velocity = std::clamp(static_cast<int>(std::lround(n.velocity * (1.0 - std::clamp(o.velocity, 0.0, 1.0) * bend(f, o.velocityTension)))), 1, 127);
    }
  }
  sortSel(notes, sel);
}

// -------------------------------------------------------------------- arpeggio ----

const char* arpPatternName(ArpPattern p) {
  static const char* const n[] = {"Up", "Down", "Up and down", "Down and up", "Converge", "Diverge", "Random", "Chord"};
  int i = static_cast<int>(p);
  return i >= 0 && i < static_cast<int>(ArpPattern::Count) ? n[i] : "";
}

void arpeggiateEx(std::vector<Note>& notes, NoteSel& sel, const ArpOpts& o) {
  if (o.step <= 0) return;
  sortSel(notes, sel);
  auto t = targets(notes, sel);
  const int octaves = std::clamp(o.octaves, 1, 4);
  Rng rng(o.seed);
  std::vector<bool> gone(notes.size(), false);
  std::vector<Note> add;
  NoteSel addSel;
  for (auto& chord : chordsOf(notes, t)) {
    const Tick start = chord.first;
    const std::vector<size_t>& idx = chord.second;
    if (idx.size() < 2 && octaves < 2) continue;
    Tick len = 0;
    for (size_t i : idx) len = std::max(len, notes[i].length), gone[i] = true;
    // Pitch list over the octave range.
    std::vector<int> base;
    for (size_t i : idx) base.push_back(notes[i].pitch);
    std::vector<int> list;
    if (o.rangeMode == 2) {
      for (int p : base)
        for (int oc = 0; oc < octaves; ++oc) list.push_back(p + 12 * oc);
    } else {
      for (int oc = 0; oc < octaves; ++oc) {
        std::vector<int> pass;
        for (int p : base) pass.push_back(p + 12 * oc);
        if (o.rangeMode == 1 && oc % 2 == 1) std::reverse(pass.begin(), pass.end());
        list.insert(list.end(), pass.begin(), pass.end());
      }
    }
    for (int& p : list) p = clampPitch(p);
    std::vector<int> seq;
    const size_t n = list.size();
    switch (o.pattern) {
      case ArpPattern::Down: seq.assign(list.rbegin(), list.rend()); break;
      case ArpPattern::UpDown:
        seq = list;
        for (size_t k = n >= 2 ? n - 2 : 0; k >= 1 && k < n; --k) seq.push_back(list[k]);
        break;
      case ArpPattern::DownUp:
        seq.assign(list.rbegin(), list.rend());
        for (size_t k = 1; k + 1 < n; ++k) seq.push_back(list[k]);
        break;
      case ArpPattern::Converge:
        for (size_t lo = 0, hi = n - 1; lo <= hi && hi < n; ++lo, --hi) {
          seq.push_back(list[lo]);
          if (hi != lo) seq.push_back(list[hi]);
          if (hi == 0) break;
        }
        break;
      case ArpPattern::Diverge: {
        std::vector<int> conv;
        for (size_t lo = 0, hi = n - 1; lo <= hi && hi < n; ++lo, --hi) {
          conv.push_back(list[lo]);
          if (hi != lo) conv.push_back(list[hi]);
          if (hi == 0) break;
        }
        seq.assign(conv.rbegin(), conv.rend());
        break;
      }
      default: seq = list; break;
    }
    if (seq.empty()) seq = list;
    const Note& proto = notes[idx[0]];
    const Tick noteLen = std::max<Tick>(1, roundTick(static_cast<double>(o.step) * std::clamp(o.gate, 0.05, 1.0)));
    int j = 0;
    for (Tick at = 0; at < len; at += o.step, ++j) {
      const Tick pos = start + at;
      const Tick k = o.sync == 0 ? pos / o.step : j;
      auto emit = [&](int pitch) {
        Note c = proto;
        // Velocity of the chord note this pitch comes from.
        for (size_t i : idx)
          if ((pitch - notes[i].pitch) % 12 == 0) c.velocity = notes[i].velocity;
        c.start = pos;
        c.length = std::min(noteLen, len - at);
        c.pitch = pitch;
        if (k % 4 != 0) c.velocity = std::clamp(static_cast<int>(std::lround(c.velocity * (1.0 - 0.45 * std::clamp(o.velocityAccent, 0.0, 1.0)))), 1, 127);
        add.push_back(c);
        addSel.push_back(sel[idx[0]]);
      };
      if (o.pattern == ArpPattern::Chord) {
        for (int p : list) emit(p);
      } else if (o.pattern == ArpPattern::Random) {
        emit(list[rng.next() % n]);
      } else {
        emit(seq[static_cast<size_t>(k % static_cast<Tick>(seq.size()))]);
      }
    }
  }
  std::vector<Note> out;
  NoteSel os;
  for (size_t i = 0; i < notes.size(); ++i)
    if (!gone[i]) out.push_back(notes[i]), os.push_back(sel[i]);
  out.insert(out.end(), add.begin(), add.end());
  os.insert(os.end(), addSel.begin(), addSel.end());
  notes.swap(out);
  sel.swap(os);
  sortSel(notes, sel);
}

// ------------------------------------------------------------------------ flam ----

const std::vector<FlamPreset>& flamPresets() {
  static const std::vector<FlamPreset> p = {
      {"Single grace note", {4, 0.55, 1, true}},
      {"Single grace, on the beat", {4, 0.55, 1, false}},
      {"Double grace notes", {3, 0.5, 2, true}},
      {"Double grace, on the beat", {3, 0.5, 2, false}},
  };
  return p;
}

FlamOpts flamPresetOpts(int index, int ppq) {
  const auto& p = flamPresets();
  FlamOpts o = p[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(p.size()) - 1))].opts;
  o.time = std::max<Tick>(1, o.time * ppq / 48);
  return o;
}

void flam(std::vector<Note>& notes, NoteSel& sel, const FlamOpts& o) {
  if (o.time <= 0 || o.count < 1) return;
  auto t = targets(notes, sel);
  std::vector<Note> out;
  NoteSel os;
  for (size_t i = 0; i < notes.size(); ++i) {
    Note n = notes[i];
    if (!t[i]) {
      out.push_back(n);
      os.push_back(sel[i]);
      continue;
    }
    const int vel = std::clamp(static_cast<int>(std::lround(n.velocity * std::clamp(o.velocity, 0.0, 1.0))), 1, 127);
    if (o.early) {
      for (int g = o.count; g >= 1; --g) {
        Tick s = n.start - o.time * g;
        if (s < 0) continue;
        Note gn = n;
        gn.start = s;
        gn.length = o.time;
        gn.velocity = vel;
        out.push_back(gn);
        os.push_back(false);
      }
    } else {
      for (int g = 0; g < o.count; ++g) {
        Note gn = n;
        gn.start = n.start + o.time * g;
        gn.length = o.time;
        gn.velocity = vel;
        out.push_back(gn);
        os.push_back(false);
      }
      Tick shift = o.time * o.count;
      n.start += shift;
      n.length = std::max<Tick>(1, n.length - shift);
    }
    out.push_back(n);
    os.push_back(sel[i]);
  }
  notes.swap(out);
  sel.swap(os);
  sortSel(notes, sel);
}

// ------------------------------------------------------------------ articulate ----

const std::vector<ArticulatePreset>& articulatePresets() {
  static const std::vector<ArticulatePreset> p = {
      {"Connected", {1.0, 0, 0, false, 1}},
      {"Slightly detached", {0.8, 0, 0, false, 1}},
      {"Detached", {0.5, 0, 0, false, 1}},
      {"Short", {0.25, 0, 0, false, 1}},
      {"Connected with a small gap", {1.0, 2, 0, false, 1}},
      {"Trim overlaps only", {1.0, 0, 0, true, 1}},
  };
  return p;
}

ArticulateOpts articulatePresetOpts(int index, int ppq) {
  const auto& p = articulatePresets();
  ArticulateOpts o = p[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(p.size()) - 1))].opts;
  o.gap = o.gap * ppq / 48;
  return o;
}

void articulate(std::vector<Note>& notes, NoteSel& sel, const ArticulateOpts& o) {
  sortSel(notes, sel);
  auto t = targets(notes, sel);
  Rng rng(o.seed);
  for (size_t i = 0; i < notes.size(); ++i) {
    if (!t[i]) continue;
    Note& n = notes[i];
    Tick next = -1;
    for (size_t j = i + 1; j < notes.size(); ++j)
      if (t[j] && notes[j].start > n.start) {
        next = notes[j].start;
        break;
      }
    if (o.onlyOverlaps) {
      if (next >= 0 && n.start + n.length > next - o.gap) n.length = std::max<Tick>(1, next - o.gap - n.start);
      continue;
    }
    const double avail = next >= 0 ? static_cast<double>(next - n.start) : static_cast<double>(n.length);
    double len = avail * std::clamp(o.length, 0.01, 4.0) - static_cast<double>(o.gap);
    if (o.variation > 0) len *= 1.0 + std::clamp(o.variation, 0.0, 1.0) * 0.5 * rng.bipolar();
    n.length = std::max<Tick>(1, roundTick(len));
  }
}

// -------------------------------------------------------------------- randomize ----

std::vector<Note> randomNotes(const RandomOpts& o) {
  std::vector<Note> out;
  if (o.step <= 0 || o.span <= 0) return out;
  Rng rng(o.seed);
  const auto& sc = scales();
  const auto& steps = sc[static_cast<size_t>(std::clamp(o.scale, 0, static_cast<int>(sc.size()) - 1))].steps;
  const int base = 12 * (std::clamp(o.octave, 0, 9) + 1) + ((o.root % 12) + 12) % 12;
  std::vector<int> pool;
  for (int oc = 0; oc < std::clamp(o.octaves, 1, 6); ++oc)
    for (int s : steps)
      if (base + s + 12 * oc <= 127) pool.push_back(base + s + 12 * oc);
  if (pool.empty()) pool.push_back(clampPitch(base));
  for (Tick at = 0; at < o.span; at += o.step) {
    if (rng.unit() >= std::clamp(o.probability, 0.0, 1.0)) continue;
    int count = rng.range(1, std::max(1, o.maxPoly));
    std::set<int> used;
    for (int k = 0; k < count; ++k) {
      int p = pool[rng.next() % pool.size()];
      if (!used.insert(p).second) continue;
      double len = std::max(0.05, o.length) * (1.0 + std::clamp(o.lengthVariation, 0.0, 1.0) * rng.bipolar() * 0.9);
      Note n;
      n.start = o.from + at;
      n.length = std::max<Tick>(1, std::min<Tick>(roundTick(len * o.step), o.span - at));
      n.pitch = p;
      n.velocity = std::clamp(o.velocity + rng.range(-o.velocityRange, o.velocityRange), 1, 127);
      out.push_back(n);
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  return out;
}

void randomizeLevels(std::vector<Note>& notes, NoteSel& sel, const LevelRandom& o) {
  auto t = targets(notes, sel);
  Rng rng(o.seed);
  for (size_t i = 0; i < notes.size(); ++i) {
    if (!t[i]) continue;
    for (int k = 0; k < static_cast<int>(NoteProp::Count); ++k) {
      const double s = std::clamp(o.strength[k], 0.0, 1.0);
      const double r = rng.bipolar();  // drawn for every property so results do not depend on other strengths
      if (s <= 0) continue;
      NoteProp p = static_cast<NoteProp>(k);
      setNotePropNorm(notes[i], p, notePropNorm(notes[i], p) + r * s * 0.5);
    }
  }
}

// ------------------------------------------------------------------- note LFO ----

void noteLfo(std::vector<Note>& notes, NoteSel& sel, const NoteLfoOpts& o) {
  auto t = targets(notes, sel);
  Tick lo = -1, hi = 0;
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) lo = lo < 0 ? notes[i].start : std::min(lo, notes[i].start), hi = std::max(hi, notes[i].start);
  if (lo < 0) return;
  const double span = static_cast<double>(std::max<Tick>(1, hi - lo));
  for (size_t i = 0; i < notes.size(); ++i) {
    if (!t[i]) continue;
    const double x = static_cast<double>(notes[i].start - lo);
    const double f = hi > lo ? x / span : 0.0;
    const double centre = o.startValue + (o.endValue - o.startValue) * f;
    const double range = o.startRange + (o.endRange - o.startRange) * f;
    const double p0 = static_cast<double>(std::max<Tick>(1, o.startPeriod));
    const double p1 = static_cast<double>(std::max<Tick>(1, o.endPeriod));
    // Phase: integral of 1/period with the period moving linearly over the span.
    double phase;
    if (std::fabs(p1 - p0) < 1e-9 || hi == lo) {
      phase = x / p0;
    } else {
      const double k = (p1 - p0) / span;
      phase = std::log((p0 + k * x) / p0) / k;
    }
    const double fr = phase - std::floor(phase);
    double w;
    switch (o.wave) {
      case LfoWave::Triangle: w = 1.0 - 4.0 * std::fabs(fr - 0.5); w = -w; break;
      case LfoWave::Square: w = fr < std::clamp(o.gate, 0.01, 0.99) ? 1.0 : -1.0; break;
      default: w = std::sin(2.0 * kPi * fr); break;
    }
    setNotePropNorm(notes[i], o.prop, centre + 0.5 * range * w);
  }
}

// --------------------------------------------------------------- scale levels ----

void scaleLevels(std::vector<Note>& notes, NoteSel& sel, const ScaleLevelOpts& o) {
  auto t = targets(notes, sel);
  Tick lo = -1, hi = 0;
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) lo = lo < 0 ? notes[i].start : std::min(lo, notes[i].start), hi = std::max(hi, notes[i].start);
  if (lo < 0) return;
  for (size_t i = 0; i < notes.size(); ++i) {
    if (!t[i]) continue;
    const double f = hi > lo ? static_cast<double>(notes[i].start - lo) / static_cast<double>(hi - lo) : 0.0;
    const double ramp = o.rampFrom + (o.rampTo - o.rampFrom) * f;
    const double v = notePropNorm(notes[i], o.prop);
    setNotePropNorm(notes[i], o.prop, o.centre + (v - o.centre) * o.scale * ramp + o.offset);
  }
}

// ------------------------------------------------------------------------ limit ----

const char* limitModeName(LimitMode m) {
  static const char* const n[] = {"Fold by octaves", "Clamp", "Remove"};
  int i = static_cast<int>(m);
  return i >= 0 && i < static_cast<int>(LimitMode::Count) ? n[i] : "";
}

void limitKeys(std::vector<Note>& notes, NoteSel& sel, int lo, int hi, LimitMode mode) {
  lo = clampPitch(lo);
  hi = clampPitch(hi);
  if (lo > hi) std::swap(lo, hi);
  auto t = targets(notes, sel);
  std::vector<Note> out;
  NoteSel os;
  for (size_t i = 0; i < notes.size(); ++i) {
    Note n = notes[i];
    if (t[i] && (n.pitch < lo || n.pitch > hi)) {
      if (mode == LimitMode::Remove) continue;
      if (mode == LimitMode::Fold) {
        while (n.pitch < lo && n.pitch + 12 <= 127) n.pitch += 12;
        while (n.pitch > hi && n.pitch - 12 >= 0) n.pitch -= 12;
      }
      n.pitch = std::clamp(n.pitch, lo, hi);
    }
    out.push_back(n);
    os.push_back(sel[i]);
  }
  notes.swap(out);
  sel.swap(os);
  sortSel(notes, sel);
}

// -------------------------------------------------------------- riff generator ----

const char* riffRhythmName(int r) {
  static const char* const n[] = {"Even", "Syncopated", "Long notes", "Driving"};
  return n[std::clamp(r, 0, 3)];
}

std::vector<Note> generateRiff(const RiffOpts& o) {
  std::vector<Note> out;
  if (o.step <= 0 || o.barTicks < o.step) return out;
  Rng rng(o.seed);
  const auto& sc = scales();
  const auto& st = sc[static_cast<size_t>(std::clamp(o.scale, 0, static_cast<int>(sc.size()) - 1))].steps;
  const int L = static_cast<int>(st.size());
  const int base = 12 * (std::clamp(o.octave, 0, 9) + 1) + ((o.root % 12) + 12) % 12;
  auto pitchOf = [&](int d) {
    int oct = d >= 0 ? d / L : -((-d + L - 1) / L);
    int k = d - oct * L;
    return clampPitch(base + st[static_cast<size_t>(k)] + 12 * oct);
  };
  const int steps = static_cast<int>(o.barTicks / o.step);
  // Onset weight per step position (16th grid assumed for the shapes; other grids scale).
  auto weight = [&](int s) {
    const bool beat = s % 4 == 0, half = s % 2 == 0;
    switch (o.rhythm) {
      case 1: return beat ? 0.55 : (s % 4 == 3 || s % 4 == 2) ? 1.0 : 0.45;  // syncopated: before the beat
      case 2: return beat ? 1.0 : half ? 0.25 : 0.05;                       // long notes on beats
      case 3: return half ? 1.0 : 0.8;                                      // driving
      default: return beat ? 1.0 : half ? 0.8 : 0.3;                       // even
    }
  };
  struct Ev { int step, len, degree; };
  std::vector<Ev> motif;
  int deg = 0;
  const int range = std::max(1, o.range);
  auto walk = [&](int d) {
    int move;
    if (rng.unit() < std::clamp(o.leap, 0.0, 1.0))
      move = rng.range(2, 4) * (rng.unit() < 0.5 ? -1 : 1);
    else
      move = rng.range(-1, 1);
    d += move;
    if (d > range) d = range - (d - range);
    if (d < -range) d = -range + (-range - d);
    return std::clamp(d, -range, range);
  };
  std::vector<int> onsets;
  for (int s = 0; s < steps; ++s)
    if (s == 0 || rng.unit() < std::clamp(o.density, 0.0, 1.0) * weight(s)) onsets.push_back(s);
  for (size_t k = 0; k < onsets.size(); ++k) {
    int next = k + 1 < onsets.size() ? onsets[k + 1] : steps;
    int len = next - onsets[k];
    if (o.rhythm != 2) len = std::min(len, o.rhythm == 3 ? 1 : 2);
    if (k > 0) deg = walk(deg);
    motif.push_back({onsets[k], std::max(1, len), deg});
  }
  const int bars = std::clamp(o.bars, 1, 64);
  for (int b = 0; b < bars; ++b) {
    int prev = 0;
    for (size_t k = 0; k < motif.size(); ++k) {
      Ev e = motif[k];
      if (b > 0 && rng.unit() >= std::clamp(o.repeat, 0.0, 1.0)) e.degree = walk(k ? prev : e.degree);
      if (b == bars - 1 && k + 1 == motif.size() && bars > 1) e.degree = (e.degree >= L / 2) ? L : 0;  // resolve home
      prev = e.degree;
      Note n;
      n.start = o.barTicks * b + o.step * e.step;
      n.length = o.step * e.len;
      n.pitch = pitchOf(e.degree);
      n.velocity = std::clamp(e.step % 4 == 0 ? o.velocity : o.velocity - 16, 1, 127);
      out.push_back(n);
      if (o.chords && k == 0)
        for (int add : {2, 4}) {
          Note c = n;
          c.pitch = pitchOf(e.degree - L + add);  // triad below the melody note
          c.length = o.barTicks;
          c.velocity = std::max(1, o.velocity - 24);
          out.push_back(c);
        }
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  return out;
}

// ------------------------------------------------------------- misc note tools ----

void setNotesProp(std::vector<Note>& notes, NoteSel& sel, NoteProp p, double norm) {
  auto t = targets(notes, sel);
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) setNotePropNorm(notes[i], p, norm);
}

void setNotesColor(std::vector<Note>& notes, NoteSel& sel, int color) {
  auto t = targets(notes, sel);
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) notes[i].color = std::clamp(color, 0, kNoteColors - 1);
}

namespace {
void setFlag(std::vector<Note>& notes, NoteSel& sel, bool Note::*field, int mode) {
  auto t = targets(notes, sel);
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) notes[i].*field = mode == 2 ? !(notes[i].*field) : mode == 1;
}
}  // namespace

void setNotesMuted(std::vector<Note>& notes, NoteSel& sel, int mode) { setFlag(notes, sel, &Note::muted, mode); }
void setNotesSlide(std::vector<Note>& notes, NoteSel& sel, int mode) { setFlag(notes, sel, &Note::slide, mode); }
void setNotesPorta(std::vector<Note>& notes, NoteSel& sel, int mode) { setFlag(notes, sel, &Note::porta, mode); }

void selectColor(const std::vector<Note>& notes, NoteSel& sel, int color) {
  sel.assign(notes.size(), false);
  for (size_t i = 0; i < notes.size(); ++i) sel[i] = notes[i].color == color;
}

void selectMuted(const std::vector<Note>& notes, NoteSel& sel) {
  sel.assign(notes.size(), false);
  for (size_t i = 0; i < notes.size(); ++i) sel[i] = notes[i].muted;
}

void detune(std::vector<Note>& notes, NoteSel& sel, int cents) {
  auto t = targets(notes, sel);
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) notes[i].fine = std::clamp(notes[i].fine + cents, -kNoteFineMax, kNoteFineMax);
}

std::vector<size_t> notesAt(const std::vector<Note>& notes, Tick t) {
  std::vector<size_t> out;
  for (size_t i = 0; i < notes.size(); ++i)
    if (notes[i].start <= t && t < notes[i].start + notes[i].length) out.push_back(i);
  return out;
}

// ======================================================= step sequencer rows ====

void alignStepProps(const StepRow& row, StepPropRow& props) { props.resize(row.size()); }

void fillEvery(StepRow& row, int every, int offset, uint8_t velocity) {
  every = std::max(1, every);
  for (size_t k = 0; k < row.size(); ++k) {
    int i = static_cast<int>(k) - offset;
    row[k] = i >= 0 && i % every == 0 ? velocity : 0;
  }
}

void rotateSteps(StepRow& row, StepPropRow& props, int n) {
  alignStepProps(row, props);
  if (row.empty()) return;
  const int sz = static_cast<int>(row.size());
  n = ((n % sz) + sz) % sz;
  if (n == 0) return;
  std::rotate(row.begin(), row.begin() + (sz - n), row.end());
  std::rotate(props.begin(), props.begin() + (sz - n), props.end());
}

void shiftSteps(StepRow& row, StepPropRow& props, int n) {
  alignStepProps(row, props);
  const int sz = static_cast<int>(row.size());
  StepRow r(row.size(), 0);
  StepPropRow p(row.size());
  for (int k = 0; k < sz; ++k) {
    int d = k + n;
    if (d < 0 || d >= sz) continue;
    r[static_cast<size_t>(d)] = row[static_cast<size_t>(k)];
    p[static_cast<size_t>(d)] = props[static_cast<size_t>(k)];
  }
  row.swap(r);
  props.swap(p);
}

void reverseSteps(StepRow& row, StepPropRow& props) {
  alignStepProps(row, props);
  std::reverse(row.begin(), row.end());
  std::reverse(props.begin(), props.end());
}

void invertSteps(StepRow& row, uint8_t velocity) {
  for (auto& v : row) v = v ? 0 : velocity;
}

void insertStep(StepRow& row, StepPropRow& props, int at) {
  alignStepProps(row, props);
  if (at < 0 || at >= static_cast<int>(row.size())) return;
  row.insert(row.begin() + at, 0);
  row.pop_back();
  props.insert(props.begin() + at, StepProps{});
  props.pop_back();
}

void deleteStep(StepRow& row, StepPropRow& props, int at) {
  alignStepProps(row, props);
  if (at < 0 || at >= static_cast<int>(row.size())) return;
  row.erase(row.begin() + at);
  row.push_back(0);
  props.erase(props.begin() + at);
  props.push_back(StepProps{});
}

void randomizeSteps(StepRow& row, double probability, int velLo, int velHi, uint32_t seed) {
  Rng rng(seed);
  velLo = std::clamp(velLo, 1, 127);
  velHi = std::clamp(velHi, velLo, 127);
  for (auto& v : row) {
    const bool on = rng.unit() < std::clamp(probability, 0.0, 1.0);
    const int vel = rng.range(velLo, velHi);
    v = on ? static_cast<uint8_t>(vel) : 0;
  }
}

void euclidSteps(StepRow& row, int hits, int length, int rotation, uint8_t velocity) {
  length = std::max(1, length);
  hits = std::clamp(hits, 0, length);
  for (size_t k = 0; k < row.size(); ++k) {
    int i = static_cast<int>(k % static_cast<size_t>(length));
    int j = ((i - rotation) % length + length) % length;
    // Bresenham-style spread: a hit wherever the running count crosses an integer.
    bool on = hits > 0 && (j * hits) % length < hits;
    row[k] = on ? velocity : 0;
  }
}

const char* stepLaneName(StepLane l) {
  static const char* const n[] = {"Velocity", "Pan", "Release", "Fine pitch", "Mod X", "Mod Y", "Shift", "Note"};
  int i = static_cast<int>(l);
  return i >= 0 && i < static_cast<int>(StepLane::Count) ? n[i] : "";
}

double stepLaneNorm(uint8_t vel, const StepProps& p, StepLane l) {
  switch (l) {
    case StepLane::Pan: return 0.5 + 0.5 * p.pan / kNotePanMax;
    case StepLane::Release: return p.release / 127.0;
    case StepLane::Fine: return 0.5 + 0.5 * p.fine / kNoteFineMax;
    case StepLane::ModX: return static_cast<double>(p.modX) / kNoteModMax;
    case StepLane::ModY: return static_cast<double>(p.modY) / kNoteModMax;
    case StepLane::Shift: return static_cast<double>(p.shift) / kStepShiftMax;
    case StepLane::Pitch: return (p.pitch < 0 ? 60 : p.pitch) / 127.0;
    default: return vel / 127.0;
  }
}

void setStepLaneNorm(uint8_t& vel, StepProps& p, StepLane l, double v) {
  v = std::clamp(v, 0.0, 1.0);
  switch (l) {
    case StepLane::Pan: p.pan = static_cast<int>(std::lround((v * 2 - 1) * kNotePanMax)); break;
    case StepLane::Release: p.release = static_cast<int>(std::lround(v * 127)); break;
    case StepLane::Fine: p.fine = static_cast<int>(std::lround((v * 2 - 1) * kNoteFineMax / 10.0)) * 10; break;
    case StepLane::ModX: p.modX = static_cast<int>(std::lround(v * kNoteModMax)); break;
    case StepLane::ModY: p.modY = static_cast<int>(std::lround(v * kNoteModMax)); break;
    case StepLane::Shift: p.shift = static_cast<int>(std::lround(v * kStepShiftMax)); break;
    case StepLane::Pitch: {
      int pitch = static_cast<int>(std::lround(v * 127));
      p.pitch = pitch == 60 ? -1 : pitch;
      break;
    }
    default: vel = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v * 127)), 1, 127)); break;
  }
}

std::vector<Note> stepsToNotes(const StepRow& row, const StepPropRow& props, Tick stepTicks, int defaultPitch) {
  std::vector<Note> out;
  if (stepTicks <= 0) return out;
  for (size_t k = 0; k < row.size(); ++k) {
    if (!row[k]) continue;
    const StepProps sp = k < props.size() ? props[k] : StepProps{};
    Note n;
    n.start = static_cast<Tick>(k) * stepTicks + stepTicks * sp.shift / kStepShiftMax;
    n.length = stepTicks;
    n.pitch = sp.pitch < 0 ? defaultPitch : sp.pitch;
    n.velocity = row[k];
    n.pan = sp.pan;
    n.release = sp.release;
    n.fine = sp.fine;
    n.modX = sp.modX;
    n.modY = sp.modY;
    out.push_back(n);
  }
  return out;
}

void notesToSteps(const std::vector<Note>& notes, Tick stepTicks, int total, StepRow& row, StepPropRow& props,
                  std::vector<Note>* rest, int defaultPitch) {
  row.assign(static_cast<size_t>(std::max(0, total)), 0);
  props.assign(row.size(), StepProps{});
  if (rest) rest->clear();
  for (const auto& n : notes) {
    bool placed = false;
    if (stepTicks > 0 && !n.muted && !n.slide) {
      const Tick k = n.start / stepTicks;
      const Tick off = n.start - k * stepTicks;
      const int shift = static_cast<int>(std::lround(static_cast<double>(off) * kStepShiftMax / static_cast<double>(stepTicks)));
      if (k < total && shift < kStepShiftMax && stepTicks * shift / kStepShiftMax == off && !row[static_cast<size_t>(k)]) {
        row[static_cast<size_t>(k)] = static_cast<uint8_t>(std::clamp(n.velocity, 1, 127));
        StepProps& sp = props[static_cast<size_t>(k)];
        sp.pitch = n.pitch == defaultPitch ? -1 : n.pitch;
        sp.pan = n.pan;
        sp.release = n.release;
        sp.fine = n.fine;
        sp.modX = n.modX;
        sp.modY = n.modY;
        sp.shift = shift;
        placed = true;
      }
    }
    if (!placed && rest) rest->push_back(n);
  }
}

}  // namespace gl::tools

// ========================================================= song-level edits ====
namespace gl::ops {

namespace {
bool allDefault(const tools::StepPropRow& p) {
  return std::all_of(p.begin(), p.end(), [](const StepProps& x) { return x.isDefault(); });
}
void storeRow(Pattern& p, ObjectId ch, const tools::StepRow& row, const tools::StepPropRow& props) {
  if (std::all_of(row.begin(), row.end(), [](uint8_t v) { return v == 0; }))
    p.steps.erase(ch);
  else
    p.steps[ch] = row;
  if (allDefault(props))
    p.stepProps.erase(ch);
  else
    p.stepProps[ch] = props;
}
}  // namespace

void readStepRow(const Pattern& p, ObjectId ch, tools::StepRow& row, tools::StepPropRow& props) {
  row.clear();
  props.clear();
  if (auto it = p.steps.find(ch); it != p.steps.end()) row = it->second;
  if (auto it = p.stepProps.find(ch); it != p.stepProps.end()) props = it->second;
  row.resize(static_cast<size_t>(std::max(0, p.bars * p.stepsPerBar)), 0);
  props.resize(row.size());
}

namespace {
void loadRow(const Pattern& p, ObjectId ch, tools::StepRow& row, tools::StepPropRow& props) { readStepRow(p, ch, row, props); }
void sortByStart(std::vector<Note>& v) {
  std::stable_sort(v.begin(), v.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
}
}  // namespace

bool editStepRow(Document& d, ObjectId pattern, ObjectId channel, const std::string& label,
                 const std::function<void(tools::StepRow&, tools::StepPropRow&)>& fn, uint64_t mergeKey) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p || !d.song().channel(channel)) return false;
  tools::StepRow row;
  tools::StepPropRow props;
  loadRow(*p, channel, row, props);
  fn(row, props);
  tools::alignStepProps(row, props);
  return d.editPattern(pattern, label, [&](Pattern& q) { storeRow(q, channel, row, props); }, mergeKey);
}

bool convertStepsToNotes(Document& d, ObjectId pattern, ObjectId channel) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p || !d.song().channel(channel) || !p->steps.count(channel)) return false;
  const Tick tps = d.song().ticksPerStep(*p);
  return d.editPattern(pattern, "steps to piano roll", [&](Pattern& q) {
    tools::StepRow row;
    tools::StepPropRow props;
    loadRow(q, channel, row, props);
    auto add = tools::stepsToNotes(row, props, tps);
    auto& ns = q.notes[channel];
    ns.insert(ns.end(), add.begin(), add.end());
    sortByStart(ns);
    q.steps.erase(channel);
    q.stepProps.erase(channel);
  });
}

bool convertNotesToSteps(Document& d, ObjectId pattern, ObjectId channel) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p || !d.song().channel(channel)) return false;
  auto it = p->notes.find(channel);
  if (it == p->notes.end() || it->second.empty()) return false;
  const Tick tps = d.song().ticksPerStep(*p);
  const int total = p->bars * p->stepsPerBar;
  return d.editPattern(pattern, "piano roll to steps", [&](Pattern& q) {
    tools::StepRow row;
    tools::StepPropRow props;
    std::vector<Note> rest;
    tools::notesToSteps(q.notes[channel], tps, total, row, props, &rest);
    // Existing steps stay where no note landed.
    tools::StepRow oldRow;
    tools::StepPropRow oldProps;
    loadRow(q, channel, oldRow, oldProps);
    for (size_t k = 0; k < row.size() && k < oldRow.size(); ++k)
      if (!row[k] && oldRow[k]) row[k] = oldRow[k], props[k] = oldProps[k];
    storeRow(q, channel, row, props);
    if (rest.empty())
      q.notes.erase(channel);
    else
      q.notes[channel] = rest;
  });
}

ObjectId channelsToNewPattern(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, bool move) {
  const Song& s = d.song();
  const Pattern* p = s.pattern(pattern);
  if (!p || channels.empty() || static_cast<int>(s.patterns.size()) >= kMaxPatterns) return kNoId;
  const Pattern src = *p;
  d.beginGroup(move ? "move channels to a new pattern" : "copy channels to a new pattern");
  ObjectId np = d.addPattern(nextPatternName(s), src.bars, src.stepsPerBar, s.patternIndex(pattern) + 1);
  d.editPattern(np, "copy channel data", [&](Pattern& q) {
    q.color = src.color;
    for (ObjectId ch : channels) {
      if (auto it = src.steps.find(ch); it != src.steps.end()) q.steps[ch] = it->second;
      if (auto it = src.stepProps.find(ch); it != src.stepProps.end()) q.stepProps[ch] = it->second;
      if (auto it = src.notes.find(ch); it != src.notes.end()) q.notes[ch] = it->second;
    }
  });
  if (move)
    d.editPattern(pattern, "move channel data", [&](Pattern& q) {
      for (ObjectId ch : channels) q.steps.erase(ch), q.stepProps.erase(ch), q.notes.erase(ch);
    });
  d.endGroup();
  return np;
}

bool renameChannelGroup(Document& d, int group, const std::string& name) {
  if (group < 0 || group >= static_cast<int>(d.song().channelGroups.size()) || name.empty()) return false;
  d.editSong("rename channel group", [&](Song& x) { x.channelGroups[static_cast<size_t>(group)] = name; });
  return true;
}

bool sendNotesToChannel(Document& d, ObjectId pattern, ObjectId from, ObjectId to, const std::vector<Note>& notes, bool move,
                        const std::vector<Note>& keep) {
  if (!d.song().pattern(pattern) || !d.song().channel(to) || from == to || notes.empty()) return false;
  return d.editPattern(pattern, move ? "move notes to channel" : "copy notes to channel", [&](Pattern& q) {
    auto& dst = q.notes[to];
    dst.insert(dst.end(), notes.begin(), notes.end());
    sortByStart(dst);
    if (move) {
      if (keep.empty())
        q.notes.erase(from);
      else
        q.notes[from] = keep;
    }
  });
}

// --------------------------------------------------------------------- playlist ----

ObjectId sliceClip(Document& d, ObjectId clip, Tick at) {
  const Song& s = d.song();
  const Clip* c = s.clip(clip);
  if (!c || c->kind == ClipKind::Audio) return kNoId;  // audio clip slicing belongs to the audio clip tools
  const Tick len = s.clipLength(*c);
  if (at <= c->start || at >= c->start + len) return kNoId;
  ObjectId out = kNoId;
  d.editSong("slice clip", [&](Song& x) {
    Clip* k = x.clip(clip);
    Clip right = *k;
    right.id = x.allocateId();
    right.start = at;
    right.length = k->start + len - at;
    right.offset = k->offset + (at - k->start);
    k->length = at - k->start;
    x.playlist.push_back(right);
    out = right.id;
  });
  return out;
}

std::vector<ObjectId> sliceAt(Document& d, Tick at, int track) {
  std::vector<ObjectId> hits;
  for (const auto& c : d.song().playlist)
    if ((track < 0 || c.track == track) && c.kind != ClipKind::Audio && at > c.start && at < c.start + d.song().clipLength(c))
      hits.push_back(c.id);
  std::vector<ObjectId> out;
  if (hits.empty()) return out;
  d.beginGroup("slice clips");
  for (ObjectId id : hits)
    if (ObjectId r = sliceClip(d, id, at)) out.push_back(r);
  d.endGroup();
  return out;
}

bool slipClip(Document& d, ObjectId clip, Tick delta, uint64_t mergeKey) {
  const Clip* c = d.song().clip(clip);
  if (!c || c->kind == ClipKind::Audio || delta == 0) return false;
  return d.editClip(clip, "slip clip content", [&](Clip& k) { k.offset = std::max<Tick>(0, k.offset + delta); }, mergeKey);
}

bool muteClips(Document& d, const std::set<ObjectId>& clips, int mode) {
  if (clips.empty()) return false;
  return d.editClips(mode == 0 ? "unmute clips" : mode == 1 ? "mute clips" : "toggle clip mute", [&](std::vector<Clip>& cs) {
    for (auto& c : cs)
      if (clips.count(c.id)) c.muted = mode == 2 ? !c.muted : mode == 1;
  });
}

bool muteTrackClips(Document& d, int track, bool muted) {
  std::set<ObjectId> ids;
  for (const auto& c : d.song().playlist)
    if (c.track == track) ids.insert(c.id);
  return muteClips(d, ids, muted ? 1 : 0);
}

ObjectId mergeTrackPatternClips(Document& d, int track) {
  std::set<ObjectId> ids;
  for (const auto& c : d.song().playlist)
    if (c.track == track && c.kind == ClipKind::Pattern) ids.insert(c.id);
  if (ids.size() < 2) return kNoId;
  return mergeClips(d, ids);
}

std::vector<ObjectId> chopClips(Document& d, const std::set<ObjectId>& clips, Tick every) {
  std::vector<ObjectId> out;
  if (every <= 0) return out;
  std::vector<std::pair<ObjectId, std::vector<Tick>>> cuts;
  for (const auto& c : d.song().playlist) {
    if (!clips.count(c.id) || c.kind == ClipKind::Audio) continue;
    std::vector<Tick> at;
    const Tick len = d.song().clipLength(c);
    for (Tick t = every; t < len; t += every) at.push_back(c.start + t);
    if (!at.empty()) cuts.push_back({c.id, at});
  }
  if (cuts.empty()) return out;
  d.beginGroup("chop clips");
  for (auto& [id, at] : cuts) {
    ObjectId cur = id;
    out.push_back(id);
    for (Tick t : at) {
      cur = sliceClip(d, cur, t);
      if (!cur) break;
      out.push_back(cur);
    }
  }
  d.endGroup();
  return out;
}

std::vector<ObjectId> splitClipByChannel(Document& d, ObjectId clip) {
  const Clip* c = d.song().clip(clip);
  if (!c || c->kind != ClipKind::Pattern) return {};
  d.beginGroup("split clip by channel");
  // A pattern used by other clips gets its own copy so only this clip changes.
  int users = 0;
  for (const auto& k : d.song().playlist) users += k.kind == ClipKind::Pattern && k.pattern == c->pattern;
  ObjectId pat = c->pattern;
  if (users > 1) pat = makeClipUnique(d, clip);
  std::vector<ObjectId> out = pat ? splitPatternByChannel(d, pat) : std::vector<ObjectId>{};
  d.endGroup();
  return out;
}

std::set<ObjectId> clipsOfSameSource(const Song& s, ObjectId clip) {
  std::set<ObjectId> out;
  const Clip* c = s.clip(clip);
  if (!c) return out;
  for (const auto& k : s.playlist) {
    if (k.kind != c->kind) continue;
    bool same = c->kind == ClipKind::Pattern ? k.pattern == c->pattern : k.source == c->source;
    if (same) out.insert(k.id);
  }
  return out;
}

bool setTrackLocked(Document& d, int track, bool locked) {
  return d.editPlaylistTrack(track, locked ? "lock playlist track" : "unlock playlist track", [&](PlaylistTrack& t) { t.locked = locked; });
}

bool setTrackHeights(Document& d, const std::vector<int>& tracks, float height) {
  if (tracks.empty()) return false;
  const int n = static_cast<int>(d.song().tracks.size());
  d.editSong("resize playlist tracks", [&](Song& x) {
    for (int t : tracks)
      if (t >= 0 && t < n) x.tracks[static_cast<size_t>(t)].height = std::clamp(height, 0.25f, 4.0f);
  });
  return true;
}

namespace {
void sortMarkers(std::vector<Marker>& m) {
  std::stable_sort(m.begin(), m.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });
}
}  // namespace

int addMarker(Document& d, const Marker& m) {
  int idx = -1;
  d.editSong("add marker", [&](Song& x) {
    auto it = std::upper_bound(x.markers.begin(), x.markers.end(), m.pos, [](Tick p, const Marker& k) { return p < k.pos; });
    idx = static_cast<int>(it - x.markers.begin());
    x.markers.insert(it, m);
  });
  return idx;
}

bool editMarker(Document& d, int index, const std::string& label, const std::function<void(Marker&)>& fn) {
  if (index < 0 || index >= static_cast<int>(d.song().markers.size())) return false;
  Marker m = d.song().markers[static_cast<size_t>(index)];
  fn(m);
  if (m == d.song().markers[static_cast<size_t>(index)]) return false;
  d.editSong(label, [&](Song& x) {
    x.markers[static_cast<size_t>(index)] = m;
    sortMarkers(x.markers);
  });
  return true;
}

int addMarkersEvery(Document& d, Tick from, Tick to, Tick every) {
  if (every <= 0 || to <= from) return 0;
  int n = 0;
  d.editSong("add markers", [&](Song& x) {
    for (Tick t = from; t < to && n < 512; t += every) {
      bool exists = false;
      for (const auto& m : x.markers) exists = exists || m.pos == t;
      if (exists) continue;
      Marker m;
      m.pos = t;
      m.name = std::to_string(t / std::max<Tick>(1, x.barTicks()) + 1);
      x.markers.push_back(m);
      ++n;
    }
    sortMarkers(x.markers);
  });
  return n;
}

const char* markerActionName(MarkerAction a) {
  static const char* const n[] = {"None", "Loop to the next marker", "Skip to the next marker", "Pause", "Stop"};
  int i = static_cast<int>(a);
  return i >= 0 && i < static_cast<int>(MarkerAction::Count) ? n[i] : "";
}

std::pair<Tick, Tick> markerLoopRange(const Song& s, int index) {
  if (index < 0 || index >= static_cast<int>(s.markers.size())) return {0, 0};
  Tick a = s.markers[static_cast<size_t>(index)].pos;
  Tick b = 0;
  for (const auto& m : s.markers)
    if (m.pos > a) {
      b = m.pos;
      break;
    }
  if (b <= a) b = std::max(s.lengthTicks(), a + s.barTicks());
  return {a, b};
}

}  // namespace gl::ops
