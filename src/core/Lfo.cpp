// Gridline - K-247: the LFO board (see Lfo.h).
#include "Lfo.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include "Document.h"
#include "I18n.h"
#include "Macros.h"
#include "Presets.h"
#include "Project.h"

namespace gl::lfo {

double rateBeats(int rateNote, int rateKind) {
  const double b = kRateBeats[std::clamp(rateNote, 0, kRateCount - 1)];
  return rateKind == 1 ? b * 1.5 : rateKind == 2 ? b * 2.0 / 3.0 : b;
}

std::string rateLabel(const SongLfo& l) {
  char buf[32];
  if (!l.tempoSync) {
    const double hz = l.rateHz;
    std::snprintf(buf, sizeof buf, hz < 1 ? "%.2f Hz" : hz < 10 ? "%.1f Hz" : "%.0f Hz", hz);
    return buf;
  }
  std::string s = kRateNames[std::clamp(l.rateNote, 0, kRateCount - 1)];
  if (l.rateKind == 1) s += " D";
  if (l.rateKind == 2) s += " T";
  return s;
}

double rateHz(const SongLfo& l, double bpm) {
  if (!l.tempoSync) return std::clamp(static_cast<double>(l.rateHz), static_cast<double>(kMinHz), static_cast<double>(kMaxHz));
  return std::max(1.0, bpm) / 60.0 / rateBeats(l);
}

const char* shapeName(LfoShape s) {
  switch (s) {
    case LfoShape::Sine: return i18n::N_("Sine");
    case LfoShape::Triangle: return i18n::N_("Triangle");
    case LfoShape::SawUp: return i18n::N_("Saw up");
    case LfoShape::SawDown: return i18n::N_("Saw down");
    case LfoShape::Square: return i18n::N_("Square");
    case LfoShape::RandomHold: return i18n::N_("Random hold");
    case LfoShape::RandomGlide: return i18n::N_("Random glide");
    case LfoShape::Drawn: return i18n::N_("Drawn");
    default: return "?";
  }
}
const char* shapeShortName(LfoShape s) { return shapeName(s); }

const char* restartName(LfoRestart r) {
  switch (r) {
    case LfoRestart::Free: return i18n::N_("Free");
    case LfoRestart::OnPlay: return i18n::N_("Play");
    case LfoRestart::EachNote: return i18n::N_("Note");
    case LfoRestart::EachBar: return i18n::N_("Bar");
    default: return "?";
  }
}

const char* kindName(LfoKind k) {
  switch (k) {
    case LfoKind::Chaos: return i18n::N_("Chaos");
    case LfoKind::Random: return i18n::N_("Random");
    default: return i18n::N_("LFO");
  }
}

const char* triggerName(RandomTrigger t) {
  switch (t) {
    case RandomTrigger::Tempo: return i18n::N_("Tempo");
    case RandomTrigger::Hz: return i18n::N_("Hz");
    case RandomTrigger::Note: return i18n::N_("Note");
    case RandomTrigger::Roll: return i18n::N_("Roll");
    default: return "?";
  }
}

const char* characterName(float c) { return c < 0.34f ? i18n::N_("Calm") : c < 0.67f ? i18n::N_("Lively") : i18n::N_("Wild"); }

const char* kindSummary(const SongLfo& l) {
  if (l.kind == LfoKind::Chaos) return characterName(l.character);
  if (l.kind == LfoKind::Random) return triggerName(l.trigger);
  return shapeShortName(l.shape);
}

std::string number(const Song& s, ObjectId id) {
  const SongLfo* l = s.lfo(id);
  if (!l) return "LFO";
  int n = 0;  // its place among the sources of its kind
  for (const SongLfo& o : s.lfos) {
    if (o.kind == l->kind) ++n;
    if (o.id == id) break;
  }
  return std::string(kindName(l->kind)) + " " + std::to_string(n);
}

std::string label(const Song& s, ObjectId id) {
  const SongLfo* l = s.lfo(id);
  if (!l) return "?";
  return l->name.empty() ? number(s, id) : number(s, id) + " " + l->name;
}

int nextColour(const Song& s) {
  bool used[kLfoColours] = {};
  for (const auto& l : s.lfos) used[std::clamp(l.colour, 0, kLfoColours - 1)] = true;
  for (int c = 0; c < kLfoColours; ++c)
    if (!used[c]) return c;
  return static_cast<int>(s.lfos.size() % kLfoColours);
}

bool isLfoParam(const Song& s, ParamId pid) {
  const SongLfo* l = s.lfo(pid.owner);
  if (!l || pid.index >= static_cast<uint32_t>(LfoParam::Count)) return false;
  return pid.index != static_cast<uint32_t>(LfoParam::Character) || l->kind == LfoKind::Chaos;
}

std::vector<LfoPoint> defaultPoints() { return {{0.0f, -0.6f, 0.4f}, {0.5f, 0.9f, -0.4f}, {0.8f, -0.1f, 0.0f}}; }

void buildTable(const SongLfo& l, DrawnTable& out) {
  std::vector<LfoPoint> pts = l.points.empty() ? defaultPoints() : l.points;
  if (pts.size() > static_cast<size_t>(kLfoMaxPoints)) pts.resize(static_cast<size_t>(kLfoMaxPoints));
  for (auto& p : pts) {
    p.x = std::clamp(p.x, 0.0f, 1.0f);
    p.y = std::clamp(p.y, -1.0f, 1.0f);
    p.curve = std::clamp(p.curve, -1.0f, 1.0f);
    if (p.kind > kLfoPointStep) p.kind = kLfoPointCurve;
  }
  std::stable_sort(pts.begin(), pts.end(), [](const LfoPoint& a, const LfoPoint& b) { return a.x < b.x; });
  const size_t n = pts.size();
  for (int i = 0; i < kTableSize; ++i) {
    const double x = static_cast<double>(i) / kTableSize;
    // The segment that holds x: from the last point at or before x (wrapping round from the last point).
    size_t k = n - 1;
    for (size_t j = 0; j < n; ++j)
      if (pts[j].x <= x) k = j;
    const LfoPoint& a = pts[k];
    const LfoPoint& b = pts[(k + 1) % n];
    double ax = a.x, bx = b.x;
    double xx = x;
    if (k + 1 >= n) bx += 1.0;  // the wrap to the first point of the next cycle
    if (xx < ax) xx += 1.0;     // before the first point: on the wrap segment
    const double span = bx - ax;
    double t = span > 1e-9 ? (xx - ax) / span : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    if (l.drawnSteps || n == 1 || a.kind == kLfoPointStep) {
      out[static_cast<size_t>(i)] = a.y;
      continue;
    }
    if (a.kind == kLfoPointSmooth) {  // 0.7.9: an S curve, flat at both ends
      t = 0.5 - 0.5 * std::cos(t * 3.141592653589793);
      out[static_cast<size_t>(i)] = static_cast<float>(a.y + (b.y - a.y) * t);
      continue;
    }
    const double c = a.curve;
    if (c > 0) t = std::pow(t, 1.0 + c * 4.0);
    else if (c < 0) t = 1.0 - std::pow(1.0 - t, 1.0 - c * 4.0);
    out[static_cast<size_t>(i)] = static_cast<float>(a.y + (b.y - a.y) * t);
  }
}

namespace {
std::vector<LfoPoint> sortedPoints(std::vector<LfoPoint> v) {
  for (auto& p : v) p.x = std::clamp(p.x, 0.0f, 1.0f), p.y = std::clamp(p.y, -1.0f, 1.0f), p.curve = std::clamp(p.curve, -1.0f, 1.0f);
  std::stable_sort(v.begin(), v.end(), [](const LfoPoint& a, const LfoPoint& b) { return a.x < b.x; });
  // Two points at one place: the later one wins.
  std::vector<LfoPoint> out;
  for (const auto& p : v) {
    if (!out.empty() && std::fabs(out.back().x - p.x) < 1e-5f) out.back() = p;
    else out.push_back(p);
  }
  if (out.empty()) out.push_back({0.0f, 0.0f, 0.0f, kLfoPointCurve});
  return out;
}
// The drawn shape's value at x (0..1) as it plays (its table).
float drawnAt(const SongLfo& src, const std::vector<LfoPoint>& pts, float x) {
  SongLfo l = src;
  l.points = pts;
  DrawnTable t{};
  buildTable(l, t);
  const double pos = std::clamp(static_cast<double>(x), 0.0, 1.0) * kTableSize;
  const int i = static_cast<int>(pos) % kTableSize;
  return t[static_cast<size_t>(i)];
}
}  // namespace

float snapX(float x, int steps) {
  if (steps < 2) return std::clamp(x, 0.0f, 1.0f);
  return std::clamp(std::round(x * static_cast<float>(steps)) / static_cast<float>(steps), 0.0f, 1.0f);
}

std::vector<LfoPoint> pointsFromShape(const SongLfo& l) {
  constexpr float kEnd = 1.0f - 1.0f / kTableSize;  // the last place before the cycle wraps
  switch (l.shape) {
    case LfoShape::Sine: return {{0.25f, 1.0f, 0.0f, kLfoPointSmooth}, {0.75f, -1.0f, 0.0f, kLfoPointSmooth}};
    case LfoShape::Triangle: return {{0.25f, 1.0f, 0.0f, kLfoPointCurve}, {0.75f, -1.0f, 0.0f, kLfoPointCurve}};
    case LfoShape::SawUp: return {{0.0f, -1.0f, 0.0f, kLfoPointCurve}, {kEnd, 1.0f, 0.0f, kLfoPointStep}};
    case LfoShape::SawDown: return {{0.0f, 1.0f, 0.0f, kLfoPointCurve}, {kEnd, -1.0f, 0.0f, kLfoPointStep}};
    case LfoShape::Square: return {{0.0f, 1.0f, 0.0f, kLfoPointStep}, {std::clamp(l.pulseWidth, 0.02f, 0.98f), -1.0f, 0.0f, kLfoPointStep}};
    case LfoShape::RandomHold:
    case LfoShape::RandomGlide: {
      const int n = std::clamp(l.randomSteps, 1, 16);
      std::vector<LfoPoint> v;
      for (int k = 0; k < n; ++k)
        v.push_back({static_cast<float>(k) / static_cast<float>(n), randomValue(l.seed, 0, static_cast<uint32_t>(k)), 0.0f,
                     l.shape == LfoShape::RandomHold ? kLfoPointStep : kLfoPointSmooth});
      return v;
    }
    default: break;
  }
  return l.points.empty() ? defaultPoints() : l.points;
}

std::vector<LfoPoint> flipPoints(const std::vector<LfoPoint>& in) {
  const std::vector<LfoPoint> pts = sortedPoints(in);
  const size_t n = pts.size();
  std::vector<LfoPoint> out;
  for (size_t k = 0; k < n; ++k) {
    // The line from point k to point k+1 runs backwards: from (1 - x[k+1]) to (1 - x[k]).
    const LfoPoint& a = pts[k];
    const LfoPoint& b = pts[(k + 1) % n];
    LfoPoint q;
    float x = 1.0f - b.x;
    if (k + 1 >= n) x = 1.0f - (b.x + 1.0f);  // the wrap segment
    q.x = x - std::floor(x);
    if (q.x >= 1.0f) q.x = 0.0f;
    q.kind = a.kind;
    q.y = a.kind == kLfoPointStep ? a.y : b.y;
    q.curve = a.kind == kLfoPointCurve ? -a.curve : 0.0f;
    out.push_back(q);
  }
  return sortedPoints(out);
}

std::vector<LfoPoint> invertPoints(const std::vector<LfoPoint>& in) {
  std::vector<LfoPoint> out = in;
  for (auto& p : out) p.y = -p.y;
  return out;
}

std::vector<LfoPoint> clearPoints() { return {{0.0f, 0.0f, 0.0f, kLfoPointCurve}}; }

std::vector<LfoPoint> drawStroke(const SongLfo& l, const std::vector<std::pair<float, float>>& stroke, int steps) {
  std::vector<LfoPoint> base = sortedPoints(l.points.empty() ? defaultPoints() : l.points);
  if (stroke.empty()) return base;
  const bool cells = steps >= 2;
  const int n = cells ? std::min(steps, 64) : 64;
  // Which cells the stroke crosses, and its value in each (later passes win).
  std::array<float, 64> val{};
  std::array<bool, 64> hit{};
  auto cellOf = [&](float x) { return std::clamp(static_cast<int>(std::floor(std::clamp(x, 0.0f, 1.0f) * static_cast<float>(n))), 0, n - 1); };
  auto put = [&](float x, float y) {
    const int c = cellOf(x);
    val[static_cast<size_t>(c)] = std::clamp(y, -1.0f, 1.0f);
    hit[static_cast<size_t>(c)] = true;
  };
  put(stroke.front().first, stroke.front().second);
  for (size_t i = 1; i < stroke.size(); ++i) {
    const auto [x0, y0] = stroke[i - 1];
    const auto [x1, y1] = stroke[i];
    const int c0 = cellOf(x0), c1 = cellOf(x1);
    const int dir = c1 >= c0 ? 1 : -1;
    for (int c = c0; c != c1 + dir; c += dir) {  // every cell between, at the stroke's height there
      const float cx = (static_cast<float>(c) + 0.5f) / static_cast<float>(n);
      const float t = std::fabs(x1 - x0) > 1e-6f ? std::clamp((cx - x0) / (x1 - x0), 0.0f, 1.0f) : 1.0f;
      val[static_cast<size_t>(c)] = std::clamp(y0 + (y1 - y0) * t, -1.0f, 1.0f);
      hit[static_cast<size_t>(c)] = true;
    }
    put(x1, y1);
  }
  // Keep the old points outside the touched cells; a point at the end of every touched run carries
  // the old shape on from there.
  std::vector<LfoPoint> out;
  auto inHit = [&](float x) { return hit[static_cast<size_t>(cellOf(x))] && x < 1.0f; };
  for (const auto& p : base)
    if (!inHit(p.x)) out.push_back(p);
  for (int c = 0; c < n; ++c) {
    if (!hit[static_cast<size_t>(c)]) continue;
    const float x = static_cast<float>(c) / static_cast<float>(n);
    out.push_back({x, val[static_cast<size_t>(c)], 0.0f, cells ? kLfoPointStep : kLfoPointCurve});
    // Where a run starts, the old shape holds up to it (a point just before it, then a step).
    const bool runStarts = c == 0 ? !hit[static_cast<size_t>(n - 1)] : !hit[static_cast<size_t>(c - 1)];
    if (runStarts) {
      const float px = (c == 0 ? 1.0f : x) - 1.0f / kTableSize;
      bool has = false;
      for (const auto& p : out) has = has || std::fabs(p.x - px) < 1e-5f;
      if (!has) out.push_back({px, drawnAt(l, base, px), 0.0f, kLfoPointStep});
    }
    const bool runEnds = c + 1 >= n ? !hit[0] : !hit[static_cast<size_t>(c + 1)];
    if (runEnds) {
      const float ex = static_cast<float>(c + 1) / static_cast<float>(n);
      const float e = ex >= 1.0f ? 0.0f : ex;
      bool has = false;
      for (const auto& p : out) has = has || std::fabs(p.x - e) < 1e-5f;
      if (!has) {
        // The old shape's value there and the kind of the old segment it sits in.
        uint8_t kind = kLfoPointCurve;
        for (const auto& p : base)
          if (p.x <= e) kind = p.kind;
        out.push_back({e, drawnAt(l, base, e), 0.0f, kind == kLfoPointStep ? kLfoPointStep : kLfoPointCurve});
      }
    }
  }
  out = sortedPoints(out);
  // Too many points: drop every other freehand one until it fits.
  while (out.size() > static_cast<size_t>(kLfoMaxPoints)) {
    std::vector<LfoPoint> thin;
    for (size_t i = 0; i < out.size(); ++i)
      if (i % 2 == 0 || !inHit(out[i].x)) thin.push_back(out[i]);
    if (thin.size() == out.size()) thin.resize(static_cast<size_t>(kLfoMaxPoints));
    out = std::move(thin);
  }
  return out;
}


double linkValue(const LinkShape& k, float v) {
  // The unit value (0..1) of the LFO's output, turned over when the link inverts it.
  double u = k.unipolar ? v : (static_cast<double>(v) + 1.0) * 0.5;
  if (k.invert) u = 1.0 - u;
  u = std::clamp(u, 0.0, 1.0);
  if (k.curve != ModCurve::Linear && k.curve != ModCurve::Formula) u = applyModCurve(k.curve, u);
  if (k.mode == kModAbsolute) return k.min + u * (k.max - k.min);
  double s;
  switch (k.direction) {
    case kModUpOnly: s = u; break;
    case kModDownOnly: s = -u; break;
    default: s = k.unipolar ? u : u * 2.0 - 1.0; break;
  }
  return static_cast<double>(k.amount) * s;
}

std::vector<const ModLink*> linksOf(const Song& s, ObjectId id) {
  std::vector<const ModLink*> out;
  for (const auto& l : s.modLinks)
    if (l.isLfo() && l.sourceOwner == id) out.push_back(&l);
  return out;
}

bool canTarget(const Song& s, ParamId target) {
  if (target.owner == kSongId || target.owner == kNoId) return false;
  const auto d = describeParam(s, target);
  if (!d) return false;
  if (isLfoParam(s, target)) return static_cast<LfoParam>(target.index) != LfoParam::RateNote;  // a choice (automation only)
  return true;
}

std::string targetLabel(const Song& s, ParamId target) {
  if (const SongLfo* l = s.lfo(target.owner)) {
    (void)l;
    const auto d = describeParam(s, target);
    return number(s, target.owner) + " · " + (d ? d->name : std::string("?"));
  }
  std::string n = paramDisplayName(s, target);
  for (size_t p = n.find(" › "); p != std::string::npos; p = n.find(" › ", p)) n.replace(p, std::string(" › ").size(), " · ");
  if (macro::isMacroParam(target)) {
    // "Lead · Macro 1 Brightness": the macro's number and its own name.
    const int k = static_cast<int>(target.index - kLayerMacroBase);
    const LayerMacro* m = macro::find(s, target.owner, k);
    std::string owner;
    if (const Channel* c = s.channel(target.owner)) owner = c->name;
    else if (int mi = s.mixerIndex(target.owner); mi >= 0) owner = mixerTrackName(s, static_cast<size_t>(mi));
    return owner + " · Macro " + std::to_string(k + 1) + (m && !m->name.empty() ? " " + m->name : std::string());
  }
  return n;
}

Rgb targetColour(const Song& s, ParamId target) {
  if (const Channel* c = s.channel(target.owner)) return c->color;
  if (const MixerTrack* m = s.mixerTrack(target.owner)) return m->color;
  return 0;
}

namespace {
// A node of the loop check: an LFO (its id, all bits of the index set) or a macro (its ParamId).
constexpr uint32_t kLfoNode = 0xFFFFFFFFu;
uint64_t lfoNode(ObjectId id) { return ParamId{id, kLfoNode}.packed(); }
// The node a link's target is (0: neither an LFO nor a macro).
uint64_t targetNode(const Song& s, ParamId t) {
  if (s.lfo(t.owner)) return lfoNode(t.owner);
  if (macro::isMacroParam(t) && macro::hasMacros(s, t.owner)) return t.packed();
  return 0;
}
uint64_t sourceNode(const ModLink& l) {
  if (l.isLfo()) return lfoNode(l.sourceOwner);
  if (l.sourceSlot == kLayerMacroSlot) return layerMacroParam(l.sourceOwner, l.sourceOutput).packed();
  return 0;
}
std::string nodeName(const Song& s, uint64_t node) {
  const ParamId p = ParamId::unpack(node);
  if (p.index == kLfoNode) return number(s, p.owner);
  return "Macro " + std::to_string(p.index - kLayerMacroBase + 1);
}
}  // namespace

bool wouldLoop(const Song& s, const ModLink& link, std::string* why) {
  const uint64_t from = sourceNode(link);
  const uint64_t to = targetNode(s, {link.owner, link.index});
  if (!from || !to) return false;
  // Does `to` reach `from` through the links already in the song?
  std::vector<std::pair<uint64_t, uint64_t>> todo{{to, 0}};
  std::set<uint64_t> seen;
  std::map<uint64_t, uint64_t> parent;
  bool loop = to == from;
  while (!todo.empty() && !loop) {
    const uint64_t n = todo.back().first;
    todo.pop_back();
    if (!seen.insert(n).second) continue;
    for (const auto& l : s.modLinks) {
      if (sourceNode(l) != n) continue;
      // The link `link` replaces (the same LFO's link to its target, or the target's other link).
      if (l.owner == link.owner && l.index == link.index && l.isLfo() == link.isLfo() && (!link.isLfo() || l.sourceOwner == link.sourceOwner))
        continue;
      const uint64_t t = targetNode(s, {l.owner, l.index});
      if (!t || seen.count(t)) continue;
      parent.emplace(t, n);
      if (t == from) {
        loop = true;
        break;
      }
      todo.push_back({t, n});
    }
  }
  if (loop && why) {
    // "This would make a loop: LFO 1 moves Macro 1, which moves LFO 1".
    std::vector<uint64_t> path{from};
    for (uint64_t n = from; n != to && parent.count(n);) {
      n = parent[n];
      path.push_back(n);
    }
    std::reverse(path.begin(), path.end());  // to ... from
    std::string m = "This would make a loop: " + nodeName(s, from) + " moves " + nodeName(s, to);
    for (size_t i = 1; i < path.size(); ++i) m += ", which moves " + nodeName(s, path[i]);
    if (to == from) m = "This would make a loop: " + nodeName(s, from) + " would move itself";
    *why = m;
  }
  return loop;
}

ModLink makeLink(ObjectId lfo, ParamId target, float amount) {
  ModLink l;
  l.sourceOwner = lfo;
  l.sourceSlot = kSongLfoSlot;
  l.sourceOutput = 0;
  l.owner = target.owner;
  l.index = target.index;
  l.mode = kModAround;
  l.amount = std::clamp(amount, -1.0f, 1.0f);
  l.min = 0;
  l.max = 1;
  l.enabled = true;
  return l;
}

SongLfo makeDefault(const Song& s, LfoKind kind) {
  SongLfo l;
  l.colour = nextColour(s);
  l.kind = kind;
  if (kind == LfoKind::Chaos) {
    l.rateNote = 3;  // 1/1: one bend a bar
    l.character = 0.3f;  // calm
    l.smoothMs = 20;
  } else if (kind == LfoKind::Random) {
    l.rateNote = 6;  // 1/8
    l.trigger = RandomTrigger::Tempo;
  }
  // A seed of its own, from its id (the same on every computer of a shared session).
  if (kind != LfoKind::Lfo) l.seed = static_cast<uint32_t>(s.peekId() * 2654435761u) | 1u;
  return l;
}

ObjectId addNew(Document& d, LfoKind kind) {
  const char* label = kind == LfoKind::Chaos ? "add chaos source" : kind == LfoKind::Random ? "add random source" : "add LFO";
  return d.addLfo(makeDefault(d.song(), kind), -1, label);
}

bool roll(Document& d, ObjectId id) {
  const SongLfo* l = d.song().lfo(id);
  if (!l || l->kind != LfoKind::Random) return false;
  return d.editLfo(id, "roll a random value", [](SongLfo& x) { ++x.roll; });
}

bool link(Document& d, ObjectId lfo, ParamId target, float amount) {
  const Song& s = d.song();
  if (!s.lfo(lfo) || !canTarget(s, target)) return false;
  ModLink l = makeLink(lfo, target, amount);
  if (const ModLink* cur = findLfoLink(s, lfo, target.owner, target.index)) l = *cur, l.enabled = true;
  return d.setModLink(l);
}

ObjectId newForKnob(Document& d, ParamId target, const SongLfo* preset, LfoKind kind) {
  const Song& s = d.song();
  if (!canTarget(s, target) || s.lfos.size() >= static_cast<size_t>(kMaxSongLfos)) return kNoId;
  SongLfo l = makeDefault(s, kind);
  if (preset) applyPreset(l, *preset);
  const char* label = l.kind == LfoKind::Chaos ? "new chaos for this knob" : l.kind == LfoKind::Random ? "new random for this knob" : "new LFO for this knob";
  d.beginGroup(label);
  const ObjectId id = d.addLfo(l, -1, label);
  const bool ok = id != kNoId && link(d, id, target);
  d.endGroup(false);
  if (!ok && id != kNoId) d.undo();
  return ok ? id : kNoId;
}

const std::vector<Preset>& presets() {
  static const std::vector<Preset> v = [] {
    std::vector<Preset> out;
    auto mk = [](LfoShape shape, int note, int kind, bool unipolar, float depth) {
      SongLfo l;
      l.shape = shape;
      l.rateNote = note;  // kRateNames: 1 "4 bars", 3 "1/1", 4 "1/2", 5 "1/4", 6 "1/8", 7 "1/16"
      l.rateKind = kind;
      l.unipolar = unipolar;
      l.depth = depth;
      return l;
    };
    auto add = [&out](const char* name, const SongLfo& l) {
      Preset p;
      p.name = name;
      p.lfo = l;
      p.factory = true;
      out.push_back(std::move(p));
    };
    SongLfo swell = mk(LfoShape::Triangle, 1, 0, true, 0.7f);  // 4 bars, growing in over a bar
    swell.fadeIn = 1;
    swell.smoothMs = 60;
    add(i18n::N_("Slow swell"), swell);
    SongLfo wobble = mk(LfoShape::Sine, 5, 0, false, 0.8f);
    wobble.smoothMs = 20;
    add(i18n::N_("Wobble 1/4"), wobble);
    SongLfo dotted = mk(LfoShape::Sine, 6, 1, false, 0.8f);  // 1/8 dotted
    dotted.smoothMs = 30;
    add(i18n::N_("Dotted wobble"), dotted);
    SongLfo trem = mk(LfoShape::Sine, 6, 2, true, 0.6f);  // 1/8 triplet
    add(i18n::N_("Triplet tremolo"), trem);
    SongLfo gate = mk(LfoShape::Square, 6, 0, true, 1.0f);
    gate.smoothMs = 4;
    add(i18n::N_("Gate 1/8"), gate);
    SongLfo ramp = mk(LfoShape::SawUp, 3, 0, true, 1.0f);  // one rise a bar
    ramp.smoothMs = 6;
    add(i18n::N_("Ramp up bar"), ramp);
    SongLfo steps = mk(LfoShape::RandomHold, 3, 0, false, 0.3f);  // a new value every 1/16 over a bar
    steps.randomSteps = 16;
    add(i18n::N_("Random steps"), steps);
    SongLfo drift = mk(LfoShape::RandomGlide, 2, 0, false, 0.35f);
    drift.randomSteps = 2;
    drift.restart = LfoRestart::Free;
    drift.tempoSync = false;
    drift.rateHz = 0.21f;
    drift.smoothMs = 80;
    add(i18n::N_("Drift"), drift);
    // Drawn: a bar of sixteenths, some of them silent.
    SongLfo stutter = mk(LfoShape::Drawn, 3, 0, true, 1.0f);
    {
      const int on[16] = {1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0};
      for (int k = 0; k < 16; ++k) stutter.points.push_back({static_cast<float>(k) / 16.0f, on[k] ? 1.0f : -1.0f, 0.0f, kLfoPointStep});
      stutter.smoothMs = 3;
    }
    add(i18n::N_("Stutter gate"), stutter);
    // Drawn: down on the beat, a quick climb back, then held up until the next beat.
    SongLfo pump = mk(LfoShape::Drawn, 5, 0, true, 1.0f);
    pump.points = {{0.0f, -1.0f, -0.6f, kLfoPointCurve}, {0.55f, 1.0f, 0.0f, kLfoPointStep}};
    pump.smoothMs = 2;
    add(i18n::N_("Sidechain pump"), pump);
    // Drawn: two soft bumps a cycle, the second smaller.
    SongLfo bounce = mk(LfoShape::Drawn, 4, 0, false, 0.8f);
    bounce.points = {{0.0f, -1.0f, 0.0f, kLfoPointSmooth}, {0.25f, 1.0f, 0.0f, kLfoPointSmooth}, {0.5f, -1.0f, 0.0f, kLfoPointSmooth},
                     {0.75f, 0.3f, 0.0f, kLfoPointSmooth}};
    add(i18n::N_("Bounce 1/2"), bounce);
    return out;
  }();
  return v;
}

SongLfo presetSettings(const SongLfo& in) {
  SongLfo l = in;
  const SongLfo d;
  l.id = kNoId;
  l.name.clear();
  l.colour = d.colour;
  l.on = d.on;
  l.noteChannel = kNoId;
  l.roll = 0;
  return l;
}

std::string presetFolder(const std::string& presetRoot) { return (std::filesystem::u8path(presetRoot) / "lfos").u8string(); }

std::string presetToText(const std::string& name, const SongLfo& l) {
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString("gridline-lfo");
  j.object["version"] = Json::makeNumber(1);
  j.object["name"] = Json::makeString(name);
  j.object["lfo"] = lfoToJson(presetSettings(l));
  return jsonStringify(j) + "\n";
}

bool presetFromText(const std::string& text, Preset& out, std::string* err) {
  Json j;
  if (!jsonParse(text, j) || !j.is(Json::Type::Object)) {
    if (err) *err = "not an LFO preset";
    return false;
  }
  if (j.text("format") != "gridline-lfo") {
    if (err) *err = "not an LFO preset";
    return false;
  }
  const Json* jl = j.find("lfo");
  SongLfo l;
  if (!jl || !jl->is(Json::Type::Object) || !lfoFromJson(*jl, l)) {
    if (err) *err = "the preset's settings are missing or of a newer kind";
    return false;
  }
  out = Preset{};
  out.name = j.text("name");
  out.lfo = presetSettings(l);
  return true;
}

bool loadPresetFile(const std::string& file, Preset& out, std::string* err) {
  std::ifstream in(std::filesystem::u8path(file), std::ios::binary);
  if (!in) {
    if (err) *err = "cannot read " + file;
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  if (ss.str().size() > (1u << 20)) {
    if (err) *err = "the file is too large for an LFO preset";
    return false;
  }
  if (!presetFromText(ss.str(), out, err)) return false;
  if (out.name.empty()) out.name = std::filesystem::u8path(file).stem().u8string();
  return true;
}

std::vector<Preset> listUserPresets(const std::string& presetRoot) {
  std::vector<Preset> out;
  if (presetRoot.empty()) return out;
  for (const PresetEntry& e : listLfoPresets(presetRoot)) {
    Preset p;
    if (!loadPresetFile(e.file, p)) continue;
    p.name = e.name;  // the file name is its name in the library
    p.folder = e.folder;
    p.file = e.file;
    p.factory = false;
    out.push_back(std::move(p));
  }
  return out;
}

std::vector<Preset> allPresets(const std::string& presetRoot) {
  std::vector<Preset> out = presets();
  for (Preset& p : listUserPresets(presetRoot)) out.push_back(std::move(p));
  return out;
}

bool saveUserPreset(const std::string& presetRoot, const SongLfo& l, const std::string& relName, std::string* path, std::string* err) {
  namespace fs = std::filesystem;
  fs::path f = fs::u8path(presetFolder(presetRoot));
  std::string part;
  std::vector<std::string> parts;
  for (char c : relName + "/") {
    if (c == '/' || c == '\\') {
      const size_t b0 = part.find_first_not_of(" \t"), b1 = part.find_last_not_of(" \t");
      part = b0 == std::string::npos ? std::string() : part.substr(b0, b1 - b0 + 1);
      if (!part.empty() && part != "." && part != "..") parts.push_back(safePresetName(part));
      part.clear();
    } else {
      part += c;
    }
  }
  parts.erase(std::remove_if(parts.begin(), parts.end(), [](const std::string& x) { return x.empty(); }), parts.end());
  if (parts.empty()) {
    if (err) *err = "the preset needs a name";
    return false;
  }
  for (size_t i = 0; i + 1 < parts.size(); ++i) f /= fs::u8path(parts[i]);
  f /= fs::u8path(parts.back() + ".json");
  std::error_code ec;
  fs::create_directories(f.parent_path(), ec);
  if (!writeFileAtomic(f.u8string(), presetToText(parts.back(), l), err)) return false;
  if (path) *path = f.u8string();
  return true;
}

bool deleteUserPreset(const Preset& p) {
  if (p.factory || p.file.empty()) return false;
  std::error_code ec;
  return std::filesystem::remove(std::filesystem::u8path(p.file), ec);
}

void applyPreset(SongLfo& l, const SongLfo& p) {
  const ObjectId id = l.id;
  const std::string name = l.name;
  const int colour = l.colour;
  const bool on = l.on;
  const ObjectId notes = l.noteChannel;
  l = p;
  l.id = id;
  l.name = name;
  l.colour = colour;
  l.on = on;
  l.noteChannel = notes;
}

}  // namespace gl::lfo
