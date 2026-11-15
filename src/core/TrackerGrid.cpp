// Gridline - the tracker view of a pattern (K-106). See TrackerGrid.h.
#include "TrackerGrid.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <iterator>
#include <map>
#include <numeric>
#include <set>
#include "ClipLoop.h"
#include "Document.h"

namespace gl::trk {

Grid gridOf(const Song& s, const Pattern& p) {
  Grid g;
  const Tick tps = s.ticksPerStep(p);
  g.rowTicks = tps > 0 ? tps : std::max<Tick>(1, static_cast<Tick>(s.ppq) / 4);
  g.rows = std::max(1, p.stepsPerBar * std::max(1, p.bars));
  g.rowsPerBar = std::max(1, p.stepsPerBar);
  const Tick beat = s.patternBeatTicks(p);
  g.rowsPerBeat = beat > 0 && beat % g.rowTicks == 0 ? std::max<int>(1, static_cast<int>(beat / g.rowTicks)) : g.rowsPerBar;
  return g;
}

int fieldDigits(Field f) {
  switch (f) {
    case Field::Note: return 1;
    case Field::Effect: return 3;
    default: return 2;
  }
}

int fieldChars(Field f) { return f == Field::Note || f == Field::Effect ? 3 : 2; }

const Cell& Track::at(int row, int column) const {
  static const Cell empty;
  if (row < 0 || row >= rows || column < 0 || column >= columns) return empty;
  const size_t i = static_cast<size_t>(row) * static_cast<size_t>(columns) + static_cast<size_t>(column);
  return i < cells.size() ? cells[i] : empty;
}

namespace {

struct Span {
  double start = 0, end = 0;
  int pitch = 0;
  size_t index = 0;
};

// First free column, in start order; -1 beyond kMaxColumns.
std::vector<int> assignSpans(std::vector<Span> spans, int* count) {
  std::vector<int> col(spans.size(), -1);
  std::stable_sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
  });
  std::vector<double> busyUntil;
  int used = 0;
  for (const Span& sp : spans) {
    int c = -1;
    for (size_t k = 0; k < busyUntil.size(); ++k)
      if (busyUntil[k] <= sp.start) {
        c = static_cast<int>(k);
        break;
      }
    if (c < 0 && busyUntil.size() < static_cast<size_t>(kMaxColumns)) {
      busyUntil.push_back(0);
      c = static_cast<int>(busyUntil.size()) - 1;
    }
    if (c < 0) continue;  // more voices than columns: hidden, counted by the caller
    busyUntil[static_cast<size_t>(c)] = sp.end;
    col[sp.index] = c;
    used = std::max(used, c + 1);
  }
  if (count) *count = used;
  return col;
}

double startOf(const Note& n) { return static_cast<double>(n.start) + static_cast<double>(n.frac); }
double endOf(const Note& n) { return static_cast<double>(n.start + n.length) + static_cast<double>(n.endFrac); }

int stepPitch(const std::vector<StepProps>& props, size_t i) { return i < props.size() && props[i].pitch >= 0 ? props[i].pitch : 60; }

}  // namespace

std::vector<int> assignColumns(const std::vector<Note>& notes, int* count) {
  std::vector<Span> spans;
  for (size_t i = 0; i < notes.size(); ++i) spans.push_back({startOf(notes[i]), endOf(notes[i]), notes[i].pitch, i});
  return assignSpans(std::move(spans), count);
}

