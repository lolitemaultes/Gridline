// Gridline - K-247: the LFO board (see Lfo.h).
#include "Lfo.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include "Document.h"
#include "I18n.h"
#include "Macros.h"

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
    if (l.drawnSteps || n == 1) {
      out[static_cast<size_t>(i)] = a.y;
      continue;
    }
    const double c = a.curve;
    if (c > 0) t = std::pow(t, 1.0 + c * 4.0);
    else if (c < 0) t = 1.0 - std::pow(1.0 - t, 1.0 - c * 4.0);
    out[static_cast<size_t>(i)] = static_cast<float>(a.y + (b.y - a.y) * t);
  }
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
      l.rateNote = note;
      l.rateKind = kind;
      l.unipolar = unipolar;
      l.depth = depth;
      return l;
    };
    SongLfo wobble = mk(LfoShape::Sine, 6, 1, false, 0.8f);  // 1/8 dotted
    wobble.smoothMs = 30;
    out.push_back({i18n::N_("Dotted wobble"), wobble});
    SongLfo pump = mk(LfoShape::SawDown, 5, 0, true, 0.9f);  // 1/4, the knob at the bottom
    pump.smoothMs = 10;
    out.push_back({i18n::N_("Quarter pump"), pump});
    SongLfo drift = mk(LfoShape::RandomGlide, 2, 0, false, 0.35f);  // 2 bars
    drift.randomSteps = 2;
    drift.restart = LfoRestart::Free;
    drift.tempoSync = false;
    drift.rateHz = 0.21f;
    drift.smoothMs = 80;
    out.push_back({i18n::N_("Slow drift"), drift});
    SongLfo gate = mk(LfoShape::Square, 7, 0, true, 1.0f);  // 1/16
    gate.pulseWidth = 0.5f;
    gate.smoothMs = 4;
    out.push_back({i18n::N_("Sixteenth gate"), gate});
    SongLfo steps = mk(LfoShape::RandomHold, 3, 0, false, 0.3f);  // one value per 1/16 over a bar
    steps.randomSteps = 16;
    out.push_back({i18n::N_("Random steps"), steps});
    SongLfo swell = mk(LfoShape::Triangle, 1, 0, true, 0.7f);  // 4 bars
    swell.fadeIn = 1;
    swell.smoothMs = 60;
    out.push_back({i18n::N_("Long swell"), swell});
    return out;
  }();
  return v;
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
