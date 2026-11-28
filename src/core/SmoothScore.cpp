// Gridline — K-128: core helpers for Smooth's Event List and Score Editor.
#include "SmoothScore.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include "Meter.h"
#include "NoteTools.h"
#include "Params.h"

namespace gl::smoothscore {

namespace {

constexpr const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr const char* kFlatNoteNames[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

// Semitone offsets from C for natural diatonic degrees 0..6 (C, D, E, F, G, A, B).
constexpr int kDiatonicSemi[7] = {0, 2, 4, 5, 7, 9, 11};

// Pitch-class (0..11) spelling in sharp/neutral keys vs flat keys: {letter 0..6, accidental -1..+1}.
constexpr std::pair<int, int> kSpellSharp[12] = {
    {0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}, {3, 0}, {3, 1}, {4, 0}, {4, 1}, {5, 0}, {5, 1}, {6, 0}};
constexpr std::pair<int, int> kSpellFlat[12] = {
    {0, 0}, {1, -1}, {1, 0}, {2, -1}, {2, 0}, {3, 0}, {4, -1}, {4, 0}, {5, -1}, {5, 0}, {6, -1}, {6, 0}};

// Circle-of-fifths order of letters affected by sharps (F C G D A E B) and flats (B E A D G C F).
constexpr int kSharpLetters[7] = {3, 0, 4, 1, 5, 2, 6};
constexpr int kFlatLetters[7] = {6, 2, 5, 1, 4, 0, 3};

// Diatonic staff steps of key signature symbols (treble, then bass = treble - 14).
// C4 = 28, so F5 = 38, C5 = 35, G5 = 39, D5 = 36, A4 = 33, E5 = 37, B4 = 34.
constexpr int kTrebleSharpSteps[7] = {38, 35, 39, 36, 33, 37, 34};
constexpr int kTrebleFlatSteps[7] = {34, 37, 33, 36, 32, 35, 31};
constexpr int kBassSharpSteps[7] = {24, 21, 25, 22, 19, 23, 20};
constexpr int kBassFlatSteps[7] = {20, 23, 19, 22, 18, 21, 17};

int keySigAccidentalForLetter(int letter, int fifths) {
  if (fifths > 0) {
    const int n = std::min(7, fifths);
    for (int i = 0; i < n; ++i)
      if (kSharpLetters[i] == letter) return 1;
  } else if (fifths < 0) {
    const int n = std::min(7, -fifths);
    for (int i = 0; i < n; ++i)
      if (kFlatLetters[i] == letter) return -1;
  }
  return 0;
}

void patternMetrics(const Song& s, const Pattern* pat, Tick& barTicks, Tick& beatTicks, Tick& divTicks) {
  barTicks = std::max<Tick>(1, pat ? s.patternBarTicks(*pat) : s.barTicks());
  beatTicks = std::max<Tick>(1, pat ? s.patternBeatTicks(*pat) : s.beatTicks());
  divTicks = std::max<Tick>(1, static_cast<Tick>(s.ppq) / 4);
}

std::vector<long long> splitInts(const std::string& text) {
  std::vector<long long> out;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i])) && text[i] != '-') ++i;
    if (i >= text.size()) break;
    char* end = nullptr;
    const long long v = std::strtoll(text.c_str() + i, &end, 10);
    if (end == text.c_str() + i) break;
    out.push_back(v);
    i = static_cast<size_t>(end - text.c_str());
  }
  return out;
}

}  // namespace

std::string formatBbtPos(const Song& s, Tick t, const Pattern* pat) {
  Tick barT = 1, beatT = 1, divT = 1;
  patternMetrics(s, pat, barT, beatT, divT);
  const Tick pos = std::max<Tick>(0, t);
  const Tick bar = pos / barT;
  const Tick remBar = pos % barT;
  const Tick beat = remBar / beatT;
  const Tick remBeat = remBar % beatT;
  const Tick div = remBeat / divT;
  const Tick tick = remBeat % divT;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%lld.%lld.%lld.%lld", static_cast<long long>(bar + 1), static_cast<long long>(beat + 1),
                static_cast<long long>(div + 1), static_cast<long long>(tick + 1));
  return buf;
}

