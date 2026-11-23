// Gridline - K-255: lanes of their own grid and length (see Lanes.h).
#include "Lanes.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include "Document.h"

namespace gl::lanes {

namespace {
const PatternLane kDefaultLane{};

int64_t gcd64(int64_t a, int64_t b) {
  a = a < 0 ? -a : a;
  b = b < 0 ? -b : b;
  while (b) {
    const int64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

Tick barOf(const Song& s, const Pattern& p) { return std::max<Tick>(1, s.patternBarTicks(p)); }
}  // namespace

const PatternLane& of(const Pattern& p, ObjectId channel) {
  auto it = p.lanes.find(channel);
  return it == p.lanes.end() ? kDefaultLane : it->second;
}

int grid(const Pattern& p, ObjectId channel) {
  const int g = of(p, channel).grid;
  return g > 0 ? g : std::max(1, p.stepsPerBar);
}

bool ownGrid(const Pattern& p, ObjectId channel) { return of(p, channel).grid > 0; }

int steps(const Pattern& p, ObjectId channel) { return std::max(1, p.bars) * grid(p, channel); }

int length(const Pattern& p, ObjectId channel) {
  auto it = p.stepLoop.find(channel);
  if (it == p.stepLoop.end() || it->second <= 0) return 0;
  return it->second < steps(p, channel) ? it->second : 0;
}

LaneMode mode(const Pattern& p, ObjectId channel) { return of(p, channel).mode; }

bool runsOn(const Pattern& p, ObjectId channel) { return mode(p, channel) == LaneMode::RunOn && length(p, channel) > 0; }

bool any(const Pattern& p) {
  for (const auto& [ch, l] : p.lanes)
    if (!l.isDefault()) return true;
  return false;
}

bool anyRunOn(const Pattern& p) {
  for (const auto& [ch, l] : p.lanes)
    if (l.mode == LaneMode::RunOn && length(p, ch) > 0) return true;
  return false;
}

bool plain(const Pattern& p, ObjectId channel) { return !ownGrid(p, channel) && !runsOn(p, channel); }

double stepTick(const Song& song, const Pattern& p, ObjectId channel, int64_t s) {
  if (!ownGrid(p, channel)) return static_cast<double>(s * song.ticksPerStep(p));
  const int64_t g = grid(p, channel);
  const Tick bar = barOf(song, p);
  // Whole bars exactly, then the step's place in its bar (one division: no drift over long lanes).
  const int64_t whole = s >= 0 ? s / g : -((-s + g - 1) / g);
  const int64_t in = s - whole * g;
  return static_cast<double>(whole * bar) + static_cast<double>(in * bar) / static_cast<double>(g);
}

double stepTicks(const Song& song, const Pattern& p, ObjectId channel) {
  if (!ownGrid(p, channel)) return static_cast<double>(song.ticksPerStep(p));
  return static_cast<double>(barOf(song, p)) / static_cast<double>(grid(p, channel));
}

int64_t stepsPerPass(const Song& song, const Pattern& p, ObjectId channel) {
  const Tick len = std::max<Tick>(1, song.patternContentLength(p));
  if (!ownGrid(p, channel)) {
    const Tick st = std::max<Tick>(1, song.ticksPerStep(p));
    return std::max<int64_t>(1, len / st);
  }
  const Tick bar = barOf(song, p);
  return std::max<int64_t>(1, (len / bar) * grid(p, channel));
}

int64_t phaseOf(const Song& song, const Pattern& p, ObjectId channel, int64_t pass) {
  if (pass <= 0 || !runsOn(p, channel)) return 0;
  const int64_t loop = length(p, channel);
  const int64_t spp = stepsPerPass(song, p, channel) % loop;
  return ((pass % loop) * spp) % loop;
}

int64_t lineUpPasses(const Song& song, const Pattern& p, int64_t cap) {
  int64_t l = 1;
  for (const auto& [ch, lane] : p.lanes) {
    if (!runsOn(p, ch)) continue;
    const int64_t loop = length(p, ch);
    const int64_t spp = stepsPerPass(song, p, ch) % loop;
    const int64_t period = spp == 0 ? 1 : loop / gcd64(loop, spp);
    l = l / gcd64(l, period) * period;
    if (l >= cap) return cap;
  }
  return std::max<int64_t>(1, l);
}

int64_t lineUpBars(const Song& song, const Pattern& p, int64_t cap) {
  const Tick bar = barOf(song, p);
  const int64_t passBars = std::max<int64_t>(1, std::max<Tick>(1, song.patternContentLength(p)) / bar);
  const int64_t n = lineUpPasses(song, p, cap);
  return n >= cap / passBars ? cap : n * passBars;
}

double passTicks(const Song& song, const Pattern& p, ObjectId channel) {
  if (runsOn(p, channel)) return static_cast<double>(stepsPerPass(song, p, channel)) * stepTicks(song, p, channel);
  if (!ownGrid(p, channel)) return static_cast<double>(static_cast<Tick>(steps(p, channel)) * song.ticksPerStep(p));
  return static_cast<double>(std::max(1, p.bars)) * static_cast<double>(barOf(song, p));
}

std::vector<StepHit> playedSteps(const Song& song, const Pattern& p, ObjectId channel, int64_t pass) {
  std::vector<StepHit> out;
  auto it = p.steps.find(channel);
  if (it == p.steps.end()) return out;
  const std::vector<uint8_t>& row = it->second;
  if (!ownGrid(p, channel) && song.ticksPerStep(p) <= 0) return out;
  const int64_t loop = length(p, channel);
  const int64_t span = runsOn(p, channel) ? stepsPerPass(song, p, channel) : steps(p, channel);
  const int64_t phase = phaseOf(song, p, channel, pass);
  for (int64_t s = 0; s < span; ++s) {
    const int64_t src = loop ? (s + phase) % loop : s;
    if (src >= static_cast<int64_t>(row.size()) || row[static_cast<size_t>(src)] == 0) continue;
    out.push_back({s, static_cast<int>(src), stepTick(song, p, channel, s)});
  }
  return out;
}

std::vector<NoteRepeat> playedNotes(const Song& song, const Pattern& p, ObjectId channel, int64_t pass) {
  std::vector<NoteRepeat> out;
  auto it = p.notes.find(channel);
  if (it == p.notes.end()) return out;
  const int64_t loop = length(p, channel);
  if (loop <= 0) {
    for (const Note& n : it->second) out.push_back({&n, 0.0});
    return out;
  }
  const double st = stepTicks(song, p, channel);
  if (!(st > 0)) return out;
  const double loopT = static_cast<double>(loop) * st;
  const double passLen = passTicks(song, p, channel);
  const double back = static_cast<double>(phaseOf(song, p, channel, pass)) * st;
  for (const Note& n : it->second) {
    const double at = static_cast<double>(n.start);
    if (at >= loopT) continue;  // notes past the loop do not play (as 0.7.7)
    for (int64_t m = 0;; ++m) {
      const double off = static_cast<double>(m) * loopT - back;
      if (at + off >= passLen) break;
      if (at + off < 0) continue;
      out.push_back({&n, off});
    }
  }
  return out;
}

// ---------------------------------------------------------------- re-gridding ----

void regridRow(std::vector<uint8_t>& row, std::vector<StepProps>& props, int bars, int from, int to, Regrid how) {
  bars = std::max(1, bars);
  from = std::max(1, from);
  to = std::max(1, to);
  const size_t n = static_cast<size_t>(bars) * static_cast<size_t>(to);
  std::vector<uint8_t> out(n, 0);
  std::vector<StepProps> outProps(props.empty() ? 0 : n);
  for (size_t j = 0; j < row.size(); ++j) {
    if (row[j] == 0) continue;
    size_t k = j;
    if (how == Regrid::Nearest) {
      // round(j * to / from), halves up, in integers
      k = static_cast<size_t>((2 * static_cast<int64_t>(j) * to + from) / (2 * static_cast<int64_t>(from)));
      if (k >= n) k = n - 1;
    }
    if (k >= n) continue;  // Order: past the new row's end
    if (out[k] >= row[j]) continue;  // two on one step: the louder stays
    out[k] = row[j];
    if (!outProps.empty() && j < props.size()) outProps[k] = props[j];
  }
  row = std::move(out);
  bool anyProps = false;
  for (const auto& sp : outProps) anyProps = anyProps || !sp.isDefault();
  props = anyProps ? std::move(outProps) : std::vector<StepProps>{};
}

namespace {

void eraseDefault(Pattern& q, ObjectId ch) {
  if (auto it = q.lanes.find(ch); it != q.lanes.end() && it->second.isDefault()) q.lanes.erase(it);
}

// One channel to grid `want` (0: the pattern's), in place.
bool regridChannel(Pattern& q, ObjectId ch, int want, Regrid how) {
  const int from = grid(q, ch);
  const int to = want > 0 ? want : std::max(1, q.stepsPerBar);
  const int oldOwn = of(q, ch).grid;
  const int newOwn = want > 0 && want != q.stepsPerBar ? want : 0;
  if (oldOwn == newOwn) return false;
  if (from != to) {
    auto st = q.steps.find(ch);
    std::vector<uint8_t> row = st != q.steps.end() ? st->second : std::vector<uint8_t>{};
    auto pt = q.stepProps.find(ch);
    std::vector<StepProps> props = pt != q.stepProps.end() ? pt->second : std::vector<StepProps>{};
    if (!row.empty()) {
      regridRow(row, props, q.bars, from, to, how);
      if (std::all_of(row.begin(), row.end(), [](uint8_t v) { return v == 0; }))
        q.steps.erase(ch);
      else
        q.steps[ch] = std::move(row);
      if (props.empty())
        q.stepProps.erase(ch);
      else
        q.stepProps[ch] = std::move(props);
    }
    if (auto lp = q.stepLoop.find(ch); lp != q.stepLoop.end()) {
      int n = lp->second;
      if (how == Regrid::Nearest) n = static_cast<int>((2 * static_cast<int64_t>(n) * to + from) / (2 * static_cast<int64_t>(from)));
      const int total = std::max(1, q.bars) * to;
      if (n <= 0 || n >= total)
        q.stepLoop.erase(lp);
      else
        lp->second = n;
    }
  }
  q.lanes[ch].grid = newOwn;
  eraseDefault(q, ch);
  return true;
}

}  // namespace

bool setGrid(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, int g, Regrid how) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p || channels.empty()) return false;
  g = g <= 0 ? 0 : std::clamp(g, 1, std::max(kMaxLaneGrid, p->stepsPerBar));
  Pattern probe = *p;
  bool change = false;
  for (ObjectId ch : channels)
    if (d.song().channel(ch)) change = regridChannel(probe, ch, g, how) || change;
  if (!change) return false;
  return d.editPattern(pattern, g > 0 && g != p->stepsPerBar ? "set row grid" : "row back to the pattern grid", [&](Pattern& q) {
    for (ObjectId ch : channels)
      if (d.song().channel(ch)) regridChannel(q, ch, g, how);
  });
}

bool gridLikeAll(Document& d, ObjectId pattern, ObjectId like, Regrid how) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p) return false;
  const int g = of(*p, like).grid;
  std::vector<ObjectId> chs;
  for (const auto& c : d.song().channels)
    if (c.id != like && (p->steps.count(c.id) || p->notes.count(c.id) || p->lanes.count(c.id))) chs.push_back(c.id);
  return setGrid(d, pattern, chs, g, how);
}

