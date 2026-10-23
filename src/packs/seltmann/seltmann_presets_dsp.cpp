// Gridline — "seltmann" pack: chop layout presets. Each layout sets how the
// region is chopped (and how long the region is), the step length, and the
// steps in the pattern language (seltmann.h): letters are slots (A = the first
// slice unless reassigned), numbers are slices, "." rests, "~" ties, and
// ":r :p+7 :g-6 :l50 :x4 :o25" reverse / pitch / gain / length / stutter / offset.
// A few layouts are generated (key-aware scatter, micro melodies, shuffles)
// from a seed, so "another take" gives a new variation. Names are Gridline's own.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "seltmann.h"

namespace gl::packs::seltmann {

namespace fs = std::filesystem;

const char* categoryName(int c) {
  static const char* const n[CatCount] = {"Phrase", "Loop", "Breaks", "Micro", "Fills", "Creative", "Flip", "User"};
  return c >= 0 && c < CatCount ? n[c] : "";
}

const char* presetTabName(int tab) {
  static const char* const n[TabCount] = {"Flip", "Phrase", "Breaks", "Micro", "Creative", "Mine"};
  return tab >= 0 && tab < TabCount ? n[tab] : "";
}

int presetTabOf(int c) {
  switch (c) {
    case CatFlip: return TabFlip;
    case CatPhrase:
    case CatLoop: return TabPhrase;
    case CatBreaks:
    case CatFills: return TabBreaks;
    case CatMicro: return TabMicro;
    case CatCreative: return TabCreative;
    default: return TabMine;
  }
}

namespace {

enum Gen { GenNone = 0, GenKeyScatter, GenShuffle, GenMelody, GenArp, GenCloud, GenSkip };

Chop grid(int div, bool zero = true) {
  Chop c;
  c.mode = ChopGrid;
  c.division = div;
  c.zeroCross = zero;
  return c;
}
Chop grains(float ms) {
  Chop c;
  c.mode = ChopGrains;
  c.grainMs = ms;
  c.zeroCross = true;
  return c;
}

Preset P(const char* name, int cat, const char* hint, Chop chop, int unitDiv, int bars, const char* steps, int gen = GenNone, float swing = -1,
         int speed = -1) {
  Preset p;
  p.name = name;
  p.category = cat;
  p.hint = hint;
  p.chop = chop;
  p.unitDiv = unitDiv;
  p.regionBars = bars;
  p.steps = steps;
  p.gen = gen;
  p.swing = swing;
  p.speed = speed;
  return p;
}

}  // namespace

const std::vector<Preset>& factoryPresets() {
  static const std::vector<Preset> v = {
      // ---- Phrase: bar-long phrases re-ordered (slots A, B, C, D = bars 1..4 of the region)
      P("Verse Turn", CatPhrase, "Phrase, answer, phrase, a new ending", grid(Div1Bar), Div1Bar, 4, "A B A C"),
      P("Triple Lift", CatPhrase, "Three times the hook, then the lift", grid(Div1Bar), Div1Bar, 4, "A A A B"),
      P("See-Saw", CatPhrase, "Two bars trading places", grid(Div1Bar), Div1Bar, 4, "A B A B"),
      P("Pairs", CatPhrase, "Each phrase twice", grid(Div1Bar), Div1Bar, 4, "A A B B"),
      P("Straight Run", CatPhrase, "Four bars as they were", grid(Div1Bar), Div1Bar, 4, "A B C D"),
      P("Mirror", CatPhrase, "Out and back again", grid(Div1Bar), Div1Bar, 4, "A B B A"),
      P("Homecoming", CatPhrase, "The hook frames a detour", grid(Div1Bar), Div1Bar, 4, "A A B A"),
      P("Round Trip", CatPhrase, "Three phrases, back to the first", grid(Div1Bar), Div1Bar, 4, "A B C A"),
      P("Long Story", CatPhrase, "Eight bars: verse turn with a middle", grid(Div1Bar), Div1Bar, 8, "A B A C D B A C"),
      P("Call and Reply", CatPhrase, "Half-bar call, changing replies", grid(DivHalf), DivHalf, 4, "A B A C A B A D"),
      // ---- Loop
      P("One Bar Loop", CatLoop, "The first bar, four times", grid(Div1Bar), Div1Bar, 4, "A A A A"),
      P("Two Bar Loop", CatLoop, "Two bars, twice", grid(Div2Bars), Div2Bars, 4, "A A"),
      P("Four Bar Loop", CatLoop, "The whole four-bar phrase", grid(Div4Bars), Div4Bars, 4, "A"),
      P("Eight Bar Loop", CatLoop, "An eight-bar phrase in one piece", grid(Div8Bars), Div8Bars, 8, "A"),
      P("Half Bar Loop", CatLoop, "Two beats over and over", grid(DivHalf), DivHalf, 2, "A A A A"),
      P("Beat Loop", CatLoop, "One beat on repeat", grid(DivBeat), DivBeat, 1, "A A A A"),
      P("Reverse Tail", CatLoop, "The last bar plays backwards", grid(Div1Bar), Div1Bar, 4, "A B C D:r"),
      P("Fading Repeat", CatLoop, "One bar, quieter each time", grid(Div1Bar), Div1Bar, 4, "A A:g-3 A:g-6 A:g-10"),
      // ---- Breaks: 2 bars in eighths (slices 1..16)
      P("Original Order", CatBreaks, "The break as played, in eighths", grid(Div8th), Div8th, 2, "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16"),
      P("Kick Swap", CatBreaks, "The opening kick returns mid-bar", grid(Div8th), Div8th, 2, "1 2 3 4 1 2 7 8 9 10 11 12 1 2 15 16"),
      P("Second Bar Flip", CatBreaks, "Bar two re-cut from bar one", grid(Div8th), Div8th, 2, "1 2 3 4 5 6 7 8 1 2 3 3 5 6 7 4"),
      P("Backbeat Push", CatBreaks, "Extra snares pushed in", grid(Div8th), Div8th, 2, "1 2 3 4 5 3 7 8 9 10 11 12 13 3 15 3"),
      P("Two-Step Shuffle", CatBreaks, "Skipping two-step gaps", grid(Div8th), Div8th, 2, "1 . 3 4 . 6 7 8 1 . 3 . 5 6 7 8"),
      P("Broken Groove", CatBreaks, "Holes punched into the groove", grid(Div8th), Div8th, 2, "1 2 . 4 5 . 7 8 9 . 11 12 . 14 15 16"),
      P("Ghost Notes", CatBreaks, "Quiet in-between hits", grid(Div8th), Div8th, 2,
        "1 2:g-10 3 4:g-10 5 3:g-12 7 8 9 10:g-10 11 12 13 3:g-12 15 4:g-8"),
      P("Rewind Break", CatBreaks, "Bar two runs backwards", grid(Div8th), Div8th, 2, "1 2 3 4 5 6 7 8 8:r 7:r 6:r 5:r 4:r 3:r 2:r 1:r"),
      P("Lazy Swing", CatBreaks, "The break with a loose swing", grid(Div16th), Div16th, 1, "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16", GenNone, 0.45f),
      P("Heavy Shuffle", CatBreaks, "Deep swing on a re-cut break", grid(Div16th), Div16th, 1, "1 2 3 4 1 2 7 8 9 10 11 12 9 10 15 16", GenNone,
        0.8f),
      // ---- Micro: tiny grains re-sequenced
      P("Grain Pulse", CatMicro, "1/32 grains in a new rhythm", grid(Div32nd), Div16th, 1, "1 5 9 13 3 7 11 15 2 6 10 14 4 8 12 16"),
      P("Micro Stutter", CatMicro, "1/64 grains with stuttered repeats", grid(Div64th), Div16th, 1,
        "1:x2 9 17:x4 25 33:x2 41 49:x4 57 1:x2 13 21 29:x4 37 45:x2 53 61:x8"),
      P("Grain Cloud", CatMicro, "Random 40 ms grains, some reversed", grains(40), Div16th, 1, "", GenCloud),
      P("Micro Melody", CatMicro, "One grain played as a melody in the song's key", grains(90), Div16th, 1, "", GenMelody),
      P("Micro Arp", CatMicro, "Four grains arpeggiated across the key", grains(70), Div16th, 1, "", GenArp),
      P("Pitched Crumbs", CatMicro, "1/32 grains jumping octaves and fifths", grid(Div32nd), Div16th, 1,
        "1:p+12 2 3:p-12 4:p+7 5 6:p+12 7:p-5 8 9:p+12 10 11:p-12 12:p+7 13 14:p+5 15:p+12 16:p-12"),
      P("Glitch Cuts", CatMicro, "Thirty-second cuts with holes and flips", grid(Div64th), Div32nd, 1,
        "1 1 . 4 5:r 6 . 8 9 9 9 12 . 14:r 15 16 17 . 19 20:r 21 21 . 24 25 26 . 28:r 29 29:x2 31 32:x4"),
      P("Tiny Bounce", CatMicro, "25 ms grains bouncing in pairs", grains(25), Div16th, 1, "1:x2 2 3:x2 4 5:x2 6 7:x4 8 9:x2 10 11:x2 12 13:x4 14 15:x2 16:x8"),
      // ---- Fills
      P("Stutter Fill", CatFills, "The last beat stutters into the next bar", grid(DivBeat), DivBeat, 4,
        "1 2 3 4 5 6 7 8 9 10 11 12 13 13:x2 13:x4 13:x8"),
      P("Snare Riser", CatFills, "One hit rolling faster and louder", grid(DivBeat), DivBeat, 4,
        "2:g-14 2:g-13 2:g-12 2:g-11 2:x2:g-10 2:x2:g-9 2:x2:g-8 2:x2:g-7 2:x4:g-6 2:x4:g-5 2:x4:g-4 2:x4:g-3 2:x8:g-2 2:x8:g-1 2:x16 2:x16"),
      P("Turnaround Roll", CatFills, "Plays through, rolls at the end", grid(DivBeat), DivBeat, 4, "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15:x4 16:x8"),
      P("Reverse Swell", CatFills, "A backwards swell into the loop", grid(DivBeat), DivBeat, 4, "1 2 3 4 5 6 7 8 9 10 11 12 13 14 16:r ~"),
      P("Drop Gap", CatFills, "A beat of silence before the one", grid(DivBeat), DivBeat, 4, "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 ."),
      P("Slow Down", CatFills, "The bar winds down like a stopping deck", grid(Div8th), Div8th, 1, "1 2 3 4 5:p-1 6:p-3 7:p-6 8:p-12"),
      P("Echo Tail", CatFills, "The last hit echoes away", grid(DivBeat), DivBeat, 4, "1 2 3 4 5 6 7 8 9 10 11 12 13 13:g-6 13:g-12 13:g-18"),
      P("Triplet Fill", CatFills, "Triplet slices that roll at the end", grid(Div8T), Div8T, 1, "1 2 3 4 5 6 7 8 9 1:x2 1:x3 1:x4"),
      // ---- Creative
      P("Key Scatter", CatCreative, "Random slices moved by octaves and fifths that stay in key", grid(DivBeat), DivBeat, 2, "", GenKeyScatter),
      P("Shuffle Deck", CatCreative, "The slices dealt in a new order", grid(Div8th), Div8th, 1, "", GenShuffle),
      P("Backwards Mirror", CatCreative, "Forwards, then the same slices backwards", grid(Div8th), Div8th, 1, "1 2 3 4 4:r 3:r 2:r 1:r"),
      P("Octave Jumper", CatCreative, "Every other slice an octave away", grid(Div8th), Div8th, 1, "1 2:p+12 3 4:p-12 5 6:p+12 7 8:p-12"),
      P("Half-Time Drop", CatCreative, "Each slice at half speed, an octave down", grid(Div8th), Div8th, 1,
        "1:p-12 ~ 2:p-12 ~ 3:p-12 ~ 4:p-12 ~ 5:p-12 ~ 6:p-12 ~ 7:p-12 ~ 8:p-12 ~"),
      P("Double-Time Run", CatCreative, "The bar played twice in the time of one", grid(Div8th), Div16th, 1, "1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8"),
      P("Skipping Record", CatCreative, "Plays on, now and then sticking", grid(Div8th), Div8th, 2, "", GenSkip),
      P("Vinyl Spin", CatCreative, "A backspin at the end of the bar", grid(Div8th), Div8th, 1, "1 2 3 4 5 6 7:r 8:r:p-5"),
      P("Gated Chops", CatCreative, "Short gated beats with space between", grid(DivBeat), DivBeat, 2, "1:l40 2:l40 3:l40 4:l60 5:l40 6:l40 7:l40 8:l25:x2"),
      // ---- Flip: instant flips that work on anything (breaks, loops, songs)
      P("Instant Flip", CatFlip, "One bar re-cut: the hits trade places in pairs", grid(Div8th), Div8th, 1, "1 2 5 6 3 4 7 8"),
      P("Chop and Bounce", CatFlip, "Two bars with doubled hits that bounce at the end", grid(Div8th), Div8th, 2,
        "1 1 3 4 5 5 7 8 9 9 11 12 13 13:x2 15 16:r"),
      P("Soul Stab", CatFlip, "Big stabs from the phrase with room between them", grid(DivBeat), DivBeat, 4, "1 . . 1 . . 5 . 9 . . 9 . 13 13 ."),
      P("Halftime Flip", CatFlip, "The bar at half time: every slice held for two steps", grid(Div8th), Div8th, 1, "1 ~ 2 ~ 3 ~ 4 ~ 5 ~ 6 ~ 7 ~ 8 ~"),
      P("Reverse Hook", CatFlip, "The hook, then its last beats backwards", grid(DivBeat), DivBeat, 2, "1 2 3 4 5 6 8:r 7:r"),
      P("Stutter Drop", CatFlip, "The one stutters in, then the bar plays out", grid(Div8th), Div8th, 1, "1 1:x2 1:x4 . 5 6 7 8"),
  };
  return v;
}

namespace {

struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  int below(int n) { return n <= 0 ? 0 : static_cast<int>(next() % static_cast<uint32_t>(n)); }
  double unit() { return (next() & 0xFFFFFF) / static_cast<double>(0x1000000); }
};

uint32_t hashName(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) h = (h ^ c) * 16777619u;
  return h;
}