Track buildTrack(const Song& s, const Pattern& p, ObjectId channel, const Grid& g, int minColumns) {
  (void)s;
  Track t;
  t.channel = channel;
  t.rows = g.rows;
  static const std::vector<Note> none;
  static const std::vector<uint8_t> noSteps;
  static const std::vector<StepProps> noProps;
  const auto nit = p.notes.find(channel);
  const std::vector<Note>& notes = nit == p.notes.end() ? none : nit->second;
  const auto sit = p.steps.find(channel);
  const std::vector<uint8_t>& steps = sit == p.steps.end() ? noSteps : sit->second;
  const auto pit = p.stepProps.find(channel);
  const std::vector<StepProps>& props = pit == p.stepProps.end() ? noProps : pit->second;
  // Notes and steps share the columns: a step is one row long.
  std::vector<Span> spans;
  for (size_t i = 0; i < notes.size(); ++i) spans.push_back({startOf(notes[i]), endOf(notes[i]), notes[i].pitch, i});
  std::vector<size_t> stepAt;
  for (size_t i = 0; i < steps.size(); ++i)
    if (steps[i] > 0) {
      const double st = static_cast<double>(g.tickOf(static_cast<int>(i)));
      spans.push_back({st, st + static_cast<double>(g.rowTicks), stepPitch(props, i), notes.size() + stepAt.size()});
      stepAt.push_back(i);
    }
  t.hasSteps = !stepAt.empty();
  int used = 0;
  const std::vector<int> all = assignSpans(std::move(spans), &used);
  t.col.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(notes.size()));
  t.voices = used;
  t.columns = std::clamp(std::max(used, minColumns), 1, kMaxColumns);
  t.cells.assign(static_cast<size_t>(g.rows) * static_cast<size_t>(t.columns), Cell{});
  auto cell = [&](int row, int c) -> Cell& { return t.cells[static_cast<size_t>(row) * static_cast<size_t>(t.columns) + static_cast<size_t>(c)]; };
  // Starts first, then the OFFs where nothing starts.
  for (size_t i = 0; i < notes.size(); ++i) {
    const Note& n = notes[i];
    const int c = t.col[i];
    if (c < 0) continue;
    const int row = static_cast<int>(n.start / g.rowTicks);
    if (n.start >= g.length()) {
      ++t.pastEnd;
      continue;
    }
    if (n.start < 0) continue;
    Cell& ce = cell(row, c);
    ce.kind = Cell::On;
    ce.note = static_cast<int>(i);
    ce.offGrid = n.start % g.rowTicks != 0 || n.frac != 0.f;
  }
  for (size_t k = 0; k < stepAt.size(); ++k) {
    const size_t i = stepAt[k];
    const int c = all[notes.size() + k];
    const int row = static_cast<int>(i);
    if (c < 0 || row >= g.rows) continue;
    Cell& ce = cell(row, c);
    if (ce.kind != Cell::Empty) continue;
    ce.kind = Cell::Step;
    ce.step = row;
    ce.offGrid = i < props.size() && props[i].shift != 0;
  }
  for (size_t i = 0; i < notes.size(); ++i) {
    const Note& n = notes[i];
    const int c = t.col[i];
    if (c < 0 || n.start < 0 || n.start >= g.length()) continue;
    const Tick end = n.start + n.length;
    const int row = static_cast<int>(end / g.rowTicks);
    if (row >= g.rows || row <= n.start / g.rowTicks) continue;
    Cell& ce = cell(row, c);
    if (ce.kind != Cell::Empty) continue;
    ce.kind = Cell::Off;
    ce.note = static_cast<int>(i);
    ce.offGrid = end % g.rowTicks != 0 || n.endFrac != 0.f;
  }
  return t;
}

// ---- text ----
std::string noteText(int pitch) {
  static const char* names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
  const int p = std::clamp(pitch, 0, 127);
  const int octave = p / 12 - 1;
  std::string s = names[p % 12];
  s += octave < 0 ? '-' : static_cast<char>('0' + octave);
  return s;
}

std::string hex2(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%02X", std::clamp(v, 0, 255));
  return b;
}

std::string rowText(int row, bool hex) {
  char b[8];
  if (hex) std::snprintf(b, sizeof b, row > 0xFF ? "%03X" : "%02X", std::max(0, row));
  else std::snprintf(b, sizeof b, row > 99 ? "%03d" : "%02d", std::max(0, row));
  return b;
}

// ---- effects ----
std::vector<Effect> effectsOf(const Note& n) {
  std::vector<Effect> v;
  if (n.slide) v.push_back({'G', 1, true});
  if (n.porta) v.push_back({'P', 1, true});
  if (n.fine != 0) {
    const bool fits = n.fine >= -128 && n.fine <= 127;
    v.push_back({'T', fits ? (n.fine & 0xFF) : 0, fits});
  }
  if (n.modX != kNoteModCentre) v.push_back({'X', std::clamp(n.modX, 0, 255), true});
  if (n.modY != kNoteModCentre) v.push_back({'Y', std::clamp(n.modY, 0, 255), true});
  if (n.release != kNoteReleaseDefault) v.push_back({'R', std::clamp(n.release, 0, 255), true});
  return v;
}