std::string formatBbtLen(const Song& s, Tick len, const Pattern* pat) {
  Tick barT = 1, beatT = 1, divT = 1;
  patternMetrics(s, pat, barT, beatT, divT);
  const Tick l = std::max<Tick>(0, len);
  const Tick bars = l / barT;
  const Tick remBar = l % barT;
  const Tick beats = remBar / beatT;
  const Tick remBeat = remBar % beatT;
  const Tick divs = remBeat / divT;
  const Tick ticks = remBeat % divT;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%lld.%lld.%lld.%lld", static_cast<long long>(bars), static_cast<long long>(beats),
                static_cast<long long>(divs), static_cast<long long>(ticks));
  return buf;
}

bool parseBbtPos(const Song& s, const std::string& text, Tick& out, const Pattern* pat) {
  const auto parts = splitInts(text);
  if (parts.empty()) return false;
  Tick barT = 1, beatT = 1, divT = 1;
  patternMetrics(s, pat, barT, beatT, divT);
  const long long bar = std::max(1LL, parts[0]) - 1;
  const long long beat = parts.size() > 1 ? std::max(1LL, parts[1]) - 1 : 0;
  const long long div = parts.size() > 2 ? std::max(1LL, parts[2]) - 1 : 0;
  const long long tick = parts.size() > 3 ? std::max(1LL, parts[3]) - 1 : 0;
  out = std::max<Tick>(0, bar * barT + beat * beatT + div * divT + tick);
  return true;
}

bool parseBbtLen(const Song& s, const std::string& text, Tick& out, const Pattern* pat) {
  const auto parts = splitInts(text);
  if (parts.empty()) return false;
  Tick barT = 1, beatT = 1, divT = 1;
  patternMetrics(s, pat, barT, beatT, divT);
  const long long bars = std::max(0LL, parts[0]);
  const long long beats = parts.size() > 1 ? std::max(0LL, parts[1]) : 0;
  const long long divs = parts.size() > 2 ? std::max(0LL, parts[2]) : 0;
  const long long ticks = parts.size() > 3 ? std::max(0LL, parts[3]) : 0;
  out = std::max<Tick>(1, bars * barT + beats * beatT + divs * divT + ticks);
  return true;
}

std::string formatPitch(int pitch) {
  const int p = std::clamp(pitch, 0, 127);
  return std::string(kNoteNames[p % 12]) + std::to_string(p / 12 - 1);
}

bool parsePitch(const std::string& text, int& out) {
  size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i >= text.size()) return false;
  // Plain MIDI number?
  if (std::isdigit(static_cast<unsigned char>(text[i]))) {
    char* end = nullptr;
    const long v = std::strtol(text.c_str() + i, &end, 10);
    if (end == text.c_str() + i) return false;
    out = std::clamp(static_cast<int>(v), 0, 127);
    return true;
  }
  const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i++])));
  int semi = -1;
  switch (c) {
    case 'C': semi = 0; break;
    case 'D': semi = 2; break;
    case 'E': semi = 4; break;
    case 'F': semi = 5; break;
    case 'G': semi = 7; break;
    case 'A': semi = 9; break;
    case 'B': semi = 11; break;
    default: return false;
  }
  if (i < text.size()) {
    if (text[i] == '#') {
      ++semi;
      ++i;
    } else if (text[i] == 'b' || text[i] == 'B') {
      --semi;
      ++i;
    }
  }
  if (i >= text.size()) return false;
  char* end = nullptr;
  const long oct = std::strtol(text.c_str() + i, &end, 10);
  if (end == text.c_str() + i) return false;
  const long midi = (oct + 1) * 12 + semi;
  if (midi < 0 || midi > 127) return false;
  out = static_cast<int>(midi);
  return true;
}