// Indices of the loudest slices (RMS), loudest first.
std::vector<int> loudest(const Model& m, const Sample* s, int count) {
  std::vector<std::pair<double, int>> r;
  for (int i = 0; i < static_cast<int>(m.slices.size()); ++i) {
    double e = 0;
    if (s) {
      const auto& sl = m.slices[static_cast<size_t>(i)];
      const int64_t n = static_cast<int64_t>(s->frames());
      const int64_t a = std::clamp<int64_t>(sl.start, 0, n), b = std::clamp<int64_t>(sl.end, a, n);
      for (int64_t k = a; k < b; k += 4) e += static_cast<double>(s->left[static_cast<size_t>(k)]) * s->left[static_cast<size_t>(k)];
      e /= std::max<int64_t>(1, (b - a) / 4);
    }
    r.push_back({e, i});
  }
  std::stable_sort(r.begin(), r.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
  std::vector<int> out;
  for (int i = 0; i < count && i < static_cast<int>(r.size()); ++i) out.push_back(r[static_cast<size_t>(i)].second);
  return out;
}

// Semitone offsets of the song's scale (major / natural minor; minor pentatonic when unknown).
std::vector<int> scaleOf(const Analysis& g) {
  if (g.key < 0) return {0, 3, 5, 7, 10};
  if (g.minor) return {0, 2, 3, 5, 7, 8, 10};
  return {0, 2, 4, 5, 7, 9, 11};
}

Step slice(int k) {
  Step s;
  s.slot = false;
  s.src = k;
  return s;
}

std::vector<Step> generate(int gen, const Model& m, const Sample* s, uint32_t seed) {
  Rng r(seed);
  const int n = std::max(1, static_cast<int>(m.slices.size()));
  std::vector<Step> v;
  switch (gen) {
    case GenKeyScatter: {
      static const int kPitch[] = {0, 0, 0, 0, 12, -12, 7, -5};
      for (int i = 0; i < 8; ++i) {
        Step st = slice(i == 0 ? 0 : r.below(n));
        if (i) st.pitch = static_cast<float>(kPitch[r.below(8)]);
        v.push_back(st);
      }
      break;
    }
    case GenShuffle: {
      std::vector<int> idx(static_cast<size_t>(std::min(n, 16)));
      for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int>(i);
      for (size_t i = idx.size(); i > 1; --i) std::swap(idx[i - 1], idx[static_cast<size_t>(r.below(static_cast<int>(i)))]);
      for (int k : idx) v.push_back(slice(k));
      break;
    }
    case GenMelody: {
      const auto sc = scaleOf(m.grid);
      const int grain = loudest(m, s, 1).empty() ? 0 : loudest(m, s, 1)[0];
      int deg = 0;
      const int span = static_cast<int>(sc.size());
      for (int i = 0; i < 16; ++i) {
        if (i % 4 == 0 && r.below(3) == 0) {
          Step rest;
          rest.src = kRest;
          if (i > 0) {
            v.push_back(rest);
            continue;
          }
        }
        deg = std::clamp(deg + r.below(5) - 2, -span, 2 * span - 1);
        const int oct = deg < 0 ? -1 : deg / span;
        const int d = ((deg % span) + span) % span;
        Step st = slice(grain);
        st.pitch = static_cast<float>(sc[static_cast<size_t>(d)] + 12 * oct);
        st.length = 0.8f;
        v.push_back(st);
      }
      break;
    }
    case GenArp: {
      const auto sc = scaleOf(m.grid);
      const auto g4 = loudest(m, s, 4);
      const int span = static_cast<int>(sc.size());
      const int degs[8] = {0, 2, 4, span, 4, 2, 0, -span + 4};
      for (int i = 0; i < 16; ++i) {
        const int dd = degs[i % 8] + (i >= 8 ? 1 : 0);
        const int oct = dd < 0 ? -1 : dd / span;
        const int d = ((dd % span) + span) % span;
        Step st = slice(g4.empty() ? 0 : g4[static_cast<size_t>(i % static_cast<int>(g4.size()))]);
        st.pitch = static_cast<float>(sc[static_cast<size_t>(d)] + 12 * oct);
        st.length = 0.7f;
        v.push_back(st);
      }
      break;
    }
    case GenCloud: {
      for (int i = 0; i < 16; ++i) {
        Step st = slice(r.below(n));
        st.reverse = r.below(10) < 3;
        st.gain = -static_cast<float>(r.below(7));
        if (r.below(8) == 0) st.stutter = 2;
        v.push_back(st);
      }
      break;
    }
    case GenSkip: {
      int pos = 0;
      for (int i = 0; i < 16; ++i) {
        if (i > 2 && r.below(5) == 0) pos = std::max(0, pos - 2);  // the needle jumps back
        v.push_back(slice(pos % n));
        ++pos;
      }
      break;
    }
    default: break;
  }
  if (v.empty()) v.push_back(slice(0));
  return v;
}

}  // namespace