bool setMode(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, LaneMode m) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p) return false;
  bool change = false;
  for (ObjectId ch : channels) change = change || (d.song().channel(ch) && mode(*p, ch) != m);
  if (!change) return false;
  return d.editPattern(pattern, m == LaneMode::RunOn ? "lane runs on" : "lane restarts with the pattern", [&](Pattern& q) {
    for (ObjectId ch : channels) {
      if (!d.song().channel(ch)) continue;
      q.lanes[ch].mode = m;
      eraseDefault(q, ch);
    }
  });
}

bool setLength(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, int n, const LaneMode* m, uint64_t mergeKey) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p || channels.empty()) return false;
  auto want = [&](ObjectId ch) {
    const int total = steps(*p, ch);
    return n <= 0 || n >= total ? 0 : n;
  };
  bool change = false;
  for (ObjectId ch : channels) {
    if (!d.song().channel(ch)) continue;
    auto it = p->stepLoop.find(ch);
    const int cur = it == p->stepLoop.end() ? 0 : it->second;
    change = change || cur != want(ch) || (m && mode(*p, ch) != *m);
  }
  if (!change) return false;
  std::vector<std::pair<ObjectId, int>> todo;
  for (ObjectId ch : channels)
    if (d.song().channel(ch)) todo.push_back({ch, want(ch)});
  return d.editPattern(
      pattern, n > 0 ? "set lane length" : "stop looping the lane",
      [&](Pattern& q) {
        for (const auto& [ch, k] : todo) {
          if (k > 0)
            q.stepLoop[ch] = k;
          else
            q.stepLoop.erase(ch);
          if (m) {
            q.lanes[ch].mode = *m;
            eraseDefault(q, ch);
          }
        }
      },
      mergeKey);
}