std::vector<EventRow> buildEventRows(const Song& s, const Pattern& pat, ObjectId chId, const EventFilter& filter) {
  std::vector<EventRow> rows;
  const int chIdx = s.channelIndex(chId);
  const int chNum = chIdx >= 0 ? chIdx + 1 : 1;

  if (auto it = pat.notes.find(chId); it != pat.notes.end()) {
    const auto& ns = it->second;
    for (size_t i = 0; i < ns.size(); ++i) {
      const Note& n = ns[i];
      if (filter.notes) {
        EventRow r;
        r.kind = EventKind::Note;
        r.pos = n.start;
        r.length = n.length;
        r.channelNum = chNum;
        r.pitch = n.pitch;
        r.value = n.velocity;
        r.noteIndex = i;
        r.status = "Note";
        r.detail = formatPitch(n.pitch);
        rows.push_back(std::move(r));
      }
      if (filter.velocity && !n.pressure.empty()) {
        for (const BendPoint& pt : n.pressure) {
          EventRow r;
          r.kind = EventKind::Velocity;
          r.pos = std::max<Tick>(0, n.start + static_cast<Tick>(std::llround(pt.tick)));
          r.length = 0;
          r.channelNum = chNum;
          r.pitch = n.pitch;
          r.value = std::clamp(static_cast<int>(std::lround(pt.semis * 127.0f)), 0, 127);
          r.noteIndex = i;
          r.status = "Pressure";
          r.detail = formatPitch(n.pitch);
          rows.push_back(std::move(r));
        }
      }
      if (filter.modCc && (n.modX != kNoteModCentre || n.modY != kNoteModCentre)) {
        EventRow r;
        r.kind = EventKind::ModCc;
        r.pos = n.start;
        r.length = 0;
        r.channelNum = chNum;
        r.pitch = 74;  // CC 74 brightness / modX
        r.value = std::clamp(n.modX / 2, 0, 127);
        r.noteIndex = i;
        r.status = "Mod";
        r.detail = "Mod X/Y";
        rows.push_back(std::move(r));
      }
    }
  }

  if (filter.modCc) {
    for (size_t ai = 0; ai < pat.automation.size(); ++ai) {
      const PatternAutomation& pa = pat.automation[ai];
      if (chId != kNoId && pa.owner != chId && pa.owner != kSongId) continue;
      std::string pname = pa.name;
      if (pname.empty()) {
        if (auto d = describeParam(s, ParamId{pa.owner, pa.index})) pname = d->name;
        else pname = "CC " + std::to_string(pa.index);
      }
      for (size_t pi = 0; pi < pa.points.size(); ++pi) {
        const AutoPoint& pt = pa.points[pi];
        EventRow r;
        r.kind = EventKind::ModCc;
        r.pos = pt.pos;
        r.length = 0;
        r.channelNum = chNum;
        r.pitch = static_cast<int>(pa.index & 0x7Fu);
        r.value = std::clamp(static_cast<int>(std::lround(pt.value * 127.0f)), 0, 127);
        r.autoIndex = static_cast<int>(ai);
        r.autoPointIndex = static_cast<int>(pi);
        r.status = "CC";
        r.detail = pname;
        rows.push_back(std::move(r));
      }
    }
  }

  std::stable_sort(rows.begin(), rows.end(), [](const EventRow& a, const EventRow& b) {
    if (a.pos != b.pos) return a.pos < b.pos;
    if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    return a.pitch > b.pitch;
  });
  return rows;
}

// ---- Score Editor notation model ----

ClefMode autoClefForNotes(const std::vector<Note>& notes) {
  if (notes.empty()) return ClefMode::Grand;
  int lo = 127, hi = 0;
  for (const Note& n : notes) {
    lo = std::min(lo, n.pitch);
    hi = std::max(hi, n.pitch);
  }
  if (lo < 60 && hi >= 60) return ClefMode::Grand;
  if (hi - lo > 16) return ClefMode::Grand;
  return hi < 60 ? ClefMode::Bass : ClefMode::Treble;
}

Tick gridStepTicks(int ppq, QuantGrid grid) {
  const int p = std::max(4, ppq);
  switch (grid) {
    case QuantGrid::Quarter: return std::max<Tick>(1, p);
    case QuantGrid::Eighth: return std::max<Tick>(1, p / 2);
    case QuantGrid::Sixteenth: return std::max<Tick>(1, p / 4);
  }
  return std::max<Tick>(1, p / 4);
}

Tick quantizeTick(Tick t, int ppq, QuantGrid grid) {
  const Tick step = gridStepTicks(ppq, grid);
  return std::max<Tick>(0, ((t + step / 2) / step) * step);
}