std::string presetSummary(const Preset& p, int maxSteps) {
  if (p.gen != GenNone || p.steps.empty()) return "random take";
  Pattern tmp;
  if (!patternFromText(p.steps, tmp)) return "";
  std::string o;
  int shown = 0;
  for (const auto& st : tmp.steps) {
    if (shown >= maxSteps) {
      o += " ...";
      break;
    }
    if (!o.empty()) o += ' ';
    if (st.src == kRest) o += '.';
    else if (st.slot) o += slotName(st.src);
    else o += std::to_string(st.src + 1);
    if (st.reverse) o += "'";
    for (int t = 0; t < st.ties; ++t) o += " -";
    ++shown;
  }
  return o;
}

namespace {

// A grid division whose length is `beats` (powers of two of the bar / beat), -1 none.
int divisionFor(double beats, int bpb) {
  for (int d = 0; d < DivCount; ++d)
    if (std::fabs(divisionBeats(d, bpb) - beats) < 1e-6) return d;
  return -1;
}

// The power-of-two fraction (1, 1/2, 1/4 ... 1/64) nearest below `f`.
double pow2Below(double f) {
  double r = 1;
  while (r > f * 1.0001 && r > 1.0 / 64) r *= 0.5;
  return r;
}

}  // namespace

std::vector<std::pair<int, float>> applyPreset(Model& m, const Sample* s, const Preset& p, uint32_t seed) {
  m.chop = p.chop;
  m.chop.autoSize = false;
  const int bpb = std::max(1, m.grid.beatsPerBar);
  double unit = p.unitDiv >= 0 ? divisionBeats(p.unitDiv, bpb) : std::max(1.0 / 64, p.unitBeats);
  // steps that play each slice at its own length scale with the chop
  const bool unitIsCell = p.chop.mode == ChopGrid && p.unitDiv >= 0 && p.unitDiv == p.chop.division;
  // breaks keep their played feel: grid cuts land on the hits
  if (m.chop.mode == ChopGrid && m.grid.kind == KindBreak) m.chop.hitSnap = true;
  if (p.regionBars > 0) {
    const bool barless = m.grid.kind == KindOneShot || m.grid.kind == KindAmbient;
    const int avail = barless ? 0 : barCount(m);
    if (avail >= 1) {
      // keep the region's first bar; material shorter than the layout keeps its slice count with a finer chop
      const double b0 = beatAt(m.grid, static_cast<double>(m.regionStart));
      int firstBar = std::clamp(static_cast<int>(std::floor((b0 - m.grid.bar1) / bpb + 0.5)), 0, avail - 1);
      int use = p.regionBars;
      while (use > avail && use > 1) use /= 2;
      const double f = static_cast<double>(use) / p.regionBars;
      if (firstBar + use > avail) firstBar = std::max(0, avail - use);
      setRegionBars(m, firstBar, use);
      if (f < 1 && m.chop.mode == ChopGrid) {
        const int d = divisionFor(divisionBeats(m.chop.division, bpb) * f, bpb);
        if (d >= 0) {
          m.chop.division = d;
          if (unitIsCell) unit *= f;
        }
      }
    } else if (m.chop.mode == ChopGrid) {
      // shorter than a bar (one-shots, half-bar loops): the sound in as many equal parts as the layout has cells
      const double want = p.regionBars * bpb;
      const int cells = std::clamp(static_cast<int>(std::lround(want / divisionBeats(m.chop.division, bpb))), 1, 64);
      m.chop.mode = ChopEqual;
      m.chop.count = cells;
      if (unitIsCell) unit *= pow2Below(std::max(1e-3, regionBars(m) * bpb / want));
    }
  }
  rechop(m, s);
  m.pattern.name = p.name;
  m.pattern.unitBeats = std::max(1.0 / 64, unit);
  if (p.gen != GenNone) {
    m.pattern.steps = generate(p.gen, m, s, seed ^ hashName(p.name));
  } else {
    Pattern tmp = m.pattern;
    if (patternFromText(p.steps, tmp)) m.pattern.steps = tmp.steps;
  }
  // no step refers to a slice that is not there: fold them onto the slices that are
  foldSteps(m);
  m.preset = p.name;
  std::vector<std::pair<int, float>> params;
  params.push_back({SmSwing, p.swing >= 0 ? p.swing : 0.0f});
  params.push_back({SmSpeed, static_cast<float>(p.speed >= 0 ? p.speed : SpeedNormal)});
  return params;
}