bool setEffect(Note& n, char letter, int value) {
  value = std::clamp(value, 0, 255);
  switch (letter) {
    case 'G': n.slide = true; return true;
    case 'P': n.porta = true; return true;
    case 'T': n.fine = value >= 128 ? value - 256 : value; return true;
    case 'X': n.modX = value; return true;
    case 'Y': n.modY = value; return true;
    case 'R': n.release = std::min(value, 127); return true;
    default: return false;
  }
}

void resetEffect(Note& n, char letter) {
  switch (letter) {
    case 'G': n.slide = false; break;
    case 'P': n.porta = false; break;
    case 'T': n.fine = 0; break;
    case 'X': n.modX = kNoteModCentre; break;
    case 'Y': n.modY = kNoteModCentre; break;
    case 'R': n.release = kNoteReleaseDefault; break;
    default: break;
  }
}

std::string effectHelp(char letter, int value) {
  char b[96];
  switch (letter) {
    case 'G': return "G: glides into this note from the one sounding (slide; the speed is the channel's)";
    case 'P': return "P: starts at the previous note's pitch and bends to this one (the channel's time)";
    case 'T':
      std::snprintf(b, sizeof b, "T%s: fine tune %+d cents", hex2(value).c_str(), value >= 128 ? value - 256 : value);
      return b;
    case 'X': std::snprintf(b, sizeof b, "X%s: modulation X %d of 255 (centre 128)", hex2(value).c_str(), value); return b;
    case 'Y': std::snprintf(b, sizeof b, "Y%s: modulation Y %d of 255 (centre 128)", hex2(value).c_str(), value); return b;
    case 'R': std::snprintf(b, sizeof b, "R%s: release velocity %d", hex2(value).c_str(), value); return b;
    default: return "Effect letters: G slide, P portamento, T fine tune, X and Y modulation, R release";
  }
}

// ---- edits ----
void normalize(std::vector<Note>& notes) {
  for (Note& n : notes) {
    n.pitch = std::clamp(n.pitch, 0, 127);
    n.velocity = std::clamp(n.velocity, 1, 127);
    n.start = std::max<Tick>(n.start, 0);
    n.length = std::max<Tick>(n.length, 1);
  }
  std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
}

namespace {

// The note of `column` sounding across `tick` (started before it, ends after it), -1 when none.
int soundingAt(const std::vector<Note>& notes, const Track& t, int column, Tick tick) {
  for (size_t i = 0; i < notes.size() && i < t.col.size(); ++i)
    if (t.col[i] == column && notes[i].start < tick && notes[i].start + notes[i].length > tick) return static_cast<int>(i);
  return -1;
}

// Where the next note of `column` after `tick` starts (`fallback` when none).
Tick nextStart(const std::vector<Note>& notes, const Track& t, int column, Tick tick, Tick fallback) {
  Tick best = fallback;
  for (size_t i = 0; i < notes.size() && i < t.col.size(); ++i)
    if (t.col[i] == column && notes[i].start > tick) best = std::min(best, notes[i].start);
  return best;
}

Result refuse(const char* why) {
  Result r;
  r.refused = why;
  return r;
}

Result done(bool changed) {
  Result r;
  r.changed = changed;
  return r;
}

}  // namespace

Result putNote(std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column, int pitch, int velocity) {
  if (row < 0 || row >= g.rows) return refuse("outside the pattern");
  if (pitch < 0 || pitch > 127) return refuse("that key is outside the note range");
  const Cell& c = t.at(row, column);
  if (c.kind == Cell::Step) return refuse("a rack step: change its key in Steps");
  if (c.kind == Cell::On) {
    Note& n = notes[static_cast<size_t>(c.note)];
    if (n.pitch == pitch) return done(false);
    n.pitch = pitch;
    normalize(notes);
    return done(true);
  }
  const Tick tick = g.tickOf(row);
  const Tick next = nextStart(notes, t, column, tick, g.length());
  const int sounding = soundingAt(notes, t, column, tick);
  if (sounding >= 0) {
    Note& s = notes[static_cast<size_t>(sounding)];
    s.length = tick - s.start;
    s.endFrac = 0;
  }
  Note n(tick, std::max<Tick>(next - tick, g.rowTicks), pitch, std::clamp(velocity, 1, 127));
  notes.push_back(n);
  normalize(notes);
  return done(true);
}