int keySignatureFifths(int root, int scale) {
  if (scale <= 0) return 0;  // chromatic / none -> C major (0 accidentals)
  // Major key fifths table indexed by pitch class 0..11 (C..B).
  // Prefer <= 6 accidentals: C=0, Db=-5, D=2, Eb=-3, E=4, F=-1, F#=6, G=1, Ab=-4, A=3, Bb=-2, B=5.
  static constexpr int kMajorFifths[12] = {0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5};
  int r = ((root % 12) + 12) % 12;
  const auto& sc = tools::scales();
  if (scale >= 0 && static_cast<size_t>(scale) < sc.size()) {
    const std::string name = sc[static_cast<size_t>(scale)].name;
    // Minor family: relative major is +3 semitones.
    if (name.find("minor") != std::string::npos || name.find("Minor") != std::string::npos || name.find("Aeolian") != std::string::npos) {
      r = (r + 3) % 12;
    } else if (name.find("Dorian") != std::string::npos) {
      r = (r + 10) % 12;
    } else if (name.find("Phrygian") != std::string::npos) {
      r = (r + 8) % 12;
    } else if (name.find("Lydian") != std::string::npos) {
      r = (r + 7) % 12;
    } else if (name.find("Mixolydian") != std::string::npos) {
      r = (r + 5) % 12;
    } else if (name.find("Locrian") != std::string::npos) {
      r = (r + 1) % 12;
    }
  }
  return kMajorFifths[r];
}

std::string keySignatureLabel(int root, int scale) {
  const int r = ((root % 12) + 12) % 12;
  const int fifths = keySignatureFifths(r, scale);
  const char* rn = fifths < 0 ? kFlatNoteNames[r] : kNoteNames[r];
  if (scale <= 0) return std::string(rn) + " maj";
  const auto& sc = tools::scales();
  if (static_cast<size_t>(scale) < sc.size()) {
    const std::string name = sc[static_cast<size_t>(scale)].name;
    if (name.find("minor") != std::string::npos || name.find("Minor") != std::string::npos) return std::string(rn) + " min";
  }
  return std::string(rn) + " maj";
}

std::vector<int> keySigSteps(int fifths, bool treble) {
  std::vector<int> out;
  const int n = std::clamp(std::abs(fifths), 0, 7);
  out.reserve(static_cast<size_t>(n));
  if (fifths > 0) {
    const int* tbl = treble ? kTrebleSharpSteps : kBassSharpSteps;
    for (int i = 0; i < n; ++i) out.push_back(tbl[i]);
  } else if (fifths < 0) {
    const int* tbl = treble ? kTrebleFlatSteps : kBassFlatSteps;
    for (int i = 0; i < n; ++i) out.push_back(tbl[i]);
  }
  return out;
}

SpelledPitch spellPitch(int midiPitch, int keyFifths) {
  const int p = std::clamp(midiPitch, 0, 127);
  const int oct = p / 12 - 1;  // C4 = 60 -> oct = 4
  const int pc = p % 12;
  const auto [letter, acc] = keyFifths < 0 ? kSpellFlat[pc] : kSpellSharp[pc];
  return SpelledPitch{oct * 7 + letter, acc};
}

int diatonicToMidi(int diatonicStep, int keyFifths) {
  // Floor division for negative steps.
  const int oct = diatonicStep >= 0 ? diatonicStep / 7 : (diatonicStep - 6) / 7;
  const int letter = diatonicStep - oct * 7;
  const int acc = keySigAccidentalForLetter(letter, keyFifths);
  return std::clamp((oct + 1) * 12 + kDiatonicSemi[letter] + acc, 0, 127);
}

NoteValue classifyDuration(Tick ticks, int ppq, QuantGrid grid) {
  const Tick step = gridStepTicks(ppq, grid);
  const Tick q = std::max(step, ((ticks + step / 2) / step) * step);
  const double beats = static_cast<double>(q) / static_cast<double>(std::max(1, ppq));
  if (beats >= 3.5) return NoteValue::Whole;
  if (beats >= 2.75) return NoteValue::DottedHalf;
  if (beats >= 1.75) return NoteValue::Half;
  if (beats >= 1.25) return NoteValue::DottedQuarter;
  if (beats >= 0.75) return NoteValue::Quarter;
  if (beats >= 0.375) return NoteValue::Eighth;
  return NoteValue::Sixteenth;
}