// ------------------------------------------------------------------ user presets
std::string presetToText(const Preset& p) {
  std::ostringstream o;
  o << "seltmann-preset 1\n";
  o << "name=" << p.name << "\n";
  o << "hint=" << p.hint << "\n";
  o << "chop=" << p.chop.mode << " " << p.chop.division << " " << p.chop.sensitivity << " " << p.chop.count << " " << p.chop.grainMs << " "
    << p.chop.snap << " " << (p.chop.zeroCross ? 1 : 0) << "\n";
  o.precision(10);
  o << "unit=" << p.unitDiv << " " << p.unitBeats << "\n";
  o << "bars=" << p.regionBars << "\n";
  o << "swing=" << p.swing << "\n";
  o << "speed=" << p.speed << "\n";
  o << "gen=" << p.gen << "\n";
  o << "steps=" << p.steps << "\n";
  return o.str();
}

bool presetFromText(const std::string& text, Preset& p) {
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("seltmann-preset", 0) != 0) return false;
  Preset r;
  r.category = CatUser;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    std::istringstream l(v);
    if (k == "name") r.name = v;
    else if (k == "hint") r.hint = v;
    else if (k == "chop") {
      int z = 1;
      l >> r.chop.mode >> r.chop.division >> r.chop.sensitivity >> r.chop.count >> r.chop.grainMs >> r.chop.snap >> z;
      r.chop.zeroCross = z != 0;
      r.chop.mode = std::clamp(r.chop.mode, 0, ChopModeCount - 1);
      r.chop.division = std::clamp(r.chop.division, 0, DivCount - 1);
    } else if (k == "unit") l >> r.unitDiv >> r.unitBeats;
    else if (k == "bars") l >> r.regionBars;
    else if (k == "swing") l >> r.swing;
    else if (k == "speed") l >> r.speed;
    else if (k == "gen") l >> r.gen;
    else if (k == "steps") r.steps = v;
  }
  if (r.name.empty()) return false;
  r.unitDiv = std::clamp(r.unitDiv, -1, DivCount - 1);
  r.regionBars = std::clamp(r.regionBars, 0, 64);
  p = r;
  return true;
}

