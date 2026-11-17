// Gridline - the tracker view of a pattern (K-106). See TrackerGrid.h.
#include "TrackerGrid.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <cmath>
#include <iterator>
#include <map>
#include <numeric>
#include <set>
#include "ClipLoop.h"
#include "Document.h"
#include "TrackerStream.h"

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
std::string noteText(int pitch, bool flats) {
  static const char* names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
  static const char* flatNames[12] = {"C-", "Db", "D-", "Eb", "E-", "F-", "Gb", "G-", "Ab", "A-", "Bb", "B-"};
  const int p = std::clamp(pitch, 0, 127);
  const int octave = p / 12 - 1;
  std::string s = (flats ? flatNames : names)[p % 12];
  s += octave < 0 ? '-' : static_cast<char>('0' + octave);
  return s;
}

std::string hex2(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%02X", std::clamp(v, 0, 255));
  return b;
}

std::string hexNum(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%02X", std::clamp(v, 0, 0xFFF));
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
    case 'G': return "G01: glides into this note from the one sounding (on or off; the speed is the channel's)";
    case 'P': return "P01: starts at the previous note's pitch and bends to this one (on or off; the channel's time)";
    case 'T':
      std::snprintf(b, sizeof b, "T%s: fine tune %+d cents", hex2(value).c_str(), value >= 128 ? value - 256 : value);
      return b;
    case 'X': std::snprintf(b, sizeof b, "X%s: modulation X %d of 255 (centre 128)", hex2(value).c_str(), value); return b;
    case 'Y': std::snprintf(b, sizeof b, "Y%s: modulation Y %d of 255 (centre 128)", hex2(value).c_str(), value); return b;
    case 'R': std::snprintf(b, sizeof b, "R%s: release velocity %d", hex2(value).c_str(), value); return b;
    default: return "Effect letters: G slide, P portamento, T fine tune, X and Y modulation, R release";
  }
}

// ---- an imported module's cells ----
std::vector<tracker::Cell> moduleRows(const Note& n, int maxRows) {
  std::vector<tracker::Cell> out;
  if (n.tracker.empty() || maxRows <= 0) return out;
  tracker::StreamReader rd;
  tracker::StreamHeader h;
  if (!rd.open(n.tracker.data(), n.tracker.size(), h)) return out;
  for (int k = 0; k < maxRows && k < 1024; ++k) {
    const tracker::StreamRow r = rd.next();
    if (rd.done()) break;
    out.push_back(r.cell);
  }
  return out;
}

namespace {
// Rows a note sounds through, from its start row: up to the row its end falls in (a note ending
// exactly on a row boundary leaves that row out), within the pattern.
int coveredRows(const Note& n, const Grid& g, int startRow) {
  const Tick end = n.start + std::max<Tick>(1, n.length);
  const int endRow = static_cast<int>((end + g.rowTicks - 1) / std::max<Tick>(1, g.rowTicks));
  return std::max(1, std::min(endRow, g.rows) - startRow);
}
}  // namespace

int moduleNoteAt(const std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column, int* k) {
  if (g.rowTicks <= 0) return -1;
  for (size_t i = 0; i < notes.size() && i < t.col.size(); ++i) {
    const Note& n = notes[i];
    if (n.tracker.empty() || t.col[i] != column || n.start < 0) continue;
    const int startRow = static_cast<int>(n.start / g.rowTicks);
    if (row < startRow || row >= startRow + coveredRows(n, g, startRow)) continue;
    const int kk = row - startRow;
    if (static_cast<int>(moduleRows(n, kk + 1).size()) <= kk) continue;
    if (k) *k = kk;
    return static_cast<int>(i);
  }
  return -1;
}