Result putOff(std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column) {
  if (row < 0 || row >= g.rows) return refuse("outside the pattern");
  const Cell& c = t.at(row, column);
  if (c.kind == Cell::Step) return refuse("a rack step: it lasts one step");
  const Tick tick = g.tickOf(row);
  const int sounding = soundingAt(notes, t, column, tick);
  if (c.kind == Cell::Off && c.note == sounding) return done(false);
  bool changed = false;
  if (sounding >= 0) {
    Note& s = notes[static_cast<size_t>(sounding)];
    s.length = tick - s.start;
    s.endFrac = 0;
    changed = true;
  }
  if (c.kind == Cell::On) {  // the OFF replaces the note starting here
    notes.erase(notes.begin() + c.note);
    changed = true;
  }
  if (!changed) return refuse("no note sounds here");
  normalize(notes);
  return done(true);
}

Result clearField(std::vector<Note>& notes, std::vector<uint8_t>& steps, std::vector<StepProps>& props, const Track& t, const Grid& g,
                  int row, int column, Field f) {
  const Cell& c = t.at(row, column);
  if (c.kind == Cell::Empty) return done(false);
  if (c.kind == Cell::Step) {
    const size_t i = static_cast<size_t>(c.step);
    if (i >= steps.size()) return done(false);
    switch (f) {
      case Field::Note: steps[i] = 0; return done(true);
      case Field::Volume:
        if (steps[i] == 100) return done(false);
        steps[i] = 100;
        return done(true);
      case Field::Pan:
        if (i >= props.size() || props[i].pan == 0) return done(false);
        props[i].pan = 0;
        return done(true);
      default: return refuse("a rack step has no effect letters");
    }
  }
  Note& n = notes[static_cast<size_t>(c.note)];
  if (c.kind == Cell::Off) {
    if (f != Field::Note) return done(false);
    // The note sounds on to the next note in its column, or the end.
    const Tick end = nextStart(notes, t, column, n.start, g.length());
    if (end <= n.start + n.length) return done(false);
    n.length = end - n.start;
    n.endFrac = 0;
    normalize(notes);
    return done(true);
  }
  switch (f) {
    case Field::Note: notes.erase(notes.begin() + c.note); return done(true);
    case Field::Instrument: return refuse("the instrument is the note's channel: type another number to move it");
    case Field::Volume:
      if (n.velocity == 100) return done(false);
      n.velocity = 100;
      return done(true);
    case Field::Pan:
      if (n.pan == 0) return done(false);
      n.pan = 0;
      return done(true);
    case Field::Effect: {
      const std::vector<Effect> fx = effectsOf(n);  // the effect column shows the first letter
      if (fx.empty()) return done(false);
      resetEffect(n, fx.front().letter);
      return done(true);
    }
    default: return done(false);
  }
}

Result setVolume(std::vector<Note>& notes, std::vector<uint8_t>& steps, const Track& t, int row, int column, int value) {
  const Cell& c = t.at(row, column);
  const int v = std::clamp(value, 1, 127);
  if (c.kind == Cell::On) {
    Note& n = notes[static_cast<size_t>(c.note)];
    if (n.velocity == v) return done(false);
    n.velocity = v;
    return done(true);
  }
  if (c.kind == Cell::Step && static_cast<size_t>(c.step) < steps.size()) {
    uint8_t& s = steps[static_cast<size_t>(c.step)];
    if (s == v) return done(false);
    s = static_cast<uint8_t>(v);
    return done(true);
  }
  return refuse("no note here");
}

Result setPan(std::vector<Note>& notes, std::vector<StepProps>& props, const Track& t, int row, int column, int value) {
  const Cell& c = t.at(row, column);
  const int pan = std::clamp(value, 0, 128) - 64;
  if (c.kind == Cell::On) {
    Note& n = notes[static_cast<size_t>(c.note)];
    if (n.pan == pan) return done(false);
    n.pan = pan;
    return done(true);
  }
  if (c.kind == Cell::Step) {
    const size_t i = static_cast<size_t>(c.step);
    if (props.size() <= i) props.resize(i + 1);
    if (props[i].pan == pan) return done(false);
    props[i].pan = pan;
    return done(true);
  }
  return refuse("no note here");
}

Result setEffectAt(std::vector<Note>& notes, const Track& t, int row, int column, char shown, char letter, int value) {
  const Cell& c = t.at(row, column);
  if (c.kind == Cell::Step) return refuse("a rack step has no effect letters: convert it to a note in the piano roll");
  if (c.kind != Cell::On) return refuse("no note here");
  Note& n = notes[static_cast<size_t>(c.note)];
  const Note before = n;
  if (shown && shown != letter) resetEffect(n, shown);
  if (!setEffect(n, letter, value)) {
    n = before;
    return refuse("effect letters: G slide, P portamento, T fine tune, X and Y modulation, R release");
  }
  return done(!(n == before));
}

