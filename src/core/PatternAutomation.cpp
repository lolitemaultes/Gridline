#include "PatternAutomation.h"
#include <algorithm>
#include <map>
#include "Arrangements.h"

namespace gl {

std::shared_ptr<const AutomationClip> patternLaneCurve(const PatternAutomation& lane) {
  auto a = std::make_shared<AutomationClip>();
  a->owner = lane.owner;
  a->index = lane.index;
  a->min = 0;
  a->max = 1;
  a->points = lane.points;
  return a;
}

std::vector<AutomationLane> patternAutomationLanes(const Song& song) {
  std::map<std::pair<ObjectId, size_t>, std::shared_ptr<const AutomationClip>> curves;  // (pattern, lane) → curve
  std::map<std::pair<ObjectId, uint32_t>, AutomationLane> lanes;
  for (const auto& c : song.playlist) {
    if (c.kind != ClipKind::Pattern || c.muted || inLiveArea(song, c)) continue;
    if (c.track >= 0 && static_cast<size_t>(c.track) < song.tracks.size() && song.tracks[static_cast<size_t>(c.track)].muted) continue;
    const Pattern* p = song.pattern(c.pattern);
    if (!p || p->automation.empty()) continue;
    const Tick len = song.clipLength(c);
    for (size_t k = 0; k < p->automation.size(); ++k) {
      const PatternAutomation& pa = p->automation[k];
      if (pa.points.empty() || pa.owner == kNoId) continue;
      auto& cv = curves[{p->id, k}];
      if (!cv) cv = patternLaneCurve(pa);
      AutomationLane& l = lanes[{pa.owner, pa.index}];
      l.owner = pa.owner;
      l.index = pa.index;
      l.inst.push_back({c.start, c.start + len, c.offset, cv});
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

std::vector<AutomationLane> singlePatternLanes(const Pattern& p, Tick length, Tick offset) {
  std::vector<AutomationLane> out;
  for (const auto& pa : p.automation) {
    if (pa.points.empty() || pa.owner == kNoId) continue;
    AutomationLane l;
    l.owner = pa.owner;
    l.index = pa.index;
    l.inst.push_back({0, std::max<Tick>(1, length), offset, patternLaneCurve(pa)});
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
  if (!points.empty()) p.automation.push_back(PatternAutomation{owner, index, std::move(points)});
}

const PatternAutomation* findPatternLane(const Pattern& p, ObjectId owner, uint32_t index) {
  for (const auto& l : p.automation)
    if (l.owner == owner && l.index == index) return &l;
  return nullptr;
}

}  // namespace gl