namespace {
// R9-02, R9-04: an imported voice carries its cells for the rows after its first (core/TrackerStream.h).
// A note typed or deleted inside it, or a row inserted or deleted through it, would leave those cells
// on the wrong rows or drop them, so such edits are refused with this reason.
// R10-06: short, the useful part first (the status line shows it whole).
const char* const kInVoice = "Inside an imported voice: edit its first row, or after it ends";
// Whether an imported voice on the track covers `row` with its cells, past its first row (`first`: its
// first row counts too).
bool voiceOver(const std::vector<Note>& notes, const Grid& g, int row, bool first) {
  if (g.rowTicks <= 0) return false;
  for (const Note& n : notes) {
    if (n.tracker.empty() || n.start < 0) continue;
    const int startRow = static_cast<int>(n.start / g.rowTicks);
    const int kk = row - startRow;
    if (kk < (first ? 0 : 1) || kk >= coveredRows(n, g, startRow)) continue;
    if (static_cast<int>(moduleRows(n, kk + 1).size()) > kk) return true;
  }
  return false;
}
}  // namespace

std::string moduleVolText(uint8_t vol) {
  if (vol < 0x10) return "";
  if (vol <= 0x50) return hex2(vol - 0x10);
  static const char kLetters[] = "......-+DUSVPLRM";
  const char hexd[] = "0123456789ABCDEF";
  return std::string(1, kLetters[vol >> 4]) + hexd[vol & 0x0F];
}

std::string moduleEffectText(uint8_t efx, uint8_t param) {
  if (efx == 0 && param == 0) return "";
  const char c = efx < 10 ? static_cast<char>('0' + efx) : efx < 36 ? static_cast<char>('A' + efx - 10) : '?';
  return std::string(1, c) + hex2(param);
}

bool setStreamCell(std::vector<uint8_t>& s, int k, const tracker::Cell& c) {
  if (s.size() < 4 || s[0] != tracker::kStreamVersion || k < 0) return false;
  size_t pos = 4 + ((s[1] & tracker::kHasState) ? sizeof(tracker::State) : 0);
  int row = 0;
  auto room = [&](size_t more) { return s.size() + more <= tracker::kStreamMax; };
  while (pos < s.size()) {
    const uint8_t op = s[pos];
    if (op == 0x00) return false;
    if (op & 0x80) {
      const int n = op & 0x7F;
      if (k < row + n) {
        if (c.empty()) return true;  // empty already
        std::vector<uint8_t> rep;
        if (k > row) rep.push_back(static_cast<uint8_t>(0x80 | (k - row)));
        rep.insert(rep.end(), {0x02, c.note, c.instr, c.vol, c.efx, c.param});
        if (row + n - k - 1 > 0) rep.push_back(static_cast<uint8_t>(0x80 | (row + n - k - 1)));
        if (!room(rep.size() - 1)) return false;
        s.erase(s.begin() + static_cast<std::ptrdiff_t>(pos));
        s.insert(s.begin() + static_cast<std::ptrdiff_t>(pos), rep.begin(), rep.end());
        return true;
      }
      row += n;
      pos += 1;
      continue;
    }
    if (op == 0x02) {
      if (pos + 6 > s.size()) return false;
      if (row == k) {
        if (c.empty()) {
          s.erase(s.begin() + static_cast<std::ptrdiff_t>(pos) + 1, s.begin() + static_cast<std::ptrdiff_t>(pos) + 6);
          s[pos] = 0x81;
        } else {
          s[pos + 1] = c.note, s[pos + 2] = c.instr, s[pos + 3] = c.vol, s[pos + 4] = c.efx, s[pos + 5] = c.param;
        }
        return true;
      }
      ++row;
      pos += 6;
      continue;
    }
    if (op == 0x01) pos += 3;
    else if (op == 0x03) pos += 2;
    else if (op == 0x04) pos += 2;
    else if (op == 0x05) pos += 4;
    else return false;  // unknown: left alone
  }
  return false;
}

namespace {
Result editModuleCell(std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column, const std::function<const char*(tracker::Cell&)>& fn) {
  Result r;
  int k = -1;
  const int i = moduleNoteAt(notes, t, g, row, column, &k);
  if (i < 0) return r.refused = "No module cell here", r;
  Note& n = notes[static_cast<size_t>(i)];
  const std::vector<tracker::Cell> cells = moduleRows(n, k + 1);
  tracker::Cell c = cells[static_cast<size_t>(k)];
  const tracker::Cell was = c;
  if (const char* why = fn(c)) return r.refused = why, r;
  if (c == was) return r;
  std::vector<uint8_t> s = n.tracker;
  if (!setStreamCell(s, k, c)) return r.refused = "This module cell cannot be changed (its voice is full)", r;
  n.tracker = std::move(s);
  r.changed = true;
  return r;
}
}  // namespace