Result insertRow(std::vector<Note>& notes, std::vector<uint8_t>& steps, std::vector<StepProps>& props, const Grid& g, int row) {
  if (row < 0 || row >= g.rows) return refuse("outside the pattern");
  const size_t r = static_cast<size_t>(row);
  if (steps.size() > r && steps.back() != 0) return refuse("a rack step would fall off the end of the pattern");
  const Tick tick = g.tickOf(row);
  bool changed = false;
  for (Note& n : notes) {
    if (n.start >= tick) n.start += g.rowTicks, changed = true;
    else if (n.start + n.length > tick) n.length += g.rowTicks, changed = true;
  }
  if (steps.size() > r) {
    const size_t len = steps.size();
    steps.insert(steps.begin() + static_cast<std::ptrdiff_t>(r), 0);
    steps.resize(len);
    changed = true;
    if (props.size() > r) {
      const size_t pl = props.size();
      props.insert(props.begin() + static_cast<std::ptrdiff_t>(r), StepProps{});
      props.resize(pl);
    }
  }
  if (changed) normalize(notes);
  return done(changed);
}

Result deleteRow(std::vector<Note>& notes, std::vector<uint8_t>& steps, std::vector<StepProps>& props, const Grid& g, int row) {
  if (row < 0 || row >= g.rows) return refuse("outside the pattern");
  const Tick tick = g.tickOf(row), next = tick + g.rowTicks;
  bool changed = false;
  std::vector<Note> out;
  out.reserve(notes.size());
  for (Note n : notes) {
    if (n.start >= tick && n.start < next) {
      changed = true;
      continue;
    }
    if (n.start >= next) n.start -= g.rowTicks, changed = true;
    else if (n.start + n.length > tick) n.length -= std::min(g.rowTicks, n.start + n.length - tick), changed = true;
    out.push_back(n);
  }
  notes.swap(out);
  const size_t r = static_cast<size_t>(row);
  if (steps.size() > r) {
    steps.erase(steps.begin() + static_cast<std::ptrdiff_t>(r));
    steps.push_back(0);
    changed = true;
    if (props.size() > r) {
      props.erase(props.begin() + static_cast<std::ptrdiff_t>(r));
      props.push_back(StepProps{});
    }
  }
  if (changed) normalize(notes);
  return done(changed);
}

Result transpose(std::vector<Note>& notes, const Grid& g, int r0, int r1, int semis) {
  Result res;
  const Tick a = g.tickOf(std::min(r0, r1)), b = g.tickOf(std::max(r0, r1) + 1);
  for (Note& n : notes) {
    if (n.start < a || n.start >= b) continue;
    const int p = n.pitch + semis;
    if (p < 0 || p > 127) {
      ++res.refusedCount;
      continue;
    }
    if (p != n.pitch) n.pitch = p, res.changed = true;
  }
  if (res.changed) normalize(notes);
  if (res.refusedCount > 0 && !res.changed) res.refused = "the notes would leave the note range";
  return res;
}

