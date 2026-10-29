#include "PatternAutomation.h"
#include <algorithm>
#include <cmath>
#include <map>
#include "Arrangements.h"
#include "ClipLoop.h"
#include "Params.h"

namespace gl {

std::shared_ptr<const AutomationClip> patternLaneCurve(const PatternAutomation& lane) {
  return std::make_shared<const AutomationClip>(lane);  // J-173: range, loop and curve law come along
}

std::vector<AutomationLane> patternAutomationLanes(const Song& song) {
  std::map<std::pair<ObjectId, size_t>, std::shared_ptr<const AutomationClip>> curves;  // (pattern, lane) → curve
  std::map<std::pair<ObjectId, uint32_t>, AutomationLane> lanes;
  for (const auto& c : song.playlist) {
    if (c.kind != ClipKind::Pattern || c.muted || inLiveArea(song, c)) continue;
    if (c.track >= 0 && static_cast<size_t>(c.track) < song.tracks.size() && song.tracks[static_cast<size_t>(c.track)].muted) continue;
    const Pattern* p = song.pattern(c.pattern);
    if (!p || p->automation.empty()) continue;
    const std::vector<ClipPass> passes = clipPasses(song, c);  // Q-1: a looping pattern repeats its lanes
    for (size_t k = 0; k < p->automation.size(); ++k) {
      const PatternAutomation& pa = p->automation[k];
      if (pa.points.empty() || pa.owner == kNoId) continue;
      auto& cv = curves[{p->id, k}];
      if (!cv) cv = patternLaneCurve(pa);
      AutomationLane& l = lanes[{pa.owner, pa.index}];
      l.owner = pa.owner;
      l.index = pa.index;
      for (const ClipPass& ps : passes) l.inst.push_back({c.start + ps.at, c.start + ps.at + ps.len, ps.content, cv});
    }
  }
  std::vector<AutomationLane> out;
  for (auto& [k, l] : lanes) {
    std::stable_sort(l.inst.begin(), l.inst.end(), [](const auto& x, const auto& y) { return x.start < y.start; });
    for (size_t i = 0; i + 1 < l.inst.size(); ++i) l.inst[i].end = std::min(l.inst[i].end, l.inst[i + 1].start);
    out.push_back(std::move(l));
  }
  return out;
}

std::vector<AutomationLane> singlePatternLanes(const Pattern& p, Tick length, Tick offset, Tick loop) {
  std::vector<AutomationLane> out;
  for (const auto& pa : p.automation) {
    if (pa.points.empty() || pa.owner == kNoId) continue;
    AutomationLane l;
    l.owner = pa.owner;
    l.index = pa.index;
    const auto cv = patternLaneCurve(pa);
    for (const ClipPass& ps : loopPasses(offset, std::max<Tick>(1, length), loop)) l.inst.push_back({ps.at, ps.at + ps.len, ps.content, cv});
    out.push_back(std::move(l));
  }
  return out;
}

void setPatternLane(Pattern& p, ObjectId owner, uint32_t index, std::vector<AutoPoint> points) {
  std::stable_sort(points.begin(), points.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
  for (auto it = p.automation.begin(); it != p.automation.end(); ++it)
    if (it->owner == owner && it->index == index) {
      if (points.empty())
        p.automation.erase(it);
      else
        it->points = std::move(points);
      return;
    }
  if (points.empty()) return;
  p.automation.push_back(patternLane(owner, index, std::move(points)));
}

PatternAutomation patternLane(ObjectId owner, uint32_t index, std::vector<AutoPoint> points) {
  PatternAutomation c;
  c.name.clear();  // shown as the target's name
  c.owner = owner;
  c.index = index;
  c.flCurves = false;  // the event lane's (and the earlier lanes') curves
  c.points = std::move(points);
  return c;
}

const PatternAutomation* findPatternLane(const Pattern& p, ObjectId owner, uint32_t index) {
  for (const auto& l : p.automation)
    if (l.owner == owner && l.index == index) return &l;
  return nullptr;
}
PatternAutomation* findPatternLane(Pattern& p, ObjectId owner, uint32_t index) {
  for (auto& l : p.automation)
    if (l.owner == owner && l.index == index) return &l;
  return nullptr;
}

// ---- J-173 ------------------------------------------------------------------------

std::string patternAutoName(const Song& s, const PatternAutomation& c) {
  if (!c.name.empty()) return c.name;
  if (c.owner == kNoId) return "Automation";
  return paramDisplayName(s, ParamId{c.owner, c.index});
}

bool addPatternClip(Pattern& p, PatternAutomation c) {
  if (c.owner == kNoId || findPatternLane(p, c.owner, c.index)) return false;
  c.id = kNoId;
  std::stable_sort(c.points.begin(), c.points.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.at() < y.at(); });
  p.automation.push_back(std::move(c));
  return true;
}

bool removePatternClip(Pattern& p, ObjectId owner, uint32_t index) {
  for (auto it = p.automation.begin(); it != p.automation.end(); ++it)
    if (it->owner == owner && it->index == index) {
      p.automation.erase(it);
      return true;
    }
  return false;
}

PatternAutomation newPatternClip(const Song& s, ParamId target, float value, Tick length) {
  PatternAutomation c;
  c.name = paramDisplayName(s, target);
  c.owner = target.owner;
  c.index = target.index;
  c.flCurves = true;
  const float v = std::clamp(value, 0.0f, 1.0f);
  c.points = {AutoPoint{0, v, 0, AutoShape::Curve}, AutoPoint{std::max<Tick>(1, length), v, 0, AutoShape::Curve}};
  return c;
}

Tick patternClipEnd(const PatternAutomation& c) {
  const Tick end = c.points.empty() ? 0 : c.points.back().pos;
  return std::max(end, c.loop);
}

namespace {
// A point holding the clip's value at content tick t (clip units).
AutoPoint pointAt(const AutomationClip& src, Tick pos, double t, AutoShape shape) {
  AutoPoint q;
  q.pos = pos;
  q.value = static_cast<float>(std::clamp(automationValue(src, t), 0.0, 1.0));
  q.shape = shape;
  return q;
}
}  // namespace

PatternAutomation patternClipFromPlaylist(const Song& s, const AutomationClip& src, const Clip* placement, Tick at) {
  PatternAutomation c = src;
  c.id = kNoId;
  if (c.name.empty() || c.name == "Automation") c.name = patternAutoName(s, src);
  at = std::max<Tick>(0, at);
  c.points.clear();
  if (!placement) {
    // The content itself. A looping clip keeps its loop when it starts the
    // pattern (the loop counts from the pattern start); moved later, one cycle.
    for (AutoPoint q : src.points) {
      if (src.loop > 0 && q.pos > src.loop) break;
      q.pos += at;
      c.points.push_back(q);
    }
    c.loop = at == 0 ? src.loop : 0;
    return c;
  }
  // The window one placement plays: its passes over the (looping) content, unrolled.
  c.loop = 0;
  const Tick len = std::max<Tick>(1, s.clipLength(*placement));
  for (const ClipPass& ps : loopPasses(placement->offset, len, src.loop)) {
    const Tick base = at + ps.at;
    const Tick c0 = ps.content, c1 = ps.content + ps.len;
    bool startCovered = false;
    for (const AutoPoint& q : src.points) startCovered = startCovered || (q.pos == c0 && q.frac == 0.0f);
    if (!startCovered) c.points.push_back(pointAt(src, base, static_cast<double>(c0), AutoShape::Hold));
    Tick lastIn = -1;
    for (AutoPoint q : src.points) {
      if (q.pos < c0 || q.pos > c1 || (q.pos == c1 && q.frac > 0.0f)) continue;
      lastIn = q.pos;
      q.pos = base + (q.pos - c0);
      c.points.push_back(q);
    }
    // The window ends between two points: the value there (the cut segment ends straight).
    if (lastIn < c1 && !src.points.empty() && src.points.back().pos > c1)
      c.points.push_back(pointAt(src, base + ps.len, static_cast<double>(c1), AutoShape::Curve));
  }
  std::stable_sort(c.points.begin(), c.points.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.at() < y.at(); });
  return c;
}