Result setModuleVolume(std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column, int volume) {
  return editModuleCell(notes, t, g, row, column, [volume](tracker::Cell& c) -> const char* {
    if (c.vol > 0x50) return "A volume column effect stays as imported here: only a set volume (00 to 40) is changed";
    c.vol = volume < 0 ? 0 : static_cast<uint8_t>(0x10 + std::clamp(volume, 0, 0x40));
    return nullptr;
  });
}

Result setModuleEffect(std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column, int efx, int param) {
  return editModuleCell(notes, t, g, row, column, [efx, param](tracker::Cell& c) -> const char* {
    const bool editable = (c.efx == 0 && c.param == 0) || c.efx == 0x08 || c.efx == 0x0C;
    if (!editable) return "This module effect stays as imported: only 8 (pan) and C (volume) are changed here";
    if (efx < 0) {
      c.efx = c.param = 0;
      return nullptr;
    }
    if (efx != 0x08 && efx != 0x0C) return "Module cells take the effects 8 (pan) and C (volume) here";
    c.efx = static_cast<uint8_t>(efx);
    c.param = static_cast<uint8_t>(efx == 0x0C ? std::clamp(param, 0, 0x40) : std::clamp(param, 0, 0xFF));
    return nullptr;
  });
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

// ---- R11-05: one voice per track, as the classic tracker plays a channel ----
// A track plays one voice when no two of its notes overlap or start together and none is an
// imported module voice (those keep their own rows). `notes` sorted (normalize).
bool oneVoice(const std::vector<Note>& notes) {
  for (size_t i = 0; i < notes.size(); ++i) {
    if (!notes[i].tracker.empty()) return false;
    if (i + 1 < notes.size() && (notes[i + 1].start <= notes[i].start || notes[i].start + notes[i].length > notes[i + 1].start)) return false;
  }
  return true;
}
// The notes that ring on to the next note (or to `end`, the pattern's or the block's end): no OFF
// before it. As (start + `shift`, pitch).
using TieSet = std::set<std::pair<Tick, int>>;
void ringingNotes(const std::vector<Note>& notes, Tick end, Tick shift, TieSet& out) {
  for (size_t i = 0; i < notes.size(); ++i) {
    // The last note keeps its length as the moves always kept it (whether it reached the end on
    // purpose cannot be told); a block's last note rings on to the block's end.
    const bool last = i + 1 >= notes.size();
    if (last && end <= 0) continue;
    const Tick next = last ? end : notes[i + 1].start;
    if (notes[i].start + notes[i].length >= next) out.insert({notes[i].start + shift, notes[i].pitch});
  }
}
// After rows moved: a note that rang on rings on to the next note (or the pattern's end); no note
// lasts past the next one's start (a second voice would open a second column).
void retie(std::vector<Note>& notes, const TieSet& ringing, Tick end) {
  normalize(notes);
  for (size_t i = 0; i < notes.size(); ++i) {
    Note& n = notes[i];
    const bool last = i + 1 >= notes.size();
    const Tick next = last ? end : notes[i + 1].start;
    if (next <= n.start) continue;  // a chord after a paste, a note past the end: left as they are
    if (ringing.count({n.start, n.pitch})) n.length = next - n.start, n.endFrac = 0;
    else if (!last && n.start + n.length > next) n.length = next - n.start, n.endFrac = 0;
  }
}

Result done(bool changed) {
  Result r;
  r.changed = changed;
  return r;
}

}  // namespace

int snapRecordRow(int row, float frac, int snap, int rows) {
  if (rows <= 0) return 0;
  if (snap <= 0) return std::clamp(row, 0, rows - 1);
  const double pos = static_cast<double>(row) + std::clamp(static_cast<double>(frac), 0.0, 1.0);
  const int r = static_cast<int>(std::floor(pos / snap + 0.5)) * snap;
  return r >= rows ? 0 : std::max(0, r);
}