Result interpolate(std::vector<Note>& notes, const Grid& g, int r0, int r1, Field f) {
  Result res;
  const Tick a = g.tickOf(std::min(r0, r1)), b = g.tickOf(std::max(r0, r1) + 1);
  std::vector<size_t> in;
  for (size_t i = 0; i < notes.size(); ++i)
    if (notes[i].start >= a && notes[i].start < b) in.push_back(i);
  if (in.size() < 3) {
    res.refused = "Interpolate needs notes at both ends of the block and between them";
    return res;
  }
  const Note& first = notes[in.front()];
  const Note& last = notes[in.back()];
  char letter = 0;
  auto get = [&](const Note& n) -> int {
    if (f == Field::Pan) return n.pan;
    if (f == Field::Effect) {
      for (const Effect& e : effectsOf(n))
        if (e.letter == letter) return e.letter == 'T' ? n.fine : e.value;
      return letter == 'T' ? 0 : letter == 'R' ? kNoteReleaseDefault : kNoteModCentre;
    }
    return n.velocity;
  };
  if (f == Field::Effect) {
    const std::vector<Effect> fa = effectsOf(first), fb = effectsOf(last);
    if (fa.empty() || fb.empty() || fa.front().letter != fb.front().letter || fa.front().letter == 'G' || fa.front().letter == 'P') {
      res.refused = "Interpolate an effect: the first and last notes need the same letter with a value (T X Y R)";
      return res;
    }
    letter = fa.front().letter;
  }
  const int v0 = get(first), v1 = get(last);
  const double t0 = static_cast<double>(first.start), t1 = static_cast<double>(last.start);
  for (size_t k = 1; k + 1 < in.size(); ++k) {
    Note& n = notes[in[k]];
    const double u = t1 > t0 ? (static_cast<double>(n.start) - t0) / (t1 - t0) : 0.0;
    const int v = static_cast<int>(std::lround(v0 + (v1 - v0) * u));
    if (get(n) == v) continue;
    if (f == Field::Pan) n.pan = std::clamp(v, -64, 64);
    else if (f == Field::Effect) {
      if (letter == 'T') n.fine = std::clamp(v, -128, 127);
      else setEffect(n, letter, v);
    } else n.velocity = std::clamp(v, 1, 127);
    res.changed = true;
  }
  return res;
}

Result scaleRows(std::vector<Note>& notes, const Grid& g, int r0, int r1, bool expand) {
  const Tick a = g.tickOf(std::min(r0, r1)), b = g.tickOf(std::max(r0, r1) + 1);
  bool changed = false;
  for (Note& n : notes) {
    if (n.start < a || n.start >= b) continue;
    const Tick off = n.start - a;
    const Tick start = a + (expand ? off * 2 : off / 2), length = std::max<Tick>(1, expand ? n.length * 2 : n.length / 2);
    if (start != n.start || length != n.length) n.start = start, n.length = length, changed = true;
  }
  if (changed) normalize(notes);
  return done(changed);
}

std::vector<Note> copyRows(const std::vector<Note>& notes, const std::vector<uint8_t>& steps, const std::vector<StepProps>& props,
                           const Grid& g, int r0, int r1) {
  const int lo = std::min(r0, r1), hi = std::max(r0, r1);
  const Tick a = g.tickOf(lo), b = g.tickOf(hi + 1);
  std::vector<Note> out;
  for (const Note& n : notes)
    if (n.start >= a && n.start < b) {
      Note c = n;
      c.start -= a;
      out.push_back(c);
    }
  for (int r = lo; r <= hi && r >= 0 && static_cast<size_t>(r) < steps.size(); ++r) {
    const size_t i = static_cast<size_t>(r);
    if (!steps[i]) continue;
    Note c(g.tickOf(r) - a, g.rowTicks, stepPitch(props, i), steps[i]);
    if (i < props.size()) c.pan = props[i].pan, c.fine = props[i].fine, c.release = props[i].release, c.modX = props[i].modX, c.modY = props[i].modY;
    c.step = true;
    out.push_back(c);
  }
  normalize(out);
  return out;
}

Result cutRows(std::vector<Note>& notes, std::vector<uint8_t>& steps, const Grid& g, int r0, int r1) {
  const int lo = std::min(r0, r1), hi = std::max(r0, r1);
  const Tick a = g.tickOf(lo), b = g.tickOf(hi + 1);
  const size_t before = notes.size();
  notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const Note& n) { return n.start >= a && n.start < b; }), notes.end());
  bool changed = notes.size() != before;
  for (int r = std::max(0, lo); r <= hi && static_cast<size_t>(r) < steps.size(); ++r)
    if (steps[static_cast<size_t>(r)]) steps[static_cast<size_t>(r)] = 0, changed = true;
  return done(changed);
}

Result pasteRows(std::vector<Note>& notes, std::vector<uint8_t>& steps, const Grid& g, int row, int rows, const std::vector<Note>& block) {
  if (row < 0 || row >= g.rows || rows <= 0) return refuse("outside the pattern");
  const std::vector<Note> before = notes;
  const std::vector<uint8_t> stepsBefore = steps;
  cutRows(notes, steps, g, row, row + rows - 1);
  const Tick at = g.tickOf(row);
  for (Note n : block) {
    n.start += at;
    notes.push_back(n);
  }
  normalize(notes);
  return done(notes != before || steps != stepsBefore);
}