AutomationClip playlistClipFromPattern(const Song& s, const PatternAutomation& c) {
  AutomationClip a = c;
  a.id = kNoId;
  a.name = patternAutoName(s, c);
  return a;
}

std::vector<AutoPoint> patternClipParamPoints(const PatternAutomation& c, Tick until) {
  const double lo = c.min, r = static_cast<double>(c.max) - static_cast<double>(c.min);
  auto map = [&](AutoPoint q) {
    if (lo == 0.0 && r == 1.0) return q;
    q.value = static_cast<float>(std::clamp(lo + q.exact() * r, 0.0, 1.0));
    q.fine = 0.0;
    return q;
  };
  std::vector<AutoPoint> out;
  if (c.loop <= 0) {
    for (const AutoPoint& q : c.points) out.push_back(map(q));
    return out;
  }
  // Looping content: each cycle plays the points before the loop length, then wraps.
  until = std::max(until, c.loop);
  for (Tick base = 0; base < until && out.size() < 100000; base += c.loop) {
    for (const AutoPoint& q : c.points) {
      if (q.pos >= c.loop) break;
      AutoPoint m = map(q);
      m.pos += base;
      out.push_back(m);
    }
    // the value the cycle reaches just before it wraps
    if (base + c.loop < until) {
      AutoPoint e;
      e.pos = base + c.loop;
      e.value = static_cast<float>(std::clamp(lo + pointsValue(c.points, static_cast<double>(c.loop), c.flCurves) * r, 0.0, 1.0));
      out.push_back(e);
    }
  }
  return out;
}

std::vector<AutoPoint> patternClipSteps(const PatternAutomation& c, Tick until, Tick step) {
  const std::vector<AutoPoint> pts = patternClipParamPoints(c, until);
  step = std::max<Tick>(1, step);
  const bool fl = c.flCurves;
  std::vector<AutoPoint> out;
  for (size_t k = 0; k < pts.size(); ++k) {
    if (k > 0 && pts[k].shape != AutoShape::Hold && pts[k].value != pts[k - 1].value) {
      // a moving segment: its value at every step inside it
      for (Tick t = (pts[k - 1].pos / step + 1) * step; t < pts[k].pos && out.size() < 200000; t += step) {
        AutoPoint q;
        q.pos = t;
        q.value = static_cast<float>(std::clamp(pointsValue(pts, static_cast<double>(t), fl), 0.0, 1.0));
        q.shape = AutoShape::Hold;
        if (out.empty() || out.back().value != q.value) out.push_back(q);
      }
    }
    AutoPoint q = pts[k];
    q.shape = AutoShape::Hold;
    q.tension = 0;
    out.push_back(q);
  }
  return out;
}

}  // namespace gl