Result putNote(std::vector<Note>& notes, const Track& t, const Grid& g, int row, int column, int pitch, int velocity) {
  if (row < 0 || row >= g.rows) return refuse("outside the pattern");
  if (pitch < 0 || pitch > 127) return refuse("that key is outside the note range");
  const Cell& c = t.at(row, column);
  if (c.kind == Cell::Step) return refuse("a rack step: change its key in Steps");
  if (int k = 0; moduleNoteAt(notes, t, g, row, column, &k) >= 0 && k > 0) return refuse(kInVoice);
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
  if (moduleNoteAt(notes, t, g, row, column, nullptr) >= 0) return refuse(kInVoice);
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
  if ((f == Field::Note || f == Field::Instrument) && c.kind == Cell::On && moduleNoteAt(notes, t, g, row, column, nullptr) >= 0)
    return refuse(kInVoice);
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
  if (voiceOver(notes, g, row, false)) return refuse(kInVoice);
  const Tick tick = g.tickOf(row);
  // R11-05: the notes that ring on keep ringing on to the next note once the rows moved.
  const bool voice = oneVoice(notes);
  TieSet ring;
  if (voice) {
    TieSet was;
    ringingNotes(notes, 0, 0, was);  // (its last note keeps its length)
    for (const auto& [at, pitch] : was) ring.insert({at >= tick ? at + g.rowTicks : at, pitch});
  }
  // R10-02 (was R9-06's refusal): as the classic insert, what the insert pushes off the last row is
  // dropped (in the same undo step; the caller says so), never kept out of sight past the end.
  // An imported voice it would push off is refused instead (its later cells belong to it).
  int dropped = 0;
  for (const Note& n : notes)
    if (n.start >= tick && n.start < g.length() && n.start + g.rowTicks >= g.length()) {
      if (!n.tracker.empty()) return refuse(kInVoice);
      ++dropped;
    }
  if (steps.size() > r && steps.back() != 0) ++dropped;
  if (dropped)
    notes.erase(std::remove_if(notes.begin(), notes.end(),
                               [&](const Note& n) { return n.start >= tick && n.start < g.length() && n.start + g.rowTicks >= g.length(); }),
                notes.end());
  bool changed = dropped > 0;
  for (Note& n : notes) {
    if (n.start >= tick) n.start += g.rowTicks, changed = true;
    // R12-02: an OFF on the row (a note ending where it starts) moves down with the row, as the
    // delete takes it away with the row.
    else if (n.start + n.length >= tick) n.length += g.rowTicks, changed = true;
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
  if (changed && voice) retie(notes, ring, g.length());
  Result res = done(changed);
  res.dropped = dropped;
  return res;
}

Result deleteRow(std::vector<Note>& notes, std::vector<uint8_t>& steps, std::vector<StepProps>& props, const Grid& g, int row) {
  if (row < 0 || row >= g.rows) return refuse("outside the pattern");
  if (voiceOver(notes, g, row, true)) return refuse(kInVoice);
  const Tick tick = g.tickOf(row), next = tick + g.rowTicks;
  // R11-05: as insertRow (a note whose next note went rings on to the one after, as the classic
  // tracker's channel does: no OFF appears where none was typed).
  const bool voice = oneVoice(notes);
  TieSet ring;
  if (voice) {
    TieSet was;
    ringingNotes(notes, 0, 0, was);  // (its last note keeps its length...)
    // R12-02: ...except one ending exactly at the pattern's end: it keeps reaching it (no OFF appears
    // on the last row). One running past the end (an OFF an insert pushed off the pattern) keeps its
    // length until it reaches the end, then rings on as one with no OFF does.
    if (!notes.empty() && notes.back().start + notes.back().length == g.length()) was.insert({notes.back().start, notes.back().pitch});
    for (const auto& [at, pitch] : was)
      if (at < tick || at >= next) ring.insert({at >= next ? at - g.rowTicks : at, pitch});
    // R12-02: an OFF on the deleted row goes with the row: its note rings on to the next one.
    for (const Note& n : notes)
      if (n.start < tick && n.start + n.length >= tick && n.start + n.length < next) ring.insert({n.start, n.pitch});
  }
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
  if (changed && voice) retie(notes, ring, g.length());
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

namespace {
// R7-06: a pasted note cuts the note before it that still sounds (one each), as a typed note does,
// so the block lands in the same column instead of opening a second one. `put` is sorted here.
void cutSounding(std::vector<Note>& notes, std::vector<Note>& put) {
  std::sort(put.begin(), put.end(), [](const Note& x, const Note& y) { return x.start < y.start; });
  const size_t had = notes.size();
  std::vector<bool> cut(had, false);
  for (const Note& p : put) {
    size_t best = had;
    for (size_t i = 0; i < had; ++i)
      if (!cut[i] && notes[i].start < p.start && notes[i].start + notes[i].length > p.start && (best == had || notes[i].start > notes[best].start)) best = i;
    if (best < had) notes[best].length = p.start - notes[best].start, cut[best] = true;
  }
}
}  // namespace

Result pasteRows(std::vector<Note>& notes, std::vector<uint8_t>& steps, const Grid& g, int row, int rows, const std::vector<Note>& block) {
  if (row < 0 || row >= g.rows || rows <= 0) return refuse("outside the pattern");
  const std::vector<Note> before = notes;
  const std::vector<uint8_t> stepsBefore = steps;
  const int last = std::min(g.rows, row + rows) - 1;
  const Tick at = g.tickOf(row), end = g.tickOf(g.rows);
  // R11-05: a one-voice track with a one-voice block stays one voice: the pasted notes and the
  // track's ring on to their next notes (a pasted note is cut by the next one), as typed notes do.
  std::vector<Note> sortedBlock = block;
  normalize(sortedBlock);
  const bool voice = oneVoice(notes) && oneVoice(sortedBlock);
  TieSet ring;
  if (voice) {
    TieSet was;
    ringingNotes(notes, 0, 0, was);  // (its last note keeps its length)
    const Tick b = g.tickOf(last + 1);
    for (const auto& k : was)
      if (k.first < at || k.first >= b) ring.insert(k);
    ringingNotes(sortedBlock, static_cast<Tick>(rows) * g.rowTicks, at, ring);
  }
  cutRows(notes, steps, g, row, last);
  // R7-03: what falls past the pattern's last row is dropped and a note ends at the pattern's end
  // (as FT2 clips a block to the rows left), so nothing hidden comes back when the pattern grows.
  std::vector<Note> put;
  for (Note n : block) {
    n.start += at;
    if (n.start >= end) continue;
    if (n.start + n.length > end) n.length = end - n.start;
    put.push_back(n);
  }
  cutSounding(notes, put);
  for (const Note& n : put) notes.push_back(n);
  normalize(notes);
  if (voice) retie(notes, ring, end);
  return done(notes != before || steps != stepsBefore);
}

// ---- clipboard masks ----
bool partEmpty(const Note& n, Part p) {
  switch (p) {
    case Part::Volume: return n.velocity == Note{}.velocity;
    case Part::Pan: return n.pan == 0;
    case Part::Effect: return effectsOf(n).empty();
    default: return false;
  }
}

void copyPart(const Note& from, Note& to, Part p) {
  switch (p) {
    case Part::Volume: to.velocity = from.velocity; break;
    case Part::Pan: to.pan = from.pan; break;
    case Part::Effect:
      to.slide = from.slide, to.porta = from.porta, to.fine = from.fine;
      to.modX = from.modX, to.modY = from.modY, to.release = from.release;
      break;
    default: break;
  }
}

void maskCopied(std::vector<Note>& block, const Masks& m) {
  const Note def{};
  for (Note& n : block)
    for (Part p : {Part::Volume, Part::Pan, Part::Effect})
      if (!m.copy[static_cast<int>(p)]) copyPart(def, n, p);
}

Result pasteRowsMasked(std::vector<Note>& notes, std::vector<uint8_t>& steps, const Grid& g, int row, int rows, const std::vector<Note>& block,
                       const bool carried[kParts], const Masks& m) {
  if (row < 0 || row >= g.rows || rows <= 0) return refuse("outside the pattern");
  bool put[kParts];
  for (int i = 0; i < kParts; ++i) put[i] = carried[i] && m.paste[i];
  const bool putNote = put[static_cast<int>(Part::Note)], noteClear = putNote && !m.transparent[static_cast<int>(Part::Note)];
  const std::vector<Note> before = notes;
  const std::vector<uint8_t> stepsBefore = steps;
  const Tick rt = std::max<Tick>(1, g.rowTicks);
  // The block's notes and the pattern's, by row (in start then pitch order).
  std::map<int, std::vector<Note>> src;
  for (const Note& n : block) src[static_cast<int>(n.start / rt)].push_back(n);
  const Tick a = g.tickOf(row), b = g.tickOf(row + rows);
  // R11-05: as pasteRows, when the notes themselves are pasted.
  std::vector<Note> sortedBlock = block;
  normalize(sortedBlock);
  const bool voice = putNote && oneVoice(notes) && oneVoice(sortedBlock);
  TieSet ring;
  if (voice) {
    TieSet was;
    ringingNotes(notes, 0, 0, was);  // (its last note keeps its length)
    for (const auto& k : was)
      if (k.first < a || k.first >= b) ring.insert(k);
    ringingNotes(sortedBlock, static_cast<Tick>(rows) * g.rowTicks, a, ring);
  }
  std::map<int, std::vector<size_t>> dst;
  for (size_t i = 0; i < notes.size(); ++i)
    if (notes[i].start >= a && notes[i].start < b) dst[static_cast<int>((notes[i].start - a) / rt)].push_back(i);
  auto takeParts = [&](const Note& from, Note& to) {
    for (Part p : {Part::Volume, Part::Pan, Part::Effect}) {
      const int k = static_cast<int>(p);
      if (!put[k] || (m.transparent[k] && partEmpty(from, p))) continue;
      copyPart(from, to, p);
    }
  };
  std::vector<bool> drop(notes.size(), false);
  std::vector<Note> added;
  for (int r = 0; r < rows && row + r < g.rows; ++r) {
    const auto si = src.find(r);
    const std::vector<Note> none;
    const std::vector<Note>& s = si == src.end() ? none : si->second;
    const auto di = dst.find(r);
    const std::vector<size_t> noIdx;
    const std::vector<size_t>& d = di == dst.end() ? noIdx : di->second;
    if (putNote) {
      if (s.empty() && !noteClear) continue;  // transparent: an empty row keeps what is there
      for (size_t k = 0; k < s.size(); ++k) {
        Note n = k < d.size() ? notes[d[k]] : Note{};
        n.start = s[k].start + a, n.length = s[k].length, n.pitch = s[k].pitch, n.frac = s[k].frac, n.endFrac = s[k].endFrac;
        n.bend = s[k].bend, n.step = s[k].step;
        if (const Tick pe = g.tickOf(g.rows); n.start + n.length > pe) n.length = std::max<Tick>(1, pe - n.start);  // R7-03: ends at the pattern end
        takeParts(s[k], n);
        added.push_back(n);
      }
      for (size_t k : d) drop[k] = true;
      const size_t sr = static_cast<size_t>(row + r);
      if (sr < steps.size() && steps[sr] && (noteClear || !s.empty())) steps[sr] = 0;
    } else {
      for (size_t k = 0; k < s.size() && k < d.size(); ++k) takeParts(s[k], notes[d[k]]);
    }
  }
  std::vector<Note> out;
  for (size_t i = 0; i < notes.size(); ++i)
    if (!drop[i]) out.push_back(notes[i]);
  if (putNote) cutSounding(out, added);  // R8-01: as pasteRows (R7-06), whatever the masks
  for (const Note& n : added) out.push_back(n);
  notes = std::move(out);
  normalize(notes);
  if (voice) retie(notes, ring, g.length());
  return done(notes != before || steps != stepsBefore);
}

int playedRows(const Song& s, const Pattern& p, const Grid& g) {
  bool module = false;
  for (const auto& [ch, notes] : p.notes) {
    for (const Note& n : notes)
      if (!n.tracker.empty()) {
        module = true;
        break;
      }
    if (module) break;
  }
  if (!module || g.rowTicks <= 0) return g.rows;
  Tick far = 0;
  bool any = false;
  for (const Clip& c : s.playlist) {
    if (c.kind != ClipKind::Pattern || c.pattern != p.id) continue;
    any = true;
    far = std::max(far, c.offset + s.clipLength(c));
  }
  if (!any || far >= g.length()) return g.rows;
  return std::clamp(static_cast<int>((far + g.rowTicks - 1) / g.rowTicks), 1, g.rows);
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

double patternPlayPos(const Song& s, ObjectId pattern, bool songMode, ObjectId playingPattern, double pos) {
  if (pattern == kNoId || pos < 0) return -1;
  if (!songMode) return playingPattern == pattern ? pos : -1;
  const double whole = std::floor(pos);
  const Tick at = patternTickAt(s, pattern, static_cast<Tick>(whole));
  return at < 0 ? -1 : static_cast<double>(at) + (pos - whole);
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
// The song from `from` on moves by `by`: its clips and its markers (the song loop marker, "Loop to",
// stays on the entry it names when entries before it grow, shrink, come or go). A marker at the very
// start stays there (the song's first meter and key belong to its start).
bool shiftFrom(Document& d, Tick from, Tick by, const char* label) {
  const bool moved = d.editClips(label, [&](std::vector<Clip>& cs) {
    for (Clip& c : cs)
      if (c.start >= from) c.start += by;
  });
  bool markers = false;
  for (const Marker& m : d.song().markers) markers = markers || (m.pos >= from && m.pos > 0);
  if (markers && by != 0)
    d.editSong(label, [&](Song& s) {
      for (Marker& m : s.markers)
        if (m.pos >= from && m.pos > 0) m.pos = std::max<Tick>(0, m.pos + by);
    });
  return moved || markers;
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

void setPatternLengthInSong(Document& d, ObjectId pattern, int bars, int stepsPerBar) {
  const Song& s = d.song();
  const Pattern* p = s.pattern(pattern);
  if (!p) return;
  const Tick before = s.patternLength(*p);
  // R9-01: the song is cut into blocks of clips that play together (overlapping in time; clips that
  // only touch start the next block). A block gets longer or shorter by what its own end moves: the
  // latest of its clips' ends, with the pattern's instances at their new length. An instance among
  // longer parallel clips moves nothing. (Before, each instance's end moved the rest of the song, so
  // an instance on a parallel track shifted it a second time and left a silent bar.)
  struct Item {
    Tick start, end;
    bool own;  // plays the pattern at its own length
  };
  std::vector<Item> items;
  for (const Clip& c : s.playlist)
    if (inSong(s, c)) items.push_back({c.start, c.start + s.clipLength(c), c.pattern == pattern && c.length == 0});
  std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.start < y.start; });
  struct Block {
    Tick start, end;
    std::vector<Item> items;
  };
  std::vector<Block> blocks;
  for (const Item& it : items) {
    if (blocks.empty() || it.start >= blocks.back().end) blocks.push_back({it.start, it.end, {}});
    blocks.back().end = std::max(blocks.back().end, it.end);
    blocks.back().items.push_back(it);
  }
  d.beginGroup("pattern length");
  d.setPatternLength(pattern, bars, stepsPerBar);
  const Pattern* q = d.song().pattern(pattern);
  const Tick delta = q ? d.song().patternLength(*q) - before : 0;
  if (delta != 0)
    for (size_t k = blocks.size(); k-- > 0;) {  // latest first: a shift never moves a block not yet looked at
      const Block& b = blocks[k];
      bool has = false;
      Tick end = b.start;
      for (const Item& it : b.items) {
        has = has || it.own;
        end = std::max(end, it.own ? it.start + before + delta : it.end);
      }
      if (has && end != b.end) shiftFrom(d, b.end, end - b.end, "pattern length");
    }
  d.endGroup();
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