// ---- the song ----
namespace {
bool inSong(const Song& s, const Clip& c) {
  return c.kind == ClipKind::Pattern && c.pattern != kNoId && !(s.live.areaEnd > 0 && c.start < s.live.areaEnd);
}
bool audioInSong(const Song& s, const Clip& c) { return c.kind == ClipKind::Audio && !(s.live.areaEnd > 0 && c.start < s.live.areaEnd); }
}  // namespace

std::vector<OrderEntry> orderList(const Song& s) {
  std::vector<const Clip*> clips;
  std::set<Tick> cuts;
  for (const Clip& c : s.playlist) {
    if (!inSong(s, c)) continue;
    const Tick len = s.clipLength(c);
    if (len <= 0) continue;
    clips.push_back(&c);
    cuts.insert(c.start);
    cuts.insert(c.start + len);
  }
  std::vector<OrderEntry> out;
  for (auto it = cuts.begin(), nx = it == cuts.end() ? it : std::next(it); it != cuts.end() && nx != cuts.end(); ++it, ++nx) {
    std::vector<const Clip*> on;
    for (const Clip* c : clips)
      if (c->start <= *it && c->start + s.clipLength(*c) >= *nx) on.push_back(c);
    if (on.empty()) continue;  // a gap in the song
    std::stable_sort(on.begin(), on.end(), [](const Clip* a, const Clip* b) { return a->track < b->track; });
    OrderEntry e;
    e.start = *it;
    e.end = *nx;
    e.pattern = on.front()->pattern;
    std::set<ObjectId> pats;
    for (const Clip* c : on) e.clips.push_back(c->id), pats.insert(c->pattern);
    e.patterns = static_cast<int>(pats.size());
    out.push_back(std::move(e));
  }
  // T-13: the audio clips. Under a pattern entry they are counted on it; where no pattern clip
  // plays, the slices between their starts and ends are audio parts of their own.
  std::vector<const Clip*> audio;
  for (const Clip& c : s.playlist)
    if (audioInSong(s, c) && s.clipLength(c) > 0) audio.push_back(&c);
  if (audio.empty()) return out;
  std::stable_sort(audio.begin(), audio.end(), [](const Clip* a, const Clip* b) { return a->track < b->track; });
  for (OrderEntry& e : out)
    for (const Clip* c : audio)
      if (c->start < e.end && c->start + s.clipLength(*c) > e.start) e.audio.push_back(c->id);
  std::set<Tick> acuts;
  for (const Clip* c : audio) acuts.insert(c->start), acuts.insert(c->start + s.clipLength(*c));
  for (const OrderEntry& e : out) acuts.insert(e.start), acuts.insert(e.end);
  std::vector<OrderEntry> parts;
  for (auto it = acuts.begin(), nx = std::next(it); nx != acuts.end(); ++it, ++nx) {
    bool covered = false;
    for (const OrderEntry& e : out) covered = covered || (e.start <= *it && e.end >= *nx);
    if (covered) continue;
    OrderEntry e;
    e.start = *it, e.end = *nx, e.patterns = 0;
    for (const Clip* c : audio)
      if (c->start <= *it && c->start + s.clipLength(*c) >= *nx) e.audio.push_back(c->id);
    if (!e.audio.empty()) parts.push_back(std::move(e));
  }
  out.insert(out.end(), parts.begin(), parts.end());
  std::stable_sort(out.begin(), out.end(), [](const OrderEntry& a, const OrderEntry& b) { return a.start < b.start; });
  return out;
}

bool hasPatternEntries(const Song& s) {
  for (const Clip& c : s.playlist)
    if (inSong(s, c) && s.clipLength(c) > 0) return true;
  return false;
}

Tick patternTickAt(const Song& s, ObjectId pattern, Tick pos) {
  const Clip* best = nullptr;
  Tick at = -1;
  for (const Clip& c : s.playlist) {
    if (!inSong(s, c) || c.pattern != pattern || pos < c.start || pos >= c.start + s.clipLength(c)) continue;
    if (best && best->track <= c.track) continue;
    for (const ClipPass& p : clipPasses(s, c)) {
      const Tick rel = pos - c.start;
      if (rel >= p.at && rel < p.at + p.len) {
        best = &c;
        at = p.content + (rel - p.at);
        break;
      }
    }
  }
  return at;
}