// ---------------------------------------------------------------------- burn ----

namespace {

// A note that plays what step `src` of a row plays, from `on` for `len` ticks (exact).
Note stepNote(const Pattern& q, ObjectId ch, int src, double on, double len, uint8_t vel) {
  StepProps sp;
  if (auto pt = q.stepProps.find(ch); pt != q.stepProps.end() && static_cast<size_t>(src) < pt->second.size())
    sp = pt->second[static_cast<size_t>(src)];
  on += len * static_cast<double>(sp.shift) / static_cast<double>(kStepShiftMax);
  const double end = on + len;
  Note n;
  n.start = static_cast<Tick>(std::floor(on));
  n.frac = static_cast<float>(on - std::floor(on));
  if (n.frac >= 1.0f) n.start += 1, n.frac = 0;
  const Tick endT = static_cast<Tick>(std::floor(end));
  n.length = std::max<Tick>(1, endT - n.start);
  n.endFrac = static_cast<float>(end - static_cast<double>(n.start + n.length));
  if (n.endFrac <= -1.0f || n.endFrac >= 1.0f) n.endFrac = 0;
  n.pitch = sp.pitch < 0 ? 60 : sp.pitch;
  n.velocity = std::clamp<int>(vel, 1, 127);
  n.pan = sp.pan;
  n.fine = sp.fine;
  n.release = sp.release;
  n.modX = sp.modX;
  n.modY = sp.modY;
  return n;
}

void placeNote(std::vector<Note>& out, const Note& src, double offset) {
  Note k = src;
  const double at = static_cast<double>(src.start) + static_cast<double>(src.frac) + offset;
  const double whole = std::floor(at);
  k.start = static_cast<Tick>(whole);
  k.frac = static_cast<float>(at - whole);
  if (k.frac >= 1.0f) k.start += 1, k.frac = 0;
  // The end keeps its place too (whole-tick offsets keep length and endFrac as they are).
  const double end = static_cast<double>(src.start + src.length) + static_cast<double>(src.endFrac) + offset;
  k.length = std::max<Tick>(1, static_cast<Tick>(std::floor(end)) - k.start);
  k.endFrac = static_cast<float>(end - static_cast<double>(k.start + k.length));
  if (k.endFrac <= -1.0f || k.endFrac >= 1.0f) k.endFrac = 0;
  out.push_back(std::move(k));
}

void sortNotes(std::vector<Note>& v) {
  std::stable_sort(v.begin(), v.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
}

// The channel's content as it plays over passes [0, passes), written plainly into `q` (which
// becomes `passes` x the pattern's pass long when passes > 1). `p` is the pattern before.
void writeOut(const Song& s, const Pattern& p, Pattern& q, ObjectId ch, int64_t passes, Tick passLen) {
  const bool own = ownGrid(p, ch);
  const Tick tps = std::max<Tick>(1, s.ticksPerStep(p));
  const double st = stepTicks(s, p, ch);
  const auto rowIt = p.steps.find(ch);
  const auto propIt = p.stepProps.find(ch);
  std::vector<uint8_t> row;
  std::vector<StepProps> props;
  const size_t rowLen = static_cast<size_t>(q.bars) * static_cast<size_t>(std::max(1, q.stepsPerBar));
  std::vector<Note> notes;
  for (int64_t k = 0; k < passes; ++k) {
    const double base = static_cast<double>(k * passLen);
    for (const StepHit& h : playedSteps(s, p, ch, k)) {
      const uint8_t vel = rowIt->second[static_cast<size_t>(h.src)];
      if (own) {
        notes.push_back(stepNote(p, ch, h.src, base + h.on, st, vel));
        continue;
      }
      const size_t at = static_cast<size_t>((static_cast<Tick>(base) + static_cast<Tick>(h.on)) / tps);
      if (at >= rowLen) continue;
      if (row.size() < rowLen) row.resize(rowLen, 0);
      row[at] = vel;
      if (propIt != p.stepProps.end() && static_cast<size_t>(h.src) < propIt->second.size() && !propIt->second[static_cast<size_t>(h.src)].isDefault()) {
        if (props.size() < rowLen) props.resize(rowLen);
        props[at] = propIt->second[static_cast<size_t>(h.src)];
      }
    }
    for (const NoteRepeat& r : playedNotes(s, p, ch, k)) placeNote(notes, *r.note, base + r.offset);
  }
  if (row.empty())
    q.steps.erase(ch);
  else
    q.steps[ch] = std::move(row);
  if (props.empty())
    q.stepProps.erase(ch);
  else
    q.stepProps[ch] = std::move(props);
  sortNotes(notes);
  if (notes.empty())
    q.notes.erase(ch);
  else
    q.notes[ch] = std::move(notes);
  q.stepLoop.erase(ch);
  q.lanes.erase(ch);
}

template <typename Fn>
void eachPlaylist(Song& s, Fn fn) {
  fn(s.playlist);
  for (Arrangement& a : s.arrangements) fn(a.playlist);
}

}  // namespace

int burnPattern(Song& s, ObjectId pattern, const std::vector<ObjectId>& channels, bool* cut) {
  if (cut) *cut = false;
  const Pattern* pp = s.pattern(pattern);
  if (!pp) return 0;
  const Pattern p = *pp;
  std::vector<ObjectId> chosen;
  auto burnable = [&](ObjectId ch) { return s.channel(ch) && (length(p, ch) > 0 || ownGrid(p, ch)); };
  if (channels.empty()) {
    for (const auto& c : s.channels)
      if (burnable(c.id)) chosen.push_back(c.id);
  } else {
    for (ObjectId ch : channels)
      if (burnable(ch)) chosen.push_back(ch);
  }
  if (chosen.empty()) return 0;
  bool extend = false;
  for (ObjectId ch : chosen) extend = extend || runsOn(p, ch);
  const Tick passLen = std::max<Tick>(1, s.patternContentLength(p));
  const Tick bar = barOf(s, p);
  int64_t passes = 1;
  if (extend) {
    const int64_t passBars = std::max<int64_t>(1, passLen / bar);
    const int64_t want = lineUpPasses(s, p, kMaxBurnBars + 1);
    const int64_t room = std::max<int64_t>(1, kMaxBurnBars / passBars);
    passes = std::min(want, room);
    if (cut) *cut = want > room;
    extend = passes > 1;
  }
  Pattern q = p;
  if (!extend) {
    for (ObjectId ch : chosen) writeOut(s, p, q, ch, 1, passLen);
    *s.pattern(pattern) = std::move(q);
    return static_cast<int>(chosen.size());
  }
  // Lengthened: every channel (and the pattern's automation) is written out over the passes, so
  // what restarted with the pattern still restarts where it did.
  q.bars = static_cast<int>(std::clamp<int64_t>(passes * passLen / bar, 1, kMaxPatternBars));
  int burnt = 0;
  for (const auto& c : s.channels) {
    if (!p.steps.count(c.id) && !p.notes.count(c.id) && !p.lanes.count(c.id) && !p.stepLoop.count(c.id)) continue;
    writeOut(s, p, q, c.id, passes, passLen);
    if (burnable(c.id)) ++burnt;
  }
  for (PatternAutomation& lane : q.automation) {
    std::vector<AutoPoint> pts;
    for (int64_t k = 0; k < passes; ++k)
      for (const AutoPoint& pt : lane.points)
        if (pt.pos < passLen || (k + 1 == passes && pt.pos == passLen)) {
          AutoPoint x = pt;
          x.pos += static_cast<Tick>(k) * passLen;
          pts.push_back(x);
        }
    lane.points = std::move(pts);
  }
  // Clips keep what they played: a clip of the pattern's own length keeps that length, and a clip
  // that did not loop stops where the pattern used to end.
  eachPlaylist(s, [&](std::vector<Clip>& list) {
    for (Clip& c : list) {
      if (c.kind != ClipKind::Pattern || c.pattern != pattern) continue;
      if (c.length <= 0) c.length = passLen;
      if (!c.loop && c.offset + c.length > passLen) c.length = std::max<Tick>(1, passLen - c.offset);
    }
  });
  *s.pattern(pattern) = std::move(q);
  return burnt;
}

int burn(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, bool* cut) {
  Song probe = d.song();
  bool c = false;
  const int n = burnPattern(probe, pattern, channels, &c);
  if (cut) *cut = c;
  if (n <= 0) return 0;
  d.editSong("burn lanes into the pattern", [&](Song& s) { burnPattern(s, pattern, channels); });
  return n;
}

std::vector<Note> rowAsNotes(const Song& s, const Pattern& p, ObjectId channel) {
  std::vector<Note> out;
  auto it = p.steps.find(channel);
  if (it == p.steps.end()) return out;
  const double st = stepTicks(s, p, channel);
  for (size_t k = 0; k < it->second.size(); ++k)
    if (it->second[k] > 0)
      out.push_back(stepNote(p, channel, static_cast<int>(k), stepTick(s, p, channel, static_cast<int64_t>(k)), st, it->second[k]));
  return out;
}

bool needsBurn(const Song& s) {
  for (const auto& p : s.patterns) {
    for (const auto& [ch, l] : p.lanes)
      if (l.grid > 0) return true;
    for (const auto& [ch, n] : p.stepLoop)
      if (length(p, ch) > 0) return true;
  }
  return false;
}

int burnSong(Song& s) {
  int n = 0;
  std::vector<ObjectId> ids;
  for (const auto& p : s.patterns) ids.push_back(p.id);
  for (ObjectId id : ids) n += burnPattern(s, id);
  return n;
}

}  // namespace gl::lanes