bool saveUserPreset(const std::string& dir, const std::string& name, const Model& m, float swing, int speed) {
  if (name.empty() || dir.empty()) return false;
  Preset p;
  p.name = name;
  p.category = CatUser;
  p.chop = m.chop;
  if (p.chop.mode == ChopManual) p.chop.mode = ChopGrid;  // hand-placed cuts belong to one sample
  p.unitDiv = -1;
  p.unitBeats = m.pattern.unitBeats;
  const int bpb = std::max(1, m.grid.beatsPerBar);
  const double bars = (beatAt(m.grid, static_cast<double>(m.regionEnd)) - beatAt(m.grid, static_cast<double>(m.regionStart))) / bpb;
  p.regionBars = std::clamp(static_cast<int>(std::lround(bars)), 0, 64);
  p.swing = swing;
  p.speed = speed;
  p.steps = patternToText(m.pattern);
  std::string file;
  for (char c : name) file += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') ? c : '_';
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::ofstream out(fs::path(dir) / (file + ".seltmann"));
  if (!out) return false;
  out << presetToText(p);
  return static_cast<bool>(out);
}

std::vector<Preset> loadUserPresets(const std::string& dir) {
  std::vector<Preset> v;
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) return v;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".seltmann") continue;
    std::ifstream in(e.path());
    std::stringstream ss;
    ss << in.rdbuf();
    Preset p;
    if (presetFromText(ss.str(), p)) v.push_back(p);
  }
  std::sort(v.begin(), v.end(), [](const Preset& a, const Preset& b) { return a.name < b.name; });
  return v;
}

}  // namespace gl::packs::seltmann