ObjectId patternAt(const Song& s, Tick pos) {
  const Clip* best = nullptr;
  for (const Clip& c : s.playlist) {
    if (!inSong(s, c) || pos < c.start || pos >= c.start + s.clipLength(c)) continue;
    if (!best || c.track < best->track) best = &c;
  }
  return best ? best->pattern : kNoId;
}

int patternNumber(const Song& s, ObjectId pattern) { return s.patternIndex(pattern) + 1; }

namespace {
// Every clip from `from` on (the live area never: `from` is past it) moves by `by` ticks.
bool shiftFrom(Document& d, Tick from, Tick by, const char* label) {
  return d.editClips(label, [&](std::vector<Clip>& cs) {
    for (Clip& c : cs)
      if (c.start >= from) c.start += by;
  });
}
}  // namespace

OrderEdit orderInsert(Document& d, int at, ObjectId pattern) {
  const Song& s = d.song();
  const Pattern* p = s.pattern(pattern);
  if (!p) return OrderEdit::Nothing;
  const std::vector<OrderEntry> ol = orderList(s);
  at = std::clamp(at, 0, static_cast<int>(ol.size()));
  const Tick len = s.patternLength(*p);
  if (len <= 0) return OrderEdit::Nothing;
  Tick t = std::max<Tick>(0, s.live.areaEnd);
  int track = 0;
  if (at < static_cast<int>(ol.size())) {
    const OrderEntry& e = ol[static_cast<size_t>(at)];
    t = e.start;
    if (const Clip* c = e.clips.empty() ? nullptr : s.clip(e.clips.front())) track = c->track;
  } else if (!ol.empty()) {
    t = ol.back().end;
  }
  d.beginGroup("insert order entry");
  shiftFrom(d, t, len, "insert order entry");
  const bool ok = d.addClip(pattern, track, t) != kNoId;
  if (!ok) {
    d.cancelGroup();
    return OrderEdit::Nothing;
  }
  d.endGroup();
  return OrderEdit::Done;
}

OrderEdit orderRemove(Document& d, int at) {
  const Song& s = d.song();
  const std::vector<OrderEntry> ol = orderList(s);
  if (at < 0 || at >= static_cast<int>(ol.size())) return OrderEdit::Nothing;
  const OrderEntry e = ol[static_cast<size_t>(at)];
  if (e.pattern == kNoId) return OrderEdit::OtherClips;  // an audio part: changed on the Song page
  std::vector<ObjectId> gone;
  for (const Clip& c : s.playlist) {
    const Tick end = c.start + s.clipLength(c);
    if (inSong(s, c)) {
      if (c.start >= e.end || end <= e.start) continue;
      if (c.start < e.start || end > e.end) return OrderEdit::Spans;
      gone.push_back(c.id);
    } else if (c.kind != ClipKind::Pattern && c.start >= e.start && c.start < e.end) {
      return OrderEdit::OtherClips;
    }
  }
  if (gone.empty()) return OrderEdit::Nothing;
  d.beginGroup("remove order entry");
  for (ObjectId id : gone) d.removeClip(id);
  shiftFrom(d, e.end, -(e.end - e.start), "remove order entry");
  d.endGroup();
  return OrderEdit::Done;
}

OrderEdit orderSetPattern(Document& d, int at, ObjectId pattern) {
  const Song& s = d.song();
  const std::vector<OrderEntry> ol = orderList(s);
  const Pattern* p = s.pattern(pattern);
  if (!p || at < 0 || at >= static_cast<int>(ol.size())) return OrderEdit::Nothing;
  const OrderEntry& e = ol[static_cast<size_t>(at)];
  if (e.pattern == kNoId) return OrderEdit::OtherClips;  // an audio part: changed on the Song page
  const Clip* c = e.clips.size() == 1 ? s.clip(e.clips.front()) : nullptr;
  if (!c || c->start != e.start || c->start + s.clipLength(*c) != e.end) return OrderEdit::Spans;
  if (c->pattern == pattern) return OrderEdit::Nothing;
  const ObjectId id = c->id;
  const bool own = c->length == 0;  // plays its pattern's length: the song after it follows
  const Tick delta = own ? s.patternLength(*p) - (e.end - e.start) : 0;
  const Tick end = e.end;
  d.beginGroup("order entry pattern");
  if (delta != 0) shiftFrom(d, end, delta, "order entry pattern");
  d.editClip(id, "order entry pattern", [pattern](Clip& k) { k.pattern = pattern; });
  d.endGroup();
  return OrderEdit::Done;
}

}  // namespace gl::trk