std::vector<ScoreNote> layoutScoreNotes(const Song& s, const Pattern& pat, const std::vector<Note>& notes,
                                        ClefMode clef, QuantGrid grid, int keyFifths) {
  std::vector<ScoreNote> out;
  out.reserve(notes.size());
  const int ppq = std::max(1, s.ppq);
  const Tick barTicks = std::max<Tick>(1, s.patternBarTicks(pat));
  const Tick beatTicks = std::max<Tick>(1, s.patternBeatTicks(pat));
  const Tick step = gridStepTicks(ppq, grid);

  // Build initial entries sorted by quantized start, then pitch.
  std::vector<size_t> order(notes.size());
  for (size_t i = 0; i < notes.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (notes[a].start != notes[b].start) return notes[a].start < notes[b].start;
    return notes[a].pitch < notes[b].pitch;
  });

  // Per-bar accidental state: bar index -> (diatonicStep -> active accidental -1/0/+1).
  int curBar = -1;
  std::map<int, int> barAcc;

  for (size_t idx : order) {
    const Note& n = notes[idx];
    ScoreNote sn;
    sn.noteIndex = idx;
    sn.qStart = quantizeTick(n.start, ppq, grid);
    sn.qLength = std::max(step, quantizeTick(n.length, ppq, grid));
    sn.pitch = n.pitch;
    sn.velocity = n.velocity;
    sn.value = classifyDuration(n.length, ppq, grid);

    const SpelledPitch sp = spellPitch(n.pitch, keyFifths);
    sn.diatonicStep = sp.diatonicStep;

    const int bar = static_cast<int>(sn.qStart / barTicks);
    if (bar != curBar) {
      curBar = bar;
      barAcc.clear();
    }
    const int letter = ((sp.diatonicStep % 7) + 7) % 7;
    const int activeAcc = barAcc.count(sp.diatonicStep) ? barAcc[sp.diatonicStep] : keySigAccidentalForLetter(letter, keyFifths);
    if (sp.accidental != activeAcc) {
      sn.drawnAccidental = sp.accidental;
      barAcc[sp.diatonicStep] = sp.accidental;
    } else {
      sn.drawnAccidental = -2;
    }

    if (clef == ClefMode::Bass) sn.bassStaff = true;
    else if (clef == ClefMode::Treble) sn.bassStaff = false;
    else sn.bassStaff = n.pitch < 60;  // Grand staff splits cleanly at middle C (C4 = 60)

    // Middle line: B4 (34) on Treble, D3 (22) on Bass. Notes at or above middle line stem down.
    const int midLine = sn.bassStaff ? 22 : 34;
    sn.stemUp = sn.diatonicStep < midLine;
    out.push_back(sn);
  }

  // Group consecutive 8th / 16th notes on the same staff within the same beat for beaming.
  int nextGroup = 0;
  for (int staff = 0; staff < 2; ++staff) {
    const bool wantBass = staff == 1;
    std::vector<size_t> beamable;
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i].bassStaff == wantBass && flagCount(out[i].value) > 0) beamable.push_back(i);
    }
    size_t k = 0;
    while (k < beamable.size()) {
      const size_t startK = k;
      const Tick beatId = out[beamable[k]].qStart / beatTicks;
      Tick lastEnd = out[beamable[k]].qStart + out[beamable[k]].qLength;
      ++k;
      while (k < beamable.size()) {
        const ScoreNote& prev = out[beamable[k - 1]];
        const ScoreNote& cur = out[beamable[k]];
        if (cur.qStart / beatTicks != beatId) break;
        if (cur.qStart == prev.qStart) {  // chord tone on same start: keep going
          ++k;
          continue;
        }
        // Only beam notes separated by at most an eighth rest / contiguous subdivision.
        if (cur.qStart > lastEnd + step / 2) break;
        lastEnd = std::max(lastEnd, cur.qStart + cur.qLength);
        ++k;
      }
      // Need at least two distinct start ticks to form a beam.
      Tick firstStart = out[beamable[startK]].qStart;
      bool hasSecondTime = false;
      for (size_t j = startK + 1; j < k; ++j)
        if (out[beamable[j]].qStart != firstStart) hasSecondTime = true;
      if (hasSecondTime) {
        const int g = nextGroup++;
        // Choose unified stem direction for the beam group by furthest distance from middle line.
        const int midLine = wantBass ? 22 : 34;
        int sumDist = 0;
        for (size_t j = startK; j < k; ++j) sumDist += out[beamable[j]].diatonicStep - midLine;
        const bool up = sumDist < 0;
        for (size_t j = startK; j < k; ++j) {
          out[beamable[j]].beamGroup = g;
          out[beamable[j]].stemUp = up;
        }
      }
    }
  }

  return out;
}

}  // namespace gl::smoothscore
